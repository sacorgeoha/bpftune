/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
/* Copyright (c) 2023, Oracle and/or its affiliates. */

#define _GNU_SOURCE  
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <dirent.h>
#include <libgen.h>
#include <linux/limits.h>
#include <linux/types.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sched.h>
#include <mntent.h>

#include "libbpftune.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/libbpf_version.h>

#include "probe.skel.h"
#include "probe.skel.legacy.h"

#ifndef SO_NETNS_COOKIE
#define SO_NETNS_COOKIE 71
#endif

void *bpftune_log_ctx;

int bpftune_loglevel = LOG_INFO;

struct ring_buffer *ring_buffer;
int ring_buffer_map_fd;
int corr_map_fd;

int bpftune_log_level(void)
{
	return bpftune_loglevel;
}

void bpftune_log_stderr(__attribute__((unused)) void *ctx,
			__attribute__((unused)) int level,
			const char *fmt, va_list args)
{
	if (level <= bpftune_loglevel) {
		fprintf(stderr, "bpftune: ");
		vfprintf(stderr, fmt, args);
	}
}

void bpftune_log_syslog(__attribute__((unused)) void *ctx, int level,
			const char *fmt, va_list args)
{
	char buf[512];
	int buflen;

	buflen = vsnprintf(buf, sizeof(buf), fmt, args);
	if (buflen > 0)
		syslog(level, buf, buflen + 1);
}

void (*bpftune_logfn)(void *ctx, int level, const char *fmt, va_list args) =
	bpftune_log_stderr;

static void __bpftune_log(int level, const char *fmt, va_list args)
{
		bpftune_logfn(bpftune_log_ctx, level, fmt, args);
}

void bpftune_log(int level, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	bpftune_logfn(bpftune_log_ctx, level, fmt, args);
	va_end(args);
}

static int bpftune_printall(__attribute__((unused)) enum libbpf_print_level l,
			    const char *format, va_list args)
{
	__bpftune_log(LOG_DEBUG, format, args);
        return 0;
}

void bpftune_set_log(int level,
		     void (*logfn)(void *ctx, int level, const char *fmt,
				   va_list args))
{
	if (logfn)
		bpftune_logfn = logfn;
	bpftune_loglevel = level;
	if (logfn == bpftune_log_syslog) {
		setlogmask(LOG_UPTO(level));
                openlog("bpftune", LOG_NDELAY | LOG_PID, LOG_DAEMON);
	}
	if (level >= LOG_DEBUG)
		libbpf_set_print(bpftune_printall);
}

void bpftune_log_bpf_err(int err, const char *fmt)
{
	char errbuf[256];

	(void) libbpf_strerror(err, errbuf, sizeof(errbuf));
	bpftune_log(LOG_ERR, fmt, errbuf);
}

static char bpftune_cgroup_path[PATH_MAX];
static int __bpftune_cgroup_fd;

int bpftune_cgroup_init(const char *cgroup_path)
{
	int err;

	strncpy(bpftune_cgroup_path, cgroup_path, sizeof(bpftune_cgroup_path));
	__bpftune_cgroup_fd = open(cgroup_path, O_RDONLY);
	if (__bpftune_cgroup_fd < 0) {
		if (!mkdir(cgroup_path, 0777)) {
			err = -errno;
			bpftune_log(LOG_ERR, "couldnt create cgroup dir '%s': %s\n",
				    cgroup_path, strerror(-err));
                        return err;
		}
		close(__bpftune_cgroup_fd);
	}
	if (!mount("none" , cgroup_path, "cgroup2", 0, NULL)) {
		err = -errno;
		if (err != -EEXIST) {
			bpftune_log(LOG_ERR, "couldnt mount cgroup2 for '%s': %s\n",
				    strerror(-err));
			if (__bpftune_cgroup_fd > 0)
				close(__bpftune_cgroup_fd);
			return err;
		}
	}
	if (__bpftune_cgroup_fd < 0)
		__bpftune_cgroup_fd = open(cgroup_path, O_RDONLY);
	if (__bpftune_cgroup_fd < 0) {
		err = -errno;
		bpftune_log(LOG_ERR, "cannot open cgroup dir '%s': %s\n",
			    cgroup_path, strerror(-err));
		return err;
	}
	return 0;
}

const char *bpftune_cgroup_name(void)
{
	return bpftune_cgroup_path;
}

int bpftune_cgroup_fd(void)
{
	return __bpftune_cgroup_fd;
}

void bpftune_cgroup_fini(void)
{
	if (__bpftune_cgroup_fd)
		close(__bpftune_cgroup_fd);
}

int bpftuner_cgroup_attach(struct bpftuner *tuner, const char *prog_name,
			   enum bpf_attach_type attach_type)
{
	int prog_fd, cgroup_fd, err;
	struct bpf_program *prog;
	const char *cgroup_dir;

	/* attach to root cgroup */
	cgroup_dir = bpftune_cgroup_name();

	if (!cgroup_dir) {
		bpftune_log(LOG_ERR, "cannot get cgroup_dir\n");
		return 1;
	}
	cgroup_fd = bpftune_cgroup_fd();
	prog = bpf_object__find_program_by_name(tuner->obj, prog_name);
	if (!prog) {
		bpftune_log(LOG_ERR, "no prog '%s'\n", prog_name);
		return -EINVAL;
	}
	prog_fd = bpf_program__fd(prog);

	if (bpf_prog_attach(prog_fd, cgroup_fd, attach_type,
			    BPF_F_ALLOW_MULTI)) {
		err = -errno;
		bpftune_log(LOG_ERR, "cannot attach '%s' to cgroup '%s': %s\n",
			    prog_name, cgroup_dir, strerror(-err));
		return 1;
	}
	return 0;
}

void bpftuner_cgroup_detach(struct bpftuner *tuner, const char *prog_name,
			   enum bpf_attach_type attach_type)
{
	int prog_fd, cgroup_fd, err;
	struct bpf_program *prog;

	prog = bpf_object__find_program_by_name(tuner->obj, prog_name);
	if (prog) {
		prog_fd = bpf_program__fd(prog);
		cgroup_fd = bpftune_cgroup_fd();

		if (bpf_prog_detach2(prog_fd, cgroup_fd, attach_type)) {
                        err = -errno;
                        bpftune_log(LOG_ERR, "error detaching prog fd %d, cgroup fd %d: %s\n",
                                prog_fd, cgroup_fd, strerror(-err));
                }
        }
}

static bool force_bpf_legacy;
static bool netns_cookie_supported;

void bpftuner_force_bpf_legacy(void)
{
	force_bpf_legacy = true;
}

bool bpftune_netns_cookie_supported(void)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	unsigned long netns_cookie;
	int ret = 0;

	if (s < 0) {
		bpftune_log(LOG_ERR, "could not open socket: %s\n",
			   strerror(errno));
		return false;
	} else {
		socklen_t cookie_sz = sizeof(netns_cookie);

		ret = getsockopt(s, SOL_SOCKET, SO_NETNS_COOKIE, &netns_cookie,
				 &cookie_sz);
		close(s);
		if (ret < 0) {
			bpftune_log(LOG_DEBUG, "netns cookie not supported, cannot monitor per-netns events\n");
			return false;
		}
        }
	return true;
}

enum bpftune_support_level bpftune_bpf_support(void)
{
	enum bpftune_support_level support_level = BPFTUNE_NORMAL;
	bool ret;
	struct probe_bpf *probe_bpf = probe_bpf__open_and_load();
	struct probe_bpf_legacy *probe_bpf_legacy;

	if (probe_bpf == NULL) {
		support_level = BPFTUNE_LEGACY;
		bpftune_log(LOG_DEBUG, "full bpftune support not available: %s\n",
			    strerror(errno));
		probe_bpf_legacy = probe_bpf_legacy__open_and_load();		
		if (!probe_bpf_legacy) {
			support_level = BPFTUNE_NONE;
			bpftune_log(LOG_DEBUG, "legacy bpftune support not available: %s\n",
				    strerror(errno));
		} else {
			probe_bpf_legacy__destroy(probe_bpf_legacy);
		}
	} else {
		probe_bpf__destroy(probe_bpf);
	}
	ret = bpftune_netns_cookie_supported();
	if (!ret)
		bpftune_log(LOG_DEBUG, "netns cookie not supported\n");
	return support_level;
}

bool bpftuner_bpf_legacy(void)
{
	if (force_bpf_legacy)
		return true;

	return bpftune_bpf_support() < BPFTUNE_NORMAL;
}

int __bpftuner_bpf_load(struct bpftuner *tuner, const char **optionals)
{
	struct bpf_map *map;
	int err;	

	if (ring_buffer_map_fd > 0) {
		bpftune_log(LOG_DEBUG, "reusing ring buffer fd %d\n", ring_buffer_map_fd);
		err = bpf_map__reuse_fd(tuner->ring_buffer_map,
					ring_buffer_map_fd);
		if (err < 0) {
			bpftune_log_bpf_err(err, "could not reuse fd: %s\n");
			return err;
		}
		tuner->ring_buffer_map_fd = ring_buffer_map_fd;
	}
	
	if (corr_map_fd > 0) {
		bpftune_log(LOG_DEBUG, "reusing corr map fd %d\n", corr_map_fd);
		err = bpf_map__reuse_fd(tuner->corr_map, corr_map_fd);
		if (err < 0) {
			bpftune_log_bpf_err(err, "could not reuse corr fd: %s\n");
			return err;
		}
		tuner->corr_map_fd = corr_map_fd;
	}
	if (optionals) {
		int i;

		for (i = 0; optionals[i] != NULL; i++) {
			struct bpf_program *prog;

			prog = bpf_object__find_program_by_name(tuner->obj,
								optionals[i]);
			if (prog) {
				bpftune_log(LOG_DEBUG, "marking '%s' as optional\n",
					    optionals[i]);
				bpf_program__set_autoload(prog, false);
			}
		}
	}
	err = bpf_object__load_skeleton(tuner->skeleton);
	if (err) {
		bpftune_log_bpf_err(err, "could not load skeleton: %s\n");
		return err;
	}
	
	if (ring_buffer_map_fd == 0) {
		map = bpf_object__find_map_by_name(*tuner->skeleton->obj,
						   "ring_buffer_map");
		if (map) {
			ring_buffer_map_fd = bpf_map__fd(map);
			bpftune_log(LOG_DEBUG, "got ring_buffer_map fd %d\n",
				    ring_buffer_map_fd);
		}
		tuner->ring_buffer_map_fd = ring_buffer_map_fd;
	}
	if (corr_map_fd == 0) {
		map = bpf_object__find_map_by_name(*tuner->skeleton->obj,
						   "corr_map");
		if (map) {
			corr_map_fd = bpf_map__fd(map);
			bpftune_log(LOG_DEBUG, "got corr_map fd %d\n",
				    corr_map_fd);
			tuner->corr_map_fd = corr_map_fd;
		}
	}

	return 0;
}

int __bpftuner_bpf_attach(struct bpftuner *tuner)
{
	int err = bpf_object__attach_skeleton(tuner->skeleton);
	if (err) {
		bpftune_log_bpf_err(err, "could not attach skeleton: %s\n");
		return err;
	}
	tuner->ring_buffer_map_fd = bpf_map__fd(tuner->ring_buffer_map);

	return 0;
}

static unsigned int bpftune_num_tuners;

void bpftuner_bpf_fini(struct bpftuner *tuner)
{
	bpf_object__destroy_skeleton(tuner->skeleton);
	free(tuner->skel);
	if (bpftune_num_tuners == 0) {
		if (ring_buffer_map_fd > 0)
			close(ring_buffer_map_fd);
		if (corr_map_fd > 0)
			close(corr_map_fd);
		ring_buffer_map_fd = corr_map_fd = 0;
	}
}

static struct bpftuner *bpftune_tuners[BPFTUNE_MAX_TUNERS];

/* add a tuner to the list of tuners, or replace existing inactive tuner.
 * If successful, call init().
 */
struct bpftuner *bpftuner_init(const char *path)
{
	struct bpftuner *tuner = NULL;
	int err;

	tuner = calloc(1, sizeof(*tuner));
	if (!tuner) {
		bpftune_log(LOG_ERR, "could not allocate tuner\n");
		return NULL;
	}
	tuner->handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
	if (!tuner->handle) {
		bpftune_log(LOG_ERR, "could not dlopen '%s': %s\n",
			    path, dlerror());
		free(tuner);
		return NULL;
	}
	/* If we have a ringbuf fd from any tuner, use its fd to be re-used
 	 * for other ringbuf maps (so we can use the same ring buffer for all
 	 * BPF events.
 	 */
	tuner->ring_buffer_map_fd = ring_buffer_map_fd;
	tuner->init = dlsym(tuner->handle, "init");
	tuner->fini = dlsym(tuner->handle, "fini");
	tuner->event_handler = dlsym(tuner->handle, "event_handler");
	
	bpftune_log(LOG_DEBUG, "calling init for '%s\n", path);
	err = tuner->init(tuner);
	if (err) {
		dlclose(tuner->handle);
		bpftune_log(LOG_ERR, "error initializing '%s: %s\n",
			    path, strerror(-err));
		free(tuner);
		return NULL;
	}
	tuner->id = bpftune_num_tuners;
	tuner->state = BPFTUNE_ACTIVE;
	bpftune_tuners[bpftune_num_tuners++] = tuner;
	bpftune_log(LOG_DEBUG, "sucessfully initialized tuner %s[%d]\n",
		    tuner->name, tuner->id);
	return tuner;
}

static void bpftuner_scenario_log(struct bpftuner *tuner, unsigned int tunable,
				  unsigned int scenario, int netns_fd,
				  bool summary,
				  const char *fmt, va_list args);

unsigned long global_netns_cookie;

void bpftuner_fini(struct bpftuner *tuner, enum bpftune_state state)
{
	unsigned int i, j;

	if (!tuner || tuner->state != BPFTUNE_ACTIVE)
		return;

	bpftune_log(LOG_DEBUG, "cleaning up tuner %s with %d tunables, %d scenarios\n",
		    tuner->name, tuner->num_tunables, tuner->num_scenarios);
	/* report summary of events for tuner */
	for (i = 0; i < tuner->num_tunables; i++) {
		for (j = 0; j < tuner->num_scenarios; j++) {
			va_list args;
			bpftune_log(LOG_DEBUG, "checking scenarios for tuner %d, scenario %d\n",
				    i, j);
			bpftuner_scenario_log(tuner, i, j, 0, true, NULL, args);
			bpftuner_scenario_log(tuner, i, j, 1, true, NULL, args);
		}
	}
	if (tuner->fini)
		tuner->fini(tuner);

	tuner->state = state;
}

struct bpftuner *bpftune_tuner(unsigned int index)
{
	if (index < bpftune_num_tuners)
		return bpftune_tuners[index];
	return NULL;
}

unsigned int bpftune_tuner_num(void)
{
	return bpftune_num_tuners;
}

static int bpftune_ringbuf_event_read(void *ctx, void *data, size_t size)
{
	struct bpftune_event *event = data;
	struct bpftuner *tuner;

	if (size < sizeof(*event)) {
		bpftune_log(LOG_ERR, "unexpected size event %d\n", size);
		return 0;
	}
	if (event->tuner_id > BPFTUNE_MAX_TUNERS) {
		bpftune_log(LOG_ERR, "invalid tuner id %d\n", event->tuner_id);
		return 0;
	}
	tuner = bpftune_tuner(event->tuner_id);
	if (!tuner) {
		bpftune_log(LOG_ERR, "no tuner for id %d\n", event->tuner_id);
		return 0;
	}
	bpftune_log(LOG_DEBUG,
		    "event scenario [%d] for tuner %s[%d] netns %ld (%s)\n",
		    event->scenario_id, tuner->name, tuner->id,
		    event->netns_cookie,
		    event->netns_cookie && event->netns_cookie != global_netns_cookie ?
		    "non-global netns" : "global netns");
	tuner->event_handler(tuner, event, ctx);

	return 0;
}

int bpftuner_ring_buffer_map_fd(struct bpftuner *tuner)
{
	return tuner->ring_buffer_map_fd;
}

void *bpftune_ring_buffer_init(int ring_buffer_map_fd, void *ctx)
{
	struct ring_buffer *rb;
	int err;

	bpftune_log(LOG_DEBUG, "calling ring_buffer__new, ringbuf_map_fd %d\n",
		    ring_buffer_map_fd);
	rb = ring_buffer__new(ring_buffer_map_fd, bpftune_ringbuf_event_read, ctx, NULL);
	err = libbpf_get_error(rb);
	if (err) {
		bpftune_log_bpf_err(err, "couldnt create ring buffer: %s\n");
		return NULL;
	}
	return rb;
}

static int ring_buffer_done;

int bpftune_ring_buffer_poll(void *ring_buffer, int interval)
{
	struct ring_buffer *rb = ring_buffer;
	int err;

	while (!ring_buffer_done) {
		err = ring_buffer__poll(rb, interval);
		if (err < 0) {
			bpftune_log_bpf_err(err, "ring_buffer__poll: %s\n");
			break;
		}
	}
	ring_buffer__free(rb);
	return 0;
}

void bpftune_ring_buffer_fini(__attribute__((unused)) void *ring_buffer)
{
	ring_buffer_done = true;
}


#define BPFTUNE_PROC_SYS	"/proc/sys/"
void bpftune_sysctl_name_to_path(const char *name, char *path, size_t path_sz)
{
	size_t i;

	snprintf(path, path_sz, BPFTUNE_PROC_SYS "%s", name);
	for (i = 0; i < path_sz && path[i] != 0; i++)
		if (path[i] == '.')
			path[i] = '/';
}

int bpftune_sysctl_read(int netns_fd, const char *name, long *values)
{
	int orig_netns_fd, i, num_values = 0;
	char path[512];
	int err = 0;	
	FILE *fp;

	bpftune_sysctl_name_to_path(name, path, sizeof(path));

	err = bpftune_netns_set(netns_fd, &orig_netns_fd);
	if (err < 0)
		return err;

	fp = fopen(path, "r");
	if (!fp) {
		err = -errno;
		bpftune_log(LOG_ERR, "could not open %s (netns fd %d) for reading: %s\n",
			    path, netns_fd, strerror(-err));
		goto out;
	}
	num_values = fscanf(fp, "%ld %ld %ld",
			    &values[0], &values[1], &values[2]);
	if (num_values == 0)
		err = -ENOENT;
	else if (num_values < 0)
		err = -errno;
	fclose(fp);

	if (err) {
		bpftune_log(LOG_ERR, "could not read from %s: %s\n", path,
			    strerror(-err));
		goto out;
	}

	for (i = 0; i < num_values; i++) {
		bpftune_log(LOG_DEBUG, "Read %s[%d] = %ld\n",
			    name, i, values[i]);
	}

out:
	bpftune_netns_set(orig_netns_fd, NULL);
	return num_values;
}

int bpftune_sysctl_write(int netns_fd, const char *name, __u8 num_values, long *values)
{
	long old_values[BPFTUNE_MAX_VALUES];	
	int i, err = 0, orig_netns_fd;
	int old_num_values;
	char path[512];
	FILE *fp;

	bpftune_sysctl_name_to_path(name, path, sizeof(path));

	bpftune_log(LOG_DEBUG, "writing sysctl '%s' for netns_fd %d\n",
		    path, netns_fd);

	err = bpftune_netns_set(netns_fd, &orig_netns_fd);
	if (err < 0)
		return err;

	/* If value is already set to val, do nothing. */
	old_num_values = bpftune_sysctl_read(0, name, old_values);
	if (old_num_values < 0) {
		err = old_num_values;
		goto out;
	}
	if (num_values == old_num_values) {
		for (i = 0; i < num_values; i++) {
			if (old_values[i] != values[i])
				break;
		}
		if (i == num_values)
			return 0;
	}
        fp = fopen(path, "w");
        if (!fp) {
                err = -errno;
                bpftune_log(LOG_DEBUG, "could not open %s for writing: %s\n",
			    path, strerror(-err));
		goto out;
        }

	for (i = 0; i < num_values; i++)
		fprintf(fp, "%ld ", values[i]);
        fclose(fp);

	for (i = 0; i < num_values; i++) {
		bpftune_log(LOG_DEBUG, "Wrote %s[%d] = %ld\n",
			    name, i, values[i]);
	}
out:
	bpftune_netns_set(orig_netns_fd, NULL);
        return 0;
}

int bpftuner_tunables_init(struct bpftuner *tuner, unsigned int num_descs,
			   struct bpftunable_desc *descs,
			   unsigned int num_scenarios,
			   struct bpftunable_scenario *scenarios)
{
	unsigned int i;

	tuner->scenarios = scenarios;
	tuner->num_scenarios = num_scenarios;
	tuner->tunables = calloc(num_descs, sizeof(struct bpftunable));
	if (!tuner->tunables) {
		bpftune_log(LOG_DEBUG, "no memory to alloc tunables for %s\n",
			    tuner->name);
		return -ENOMEM;
	}
	tuner->num_tunables = num_descs;
	for (i = 0; i < num_descs; i++) {
		int num_values;

		bpftune_log(LOG_DEBUG, "handling desc %ld/%ld\n", i, num_descs);
		memcpy(&tuner->tunables[i].desc, &descs[i], sizeof(*descs));

		if (descs[i].type != BPFTUNABLE_SYSCTL)
			continue;
		num_values = bpftune_sysctl_read(0, descs[i].name,
				tuner->tunables[i].current_values);
		if (num_values < 0) {
			bpftune_log(LOG_ERR, "error reading tunable '%s': %s\n",
				    descs[i].name, strerror(-num_values));
			return num_values;
		}
		if (num_values != descs[i].num_values) {
			bpftune_log(LOG_ERR, "error reading tunable '%s'; expected %d values, got %d\n",
				    descs[i].num_values, num_values);
			return -EINVAL;
		}
		memcpy(tuner->tunables[i].initial_values,
		       tuner->tunables[i].current_values,
		       sizeof(tuner->tunables[i].initial_values));
	}

	return 0;
}

struct bpftunable *bpftuner_tunable(struct bpftuner *tuner, unsigned int index)
{
	if (index < tuner->num_tunables)
		return &tuner->tunables[index];
	return NULL;
}

static void bpftuner_tunable_stats_update(struct bpftunable *tunable,
					  unsigned int scenario, bool global_ns)
{
	if (global_ns)
		(tunable->stats.global_ns[scenario])++;
	else
		(tunable->stats.nonglobal_ns[scenario])++;
	bpftune_log(LOG_DEBUG," updated stat for tunable %s, scenario %d: %lu\n",
		    tunable->desc.name, scenario,
		    global_ns ? tunable->stats.global_ns[scenario] :
				tunable->stats.nonglobal_ns[scenario]);

}

static void bpftuner_scenario_log(struct bpftuner *tuner, unsigned int tunable,
				  unsigned int scenario, int netns_fd,
				  bool summary,
				  const char *fmt, va_list args)
{
	struct bpftunable *t = bpftuner_tunable(tuner, tunable);
	bool global_ns = netns_fd == 0;

	if (summary) {
		unsigned long count;

		count = global_ns ? t->stats.global_ns[scenario] :
				    t->stats.nonglobal_ns[scenario];
		if (!count)
			return;
		bpftune_log(LOG_INFO, "Summary: scenario '%s' occurred %ld times for tunable '%s' in %sglobal ns. %s\n",
			    tuner->scenarios[scenario].name, count,
			    t->desc.name,
			    global_ns ? "" : "non-",
			    tuner->scenarios[scenario].description);
		if (t->desc.type == BPFTUNABLE_SYSCTL) {
			char oldvals[256] = { };
			char newvals[256] = { };
			char s[256];
			__u8 i;

			for (i = 0; i < t->desc.num_values; i++) {
				snprintf(s, sizeof(s), "%ld ",
					 t->initial_values[i]);
				strcat(oldvals, s);
				snprintf(s, sizeof(s), "%ld ",
					 t->current_values[i]);
				strcat(newvals, s);
			}
			bpftune_log(LOG_INFO, "sysctl '%s' changed from (%s) -> (%s)\n",
				    t->desc.name, oldvals, newvals);
		}
	} else {
		bpftune_log(LOG_INFO, "Scenario '%s' occurred for tunable '%s' in %sglobal ns. %s\n",
			    tuner->scenarios[scenario].name,
			    t->desc.name,
			    global_ns ? "" : "non-",
			    tuner->scenarios[scenario].description);
		__bpftune_log(LOG_INFO, fmt, args);
		bpftuner_tunable_stats_update(t, scenario, global_ns);
	}
}

int bpftuner_tunable_sysctl_write(struct bpftuner *tuner, unsigned int tunable,
				  unsigned int scenario, int netns_fd,
				  __u8 num_values, long *values,
				  const char *fmt, ...)
{
	struct bpftunable *t = bpftuner_tunable(tuner, tunable);
	int ret = 0, fd;

	if (!t) {
		bpftune_log(LOG_ERR, "no tunable %d for tuner '%s'\n",
			    tunable, tuner->name);
		return -EINVAL;
	}

	fd = t->desc.namespaced ? netns_fd : 0;

	ret = bpftune_sysctl_write(fd, t->desc.name, num_values, values);
	if (!ret) {
		va_list args;
		__u8 i;

		va_start(args, fmt);
		bpftuner_scenario_log(tuner, tunable, scenario, netns_fd,
				      false, fmt, args);
		va_end(args);

		for (i = 0; i < t->desc.num_values; i++)
			t->current_values[i] = values[i];
	}

	return ret;
}

int bpftuner_tunable_update(struct bpftuner *tuner, unsigned int tunable,
			    unsigned int scenario, int netns_fd,
			    const char *fmt, ...)
{
	struct bpftunable *t = bpftuner_tunable(tuner, tunable);
	va_list args;

	if (!t) {
		bpftune_log(LOG_ERR, "no tunable %d for tuner '%s'\n",
			    tunable, tuner->name);
		return -EINVAL;
	}
	va_start(args, fmt);
	bpftuner_scenario_log(tuner, tunable, scenario, netns_fd, false,
			      fmt, args);
	va_end(args);

	return 0;
}

unsigned int bpftuner_num_tunables(struct bpftuner *tuner)
{
	return tuner->num_tunables;
}

void bpftuner_tunables_fini(struct bpftuner *tuner)
{
	tuner->num_tunables = 0;
	free(tuner->tunables);
}

static int bpftune_netns_fd(int netns_pid)
{
	char netns_path[256];
	int netns_fd;

	if (netns_pid == 0)
		return 0;
	snprintf(netns_path, sizeof(netns_path), "/proc/%d/ns/net", netns_pid);
	netns_fd = open(netns_path, O_RDONLY);
	if (netns_fd < 0)
		netns_fd = -errno;
	return netns_fd;
}

/* sets original netns fd if orig_fd is non-NULL */
int bpftune_netns_set(int new_fd, int *orig_fd)
{
	int fd, err = 0;

	if (orig_fd)
		*orig_fd = 0;

	if (!new_fd)
		return 0;
	fd = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		err = -errno;
		bpftune_log(LOG_ERR, "could not get current netns fd(%d): %s\n",
			    fd, strerror(-err));
	} else {
		err = setns(new_fd, CLONE_NEWNET);
		if (err < 0) {
			err = -errno;
			close(fd);
			bpftune_log(LOG_ERR, "could not %ssetns(%d): %s\n",
				    orig_fd ? "" : "restore",
				    new_fd, strerror(-err));
		}
	}
	if (!err && orig_fd)
		*orig_fd = fd;
	else
		close(fd);
	return err;
}

/* get fd, cookie (if non-NULL) from pid, or if pid is 0, use passed in
 * *fd to get cookie.
 */
int bpftune_netns_info(int pid, int *fd, unsigned long *cookie)
{
	int netns_fd, orig_netns_fd;
	unsigned long netns_cookie;
	bool fdnew = true;
	int ret, err;

	if (pid == 0 && fd && *fd > 0) {
		fdnew = false;
		netns_fd = *fd;
	} else {
		netns_fd = bpftune_netns_fd(pid);
		if (netns_fd <= 0)
			return netns_fd;
	}

	err = bpftune_netns_set(netns_fd, &orig_netns_fd);
	if (!err) {
		int s = socket(AF_INET, SOCK_STREAM, 0);

		if (s < 0) {
			ret = -errno;
			bpftune_log(LOG_ERR, "could not open socket in netns: %s\n",
				    strerror(errno)); 
		} else {
			socklen_t cookie_sz = sizeof(netns_cookie);

			ret = getsockopt(s, SOL_SOCKET, SO_NETNS_COOKIE, &netns_cookie,
					 &cookie_sz);
			if (ret < 0) {
				ret = -errno;
				bpftune_log(LOG_DEBUG,
					    "could not get SO_NETNS_COOKIE: %s\n",
					   strerror(-ret));
				/* system may not support SO_NETNS_COOKIE */
			}
			
			close(s);
		}
		bpftune_netns_set(orig_netns_fd, NULL);

		if (ret == 0) {
			if (fdnew && fd)
				*fd = netns_fd;
			if (cookie)
				*cookie = netns_cookie;
		}
	} else {
		bpftune_log(LOG_DEBUG, "setns failed for for fd %d\n",
			    netns_fd);
		ret = -errno;
	}
	if (fdnew && !fd)
		close(netns_fd);
	if (orig_netns_fd > 0)
		close(orig_netns_fd);
	return ret;
}

static int bpftune_netns_find(unsigned long cookie)
{
	unsigned long netns_cookie;
	struct bpftuner *t;
	struct mntent *ent;
        FILE *mounts;
	struct dirent *dirent;
	int ret = -ENOENT;
	DIR *dir;

	if (!netns_cookie_supported || cookie == 0 || (global_netns_cookie && cookie == global_netns_cookie))
		return 0;

	mounts = setmntent("/proc/mounts", "r");
	if (mounts == NULL) {
		ret = -errno;
		bpftune_log(LOG_ERR, "cannot setmntent() for /proc/mounts\n",
			    strerror(-ret));
		return ret;
	}
	while ((ent = getmntent(mounts)) != NULL) {
		int mntfd;

		if (strcmp(ent->mnt_type, "nsfs") != 0)
			continue;
		bpftune_log(LOG_DEBUG, "checking nsfs mnt %s\n",
			    ent->mnt_dir);
		mntfd = open(ent->mnt_dir, O_RDONLY);
		if (mntfd < 0)
			continue;
		if (bpftune_netns_info(0, &mntfd, &netns_cookie)) {
			close(mntfd);
			continue;
		}
		bpftune_log(LOG_DEBUG, "found netns fd %d for cookie %ld via mnt %s\n",
			    netns_cookie, ent->mnt_dir);
		if (cookie == 0) {
			close(mntfd);
			bpftune_for_each_tuner(t)
				bpftuner_netns_init(t, netns_cookie);
			ret = 0;
			continue;
		}
		ret = mntfd;
		endmntent(mounts);
		return ret;
	}
	endmntent(mounts);

	/* move on to /proc-derived namespace fds... */
	dir = opendir("/proc");
	if (!dir) { 
		ret = -errno;   
		bpftune_log(LOG_ERR, "could not open /proc: %s\n", strerror(-ret));
		return ret;
	}
	while ((dirent = readdir(dir)) != NULL) {
		char *endptr;
		int netns_fd;
		long pid;

		pid = strtol(dirent->d_name, &endptr, 10);
		if (!endptr || *endptr != '\0')
			continue;
		if (bpftune_netns_info(pid, &netns_fd, &netns_cookie))
			continue;

		if (cookie == 0) {
			close(netns_fd);
			bpftune_for_each_tuner(t)
				bpftuner_netns_init(t, netns_cookie);
			continue;
		}
		if (netns_cookie == cookie) {
			bpftune_log(LOG_DEBUG, "found netns fd %d for cookie %ld via pid %d\n",
				    netns_fd, netns_cookie, pid);
			ret = netns_fd;
			break;
		} else {
			close(netns_fd);
		}
	}
	closedir(dir);

	return ret;
}

int bpftune_netns_fd_from_cookie(unsigned long cookie)
{
	return bpftune_netns_find(cookie);
}

int bpftune_netns_init_all(void)
{
	unsigned long cookie;

	netns_cookie_supported = bpftune_netns_cookie_supported();
	if (!netns_cookie_supported)
		return 0;

	if (!bpftune_netns_info(getpid(), NULL, &cookie)) {
		global_netns_cookie = cookie;
		bpftune_log(LOG_DEBUG, "global netns cookie is %ld\n",
			    global_netns_cookie);
	}
	return bpftune_netns_find(0);
}

void bpftuner_netns_init(struct bpftuner *tuner, unsigned long cookie)
{
	struct bpftuner_netns *netns, *new = NULL;

	if (bpftuner_netns_from_cookie(tuner->id, cookie))
		return;

	for (netns = &tuner->netns; netns->next != NULL; netns = netns->next) {}

	new = calloc(1, sizeof(struct bpftuner_netns));
	if (!new) {
		bpftune_log(LOG_ERR, "unable to allocate netns for bpftuner: %s\n",
			    strerror(errno));
	} else {
		new->netns_cookie = cookie;
		netns->next = new;
	}
}

void bpftuner_netns_fini(struct bpftuner *tuner, unsigned long cookie)
{
	struct bpftuner_netns *netns, *prev = NULL;

	if (!netns_cookie_supported)
		return;
	
	for (netns = &tuner->netns; netns != NULL; netns = netns->next) {
		if (netns->netns_cookie == cookie) {
			if (prev)
				prev->next = netns->next;
			else
				tuner->netns.next = netns->next;
			return;
		}
		prev = netns;
	}
	bpftune_log(LOG_DEBUG, "netns_fini: could not find netns for cookie %ld\n",
		    cookie);
}

struct bpftuner_netns *bpftuner_netns_from_cookie(unsigned long tuner_id,
						  unsigned long cookie)
{
	struct bpftuner *tuner;
	struct bpftuner_netns *netns;
	
	if (!netns_cookie_supported)
		return NULL;

	bpftune_for_each_tuner(tuner) {
		if (tuner->id != tuner_id)
			continue;
		if (cookie == 0)
			return &tuner->netns;
		bpftuner_for_each_netns(tuner, netns) {
			if (cookie == netns->netns_cookie)
				return netns;
		}
	}
	bpftune_log(LOG_DEBUG, "no tuner netns found for tuner %d, cookie %ld\n",
		    tuner_id, cookie);
	return NULL;
}

static int bpftune_module_path(const char *name, char *modpath)
{
	struct utsname utsname;
	int ret;

	if (uname(&utsname) < 0) {
		ret = -errno;
		bpftune_log(LOG_DEBUG, "uname failed: %s\n", strerror(ret));
		return ret;
	}
	snprintf(modpath, PATH_MAX, "/usr/lib/modules/%s/kernel/%s",
		 utsname.release, name);
	return 0;
}

/* load module name, e.g. net/ipv4/foo.ko */
int bpftune_module_load(const char *name)
{
	char modpath[PATH_MAX];
	int ret, fd;

	ret = bpftune_module_path(name, modpath);
	if (ret)
		return ret;

	fd = open(modpath, O_RDONLY);
	if (fd < 0) {
		bpftune_log(LOG_DEBUG, "no module '%s' found.\n", modpath);
		return -errno;
	}
	ret = syscall(__NR_finit_module, fd, "", 0);
	if (ret) {
		bpftune_log(LOG_DEBUG, "could not init module '%s'\n",
			    modpath);
		ret = -errno;
	}
	close(fd);
	return ret;
}

int bpftune_module_delete(const char *name)
{
	int ret;

	ret = syscall(__NR_delete_module, name, 0);
	if (ret) {
		bpftune_log(LOG_DEBUG, "could not delete module '%s'\n",
			    name);
		ret = -errno;
	}
	return ret;
}
