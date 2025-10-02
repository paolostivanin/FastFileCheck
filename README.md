# FastFileCheck
FastFileCheck is a high-performance, multithreaded file integrity checker for Linux. Designed for speed and efficiency, FastFileCheck utilizes parallel processing and a lightweight database to quickly hash and verify large volumes of files, ensuring their integrity over time.

Features:
* Multithreaded processing: automatically adapts to available CPU cores for optimal performance.
* Flexible configuration: see example.conf about all configuration options.
* Efficient hashing: uses fast, non-cryptographic hashing (xxHash) to detect file changes.
* Lightweight database storage: stores file hashes in a compact, memory-mapped database (LMDB) for rapid access and minimal overhead. The following information is stored for each file:
  * Full file path
  * Hash
  * Inode number
  * Link count
  * Block count
* Three modes of operation:
  - add: to register new files in the database.
  - check: to verify files against stored information, flagging any mismatches.
  - update: to update the database with new information for existing files.

Design overwiew:
* Main thread (producer): traverses directories and feeds the queue (one thread is more than enough for most use cases)
* Dedicated consumer thread: manages queue and distributes work to threadpool
* Worker threads: compute hashes in parallel

This separation of concerns is efficient because:
* Directory traversal is I/O bound and works well in a single thread
* Queue management is centralized, preventing race conditions
* Hash computation is CPU-intensive and properly parallelized


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
- Time your run: time FastFileCheck --verbose add (or update) ...
- Monitor I/O: iostat -xz 1 or pidstat -d 1 to observe reduced await on HDD.
- You should see higher processed file counts per second in verbose progress messages.

Revert safely:
- Set flags back to false and rerun. The DB contents remain usable; flags affect how writes are flushed, not the data format.

Additional optional optimizations:
Application/config level:
- Place the LMDB database on a faster device (e.g., SSD) while scanning files from HDD. Set [database].db_path to an SSD-backed directory.
- Reduce file system noise: disable logging to file if not needed (logging.log_to_file_enabled = false) to cut extra writes.
- Increase threads carefully: settings.threads_count = 0 lets FastFileCheck auto-size; raising threads helps on fast storage/CPUs but HDDs may saturate with fewer threads.
- Adjust RAM usage: settings.ram_usage_percent controls read buffer sizes; higher values can improve streaming reads but leave headroom for the OS page cache.
- Tune scanning scope: use scanning.exclude_directories and scanning.exclude_extensions to skip junk (e.g., caches, logs, temp files).

OS/filesystem level (advanced; test before adopting):
- Mount options like noatime on the scanned HDD can reduce extra metadata writes.
- Ensure write cache is enabled on the HDD (hdparm -W), and use a UPS if enabling aggressive flags.
- Use a scheduler suitable for HDDs (e.g., BFQ for fairness or MQ-Deadline for latency) depending on your workload and kernel.

Notes:
- FastFileCheck opens LMDB with the configured flags at startup; these flags only affect database durability/throughput. File hashing correctness is unaffected.
- For safety, keep a recent backup of the LMDB directory if you rely on maximum-speed settings.
