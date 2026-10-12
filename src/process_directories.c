#include <glib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "process_directories.h"
#include "file_task.h"

#define PATH_BUFFER_SIZE PATH_MAX

typedef struct {
    GHashTable *excluded_dirs;
    GHashTable *excluded_exts;
    gboolean exclude_hidden;
    gboolean hdd_mode;
    SummaryData *summary_data;
    guint64 next_seq;
} ScanContext;

typedef struct process_context_t {
    GHashTable *visited; // Tracks visited directories to prevent loops
    guint depth;          // Current recursion depth
} ProcessContext;

typedef struct dir_entry_t {
    gchar *name;
    ino_t ino;
    guchar type;          // d_type, DT_UNKNOWN when the filesystem does not say
} DirEntry;


// g_async_queue_length() goes negative while the consumer is blocked in pop (it subtracts the
// waiting threads). Casting that to guint made an idle queue look "full" and the scanner
// slept until it happened to catch the consumer between two pops.
static gboolean
queue_is_full (GAsyncQueue *queue,
               guint        max_size)
{
    gint len = g_async_queue_length (queue);
    return len > 0 && (guint)len >= max_size;
}


static gint
compare_inodes (gconstpointer a,
                gconstpointer b)
{
    const DirEntry *ea = a;
    const DirEntry *eb = b;
    return (ea->ino > eb->ino) - (ea->ino < eb->ino);
}


static void
emit_task (FileQueueData     *file_queue_data,
           ScanContext       *scan_ctx,
           const gchar       *path,
           const struct stat *st)
{
    FileTask *task = file_task_new (path);
    task->seq = scan_ctx->next_seq++;
    if (st) {
        task->st = *st;
        task->has_stat = TRUE;
    }

    guint sleep_us = 1000; // start with 1ms
    while (queue_is_full (file_queue_data->queue, file_queue_data->max_size)) {
        // Exponential backoff up to 10ms to reduce CPU while waiting for consumers
        g_usleep (sleep_us);
        if (sleep_us < 10000) sleep_us = MIN (sleep_us * 2, 10000);
    }
    g_async_queue_push (file_queue_data->queue, task);
}


static gboolean
should_skip_entry(const gchar *entry_name,
                  const gchar *full_path,
                  ScanContext *scan_ctx)
{
    if (scan_ctx->exclude_hidden && entry_name[0] == '.') {
        return TRUE;
    }

    if (scan_ctx->excluded_dirs && g_hash_table_contains (scan_ctx->excluded_dirs, full_path)) {
        return TRUE;
    }

    if (scan_ctx->excluded_exts) {
        const gchar *dot = strrchr (entry_name, '.');
        if (dot && g_hash_table_contains (scan_ctx->excluded_exts, dot)) {
            return TRUE;
        }
    }

    return FALSE;
}


// A directory that cannot be read means its files cannot be verified: that is an error for the
// exit code. One that disappeared while we were scanning is just a race.
static void
report_dir_error (ScanContext *scan_ctx,
                  const gchar *dir_path,
                  const gchar *what,
                  int          err,
                  gboolean     nested)
{
    if (err == ENOENT && nested) {
        g_debug ("Directory vanished during the scan: %s", dir_path);
        return;
    }
    g_critical ("Failed to %s directory %s: %s", what, dir_path, g_strerror (err));
    summary_record_error (scan_ctx->summary_data);
}


// Scans one directory. Takes ownership of dir_fd.
//
// In HDD mode the entries of a directory are visited in inode order, and regular files are
// stat()ed right here in that order: inode numbers follow the on-disk inode table (ext4/xfs) or
// the fs-tree leaf order (btrfs), so the metadata is read front to back instead of at random.
// Every file of a directory is emitted before descending, which keeps them together in the queue.
static void
scan_dir (int             dir_fd,
          const gchar    *dir_path,
          guint           max_depth,
          ProcessContext *ctx,
          FileQueueData  *file_queue_data,
          ScanContext    *scan_ctx)
{
    DIR *dir = fdopendir (dir_fd);
    if (!dir) {
        report_dir_error (scan_ctx, dir_path, "open", errno, ctx->depth > 0);
        close (dir_fd);
        return;
    }

    gchar path_buffer[PATH_BUFFER_SIZE];
    GArray *entries = g_array_new (FALSE, FALSE, sizeof (DirEntry));

    errno = 0;
    struct dirent *de;
    while ((de = readdir (dir)) != NULL) {
        const gchar *name = de->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;

        int n = g_snprintf (path_buffer, PATH_BUFFER_SIZE, "%s/%s", dir_path, name);
        if (n >= PATH_BUFFER_SIZE) {
            g_warning ("Path too long, skipping: %s/%s", dir_path, name);
            continue;
        }
        if (should_skip_entry (name, path_buffer, scan_ctx)) continue;

        DirEntry entry = { .name = g_strdup (name), .ino = de->d_ino, .type = de->d_type };
        g_array_append_val (entries, entry);
    }
    if (errno != 0) {
        report_dir_error (scan_ctx, dir_path, "read", errno, ctx->depth > 0);
    }

    if (scan_ctx->hdd_mode) {
        g_array_sort (entries, compare_inodes);
    }

    const int fd = dirfd (dir);
    GPtrArray *subdirs = g_ptr_array_new_with_free_func (g_free);

    for (guint i = 0; i < entries->len; i++) {
        DirEntry *entry = &g_array_index (entries, DirEntry, i);

        if (g_atomic_int_get (&file_queue_data->abort_requested)) break;

        guchar type = entry->type;
        struct stat st;
        gboolean have_stat = FALSE;

        // Regular files are stat()ed here in HDD mode (inode order); in SSD mode the workers
        // do it in parallel. Filesystems that do not fill d_type need it for every entry.
        if ((scan_ctx->hdd_mode && type == DT_REG) || type == DT_UNKNOWN) {
            if (fstatat (fd, entry->name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno != ENOENT) {
                    g_snprintf (path_buffer, PATH_BUFFER_SIZE, "%s/%s", dir_path, entry->name);
                    g_critical ("Could not stat %s: %s", path_buffer, g_strerror (errno));
                    summary_record_error (scan_ctx->summary_data);
                }
                continue;
            }
            have_stat = TRUE;
            if (type == DT_UNKNOWN) {
                type = S_ISREG (st.st_mode) ? DT_REG : S_ISDIR (st.st_mode) ? DT_DIR : DT_LNK;
            }
        }

        if (type == DT_REG) {
            g_snprintf (path_buffer, PATH_BUFFER_SIZE, "%s/%s", dir_path, entry->name);
            emit_task (file_queue_data, scan_ctx, path_buffer, have_stat ? &st : NULL);
        } else if (type == DT_DIR) {
            g_ptr_array_add (subdirs, g_strdup (entry->name));
        }
        // symlinks, sockets, fifos, devices: skipped
    }

    for (guint i = 0; i < subdirs->len; i++) {
        const gchar *name = g_ptr_array_index (subdirs, i);

        if (g_atomic_int_get (&file_queue_data->abort_requested)) break;

        g_snprintf (path_buffer, PATH_BUFFER_SIZE, "%s/%s", dir_path, name);

        if (ctx->depth + 1 > max_depth) {
            g_log (NULL, G_LOG_LEVEL_WARNING, "Max recursion depth exceeded at: %s", path_buffer);
            continue;
        }
        if (g_hash_table_contains (ctx->visited, path_buffer)) continue;
        g_hash_table_add (ctx->visited, g_strdup (path_buffer));

        int sub_fd = openat (fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (sub_fd < 0) {
            report_dir_error (scan_ctx, path_buffer, "open", errno, TRUE);
            continue;
        }

        ctx->depth++;
        scan_dir (sub_fd, path_buffer, max_depth, ctx, file_queue_data, scan_ctx);
        ctx->depth--;
    }

    g_ptr_array_free (subdirs, TRUE);
    for (guint i = 0; i < entries->len; i++) {
        g_free (g_array_index (entries, DirEntry, i).name);
    }
    g_array_free (entries, TRUE);
    closedir (dir);
}


static GHashTable *
split_set (const gchar *list)
{
    GHashTable *set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    gchar **parts = g_strsplit (list, ",", -1);
    for (gsize i = 0; parts[i]; i++) {
        gchar *trimmed = g_strstrip (g_strdup (parts[i]));
        if (*trimmed) {
            g_hash_table_add (set, trimmed);
        } else {
            g_free (trimmed);
        }
    }
    g_strfreev (parts);
    return set;
}


void
process_directories (gchar         **dirs,
                     guint           max_depth,
                     FileQueueData  *file_queue_data,
                     ConfigData     *config_data,
                     SummaryData    *summary_data,
                     GPtrArray      *empty_roots)
{
    ProcessContext *ctx = g_try_new0 (ProcessContext, 1);
    if (!ctx) {
        g_critical ("Failed to allocate memory for ProcessContext");
        summary_record_error (summary_data);
        g_atomic_int_set (&file_queue_data->scanning_done, TRUE);
        return;
    }
    ctx->visited = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    ctx->depth = 0;

    ScanContext *scan_ctx = g_try_new0 (ScanContext, 1);
    if (!scan_ctx) {
        g_critical ("Failed to allocate memory for ScanContext");
        summary_record_error (summary_data);
        g_atomic_int_set (&file_queue_data->scanning_done, TRUE);
        g_hash_table_destroy (ctx->visited);
        g_free (ctx);
        return;
    }
    scan_ctx->exclude_hidden = config_data->exclude_hidden;
    scan_ctx->hdd_mode = config_data->hdd_mode;
    scan_ctx->summary_data = summary_data;

    if (config_data->exclude_directories) {
        scan_ctx->excluded_dirs = split_set (config_data->exclude_directories);
    }
    if (config_data->exclude_extensions) {
        scan_ctx->excluded_exts = split_set (config_data->exclude_extensions);
    }

    for (gsize i = 0; dirs[i] != NULL; i++) {
        if (g_atomic_int_get (&file_queue_data->abort_requested)) break;

        if (g_hash_table_contains (ctx->visited, dirs[i])) continue;
        g_hash_table_add (ctx->visited, g_strdup (dirs[i]));

        // The configured roots may be symlinks; everything below them is not followed
        int fd = open (dirs[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            report_dir_error (scan_ctx, dirs[i], "open", errno, FALSE);
            g_ptr_array_add (empty_roots, g_strdup (dirs[i]));
            continue;
        }

        const guint64 files_before = scan_ctx->next_seq;
        scan_dir (fd, dirs[i], max_depth, ctx, file_queue_data, scan_ctx);
        if (scan_ctx->next_seq == files_before) {
            g_ptr_array_add (empty_roots, g_strdup (dirs[i]));
        }
    }

    g_atomic_int_set (&file_queue_data->scanning_done, TRUE);

    g_hash_table_destroy (ctx->visited);
    g_free (ctx);
    if (scan_ctx->excluded_dirs) g_hash_table_destroy (scan_ctx->excluded_dirs);
    if (scan_ctx->excluded_exts) g_hash_table_destroy (scan_ctx->excluded_exts);
    g_free (scan_ctx);
}
