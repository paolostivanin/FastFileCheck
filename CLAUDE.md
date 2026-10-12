# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
# Binary: ./FastFileCheck
```

Dependencies (install via system package manager): `libxxhash`, `lmdb`, `glib-2.0` (≥2.68.0).

There is no automated test suite or linter. The project relies on strict compiler warnings (`-Werror=format-security`, `-Werror=implicit-function-declaration`, `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3` outside Debug) and must build with no warnings. When changing threading code also build with `-DCMAKE_C_FLAGS="-fsanitize=thread"` (GLib's mutexes are futex based and invisible to TSan: use a suppression file with `race:libglib-2.0.so`) and with `-fsanitize=address,undefined`.

`scripts/bench-hdd.sh DIR` is a cold-cache A/B benchmark (needs root to drop caches) of HDD mode against the unordered baseline (`storage = ssd`).

## Architecture

FastFileCheck is a multithreaded file integrity checker written in C11. It hashes files with xxHash3-64 and stores metadata (hash, inode, link count, block count, size, mtime, ctime) in LMDB. Four modes: `add` (populate DB), `check` (verify against DB; writes only the scrub bookkeeping record), `update` (sync DB with filesystem), `stats` (print DB statistics).

### Threading model (producer-consumer)

- **Main thread (producer):** Recursive directory traversal with `openat`/`readdir`/`fstatat` (symlinks are not followed). In HDD mode each directory is sorted by inode and its files are `stat`ed in that order. Pushes `FileTask`s into a bounded async queue (backoff when full; in HDD mode the bound is one scheduler window so the scanner does not fight the readers for the disk head).
- **I/O scheduler thread (`io_sched.c`, "queue-consumer"):** Pops tasks and feeds the GLib thread pool. SSD mode: pass-through. HDD mode: tasks that need no data read skip ahead; the others are collected in windows of 16384, ordered by first physical extent (FIEMAP, inode order as fallback), dispatched under a bounded in-flight byte budget, and prefetched with `POSIX_FADV_WILLNEED`.
- **Worker thread pool:** Sized by `threads_count` (CPU count, lowered by `io_threads`). Each worker hashes (mmap or chunked read in SSD mode, 4 MB `read()` into a per-thread buffer in HDD mode) and either does read-only DB lookups (CHECK) or pushes write requests to the DB writer queue (ADD/UPDATE). One `stat` and one DB lookup per file.
- **DB writer thread:** Batches LMDB writes (`db_batch_size`, default 500) into single transactions, sorted by key; a partial batch is committed after 1 s. Failures are counted as errors; `MDB_MAP_FULL` sets the run-wide abort flag.
- **Progress reporter thread:** Optional (enabled with `-V`), prints stats every 2 seconds.

### Source modules (`src/`)

| File | Role |
|---|---|
| `main.c` | CLI parsing, thread orchestration, lifecycle, storage resolution, scrub window, exit codes |
| `config.c` | Loads INI config (`/etc/ffc.conf` or `-c`), computes thread count and per-worker RAM budget |
| `storage.c` | Rotational-disk detection (`/proc/self/mountinfo` → `/sys/class/block/*/queue/rotational`) |
| `process_directories.c` | Directory scan, exclusions, depth limit, inode-ordered stat in HDD mode |
| `file_task.h` | `FileTask`: path + optional pre-taken `stat` travelling scanner → scheduler → worker |
| `queue.c` | Wraps `GAsyncQueue`, sizes the queue |
| `io_sched.c` | HDD-mode ordering, look-ahead budget, FIEMAP, prefetch |
| `process_file.c` | Hashing, quick-check/scrub skip, DB compare per mode, missing-file pass |
| `seen_set.c` | Sharded set of path hashes the workers visited (lets the missing-file pass skip `stat`) |
| `db_writer.c` | Dedicated DB writer thread |
| `database.c` | LMDB environment, format record (`DbMeta`: version, scrub cursor, last run), stats |
| `summary.c` | Thread-safe counters (changes, errors), text and JSON reports |
| `logging.c` | GLib log handler, console + optional file output |

### Key data flow

`process_directories` → bounded async queue → I/O scheduler → thread pool workers (`process_file`) → DB writer queue → `db_writer` thread → LMDB

After all files are processed, `handle_missing_files_from_fs()` walks the DB: entries whose path is in the seen-set are known to exist; only the others get an `access()` check (relevant for `check` and `update`). It is skipped if the run was aborted.

### Database format

Keys are raw paths (no trailing NUL); values are `FileEntryData` (9×8 bytes, see `process_file.c`). The reserved key `"\0ffc:meta"` holds `DbMeta`. `init_db` refuses a DB whose format version differs or that has entries but no record. Bump `DB_FORMAT_VERSION` in `database.h` when the layout changes; `DbMeta` fields are only ever appended.

### Quick check and rolling scrub

A file is not read when size, mtime and ctime (ns) match the stored entry (`quick_check`); inode/links/blocks are still compared. The scrub forces a full read of the files whose path hash lies in a slice of the 64-bit ring: slice = `elapsed / scrub_days` of the ring, starting at the stored cursor, which advances after each completed run. In `update`, content that changed while the metadata did not is reported as corruption (error, entry kept).

### Error handling

Never use `g_log(..., G_LOG_LEVEL_ERROR, ...)` or `g_error` for runtime conditions: GLib treats it as fatal and aborts the process. Use `g_critical`/`g_warning` and `summary_record_error()`. A file that vanished after the scan (ENOENT) is not an error; the missing-file pass reports it, so a path only goes into the seen-set once it was really handled (never on ENOENT, never when it is no longer a regular file). Paths over LMDB's key limit (`DatabaseData.max_key_size`) are rejected per file: one oversized key must never fail a whole write batch. The DB entry is built from the `fstat()` of the file that was actually hashed, not from the scanner's older stat. `g_async_queue_length()` is negative while a consumer is blocked in pop: never cast it to unsigned.

### Exit codes

- `0`: Success (no changes in check mode, or add/update/stats completed)
- `1`: Changes detected (check mode only)
- `2`: Error (config, DB incompatible/full, any unreadable file or directory); wins over `1`

### Configuration

See `example.conf` for all options. Key sections: `[settings]` (threads, RAM%, io_threads, storage, prefetch_mb, low_priority), `[database]` (path, size, batch_size, LMDB durability flags), `[scanning]` (directories, exclusions, recursion depth, quick_check, scrub_days), `[logging]`. Lists are comma separated.

### CLI flags

- `-c, --config PATH`: Config file path
- `-V, --verbose`: Verbose output with progress (goes to stderr with `--json`)
- `-j, --json`: JSON output
- `--full`: Force a full re-read (disable quick_check and the scrub slice logic)

### Version

Version is defined in `CMakeLists.txt` and templated into `src/version.h.in` → `build/src/version.h`.
