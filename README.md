# Influxdb Metrics for ZFS Pools

> **Note:** This is a fork of
> [richardelling/zpool_influxdb](https://github.com/richardelling/zpool_influxdb).
> It updates the original so it builds against modern OpenZFS releases
> (verified on ZFS 2.1 / Debian 12 through ZFS 2.4) and recent
> compilers/glibc. See [Changes in this fork](#changes-in-this-fork) for
> details. All credit for the original program goes to Richard Elling.

The _zpool_influxdb_ program produces 
[influxdb](https://github.com/influxdata/influxdb) line protocol
compatible metrics from zpools. In the UNIX tradition, _zpool_influxdb_
does one thing: read statistics from a pool and print them to
stdout. In many ways, this is a metrics-friendly output of 
statistics normally observed via the `zpool` command.

## Changes in this fork
Relative to the upstream repository:
* **Builds on modern OpenZFS.** The libzfs `nvlist_lookup_string()` signature
  gained a `const` qualifier in OpenZFS 2.2. The build now detects the
  signature at configure time, so the same source compiles cleanly on ZFS 2.1
  (Debian 12's `char **`) and ZFS 2.2+ (`const char **`).
* **Modern glibc fix.** Builds with `-D_LARGEFILE64_SOURCE` so the libspl
  headers can use the transitional `stat64()`/`fstat64()` interfaces.
* **Removed a stale scan field.** The `to_process` field was dropped from
  `zpool_scan_stats`; the corresponding `pss_to_process` member no longer
  exists in `pool_scan_stat_t`.
* **Sensible defaults.** `ZFS_INSTALL_BASE` now defaults to `/usr` (matching
  distribution packages); override it for source installs.
* **CPU target option.** A new `TARGET_ARCH` cmake variable controls `-march`
  and defaults to `x86-64-v4`, plus `STRIP_ISA_PROPERTY` for building on a
  higher-ISA host than the target. See
  [Target CPU architecture](#target-cpu-architecture) and
  [Building on one machine to run on another](#building-on-one-machine-to-run-on-another).
* **Frequent-polling safety options.** New `--lock-file` (run a single
  instance; skip overlapping invocations) and `--timeout` (bound a slow
  sample) options make frequent collection (e.g. telegraf every few seconds)
  safer. See [Caveat Emptor](#caveat-emptor).
* **Pool I/O rates.** A `zpool_iostat` measurement with per-second read/write
  throughput (KiB/s) and IOPS for the pool, computed like `zpool iostat`. On by
  default (`--iostat-interval=1`); pass `--iostat-interval=0` to disable. See
  [zpool_iostat Description](#zpool_iostat-description).

Verified building and running on Debian 12 (bookworm, GCC 12.2, ZFS 2.1.11)
and CachyOS/Arch (GCC 16, ZFS 2.4.2).

## ZFS Versions
There are many implementations of ZFS on many OSes. The current
version is tested to work on:
* [ZFSonLinux](https://github.com/zfsonlinux/zfs) version 0.7 and later
* [cstor](https://github.com/openebs/cstor) for userland ZFS (uZFS)

This should compile and run on other ZFS versions, though many 
do not have the latency histograms. Pull requests are welcome.

## Usage
When run without arguments, _zpool_influxdb_ runs once, reading data
from all imported pools, and prints to stdout.
```shell
zpool_influxdb [options] [poolname]
```
If no poolname is specified, then all pools are sampled.

| option | short option | description |
|---|---|---|
| --execd | -e | For use with telegraf's `execd` plugin. When [enter] is pressed, the pools are sampled. To exit, use [ctrl+D] |
| --no-histogram | -n | Do not print histogram information |
| --sum-histogram-buckets | -s | Sum histogram bucket values |
| --iostat-interval=SECONDS | -i | Emit per-second pool I/O rates (the `zpool_iostat` measurement) by sampling twice, SECONDS apart, like `zpool iostat`. Adds SECONDS of latency per run. **Default 1**; pass `--iostat-interval=0` to disable |
| --timeout=SECONDS | -t | Abort a sample that runs longer than SECONDS instead of overrunning the polling interval (0 = disabled, default). See [Caveat Emptor](#caveat-emptor) |
| --lock-file=PATH | -l | Run at most one instance at a time. If another instance still holds PATH, exit immediately with no output instead of piling up. See [Caveat Emptor](#caveat-emptor) |
| --help | -h | Print a short usage message |

#### Histogram Bucket Values
The histogram data collected by ZFS is stored as independent bucket values.
This works well out-of-the-box with an influxdb data source and grafana's
heatmap visualization. The influxdb query for a grafana heatmap 
visualization looks like:
```
field(disk_read) last() non_negative_derivative(1s)
```

Another method for storing histogram data sums the values for lower-value
buckets. For example, a latency bucket tagged "le=10" includes the values
in the bucket "le=1".
This method is often used for prometheus histograms.
The `zpool_influxdb --sum-histogram-buckets` option presents the data from ZFS
as summed values.

## Measurements
The following measurements are collected:

| measurement | description | zpool equivalent |
|---|---|---|
| zpool_stats | general size and data | zpool list |
| zpool_scan_stats | scrub, rebuild, and resilver statistics (omitted if no scan has been requested) | zpool status |
| zpool_vdev_stats | per-vdev statistics | zpool iostat -q |
| zpool_io_size | per-vdev I/O size histogram | zpool iostat -r |
| zpool_latency | per-vdev I/O latency histogram | zpool iostat -w |
| zpool_vdev_queue | per-vdev instantaneous queue depth | zpool iostat -q |
| zpool_iostat | per-second pool read/write throughput and IOPS (on by default; `--iostat-interval=0` disables) | zpool iostat |

### zpool_iostat Description
The cumulative `read_bytes`/`write_bytes`/`read_ops`/`write_ops` fields in
`zpool_stats` are counters; the usual way to get a rate is to apply a
derivative (e.g. influxdb's `non_negative_derivative`) at query time. As a
convenience, _zpool_influxdb_ also computes the pool-level rates directly the
same way `zpool iostat` does: it takes a second sample of the top-level vdev
counters `--iostat-interval` seconds later and divides the delta by the
elapsed time. **This is on by default with a 1 second interval**, which adds
~1 second of latency to each run; pass `--iostat-interval=0` to disable it, or
a larger value to average over a longer window. Keep the interval shorter than
your collection interval (and shorter than `--timeout`, if set). Rates are
reported for the whole pool (`vdev=root`); counter resets are clamped to zero.

#### zpool_iostat Tags
| label | description |
|---|---|
| name | pool name |
| vdev | always `root` (whole-pool totals) |

#### zpool_iostat Fields
| field | units | description |
|---|---|---|
| read_kib_per_sec | KiB/s | read throughput over the sample interval |
| write_kib_per_sec | KiB/s | write throughput over the sample interval |
| read_iops | ops/s | read operations per second over the interval |
| write_iops | ops/s | write operations per second over the interval |

### zpool_stats Description
zpool_stats contains top-level summary statistics for the pool.
Performance counters measure the I/Os to the pool's devices.

#### zpool_stats Tags

| label | description |
|---|---|
| name | pool name |
| state | pool state, as shown by _zpool status_ |

#### zpool_stats Fields

| field | units | description |
|---|---|---|
| alloc | bytes | allocated space |
| free | bytes | unallocated space |
| size | bytes | total pool size |
| read_bytes | bytes | bytes read since pool import |
| read_errors | count | number of read errors |
| read_ops | count | number of read operations |
| write_bytes | bytes | bytes written since pool import |
| write_errors | count | number of write errors |
| write_ops | count | number of write operations |

### zpool_scan_stats Description
Once a pool has been scrubbed, resilvered, or rebuilt, the zpool_scan_stats
contain information about the status and performance of the operation.
Otherwise, the zpool_scan_stats do not exist in the kernel, and therefore
cannot be reported by this collector.

#### zpool_scan_stats Tags

| label | description |
|---|---|
| name | pool name |
| function | name of the scan function running or recently completed |
| state | scan state, as shown by _zpool status_ |

#### zpool_scan_stats Fields

| field | units | description |
|---|---|---|
| errors | count | number of errors encountered by scan |
| examined | bytes | total data examined during scan |
| to_examine | bytes | prediction of total bytes to be scanned |
| pass_examined | bytes | data examined during current scan pass |
| processed | bytes | data reconstructed during scan |
| rate | bytes/sec | examination rate |
| start_ts | epoch timestamp | start timestamp for scan |
| pause_ts | epoch timestamp | timestamp for a scan pause request |
| end_ts | epoch timestamp | completion timestamp for scan |
| paused_t | seconds | elapsed time while paused |
| remaining_t | seconds | estimate of time remaining for scan |

### zpool_vdev_stats Description
The ZFS I/O (ZIO) scheduler uses five queues to schedule I/Os to each vdev.
These queues are further divided into active and pending states.
An I/O is pending prior to being issued to the vdev. An active
I/O has been issued to the vdev. The scheduler and its tunable
parameters are described at the 
[ZFS on Linux wiki.](https://github.com/zfsonlinux/zfs/wiki/ZIO-Scheduler)
The ZIO scheduler reports the queue depths as gauges where the value 
represents an instantaneous snapshot of the queue depth at 
the sample time. Therefore, it is not unusual to see all zeroes
for an idle pool.

#### zpool_vdev_stats Tags
| label | description |
|---|---|
| name | pool name |
| vdev | vdev name (root = entire pool) |

#### zpool_vdev_stats Fields
| field | units | description |
|---|---|---|
| sync_r_active_queue | entries | synchronous read active queue depth |
| sync_w_active_queue | entries | synchronous write active queue depth |
| async_r_active_queue | entries | asynchronous read active queue depth |
| async_w_active_queue | entries | asynchronous write active queue depth |
| async_scrub_active_queue | entries | asynchronous scrub active queue depth |
| sync_r_pend_queue | entries | synchronous read pending queue depth |
| sync_w_pend_queue | entries | synchronous write pending queue depth |
| async_r_pend_queue | entries | asynchronous read pending queue depth |
| async_w_pend_queue | entries | asynchronous write pending queue depth |
| async_scrub_pend_queue | entries | asynchronous scrub pending queue depth |

### zpool_latency Histogram
ZFS tracks the latency of each I/O in the ZIO pipeline. This latency can
be useful for observing latency-related issues that are not easily observed
using the averaged latency statistics.

The histogram fields show cumulative values from lowest to highest.
The largest bucket is tagged "le=+Inf", representing the total count
of I/Os by type and vdev.

#### zpool_latency Histogram Tags
| label | description |
|---|---|
| le | bucket for histogram, latency is less than or equal to bucket value in seconds |
| name | pool name |
| path | for leaf vdevs, the device path name, otherwise omitted |
| vdev | vdev name (root = entire pool) |

#### zpool_latency Histogram Fields
| field | units | description |
|---|---|---|
| total_read | operations | read operations of all types |
| total_write | operations | write operations of all types |
| disk_read | operations | disk read operations |
| disk_write | operations | disk write operations |
| sync_read | operations | ZIO sync reads |
| sync_write | operations | ZIO sync writes |
| async_read | operations | ZIO async reads|
| async_write | operations | ZIO async writes |
| scrub | operations | ZIO scrub/scan reads |
| trim | operations | ZIO trim (aka unmap) writes |

### zpool_io_size Histogram
ZFS tracks I/O throughout the ZIO pipeline. The size of each I/O is used
to create a histogram of the size by I/O type and vdev. For example, a
4KiB write to mirrored pool will show a 4KiB write to the top-level vdev
(root) and a 4KiB write to each of the mirror leaf vdevs.

The ZIO pipeline can aggregate I/O operations. For example, a contiguous
series of writes can be aggregated into a single, larger I/O to the leaf
vdev. The independent I/O operations reflect the logical operations and
the aggregated I/O operations reflect the physical operations.

The histogram fields show cumulative values from lowest to highest.
The largest bucket is tagged "le=+Inf", representing the total count
of I/Os by type and vdev.

Note: trim I/Os can be larger than 16MiB, but the larger sizes are 
accounted in the 16MiB bucket.

#### zpool_io_size Histogram Tags
| label | description |
|---|---|
| le | bucket for histogram, I/O size is less than or equal to bucket value in bytes |
| name | pool name |
| path | for leaf vdevs, the device path name, otherwise omitted |
| vdev | vdev name (root = entire pool) |

#### zpool_io_size Histogram Fields
| field | units | description |
|---|---|---|
| sync_read_ind | blocks | independent sync reads |
| sync_write_ind | blocks | independent sync writes |
| async_read_ind | blocks | independent async reads |
| async_write_ind | blocks | independent async writes |
| scrub_read_ind | blocks | independent scrub/scan reads |
| trim_write_ind | blocks | independent trim (aka unmap) writes |
| sync_read_agg | blocks | aggregated sync reads |
| sync_write_agg | blocks | aggregated sync writes |
| async_read_agg | blocks | aggregated async reads |
| async_write_agg | blocks | aggregated async writes |
| scrub_read_agg | blocks | aggregated scrub/scan reads |
| trim_write_agg | blocks | aggregated trim (aka unmap) writes |

#### About unsigned integers
Telegraf v1.6.2 and later support unsigned 64-bit integers which more 
closely matches the uint64_t values used by ZFS. By default, zpool_influxdb
will mask ZFS' uint64_t values and use influxdb line protocol integer type.
Eventually the monitoring world will catch up to the times and support 
unsigned integers. To support unsigned, define SUPPORT_UINT64 and compile
as described in `CMakeLists.txt`

## Building
Building uses cmake and needs a C compiler, the libzfs/libspl development
headers, and the libzfs and libnvpair libraries.

### 1. Install build dependencies

Debian / Ubuntu (the ZFS packages are in `contrib`):
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libzfslinux-dev
```

Arch / CachyOS (ZFS is provided by the `zfs-dkms`/`zfs-utils` packages or the
AUR; these install the libzfs headers):
```bash
sudo pacman -S --needed base-devel cmake zfs-utils
```

For a source install of [OpenZFS](https://github.com/openzfs/zfs), make sure
`make install` placed the libzfs headers and libraries on the system.

### 2. Configure and build
```bash
cmake .
make
```
If successful, the _zpool_influxdb_ executable is created in the current
directory.

Distribution packages install the headers and libraries under _/usr_, which is
the default _ZFS_INSTALL_BASE_. A source install of OpenZFS defaults to
_/usr/local_; in that case (or any other location) either edit _CMakeLists.txt_
and change _ZFS_INSTALL_BASE_ or pass it on the cmake command line:
```bash
cmake -D ZFS_INSTALL_BASE=/usr/local .
make
```

The libzfs API is unstable and has changed across OpenZFS releases. The build
adapts at configure time (e.g. the `nvlist_lookup_string()` signature changed in
OpenZFS 2.2) so the same source compiles on ZFS 2.1 (e.g. Debian 12) through
current releases.

### Target CPU architecture
By default the binary is compiled for the `x86-64-v4` microarchitecture
(AVX-512 baseline). Such a binary will not run on CPUs below that level. To
target a different level or a fully portable baseline, set _TARGET_ARCH_:
```bash
cmake -D TARGET_ARCH=x86-64-v3 .   # AVX2 baseline
cmake -D TARGET_ARCH= .             # compiler default (most portable)
```

### Building on one machine to run on another
`libzfs` is dynamically linked and has an unstable ABI, so a binary built on
one machine has three requirements to run on another:

1. **Matching OpenZFS version.** The binary links a specific `libzfs` SONAME
   (`libzfs.so.4` for ZFS 2.1, `.so.6` for 2.2, `.so.7` for 2.4). The target
   must have that same SONAME or it fails at startup with
   `error while loading shared libraries: libzfs.so.N: cannot open shared
   object file`. Build on (or against the headers/libs of) the **same OpenZFS
   version the target runs**. For example, TrueNAS 26 runs ZFS 2.4
   (`libzfs.so.7`), so build against ZFS 2.4, not Debian 12's ZFS 2.1.
2. **glibc no newer than the target's.** The build host's glibc symbol
   versions must be `<=` the target's. Check with
   `objdump -T zpool_influxdb | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1`.
3. **A CPU level the target supports.** Set `TARGET_ARCH` to the target's
   level (see above). Note that on distro variants built for a high ISA level
   (e.g. CachyOS's `x86-64-v4` repos), the toolchain stamps that level into
   *every* binary via the startup objects, so the loader rejects it on a
   lower CPU with `CPU ISA level is lower than required` even when
   `TARGET_ARCH` is correct. Build with `-D STRIP_ISA_PROPERTY=ON` to remove
   that gate:
   ```bash
   cmake -D TARGET_ARCH=x86-64-v3 -D STRIP_ISA_PROPERTY=ON .
   make
   ```
   This only removes the inherited ISA *requirement* note; the emitted code
   still uses no instructions above `TARGET_ARCH`.

## Installing
Installation is left as an exercise for the reader because
there are many different methods that can be used.
Ultimately the method depends on how the local metrics collection is 
implemented and the local access policies.

To install the _zpool_influxdb_ executable in _INSTALL_DIR_, use
```bash
make install
```

The simplest method is to use the exec agent in telegraf. For convenience,
a sample config file is _zpool_influxdb.conf_ which can be placed in the
telegraf config-directory (often /etc/telegraf/telegraf.d). Telegraf can
be restarted to read the config-directory files.

## Caveat Emptor
* Like the _zpool_ command, _zpool_influxdb_ takes a reader 
  lock on spa_config for each imported pool. If this lock blocks,
  then the command will also block indefinitely and might be
  unkillable. This is not a normal condition, but can occur if 
  there are bugs in the kernel modules.

  A single wedged sample is a kernel-side condition that no user-space
  program can reliably interrupt -- a thread stuck in an uninterruptible
  `ioctl()` cannot be killed by a signal or a timeout. What you *can* avoid
  is letting frequent polling pile up many of these commands so they
  contend with each other and make matters worse. This fork adds two options
  to do exactly that:
  * `--lock-file=PATH` runs at most one instance at a time. If a previous
    sample is still running (or wedged), the next invocation exits
    immediately with no output rather than spawning yet another process.
  * `--timeout=SECONDS` bounds a sample so a slow-but-recoverable run does
    not overrun the polling interval. (It cannot break a true kernel wedge,
    but it handles the common slow/contended case.)

### Recommended usage with telegraf
When polling frequently (e.g. every few seconds), prefer the `execd` plugin:
it starts a single long-lived process and samples on each trigger, so it does
not spawn a new process every interval. Combine it with `--lock-file` and
`--timeout` (and telegraf's own `timeout`) as belt-and-suspenders:
```toml
[[inputs.execd]]
  command = ["/usr/bin/zpool_influxdb", "--execd",
             "--lock-file=/run/zpool_influxdb.lock", "--timeout=4"]
  signal = "STDIN"
  data_format = "influx"
```
If you use the simpler `exec` plugin instead, set its `timeout` below your
interval and still pass `--lock-file` so overlapping runs are skipped:
```toml
[[inputs.exec]]
  commands = ["/usr/bin/zpool_influxdb --lock-file=/run/zpool_influxdb.lock"]
  interval = "5s"
  timeout = "4s"
  data_format = "influx"
```
The lock file path must be writable by the user telegraf runs the command as.

## Other collectors
There are a few other collectors for zpool statistics roaming around
the Internet. Many attempt to screen-scrape `zpool` output in various 
ways. The screen-scrape method works poorly for `zpool` output because
of its human-friendly nature. Also, they suffer from the same caveats
as this implementation. This implementation is optimized for directly
collecting the metrics and is much more efficient than the screen-scrapers.

## Feedback Encouraged
Pull requests and issues are greatly appreciated. Visit
https://github.com/richardelling/zpool_influxdb
