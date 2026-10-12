# FastFileCheck
FastFileCheck is a high-performance, multithreaded file integrity checker for Linux. Designed for speed and efficiency, FastFileCheck utilizes parallel processing and a lightweight database to quickly hash and verify large volumes of files, ensuring their integrity over time.

Features:
* Multithreaded processing: automatically adapts to available CPU cores for optimal performance.
* HDD-aware: on rotational disks files are read in on-disk order with large, prefetched requests instead of by dozens of competing threads (see "HDD mode" below).
* Flexible configuration: see example.conf about all configuration options.
* Efficient hashing: uses fast, non-cryptographic hashing (xxHash) to detect file changes.
* Fast re-checks: files whose size, mtime and ctime did not change are not read again (`quick_check`), while a rolling scrub still re-reads every file once per `scrub_days` to catch silent corruption.
* Lightweight database storage: stores file hashes in a compact, memory-mapped database (LMDB) for rapid access and minimal overhead. The following information is stored for each file:
  * Full file path
  * Hash
  * Inode number
  * Link count
  * Block count
  * Size, mtime and ctime
* Four commands:
  - add: to register new files in the database.
  - check: to verify files against stored information, flagging any mismatches. It only writes the small scrub bookkeeping record.
  - update: to update the database with new information for existing files and drop the ones that no longer exist.
  - stats: to print database statistics.

Options: `-c/--config PATH`, `-V/--verbose` (progress and diagnostics), `-j/--json` (JSON report on stdout; diagnostics go to stderr), `--full` (read every file, ignore quick_check).

Exit codes: `0` success (no changes found in check mode), `1` changes detected (check mode only), `2` error: configuration, database full or incompatible, or any file/directory that could not be read (errors win over "changes found" because an incomplete run cannot vouch for the rest).

Upgrading: the database format changed (path keys no longer include the trailing NUL, entries gained size/mtime/ctime, and a format record was added). A database made by an older build is refused with a clear message: remove the database directory and run `add` again. Other behavior changes: paths longer than LMDB's key limit (511 bytes) cannot be tracked and are reported as errors; a tracked file that was replaced by a symbolic link or a directory is reported as missing; symbolic links inside the scanned directories are skipped, `exclude_directories`/`exclude_extensions`/`directories` are comma separated, and the sample configuration is installed as `ffc.conf.example`.

Design overview:
* Main thread (producer): traverses directories (`openat`/`readdir`, symbolic links are not followed) and feeds a bounded queue.
* I/O scheduler thread: takes tasks from the queue and hands them to the thread pool. In HDD mode it also orders them (see below).
* Worker threads: compute hashes in parallel and compare against the database.
* Database writer thread (add/update): commits the new entries in batches (`db_batch_size`) so there is one fsync per batch instead of one per file.

This separation of concerns is efficient because:
* Directory traversal is I/O bound and works well in a single thread
* Queue management is centralized, preventing race conditions
* Hash computation is CPU-intensive and properly parallelized
* Database writes are batched on a single thread instead of contending on LMDB's single writer lock

## HDD mode

With `storage = auto` (default) FastFileCheck asks the kernel whether the scanned directories live on a rotational device (tested through partitions, LVM/device-mapper and btrfs subvolumes); set `storage = hdd` or `ssd` to force it. In HDD mode:

1. **Inode-ordered walk.** Each directory is read, sorted by inode number and its files are `stat`ed in that order. Inode numbers follow the inode table (ext4, xfs) or the fs-tree leaves (btrfs), so metadata is read front to back instead of at random.
2. **Only what must be read is queued for the disk.** Files that quick_check can skip (and, in check mode, files the database does not know) cost no data read and bypass the disk queue.
3. **Disk order.** The files that must be read are collected in windows of 16384, their first physical extent is looked up (`FS_IOC_FIEMAP`; where unsupported, inode order is kept) and they are handed to the workers sorted by it, so the head makes one sweep per window.
4. **Prefetch with a byte budget.** While the workers hash, the next files are already requested from the kernel (`POSIX_FADV_WILLNEED`) up to a budget (`prefetch_mb`, default 1/8 of usable RAM, 64 MB to 1 GB). The disk always has a deep queue of large, sorted requests; the workers find their data in the page cache. The scanner is also held back so it does not fight the readers for the head.
5. **Large reads instead of mmap page faults** (4 MB, reused buffer) for hashing.

On a btrfs test tree (600 files created in shuffled order, FIEMAP on an SSD) the reads in HDD mode followed the physical order with zero backward jumps, against 303 for the unordered order (head-travel proxy: 812 MiB vs 145,000 MiB). That demonstrates the ordering, not the wall-clock gain, which depends on your disk and on a cold cache: measure with `scripts/bench-hdd.sh`.

Limits: FIEMAP is unavailable on tmpfs/NFS/ZFS (inode order is used instead); on btrfs the FIEMAP address is a logical address (monotonic inside a chunk, a good but not perfect proxy); `low_priority` only matters with the BFQ scheduler; several disks given in `directories` are scanned one after another (one scheduler per disk is not implemented).

## Quick check and the rolling scrub

`quick_check` (default on) skips reading a file when size, mtime and ctime match the database. That is what makes re-checks fast, but silent corruption (bit rot) changes none of them. The rolling scrub closes that gap without giving the speed back: the database keeps a cursor on a 64-bit ring of path hashes, and each run re-reads the slice of files that became due since the previous run (elapsed time divided by `scrub_days`), then advances the cursor. Every file is therefore read completely at least once every `scrub_days` days (default 30), however often or rarely you run, and a daily run reads about 1/30 of the data. Use `--full` to read everything now, or `scrub_days = 0` to disable the scrub.

## HDD performance tuning (LMDB flags and recipes)

On rotational HDDs, LMDB’s default durability behavior can throttle throughput due to frequent fsync/metadata syncs and head seeks. FastFileCheck exposes four LMDB flags through its config to trade crash/power-loss durability for speed. All default to false (safe). Enable them only if you accept the risks.

Flags (set under [database] in the config):
- lmdb_nosync: Reduce fsync frequency. Much faster on HDD; risk of DB corruption on sudden power loss.
- lmdb_nometasync: Skip metadata syncs. Additional speed; same power-loss risk as above.
- lmdb_mapasync: Let the OS flush dirty pages asynchronously. Can improve throughput; unsafe on crash.
- lmdb_writemap: Use a writeable memory map. Faster writes, but additional risk with multiple writers; keep FastFileCheck as the only process writing to the DB.

Quick recipes:
- Safe (default): All flags = false. Maximum durability, slower on HDD.
- HDD Fast (balanced): lmdb_nosync = true, lmdb_nometasync = true. Good boost on HDDs with moderate risk.
- HDD Max (aggressive): lmdb_nosync = true, lmdb_nometasync = true, lmdb_mapasync = true, lmdb_writemap = true. Highest speed, highest risk. Use only if you can rebuild the DB.

How to enable:
1) Edit your config (system: /etc/ffc.conf, or use -c path/to/conf). See example.conf for all options.
2) Under [database], set the desired flags to true/false as per the recipes above.
3) Optionally increase db_size_mb if you store many files; LMDB requires enough map size to avoid growth pauses.
4) Run FastFileCheck as usual (add/check/update). You can pass --verbose to see progress and queue utilization.

Verify improvement:
- Cold-cache A/B on your own disk: scripts/bench-hdd.sh (needs root only to drop the page cache).
- Time your run: time FastFileCheck --verbose add (or update) ...
- Monitor I/O: iostat -xz 1 or pidstat -d 1 to observe reduced await on HDD.
- You should see higher processed file counts per second in verbose progress messages.

Revert safely:
- Set flags back to false and rerun. The DB contents remain usable; flags affect how writes are flushed, not the data format.

Additional optional optimizations:
Application/config level:
- Place the LMDB database on a faster device (e.g., SSD) while scanning files from HDD. Set [database].db_path to an SSD-backed directory.
- Reduce file system noise: disable logging to file if not needed (logging.log_to_file_enabled = false) to cut extra writes.
- Threads: settings.threads_count = 0 lets FastFileCheck auto-size. With HDD mode the workers only hash cached data, so the default is fine; before HDD mode existed, io_threads = 1 or 2 was the workaround.
- Adjust RAM usage: settings.ram_usage_percent controls read buffer sizes; higher values can improve streaming reads but leave headroom for the OS page cache.
- Tune scanning scope: use scanning.exclude_directories and scanning.exclude_extensions to skip junk (e.g., caches, logs, temp files).

OS/filesystem level (advanced; test before adopting):
- Mount options like noatime on the scanned HDD can reduce extra metadata writes.
- Ensure write cache is enabled on the HDD (hdparm -W), and use a UPS if enabling aggressive flags.
- Use a scheduler suitable for HDDs (e.g., BFQ for fairness or MQ-Deadline for latency) depending on your workload and kernel. low_priority = true only has an effect with BFQ.

Notes:
- FastFileCheck opens LMDB with the configured flags at startup; these flags only affect database durability/throughput. File hashing correctness is unaffected.
- For safety, keep a recent backup of the LMDB directory if you rely on maximum-speed settings.
