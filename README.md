# FastFileCheck
FastFileCheck is a high-performance, multithreaded file integrity checker for Linux. Designed for speed and efficiency, FastFileCheck utilizes parallel processing and a lightweight database to quickly hash and verify large volumes of files, ensuring their integrity over time.

Features:
* Multithreaded processing: automatically adapts to available CPU cores for optimal performance.
* Flexible configuration: customize thread count via a configuration file, or let the program dynamically adjust based on system resources.
* Efficient hashing: uses fast, non-cryptographic hashing (xxHash) to detect file changes.
* Lightweight database storage: stores file hashes in a compact, memory-mapped database (LMDB) for rapid access and minimal overhead.
* Two modes of operation:
  - add: to register new files in the database.
  - check: to verify files against stored hashes, flagging any mismatches.
