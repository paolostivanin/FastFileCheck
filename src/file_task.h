#pragma once

#include <glib.h>
#include <sys/stat.h>

// One unit of work travelling scanner -> I/O scheduler -> worker pool.
typedef struct file_task_t {
    gchar *path;
    struct stat st;          // valid when has_stat
    gboolean has_stat;       // HDD mode: the scanner already stat()ed it (in inode order)
    guint64 seq;             // scanner order, used as a stable tie-break
    guint64 phys;            // io_sched: physical offset of the first extent (0 = unknown/inline)
    guint64 charged_bytes;   // io_sched: look-ahead budget held by this task until it is hashed
} FileTask;

static inline FileTask *
file_task_new (const gchar *path)
{
    FileTask *task = g_new0 (FileTask, 1);
    task->path = g_strdup (path);
    return task;
}

static inline void
file_task_free (FileTask *task)
{
    if (task) {
        g_free (task->path);
        g_free (task);
    }
}
