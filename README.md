# FastFileCheck
FastFileCheck is a high-performance, multithreaded file integrity checker for Linux. Designed for speed and efficiency, FastFileCheck utilizes parallel processing and a lightweight database to quickly hash and verify large volumes of files, ensuring their integrity over time.

Features:
* Multithreaded processing: automatically adapts to available CPU cores for optimal performance.
* Flexible configuration: see example.conf about all configuration options.
* Efficient hashing: uses fast, non-cryptographic hashing (xxHash) to detect file changes.
* Lightweight database storage: stores file hashes in a compact, memory-mapped database (LMDB) for rapid access and minimal overhead.
* Three modes of operation:
  - add: to register new files in the database.
  - check: to verify files against stored information, flagging any mismatches.
  - update: to update the database with new information for existing files.

In the database itself, the following information is stored for each file:
* Full file path
* Hash
* Inode number
* Link count
* Block count