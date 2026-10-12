#include <glib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include "io_sched.h"
#include "process_file.h"

#define WINDOW_FILES        16384                    // files sorted together = one disk sweep
#define MIN_BUDGET          (64ULL << 20)            // 64MB
#define MAX_BUDGET          (1ULL << 30)             // 1GB
#define NO_PREFETCH_ABOVE   4                        // files above budget/4 are streamed, not prefetched

struct io_sched_t {
    ConsumerData *consumer_data;
    gboolean      hdd_mode;
    guint64       budget;          // max bytes in flight

    GMutex        lock;
    GCond         cond;
    guint64       in_flight;       // protected by lock

    GPtrArray    *window;          // FileTask *, owned until dispatched
};


IoSched *
io_sched_new (ConsumerData *consumer_data)
{
    IoSched *sched = g_try_new0 (IoSched, 1);
    if (!sched) {
        g_critical ("Failed to allocate memory for the I/O scheduler");
        return NULL;
    }

    const ConfigData *config = consumer_data->config_data;
    sched->consumer_data = consumer_data;
    sched->hdd_mode = config->hdd_mode;
    sched->budget = (config->prefetch_mb > 0)
                        ? (guint64)config->prefetch_mb << 20
                        : CLAMP (config->usable_ram / 8, MIN_BUDGET, MAX_BUDGET);
    g_mutex_init (&sched->lock);
    g_cond_init (&sched->cond);
    sched->window = g_ptr_array_new ();

    if (sched->hdd_mode) {
        g_debug ("I/O scheduler: HDD mode, window %u files, look-ahead budget %" G_GUINT64_FORMAT " MB",
                 (guint)WINDOW_FILES, sched->budget >> 20);
    }
    return sched;
}


void
io_sched_free (IoSched *sched)
{
    if (!sched) return;
    // tasks are owned by whoever dispatched them; anything left here never reached the pool
    for (guint i = 0; i < sched->window->len; i++) {
        file_task_free (g_ptr_array_index (sched->window, i));
    }
    g_ptr_array_free (sched->window, TRUE);
    g_mutex_clear (&sched->lock);
    g_cond_clear (&sched->cond);
    g_free (sched);
}


void
io_sched_release (IoSched *sched,
                  guint64  bytes)
{
    if (!sched || bytes == 0) return;

    g_mutex_lock (&sched->lock);
    sched->in_flight -= MIN (bytes, sched->in_flight);
    g_cond_signal (&sched->cond);
    g_mutex_unlock (&sched->lock);
}


// Physical offset of the file's first extent, 0 if it cannot be told (unsupported filesystem,
// inline or not yet allocated data). On btrfs this is a logical address, which grows with the
// physical position inside a chunk: good enough to order reads.
static guint64
first_extent_physical (const gchar *path)
{
    int fd = open_noatime (path);
    if (fd < 0) return 0;

    // struct fiemap ends in a flexible array: reserve room for exactly one extent behind it
    union {
        struct fiemap map;
        guchar        bytes[sizeof (struct fiemap) + sizeof (struct fiemap_extent)];
    } request;
    memset (&request, 0, sizeof (request));
    request.map.fm_start = 0;
    request.map.fm_length = FIEMAP_MAX_OFFSET;
    request.map.fm_extent_count = 1;

    guint64 physical = 0;
    if (ioctl (fd, FS_IOC_FIEMAP, &request) == 0 && request.map.fm_mapped_extents > 0) {
        const struct fiemap_extent *extent = &request.map.fm_extents[0];
        if (!(extent->fe_flags & (FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DATA_INLINE))) {
            physical = extent->fe_physical;
        }
    }

    close (fd);
    return physical;
}


// Ask the kernel to start reading the whole file now; returns without waiting for the data
static void
prefetch_file (const gchar *path,
               guint64      size)
{
#ifdef POSIX_FADV_WILLNEED
    int fd = open_noatime (path);
    if (fd < 0) return;
    (void)posix_fadvise (fd, 0, (off_t)size, POSIX_FADV_WILLNEED);
    close (fd);
#else
    (void)path;
    (void)size;
#endif
}


static void
dispatch (IoSched  *sched,
          FileTask *task,
          guint64   charged)
{
    task->charged_bytes = charged;
    GError *error = NULL;
    if (!g_thread_pool_push (sched->consumer_data->thread_pool, task, &error)) {
        // Cannot happen short of resource exhaustion; do not leak the task or hold its budget forever
        g_critical ("Could not queue %s: %s", task->path, error ? error->message : "unknown error");
        summary_record_error (sched->consumer_data->summary_data);
        g_clear_error (&error);
        io_sched_release (sched, charged);
        file_task_free (task);
    }
}


static gint
compare_dev_ino (gconstpointer a,
                 gconstpointer b)
{
    const FileTask *ta = *(FileTask * const *)a;
    const FileTask *tb = *(FileTask * const *)b;
    if (ta->st.st_dev != tb->st.st_dev) return (ta->st.st_dev > tb->st.st_dev) - (ta->st.st_dev < tb->st.st_dev);
    return (ta->st.st_ino > tb->st.st_ino) - (ta->st.st_ino < tb->st.st_ino);
}


static gint
compare_phys_seq (gconstpointer a,
                  gconstpointer b)
{
    const FileTask *ta = *(FileTask * const *)a;
    const FileTask *tb = *(FileTask * const *)b;
    if (ta->phys != tb->phys) return (ta->phys > tb->phys) - (ta->phys < tb->phys);
    return (ta->seq > tb->seq) - (ta->seq < tb->seq);
}


static void
flush_window (IoSched *sched)
{
    GPtrArray *window = sched->window;
    gboolean aborted = g_atomic_int_get (&sched->consumer_data->file_queue_data->abort_requested);

    if (!aborted) {
        // 1. Ask for the extents in inode order: the extent items live next to the inodes
        //    (btrfs fs-tree, ext4 inode table), so this pass is a sequential metadata sweep.
        g_ptr_array_sort (window, compare_dev_ino);
        for (guint i = 0; i < window->len; i++) {
            FileTask *task = g_ptr_array_index (window, i);
            task->phys = first_extent_physical (task->path);
        }

        // 2. Data order: one sweep over the platters
        g_ptr_array_sort (window, compare_phys_seq);
    }

    for (guint i = 0; i < window->len; i++) {
        FileTask *task = g_ptr_array_index (window, i);
        guint64 size = (guint64)task->st.st_size;

        if (g_atomic_int_get (&sched->consumer_data->file_queue_data->abort_requested)) {
            dispatch (sched, task, 0);   // process_file() returns immediately
            continue;
        }

        // Look-ahead budget: wait until the disk has caught up (a single file bigger than the
        // whole budget is let through once nothing else is in flight)
        g_mutex_lock (&sched->lock);
        while (sched->in_flight > 0 && sched->in_flight + size > sched->budget) {
            g_cond_wait (&sched->cond, &sched->lock);
        }
        sched->in_flight += size;
        g_mutex_unlock (&sched->lock);

        // Huge files are streamed by the worker; the kernel's sequential readahead handles them
        if (size <= sched->budget / NO_PREFETCH_ABOVE) {
            prefetch_file (task->path, size);
        }
        dispatch (sched, task, size);
    }

    g_ptr_array_set_size (window, 0);
}


void
io_sched_run (IoSched *sched)
{
    ConsumerData *consumer_data = sched->consumer_data;
    FileQueueData *file_queue_data = consumer_data->file_queue_data;

    while (TRUE) {
        FileTask *task = g_async_queue_timeout_pop (file_queue_data->queue, 10 * 1000);
        if (task != NULL) {
            if (!sched->hdd_mode || !process_file_needs_read (task, consumer_data)) {
                dispatch (sched, task, 0);
                continue;
            }

            g_ptr_array_add (sched->window, task);
            if (sched->window->len >= WINDOW_FILES) {
                flush_window (sched);
            }
            continue;
        }

        if (g_atomic_int_get (&file_queue_data->scanning_done) &&
            g_async_queue_length (file_queue_data->queue) == 0) {
            break;
        }
    }

    if (sched->window->len > 0) {
        flush_window (sched);
    }
}
