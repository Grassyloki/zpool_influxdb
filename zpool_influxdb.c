/*
 * Gather top-level ZFS pool and resilver/scan statistics and print using
 * influxdb line protocol
 * usage: [options] [pool_name]
 * where options are:
 *   --execd, -e           run in telegraf execd input plugin mode, [CR] on
 *                         stdin causes a sample to be printed and wait for
 *                         the next [CR]
 *   --no-histograms, -n   don't print histogram data (reduces cardinality
 *                         if you don't care about histograms)
 *   --sum-histogram-buckets, -s sum histogram bucket values
 *
 * To integrate into telegraf use one of:
 * 1. the `inputs.execd` plugin with the `--execd` option
 * 2. the `inputs.exec` plugin to simply run with no options
 *
 * NOTE: libzfs is an unstable interface. YMMV.
 * For Linux compile with:
 *    cmake . && make && make install
 *
 * The design goals of this software include:
 * + be as lightweight as possible
 * + reduce the number of external dependencies as far as possible, hence
 *   there is no dependency on a client library for managing the metric
 *   collection -- info is printed, KISS
 * + broken pools or kernel bugs can cause this process to hang in an
 *   unkillable state. For this reason, it is best to keep the damage limited
 *   to a small process like zpool_influxdb rather than a larger collector.
 *
 * Copyright 2018-2020 Richard Elling
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/fs/zfs.h>
#include <libzfs.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/file.h>

/*
 * The third argument of nvlist_lookup_string() (and the string it returns)
 * gained a const qualifier in OpenZFS 2.2. HAVE_CONST_NVLIST_LOOKUP_STRING is
 * defined by CMake when building against the newer, const-correct headers.
 * Use the matching type so the source compiles cleanly on both old (e.g.
 * Debian 12 / ZFS 2.1) and new (ZFS >= 2.2) libzfs.
 */
#ifdef HAVE_CONST_NVLIST_LOOKUP_STRING
typedef const char *nvstring_t;
#else
typedef char *nvstring_t;
#endif

#define POOL_MEASUREMENT        "zpool_stats"
#define SCAN_MEASUREMENT        "zpool_scan_stats"
#define VDEV_MEASUREMENT        "zpool_vdev_stats"
#define POOL_LATENCY_MEASUREMENT        "zpool_latency"
#define POOL_QUEUE_MEASUREMENT  "zpool_vdev_queue"
#define MIN_LAT_INDEX        10  /* minimum latency index 10 = 1024ns */
#define POOL_IO_SIZE_MEASUREMENT        "zpool_io_size"
#define MIN_SIZE_INDEX        9  /* minimum size index 9 = 512 bytes */
#define POOL_IOSTAT_MEASUREMENT "zpool_iostat"

/*
 * telegraf 1.6.4 can handle uint64, which is the native ZFS type
 * telegraf also handles the input of uint64 and will convert to match
 * influxdb via the outputs.influxdb plugin. This is the easiest method
 * for future compatibility. If is it not possible to use telegraf as a
 * metrics broker and unsigned 64-bit is not possible, then consider
 * defining SUPPORT_UINT64 in the CMakeLists.txt or Makefile.
 *
 * influxdb 1.x requires an option to enable uint64
 * influxdb 2.x supports uint64
 */
#ifdef SUPPORT_UINT64
#define IFMT "%luu"
#define MASK_UINT64(x) (x)
#else
#define IFMT "%lui"
#define MASK_UINT64(x) ((x) & INT64_MAX)
#endif

/* global options */
int execd_mode = 0;
int no_histograms = 0;
int sum_histogram_buckets = 0;
int timeout_secs = 0;       /* watchdog timeout in seconds, 0 = disabled */
char *lock_file = NULL;     /* single-instance lock file path, NULL = disabled */
int iostat_interval = 1;    /* zpool-iostat sampling interval (s); 0 disables.
                             * Default 1 adds ~1s/run to emit zpool_iostat. */
uint64_t timestamp = 0;

/*
 * Watchdog: if a sample takes longer than timeout_secs, give up rather than
 * overrunning the caller's polling interval. This bounds the common case of a
 * slow or contended sample. Note: it cannot interrupt a thread wedged in an
 * uninterruptible kernel ioctl (see "Caveat Emptor" in the README); such a
 * process can linger until the kernel condition clears.
 */
static void
timeout_handler(int sig) {
    (void) sig;
    static const char msg[] =
        "error: zpool_influxdb sample timed out; pool may be unresponsive\n";
    (void) write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(EXIT_FAILURE);
}

static void
arm_timeout(void) {
    if (timeout_secs > 0)
        (void) alarm((unsigned int) timeout_secs);
}

static void
disarm_timeout(void) {
    if (timeout_secs > 0)
        (void) alarm(0);
}

/*
 * Take a non-blocking exclusive lock on lock_file so that only one instance
 * runs at a time. When zpool_influxdb is polled frequently (e.g. by telegraf
 * every few seconds) and a sample runs long -- or wedges on an unresponsive
 * pool -- this stops new invocations from piling up and contending with the
 * stuck one. If the lock is already held, exit cleanly with no output so the
 * caller simply records no data for this interval. If the lock file cannot be
 * used we warn and continue rather than dropping samples.
 */
static void
acquire_single_instance_lock(void) {
    int fd;

    if (lock_file == NULL)
        return;

    fd = open(lock_file, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        (void) fprintf(stderr, "warning: cannot open lock file %s: %s\n",
                       lock_file, strerror(errno));
        return;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            /* another instance is still running; skip this sample */
            exit(EXIT_SUCCESS);
        }
        (void) fprintf(stderr, "warning: cannot lock %s: %s\n",
                       lock_file, strerror(errno));
        (void) close(fd);
        return;
    }
    /* leave fd open on purpose: the lock is held until this process exits */
}
int complained_about_sync = 0;

/*
 * in cases where ZFS is installed, but not the ZFS dev environment, copy in
 * the needed definitions from libzfs_impl.h
 */
#ifndef _LIBZFS_IMPL_H
struct zpool_handle {
    libzfs_handle_t *zpool_hdl;
    zpool_handle_t *zpool_next;
    char zpool_name[ZFS_MAX_DATASET_NAME_LEN];
    int zpool_state;
    size_t zpool_config_size;
    nvlist_t *zpool_config;
    nvlist_t *zpool_old_config;
    nvlist_t *zpool_props;
    diskaddr_t zpool_start_block;
};
#endif

/*
 * influxdb line protocol rules for escaping are important because the
 * zpool name can include characters that need to be escaped
 *
 * caller is responsible for freeing result
 */
char *
escape_string(char *s) {
	char *c, *d;
	char *t = (char *) malloc(ZFS_MAX_DATASET_NAME_LEN * 2);
	if (t == NULL) {
		fprintf(stderr, "error: cannot allocate memory\n");
		exit(1);
	}

	for (c = s, d = t; *c != '\0'; c++, d++) {
		switch (*c) {
			case ' ':
			case ',':
			case '=':
			case '\\':
				*d++ = '\\';
			default:
				*d = *c;
		}
	}
	*d = '\0';
	return (t);
}

/*
 * print_scan_status() prints the details as often seen in the "zpool status"
 * output. However, unlike the zpool command, which is intended for humans,
 * this output is suitable for long-term tracking in influxdb.
 * TODO: update to include issued scan data
 */
int
print_scan_status(nvlist_t *nvroot, const char *pool_name) {
	uint_t c;
	int64_t elapsed;
	uint64_t examined, pass_exam, paused_time, paused_ts, rate;
	uint64_t remaining_time;
	pool_scan_stat_t *ps = NULL;
	double pct_done;
	char *state[DSS_NUM_STATES] = {"none", "scanning", "finished",
	                               "canceled"};
	char *func;

	(void) nvlist_lookup_uint64_array(nvroot,
	    ZPOOL_CONFIG_SCAN_STATS,
	    (uint64_t **) &ps, &c);

	/*
	 * ignore if there are no stats
	 */
	if (ps == NULL)
	    return (0);

	/*
	 * return error if state is bogus
	 */
	if (ps->pss_state >= DSS_NUM_STATES ||
	    ps->pss_func >= POOL_SCAN_FUNCS) {
	    if (complained_about_sync % 1000 == 0) {
            fprintf(stderr, "error: cannot decode scan stats: ZFS is "
                            "out of sync with compiled zpool_influxdb");
            complained_about_sync++;
        }
		return (1);
	}

	switch (ps->pss_func) {
		case POOL_SCAN_NONE:
			func = "none_requested";
			break;
		case POOL_SCAN_SCRUB:
			func = "scrub";
			break;
		case POOL_SCAN_RESILVER:
			func = "resilver";
			break;
#ifdef POOL_SCAN_REBUILD
		case POOL_SCAN_REBUILD:
				func = "rebuild";
				break;
#endif
		default:
			func = "scan";
	}

	/* overall progress */
	examined = ps->pss_examined ? ps->pss_examined : 1;
	pct_done = 0.0;
	if (ps->pss_to_examine > 0)
		pct_done = 100.0 * examined / ps->pss_to_examine;

#ifdef EZFS_SCRUB_PAUSED
	paused_ts = ps->pss_pass_scrub_pause;
	paused_time = ps->pss_pass_scrub_spent_paused;
#else
	paused_ts = 0;
	paused_time = 0;
#endif

	/* calculations for this pass */
	if (ps->pss_state == DSS_SCANNING) {
		elapsed = (int64_t) time(NULL) - (int64_t) ps->pss_pass_start -
		          (int64_t) paused_time;
		elapsed = (elapsed > 0) ? elapsed : 1;
		pass_exam = ps->pss_pass_exam ? ps->pss_pass_exam : 1;
		rate = pass_exam / elapsed;
		rate = (rate > 0) ? rate : 1;
		remaining_time = ps->pss_to_examine - examined / rate;
	} else {
		elapsed =
		    (int64_t) ps->pss_end_time - (int64_t) ps->pss_pass_start -
		    (int64_t) paused_time;
		elapsed = (elapsed > 0) ? elapsed : 1;
		pass_exam = ps->pss_pass_exam ? ps->pss_pass_exam : 1;
		rate = pass_exam / elapsed;
		remaining_time = 0;
	}
	rate = rate ? rate : 1;

	/* influxdb line protocol format: "tags metrics timestamp" */
	(void) printf("%s,function=%s,name=%s,state=%s ",
	    SCAN_MEASUREMENT, func, pool_name, state[ps->pss_state]);
	(void) printf("end_ts="IFMT",errors="IFMT",examined="IFMT","
	              "pass_examined="IFMT",pause_ts="IFMT",paused_t="IFMT","
	              "pct_done=%.2f,processed="IFMT",rate="IFMT","
	              "remaining_t="IFMT",start_ts="IFMT","
	              "to_examine="IFMT" ",
	    MASK_UINT64(ps->pss_end_time),
	    MASK_UINT64(ps->pss_errors),
	    MASK_UINT64(examined),
	    MASK_UINT64(pass_exam),
	    MASK_UINT64(paused_ts),
	    MASK_UINT64(paused_time),
	    pct_done,
	    MASK_UINT64(ps->pss_processed),
	    MASK_UINT64(rate),
	    MASK_UINT64(remaining_time),
	    MASK_UINT64(ps->pss_start_time),
	    MASK_UINT64(ps->pss_to_examine)
	);
	(void) printf("%lu\n", timestamp);
	return (0);
}

/*
 * get a vdev name that corresponds to the top-level vdev names
 * printed by `zpool status`
 */
char *
get_vdev_name(nvlist_t *nvroot, const char *parent_name) {
    static char vdev_name[256];
    nvstring_t vdev_type = NULL;
    uint64_t vdev_id = 0;

    if (nvlist_lookup_string(nvroot, ZPOOL_CONFIG_TYPE,
                             &vdev_type) != 0) {
        vdev_type = "unknown";
    }
    if (nvlist_lookup_uint64(
        nvroot, ZPOOL_CONFIG_ID, &vdev_id) != 0) {
        vdev_id = UINT64_MAX;
    }
    if (parent_name == NULL) {
        (void) snprintf(vdev_name, sizeof(vdev_name), "%s",
                        vdev_type);
    } else {
        (void) snprintf(vdev_name, sizeof(vdev_name),
                        "%s/%s-%lu",
                        parent_name, vdev_type, vdev_id);
    }
    return (vdev_name);
}

/*
 * get a string suitable for an influxdb tag that describes this vdev
 *
 * By default only the vdev hierarchical name is shown, separated by '/'
 * If the vdev has an associated path, which is typical of leaf vdevs,
 * then the path is added.
 * It would be nice to have the devid instead of the path, but under
 * Linux we cannot be sure a devid will exist and we'd rather have
 * something than nothing, so we'll use path instead.
 */
char *
get_vdev_desc(nvlist_t *nvroot, const char *parent_name) {
    static char vdev_desc[2 * MAXPATHLEN];
    nvstring_t vdev_type = NULL;
    uint64_t vdev_id = 0;
    char vdev_value[MAXPATHLEN];
    nvstring_t vdev_path = NULL;
    char *s, *t;

    if (nvlist_lookup_string(nvroot, ZPOOL_CONFIG_TYPE, &vdev_type) != 0) {
        vdev_type = "unknown";
    }
    if (nvlist_lookup_uint64(nvroot, ZPOOL_CONFIG_ID, &vdev_id) != 0) {
        vdev_id = UINT64_MAX;
    }
    if (nvlist_lookup_string(
        nvroot, ZPOOL_CONFIG_PATH, &vdev_path) != 0) {
        vdev_path = NULL;
    }

    if (parent_name == NULL) {
        s = escape_string((char *)vdev_type);
        (void) snprintf(vdev_value, sizeof(vdev_value), "vdev=%s", s);
        free(s);
    } else {
        s = escape_string((char *)parent_name);
        t = escape_string((char *)vdev_type);
        (void) snprintf(vdev_value, sizeof(vdev_value),
                        "vdev=%s/%s-%lu", s, t, vdev_id);
        free(s);
        free(t);
    }
    if (vdev_path == NULL) {
        (void) snprintf(vdev_desc, sizeof(vdev_desc), "%s",
                        vdev_value);
    } else {
        s = escape_string((char *)vdev_path);
        (void) snprintf(vdev_desc, sizeof(vdev_desc), "path=%s,%s",
                        s, vdev_value);
        free(s);
    }
    return (vdev_desc);
}

/*
 * vdev summary stats are a combination of the data shown by
 * `zpool status` and `zpool list -v`
 */
int
print_summary_stats(nvlist_t *nvroot, const char *pool_name,
                    const char *parent_name) {
    uint_t c;
    vdev_stat_t *vs;
    char *vdev_desc = NULL;
    vdev_desc = get_vdev_desc(nvroot, parent_name);

    if (nvlist_lookup_uint64_array(nvroot,
                                   ZPOOL_CONFIG_VDEV_STATS,
                                   (uint64_t **) &vs, &c) != 0) {
        return (1);
    }
    (void) printf("%s,name=%s,state=%s,%s ", POOL_MEASUREMENT, pool_name,
                  zpool_state_to_name((vdev_state_t) vs->vs_state,
                                      (vdev_aux_t) vs->vs_aux),
                                      vdev_desc);
    (void) printf("alloc="IFMT",free="IFMT",size="IFMT","
                  "read_bytes="IFMT",read_errors="IFMT",read_ops="IFMT","
                  "write_bytes="IFMT",write_errors="IFMT",write_ops="IFMT","
                  "checksum_errors="IFMT",fragmentation="IFMT"",
                  MASK_UINT64(vs->vs_alloc),
                  MASK_UINT64(vs->vs_space - vs->vs_alloc),
                  MASK_UINT64(vs->vs_space),
                  MASK_UINT64(vs->vs_bytes[ZIO_TYPE_READ]),
                  MASK_UINT64(vs->vs_read_errors),
                  MASK_UINT64(vs->vs_ops[ZIO_TYPE_READ]),
                  MASK_UINT64(vs->vs_bytes[ZIO_TYPE_WRITE]),
                  MASK_UINT64(vs->vs_write_errors),
                  MASK_UINT64(vs->vs_ops[ZIO_TYPE_WRITE]),
                  MASK_UINT64(vs->vs_checksum_errors),
                  MASK_UINT64(vs->vs_fragmentation));
    (void) printf(" %lu\n", timestamp);
    return (0);
}

/*
 * vdev latency stats are histograms stored as nvlist arrays of uint64.
 * Latency stats include the ZIO scheduler classes plus lower-level
 * vdev latencies.
 *
 * In many cases, the top-level "root" view obscures the underlying
 * top-level vdev operations. For example, if a pool has a log, special,
 * or cache device, then each can behave very differently. It is useful
 * to see how each is responding.
 */
int
print_vdev_latency_stats(nvlist_t *nvroot, const char *pool_name,
                         const char *parent_name) {
    uint_t c, end = 0;
    nvlist_t *nv_ex;
    char *vdev_desc = NULL;

    /* short_names become part of the metric name and are influxdb-ready */
    struct lat_lookup {
        char *name;
        char *short_name;
        uint64_t sum;
        uint64_t *array;
    };
    struct lat_lookup lat_type[] = {
        {ZPOOL_CONFIG_VDEV_TOT_R_LAT_HISTO,   "total_read", 0},
        {ZPOOL_CONFIG_VDEV_TOT_W_LAT_HISTO,   "total_write", 0},
        {ZPOOL_CONFIG_VDEV_DISK_R_LAT_HISTO,  "disk_read", 0},
        {ZPOOL_CONFIG_VDEV_DISK_W_LAT_HISTO,  "disk_write", 0},
        {ZPOOL_CONFIG_VDEV_SYNC_R_LAT_HISTO,  "sync_read", 0},
        {ZPOOL_CONFIG_VDEV_SYNC_W_LAT_HISTO,  "sync_write", 0},
        {ZPOOL_CONFIG_VDEV_ASYNC_R_LAT_HISTO, "async_read", 0},
        {ZPOOL_CONFIG_VDEV_ASYNC_W_LAT_HISTO, "async_write", 0},
        {ZPOOL_CONFIG_VDEV_SCRUB_LAT_HISTO,   "scrub", 0},
#ifdef ZPOOL_CONFIG_VDEV_TRIM_LAT_HISTO
        {ZPOOL_CONFIG_VDEV_TRIM_LAT_HISTO,    "trim", 0},
#endif
        {NULL,                                NULL}
    };

    if (nvlist_lookup_nvlist(nvroot,
                             ZPOOL_CONFIG_VDEV_STATS_EX, &nv_ex) != 0) {
        return (6);
    }

    vdev_desc = get_vdev_desc(nvroot, parent_name);

    for (int i = 0; lat_type[i].name; i++) {
        if (nvlist_lookup_uint64_array(nv_ex,
                                       lat_type[i].name,
                                       &lat_type[i].array,
                                       &c) != 0) {
            fprintf(stderr, "error: can't get %s\n", lat_type[i].name);
            return (3);
        }
        /* end count count, all of the arrays are the same size */
        end = c - 1;
    }

    for (int bucket = 0; bucket <= end; bucket++) {
        if (bucket < MIN_LAT_INDEX) {
            /* don't print, but collect the sum */
            for (int i = 0; lat_type[i].name; i++) {
                lat_type[i].sum += lat_type[i].array[bucket];
            }
            continue;
        }
        if (bucket < end) {
            printf("%s,le=%0.6f,name=%s,%s ",
                POOL_LATENCY_MEASUREMENT, (float) (1ULL << bucket) * 1e-9,
                pool_name, vdev_desc);
        } else {
            printf("%s,le=+Inf,name=%s,%s ",
                         POOL_LATENCY_MEASUREMENT, pool_name, vdev_desc);
        }
        for (int i = 0; lat_type[i].name; i++) {
            if (bucket <= MIN_LAT_INDEX || sum_histogram_buckets) {
                lat_type[i].sum += lat_type[i].array[bucket];
            } else {
                lat_type[i].sum = lat_type[i].array[bucket];
            }
            printf("%s="IFMT, lat_type[i].short_name, lat_type[i].sum);
            if (lat_type[i + 1].name != NULL) {
                printf(",");
            }
        }
        printf(" %lu\n", timestamp);
    }
    return (0);
}

/*
 * vdev request size stats are histograms stored as nvlist arrays of uint64.
 * Request size stats include the ZIO scheduler classes plus lower-level
 * vdev sizes. Both independent (ind) and aggregated (agg) sizes are reported.
 *
 * In many cases, the top-level "root" view obscures the underlying
 * top-level vdev operations. For example, if a pool has a log, special,
 * or cache device, then each can behave very differently. It is useful
 * to see how each is responding.
 */
int
print_vdev_size_stats(nvlist_t *nvroot, const char *pool_name,
                      const char *parent_name) {
    uint_t c, end = 0;
    nvlist_t *nv_ex;
    char *vdev_desc = NULL;

    /* short_names become the field name */
    struct size_lookup {
        char *name;
        char *short_name;
        uint64_t sum;
        uint64_t *array;
    };
    struct size_lookup size_type[] = {
        {ZPOOL_CONFIG_VDEV_SYNC_IND_R_HISTO,   "sync_read_ind"},
        {ZPOOL_CONFIG_VDEV_SYNC_IND_W_HISTO,   "sync_write_ind"},
        {ZPOOL_CONFIG_VDEV_ASYNC_IND_R_HISTO,  "async_read_ind"},
        {ZPOOL_CONFIG_VDEV_ASYNC_IND_W_HISTO,  "async_write_ind"},
        {ZPOOL_CONFIG_VDEV_IND_SCRUB_HISTO,    "scrub_read_ind"},
        {ZPOOL_CONFIG_VDEV_SYNC_AGG_R_HISTO,   "sync_read_agg"},
        {ZPOOL_CONFIG_VDEV_SYNC_AGG_W_HISTO,   "sync_write_agg"},
        {ZPOOL_CONFIG_VDEV_ASYNC_AGG_R_HISTO,  "async_read_agg"},
        {ZPOOL_CONFIG_VDEV_ASYNC_AGG_W_HISTO,  "async_write_agg"},
        {ZPOOL_CONFIG_VDEV_AGG_SCRUB_HISTO,    "scrub_read_agg"},
#ifdef ZPOOL_CONFIG_VDEV_IND_TRIM_HISTO
        {ZPOOL_CONFIG_VDEV_IND_TRIM_HISTO,    "trim_write_ind"},
            {ZPOOL_CONFIG_VDEV_AGG_TRIM_HISTO,    "trim_write_agg"},
#endif
        {NULL,                                NULL}
    };

    if (nvlist_lookup_nvlist(nvroot,
                             ZPOOL_CONFIG_VDEV_STATS_EX, &nv_ex) != 0) {
        return (6);
    }

    vdev_desc = get_vdev_desc(nvroot, parent_name);

    for (int i = 0; size_type[i].name; i++) {
        if (nvlist_lookup_uint64_array(nv_ex,
                                       size_type[i].name,
                                       &size_type[i].array,
                                       &c) != 0) {
            fprintf(stderr, "error: can't get %s\n", size_type[i].name);
            return (3);
        }
        /* end count count, all of the arrays are the same size */
        end = c - 1;
    }

   for (int bucket = 0; bucket <= end; bucket++) {
       if (bucket < MIN_SIZE_INDEX) {
           /* don't print, but collect the sum */
           for (int i = 0; size_type[i].name; i++) {
               size_type[i].sum += size_type[i].array[bucket];
           }
           continue;
       }

       if (bucket < end) {
            printf("%s,le=%llu,name=%s,%s ",
                   POOL_IO_SIZE_MEASUREMENT, 1ULL << bucket,
                   pool_name, vdev_desc);
       } else {
           printf("%s,le=+Inf,name=%s,%s ",
                  POOL_IO_SIZE_MEASUREMENT, pool_name, vdev_desc);
       }
       for (int i = 0; size_type[i].name; i++) {
           if (bucket <= MIN_SIZE_INDEX || sum_histogram_buckets) {
               size_type[i].sum += size_type[i].array[bucket];
           } else {
               size_type[i].sum = size_type[i].array[bucket];
           }
           printf("%s="IFMT, size_type[i].short_name, size_type[i].sum);
           if (size_type[i + 1].name != NULL) {
               printf(",");
           }
       }
       printf(" %lu\n", timestamp);
    }
    return (0);
}

/*
 * ZIO scheduler queue stats are stored as gauges. This is unfortunate
 * because the values can change very rapidly and any point-in-time
 * value will quickly be obsoleted. It is also not easy to downsample.
 * Thus only the top-level queue stats might be beneficial... maybe.
 */
int
print_queue_stats(nvlist_t *nvroot, const char *pool_name,
                  const char *parent_name) {
    nvlist_t *nv_ex;
    uint64_t value;

    /* short_names are used for the field name */
    struct queue_lookup {
        char *name;
        char *short_name;
    };
    struct queue_lookup queue_type[] = {
        {ZPOOL_CONFIG_VDEV_SYNC_R_ACTIVE_QUEUE,  "sync_r_active"},
        {ZPOOL_CONFIG_VDEV_SYNC_W_ACTIVE_QUEUE,  "sync_w_active"},
        {ZPOOL_CONFIG_VDEV_ASYNC_R_ACTIVE_QUEUE, "async_r_active"},
        {ZPOOL_CONFIG_VDEV_ASYNC_W_ACTIVE_QUEUE, "async_w_active"},
        {ZPOOL_CONFIG_VDEV_SCRUB_ACTIVE_QUEUE,  "async_scrub_active"},
        {ZPOOL_CONFIG_VDEV_SYNC_R_PEND_QUEUE,    "sync_r_pend"},
        {ZPOOL_CONFIG_VDEV_SYNC_W_PEND_QUEUE,    "sync_w_pend"},
        {ZPOOL_CONFIG_VDEV_ASYNC_R_PEND_QUEUE,   "async_r_pend"},
        {ZPOOL_CONFIG_VDEV_ASYNC_W_PEND_QUEUE,   "async_w_pend"},
        {ZPOOL_CONFIG_VDEV_SCRUB_PEND_QUEUE,     "async_scrub_pend"},
        {NULL,                                   NULL}
    };

    if (nvlist_lookup_nvlist(nvroot,
                             ZPOOL_CONFIG_VDEV_STATS_EX, &nv_ex) != 0) {
        return (6);
    }

    printf("%s,name=%s,%s ",
           POOL_QUEUE_MEASUREMENT, pool_name,
           get_vdev_desc(nvroot, parent_name));
    for (int i = 0; queue_type[i].name; i++) {
        if (nvlist_lookup_uint64(nv_ex,
                                 queue_type[i].name, &value) != 0) {
            fprintf(stderr, "error: can't get %s\n",
                    queue_type[i].name);
            return (3);
        }
        printf("%s="IFMT, queue_type[i].short_name, value);
        if (queue_type[i + 1].name != NULL) {
            printf(",");
        }
    }
    printf(" %lu\n", timestamp);
    return (0);
}

/*
 * top-level vdev stats are at the pool level
 */
int
print_top_level_vdev_stats(nvlist_t *nvroot, const char *pool_name) {
	nvlist_t *nv_ex;
	uint64_t value;

	/* short_names become part of the metric name */
	struct queue_lookup {
	    char *name;
	    char *short_name;
	};
	struct queue_lookup queue_type[] = {
	    {ZPOOL_CONFIG_VDEV_SYNC_R_ACTIVE_QUEUE, "sync_r_active_queue"},
	    {ZPOOL_CONFIG_VDEV_SYNC_W_ACTIVE_QUEUE, "sync_w_active_queue"},
	    {ZPOOL_CONFIG_VDEV_ASYNC_R_ACTIVE_QUEUE, "async_r_active_queue"},
	    {ZPOOL_CONFIG_VDEV_ASYNC_W_ACTIVE_QUEUE, "async_w_active_queue"},
	    {ZPOOL_CONFIG_VDEV_SCRUB_ACTIVE_QUEUE, "async_scrub_active_queue"},
	    {ZPOOL_CONFIG_VDEV_SYNC_R_PEND_QUEUE, "sync_r_pend_queue"},
	    {ZPOOL_CONFIG_VDEV_SYNC_W_PEND_QUEUE, "sync_w_pend_queue"},
	    {ZPOOL_CONFIG_VDEV_ASYNC_R_PEND_QUEUE, "async_r_pend_queue"},
	    {ZPOOL_CONFIG_VDEV_ASYNC_W_PEND_QUEUE, "async_w_pend_queue"},
	    {ZPOOL_CONFIG_VDEV_SCRUB_PEND_QUEUE, "async_scrub_pend_queue"},
	    {NULL, NULL}
	};

	if (nvlist_lookup_nvlist(nvroot,
	    ZPOOL_CONFIG_VDEV_STATS_EX, &nv_ex) != 0) {
		return (6);
	}

	(void) printf("%s,name=%s,vdev=root ", VDEV_MEASUREMENT, pool_name);
	for (int i = 0; queue_type[i].name; i++) {
		if (nvlist_lookup_uint64(nv_ex,
                                 queue_type[i].name, &value) != 0) {
			fprintf(stderr, "error: can't get %s\n",
			    queue_type[i].name);
			return (3);
		}
		if (i > 0)
			printf(",");
		printf("%s="IFMT, queue_type[i].short_name, MASK_UINT64(value));
	}

	(void) printf(" %lu\n", timestamp);
	return (0);
}

/*
 * recursive stats printer
 */
typedef int (*stat_printer_f)(nvlist_t *, const char *, const char *);

int
print_recursive_stats(stat_printer_f func, nvlist_t *nvroot,
                      const char *pool_name, const char *parent_name,
                      int descend) {
    uint_t c, children;
    nvlist_t **child;
    char vdev_name[256];
    int err;

    err = func(nvroot, pool_name, parent_name);
    if (err)
        return (err);

    if (descend && nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
                                              &child, &children) == 0) {
        (void) strncpy(vdev_name, get_vdev_name(nvroot, parent_name),
                       sizeof(vdev_name));
        vdev_name[sizeof(vdev_name) - 1] = '\0';

        for (c = 0; c < children; c++) {
            print_recursive_stats(func, child[c], pool_name,
                                  vdev_name, descend);
        }
    }
    return (0);
}

/*
 * Print per-second pool I/O rates, similar to `zpool iostat`.
 *
 * The vdev_stat_t counters (vs_ops, vs_bytes) are cumulative, so a rate
 * requires two samples. We take a second sample of the top-level (root) vdev
 * iostat_interval seconds after the first and divide the delta by the elapsed
 * time reported by vs_timestamp (in nanoseconds), falling back to the
 * requested interval if the timestamp did not advance. Counter resets (e.g. a
 * pool re-import) are clamped to zero.
 *
 * o_* are the sample-1 values captured by the caller before sleeping.
 */
int
print_pool_iostat(zpool_handle_t *zhp, const char *pool_name,
                  uint64_t o_read_ops, uint64_t o_write_ops,
                  uint64_t o_read_bytes, uint64_t o_write_bytes,
                  uint64_t o_timestamp) {
    uint_t c;
    boolean_t missing;
    nvlist_t *config, *nvroot;
    vdev_stat_t *vs;
    double elapsed;
    struct timespec tv;

    sleep((unsigned int) iostat_interval);

    if (zpool_refresh_stats(zhp, &missing) != 0)
        return (1);
    config = zpool_get_config(zhp, NULL);
    if (nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE, &nvroot) != 0)
        return (2);
    if (nvlist_lookup_uint64_array(nvroot, ZPOOL_CONFIG_VDEV_STATS,
                                   (uint64_t **) &vs, &c) != 0)
        return (3);

    /* elapsed seconds, preferring the vdev's own timestamp */
    if (vs->vs_timestamp > o_timestamp)
        elapsed = (double) (vs->vs_timestamp - o_timestamp) / 1e9;
    else
        elapsed = (double) iostat_interval;
    if (elapsed <= 0.0)
        elapsed = (double) iostat_interval;

#define IO_DELTA(new, old) (((new) > (old)) ? ((new) - (old)) : 0)
    double read_kib = IO_DELTA(vs->vs_bytes[ZIO_TYPE_READ], o_read_bytes) /
                      1024.0 / elapsed;
    double write_kib = IO_DELTA(vs->vs_bytes[ZIO_TYPE_WRITE], o_write_bytes) /
                       1024.0 / elapsed;
    double read_iops = IO_DELTA(vs->vs_ops[ZIO_TYPE_READ], o_read_ops) /
                       elapsed;
    double write_iops = IO_DELTA(vs->vs_ops[ZIO_TYPE_WRITE], o_write_ops) /
                        elapsed;
#undef IO_DELTA

    if (clock_gettime(CLOCK_REALTIME, &tv) != 0)
        timestamp = (uint64_t) time(NULL) * 1000000000;
    else
        timestamp = ((uint64_t) tv.tv_sec * 1000000000) + (uint64_t) tv.tv_nsec;

    (void) printf("%s,name=%s,vdev=root ", POOL_IOSTAT_MEASUREMENT, pool_name);
    (void) printf("read_kib_per_sec=%.2f,write_kib_per_sec=%.2f,"
                  "read_iops=%.2f,write_iops=%.2f",
                  read_kib, write_kib, read_iops, write_iops);
    (void) printf(" %lu\n", timestamp);
    return (0);
}

/*
 * call-back to print the stats from the pool config
 *
 * Note: if the pool is broken, this can hang indefinitely and perhaps in an
 * unkillable state.
 */
int
print_stats(zpool_handle_t *zhp, void *data) {
	uint_t c;
	int err;
	boolean_t missing;
	nvlist_t *config, *nvroot;
	vdev_stat_t *vs;
	struct timespec tv;
	char *pool_name;

	/* if not this pool return quickly */
	if (data &&
	    strncmp(data, zhp->zpool_name, ZFS_MAX_DATASET_NAME_LEN) != 0) {
        zpool_close(zhp);
        return (0);
    }

	if (zpool_refresh_stats(zhp, &missing) != 0) {
        zpool_close(zhp);
        return (1);
    }

	config = zpool_get_config(zhp, NULL);
	if (clock_gettime(CLOCK_REALTIME, &tv) != 0)
		timestamp = (uint64_t) time(NULL) * 1000000000;
	else
		timestamp =
		    ((uint64_t) tv.tv_sec * 1000000000) + (uint64_t) tv.tv_nsec;

	if (nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE, &nvroot) != 0) {
        zpool_close(zhp);
		return (2);
	}
	if (nvlist_lookup_uint64_array(nvroot, ZPOOL_CONFIG_VDEV_STATS,
	        (uint64_t **) &vs, &c) != 0) {
        zpool_close(zhp);
		return (3);
	}

	/*
	 * Capture the root vdev's cumulative counters as the first iostat
	 * sample; zpool_refresh_stats() in print_pool_iostat() will invalidate
	 * this nvlist, so copy the scalars out now.
	 */
	uint64_t io_read_ops = vs->vs_ops[ZIO_TYPE_READ];
	uint64_t io_write_ops = vs->vs_ops[ZIO_TYPE_WRITE];
	uint64_t io_read_bytes = vs->vs_bytes[ZIO_TYPE_READ];
	uint64_t io_write_bytes = vs->vs_bytes[ZIO_TYPE_WRITE];
	uint64_t io_timestamp = vs->vs_timestamp;

	pool_name = escape_string(zhp->zpool_name);
    err = print_recursive_stats(print_summary_stats, nvroot,
            pool_name, NULL, 1);
	/* if any of these return an error, skip the rest */
	if (err == 0)
        err = print_top_level_vdev_stats(nvroot, pool_name);

	if (no_histograms == 0) {
        if (err == 0)
            err = print_recursive_stats(print_vdev_latency_stats, nvroot,
                                        pool_name, NULL, 1);
        if (err == 0)
            err = print_recursive_stats(print_vdev_size_stats, nvroot,
                                        pool_name, NULL, 1);
        if (err == 0)
            err = print_recursive_stats(print_queue_stats, nvroot,
                                        pool_name, NULL, 0);
    }
    if (err == 0)
        err = print_scan_status(nvroot, pool_name);

    /*
     * Must be last: print_pool_iostat() sleeps and refreshes the pool config,
     * which invalidates nvroot.
     */
    if (err == 0 && iostat_interval > 0)
        err = print_pool_iostat(zhp, pool_name, io_read_ops, io_write_ops,
                                io_read_bytes, io_write_bytes, io_timestamp);

    free(pool_name);
    zpool_close(zhp);
	return (err);
}


void
usage(char* name) {
    fprintf(stderr, "usage: %s [--execd][--no-histograms]"
                    "[--sum-histogram-buckets][--iostat-interval=SECONDS]"
                    "[--timeout=SECONDS][--lock-file=PATH] [poolname]\n", name);
    exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[]) {
    int opt;
    int ret = 8;
    char *line = NULL;
    size_t len = 0;
    struct option long_options[] = {
        {"execd", no_argument, NULL, 'e'},
        {"help", no_argument, NULL, 'h'},
        {"no-histograms", no_argument, NULL, 'n'},
        {"sum-histogram-buckets", no_argument, NULL, 's'},
        {"timeout", required_argument, NULL, 't'},
        {"lock-file", required_argument, NULL, 'l'},
        {"iostat-interval", required_argument, NULL, 'i'},
        {0, 0, 0, 0}
    };
    while ((opt = getopt_long(argc, argv, "ehnst:l:i:", long_options, NULL))
           != -1) {
        switch (opt) {
            case 'e':
                execd_mode = 1;
                break;
            case 'n':
                no_histograms = 1;
                break;
            case 's':
                sum_histogram_buckets = 1;
                break;
            case 't':
                timeout_secs = atoi(optarg);
                if (timeout_secs < 0)
                    timeout_secs = 0;
                break;
            case 'l':
                lock_file = optarg;
                break;
            case 'i':
                iostat_interval = atoi(optarg);
                if (iostat_interval < 0)
                    iostat_interval = 0;
                break;
            default:
                usage(argv[0]);
        }
    }

    /*
     * Ensure only one instance runs at a time before doing any libzfs work,
     * then install the watchdog. Both are no-ops unless the corresponding
     * option was given.
     */
    acquire_single_instance_lock();
    if (timeout_secs > 0)
        (void) signal(SIGALRM, timeout_handler);

	libzfs_handle_t *g_zfs;
	if ((g_zfs = libzfs_init()) == NULL) {
		fprintf(stderr,
		    "error: cannot initialize libzfs. "
		    "Is the zfs module loaded or zrepl running?");
		exit(EXIT_FAILURE);
	}
	if (execd_mode == 0) {
        arm_timeout();
        ret = zpool_iter(g_zfs, print_stats, argv[optind]);
        disarm_timeout();
        return (ret);
    }
    while (getline(&line, &len, stdin) != -1) {
        arm_timeout();
        ret = zpool_iter(g_zfs, print_stats, argv[optind]);
        disarm_timeout();
        fflush(stdout);
	}
    return (ret);
}

