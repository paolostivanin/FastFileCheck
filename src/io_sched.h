#pragma once

#include <glib.h>
#include "queue.h"
#include "file_task.h"

// The stage between the scanner and the worker pool.
//
// SSD mode: a pass-through, every task goes straight to the pool.
//
// HDD mode ("plan -> order -> prefetch -> hash"):
//   * Files that need no data read (unchanged under quick_check, unknown in CHECK, empty) skip
//     the queue and go to the pool at once: they only cost CPU.
//   * Files that must be read are collected in a window, put in on-disk order (physical offset of
//     the first extent via FIEMAP, inode order as the fallback) and handed to the workers in that
//     order, so the disk head makes one sweep per window instead of jumping between files.
//   * A byte budget limits how much data is "in flight" (asked from the disk but not hashed
//     yet). While the budget allows, the next files are requested from the kernel with
//     POSIX_FADV_WILLNEED, so the disk always has a deep, sorted queue of large requests and the
//     workers find their data already in the page cache.
typedef struct io_sched_t IoSched;

IoSched *io_sched_new     (ConsumerData *consumer_data);

// Body of the queue-consumer thread: returns when the scanner is done and everything is dispatched
void     io_sched_run     (IoSched      *sched);

// Called by a worker when it is done with a task (returns the task's in-flight budget)
void     io_sched_release (IoSched      *sched,
                           guint64       bytes);

void     io_sched_free    (IoSched      *sched);
