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