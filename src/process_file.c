#include <glib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <xxhash.h>
#include "process_file.h"
#include "queue.h"
#include "summary.h"
#include "db_writer.h"
#include "seen_set.h"
#include "file_task.h"

#define MMAP_THRESHOLD_RATIO 0.75
#define MIN_BUFFER_SIZE (10 * 1024 * 1024)  // 10MB
#define MAX_BUFFER_SIZE (128 * 1024 * 1024) // 128MB
#define HDD_READ_BUFFER (4 * 1024 * 1024)   // 4MB: big requests, few seeks

// Database value (format version 2, see database.h). Fixed-width fields only, so the
// layout is the same everywhere and there is no padding: 9 * 8 bytes.
typedef struct file_entry_t {
    guint64 hash;
    guint64 inode;
    guint64 link_count;
    gint64  block_count;
    gint64  file_size;
    gint64  mtime_sec;
    gint64  mtime_nsec;
    gint64  ctime_sec;
    gint64  ctime_nsec;
} FileEntryData;

G_STATIC_ASSERT (sizeof (FileEntryData) == 9 * 8);

typedef enum lookup_result_t {
    LOOKUP_ERROR,
    LOOKUP_FOUND,
    LOOKUP_NOT_FOUND
} LookupResult;


// A file that vanished after the scan is not an error: the missing-file pass reports it.
// Anything else (EACCES, EIO, ...) means we could not verify the file, so it counts.
static void
report_file_error (SummaryData *summary_data,
                   const char  *what,
                   const char  *filepath,
                   int          err)
{
    if (err == ENOENT) {
        g_debug ("File vanished before it could be read (%s): %s", what, filepath);
        return;
    }
    g_critical ("Could not %s %s: %s", what, filepath, g_strerror (err));
    summary_record_error (summary_data);
}


int
open_noatime (const char *filepath)
{
    // O_NONBLOCK does nothing for regular files, but keeps open() from hanging forever on a
    // path that was swapped for a FIFO after the scan (the fstat() in compute_hash rejects it).
    int fd = open (filepath, O_RDONLY | O_CLOEXEC | O_NOATIME | O_NONBLOCK);
    if (fd < 0 && errno == EPERM) {
        // O_NOATIME is only allowed for the file owner (or CAP_FOWNER)
        fd = open (filepath, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    }
    return fd;
}


// One read buffer per worker thread, allocated on first use and freed when the thread exits
static GPrivate hdd_buffer_key = G_PRIVATE_INIT (g_free);

static guchar *
get_hdd_buffer (void)
{
    guchar *buffer = g_private_get (&hdd_buffer_key);
    if (!buffer) {
        buffer = g_try_malloc (HDD_READ_BUFFER);
        if (buffer) g_private_set (&hdd_buffer_key, buffer);
    }
    return buffer;
}


static ssize_t
read_retry (int     fd,
            guchar *buffer,
            size_t  len)
{
    ssize_t n;
    do {
        n = read (fd, buffer, len);
    } while (n < 0 && errno == EINTR);
    return n;
}


// HDD mode: hash with large read()s into a reused buffer. Unlike mmap, where every page fault
// is a small synchronous request, this gives the kernel (and the disk) big sequential requests,
// and a file that fits in the buffer costs one read() and no allocation.
static gboolean
hash_by_read (int      fd,
              off_t    file_size,
              guint64 *hash_out,
              int     *err_out)
{
    guchar *buffer = get_hdd_buffer ();
    if (!buffer) {
        *err_out = ENOMEM;
        return FALSE;
    }

    ssize_t n = read_retry (fd, buffer, HDD_READ_BUFFER);
    if (n < 0) {
        *err_out = errno;
        return FALSE;
    }
    if ((off_t)n == file_size) {
        *hash_out = XXH3_64bits (buffer, (size_t)n);
        return TRUE;
    }

    XXH3_state_t *state = XXH3_createState ();
    if (!state) {
        *err_out = ENOMEM;
        return FALSE;
    }
    XXH3_64bits_reset (state);
    if (n > 0) XXH3_64bits_update (state, buffer, (size_t)n);

    while ((n = read_retry (fd, buffer, HDD_READ_BUFFER)) > 0) {
        XXH3_64bits_update (state, buffer, (size_t)n);
    }

    gboolean ok = (n == 0);
    if (ok) {
        *hash_out = XXH3_64bits_digest (state);
    } else {
        *err_out = errno;
    }
    XXH3_freeState (state);
    return ok;
}


// Hashes the content of an open regular file of `file_size` bytes. The caller closes the fd.
static gboolean
hash_fd (int            fd,
         off_t          file_size,
         const guint64  per_thread_ram,
         gboolean       hdd_mode,
         guint64       *hash_out,
         int           *err_out)
{
    if (hdd_mode) {
        return hash_by_read (fd, file_size, hash_out, err_out);
    }

    // Use memory mapping if file size is less than 75% of per-thread RAM
    if ((gdouble)file_size < ((gdouble)per_thread_ram * MMAP_THRESHOLD_RATIO)) {
        void *mapped = mmap (NULL, (size_t)file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped != MAP_FAILED) {
            madvise (mapped, (size_t)file_size, MADV_SEQUENTIAL);
            *hash_out = XXH3_64bits (mapped, (size_t)file_size);
            munmap (mapped, (size_t)file_size);
            return TRUE;
        }
    }

    // Fall back to chunked reading
    const gsize buffer_size = CLAMP(per_thread_ram / 4, MIN_BUFFER_SIZE, MAX_BUFFER_SIZE);

    guchar *buffer = g_try_malloc_n (buffer_size, sizeof(guchar));
    if (!buffer) {
        *err_out = ENOMEM;
        return FALSE;
    }

    XXH3_state_t *state = XXH3_createState ();
    if (!state) {
        *err_out = ENOMEM;
        g_free (buffer);
        return FALSE;
    }

    XXH3_64bits_reset (state);

    gboolean ok = TRUE;
    while (TRUE) {
        ssize_t bytes_read = read_retry (fd, buffer, buffer_size);
        if (bytes_read > 0) {
            XXH3_64bits_update (state, buffer, (size_t)bytes_read);
            continue;
        }
        if (bytes_read == 0) {
            break;
        }
        *err_out = errno;
        ok = FALSE;
        break;
    }

    if (ok) {
        *hash_out = XXH3_64bits_digest (state);
    }

    g_free (buffer);
    XXH3_freeState (state);
    return ok;
}


static gboolean
same_size_and_times (const struct stat *a,
                     const struct stat *b)
{
    return a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}


// Opens and hashes a file. `st_out` receives the fstat() of the file we are really reading, taken
// before the first byte is read: it, not an older stat() from the scanner (which can be hours old
// in HDD mode), describes the hashed content and is what the database entry must record.
// `changed_out` is set when the file was modified while it was being read (the hash is then not
// a consistent snapshot and must not be stored or judged).
static gboolean
compute_hash (const char    *filepath,
              const guint64  per_thread_ram,
              gboolean       hdd_mode,
              guint64       *hash_out,
              struct stat   *st_out,
              gboolean      *changed_out,
              int           *err_out)
{
    *changed_out = FALSE;

    int fd = open_noatime (filepath);
    if (fd < 0) {
        *err_out = errno;
        return FALSE;
    }

    if (fstat (fd, st_out) != 0 || !S_ISREG (st_out->st_mode)) {
        *err_out = (errno != 0) ? errno : EINVAL;
        close (fd);
        return FALSE;
    }

    if (st_out->st_size == 0) {
        *hash_out = XXH3_64bits (NULL, 0);
        close (fd);
        return TRUE;
    }

#ifdef POSIX_FADV_SEQUENTIAL
    (void)posix_fadvise (fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    gboolean ok = hash_fd (fd, st_out->st_size, per_thread_ram, hdd_mode, hash_out, err_out);

    if (ok) {
        struct stat after;
        *changed_out = (fstat (fd, &after) != 0) || !same_size_and_times (st_out, &after);
    }

#ifdef POSIX_FADV_DONTNEED
    (void)posix_fadvise (fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    close (fd);
    return ok;
}


static FileEntryData
make_entry (const struct stat *st,
            guint64            hash)
{
    return (FileEntryData) {
        .hash = hash,
        .inode = (guint64)st->st_ino,
        .link_count = (guint64)st->st_nlink,
        .block_count = (gint64)st->st_blocks,
        .file_size = (gint64)st->st_size,
        .mtime_sec = (gint64)st->st_mtim.tv_sec,
        .mtime_nsec = (gint64)st->st_mtim.tv_nsec,
        .ctime_sec = (gint64)st->st_ctim.tv_sec,
        .ctime_nsec = (gint64)st->st_ctim.tv_nsec
    };
}


// Same size, mtime and ctime (nanosecond resolution). ctime cannot be set from user space,
// so tools that restore an old mtime (rsync -t, cp -p, touch -d) still invalidate the match.
static gboolean
stat_unchanged (const FileEntryData *stored,
                const FileEntryData *now)
{
    return stored->file_size == now->file_size &&
           stored->mtime_sec == now->mtime_sec && stored->mtime_nsec == now->mtime_nsec &&
           stored->ctime_sec == now->ctime_sec && stored->ctime_nsec == now->ctime_nsec;
}


static gboolean
entry_equal (const FileEntryData *a,
             const FileEntryData *b)
{
    return a->hash == b->hash &&
           a->inode == b->inode &&
           a->link_count == b->link_count &&
           a->block_count == b->block_count &&
           stat_unchanged (a, b);
}


// Rolling scrub: is this path in the slice that is due for a full re-hash in this run?
static gboolean
scrub_selected (const ConsumerData *consumer_data,
                const char         *filepath)
{
    if (consumer_data->scrub_all) return TRUE;
    if (consumer_data->scrub_len == 0) return FALSE;
    // unsigned wrap-around makes this work for a slice that crosses the end of the ring
    guint64 position = (guint64)XXH3_64bits (filepath, strlen (filepath)) - consumer_data->scrub_start;
    return position < consumer_data->scrub_len;
}


static LookupResult
lookup_entry (DatabaseData  *db_data,
              const char    *filepath,
              FileEntryData *out)
{
    MDB_txn *txn;
    int rc = mdb_txn_begin (db_data->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0) {
        g_critical ("mdb_txn_begin failed: %s", mdb_strerror (rc));
        return LOOKUP_ERROR;
    }

    MDB_val key = { .mv_size = strlen (filepath), .mv_data = (void *)filepath };
    MDB_val data;
    LookupResult result;

    rc = mdb_get (txn, db_data->dbi, &key, &data);
    if (rc == 0) {
        if (data.mv_size == sizeof (FileEntryData)) {
            // LMDB values are not aligned: copy instead of casting the pointer
            memcpy (out, data.mv_data, sizeof (FileEntryData));
            result = LOOKUP_FOUND;
        } else {
            g_critical ("Corrupt database entry for %s (%zu bytes)", filepath, data.mv_size);
            result = LOOKUP_ERROR;
        }
    } else if (rc == MDB_NOTFOUND) {
        result = LOOKUP_NOT_FOUND;
    } else {
        g_critical ("Database lookup failed for %s: %s", filepath, mdb_strerror (rc));
        result = LOOKUP_ERROR;
    }

    mdb_txn_abort (txn);
    return result;
}


void
handle_missing_files_from_fs (DatabaseData *db_data,
                              SummaryData  *summary_data,
                              SeenSet      *seen,
                              gboolean      delete_file_from_db)
{
    MDB_txn *txn;
    MDB_cursor *cursor;
    MDB_val key, data;

    int txn_flags = delete_file_from_db ? 0 : MDB_RDONLY;
    int rc = mdb_txn_begin (db_data->env, NULL, txn_flags, &txn);
    if (rc != 0) {
        g_critical ("mdb_txn_begin failed: %s", mdb_strerror (rc));
        summary_record_error (summary_data);
        return;
    }

    rc = mdb_cursor_open (txn, db_data->dbi, &cursor);
    if (rc != 0) {
        g_critical ("mdb_cursor_open failed: %s", mdb_strerror (rc));
        summary_record_error (summary_data);
        mdb_txn_abort (txn);
        return;
    }

    while (mdb_cursor_get (cursor, &key, &data, MDB_NEXT) == 0) {
        if (db_is_meta_key (&key)) continue;

        // The workers already touched this path during this run, so it exists: no syscall needed.
        if (seen && seen_set_contains (seen, key.mv_data, key.mv_size)) continue;

        // Not seen: deleted, excluded from the scan, replaced by a symlink/directory, vanished while
        // being read, or its stat failed. Ask the filesystem (without following symlinks: the scanner
        // only tracks regular files, so a tracked file that became a symlink is gone as far as we care).
        gchar *db_filepath = g_strndup (key.mv_data, key.mv_size);
        struct stat lst;
        int lst_rc = lstat (db_filepath, &lst);
        int lst_errno = errno;
        if (lst_rc != 0 || !S_ISREG (lst.st_mode)) {
            if (lst_rc != 0 && lst_errno != ENOENT && lst_errno != ENOTDIR) {
                // Cannot tell (e.g. EACCES on a parent directory): never report or delete on a guess
                g_critical ("Could not check %s: %s", db_filepath, g_strerror (lst_errno));
                summary_record_error (summary_data);
            } else if (delete_file_from_db == FALSE) {
                record_change (summary_data, db_filepath, CHANGE_MISSING_IN_FS);
            } else {
                rc = mdb_cursor_del (cursor, 0);
                if (rc != 0) {
                    g_critical ("mdb_cursor_del failed for %s: %s", db_filepath, mdb_strerror (rc));
                    summary_record_error (summary_data);
                }
            }
        }
        g_free (db_filepath);
    }
    mdb_cursor_close (cursor);

    if (delete_file_from_db) {
        rc = mdb_txn_commit (txn);
        if (rc != 0) {
            g_critical ("mdb_txn_commit failed while removing missing files: %s", mdb_strerror (rc));
            summary_record_error (summary_data);
        }
    } else {
        mdb_txn_abort (txn);
    }
}


// Would process_file() read the file's content? The I/O scheduler asks this to keep files that
// need no data read (unchanged under quick_check, unknown to the DB in CHECK, empty) out of the
// physically-ordered read queue. It repeats the DB lookup the worker will do again: a lookup in
// the (prewarmed) DB is a couple of microseconds, a wrongly ordered disk read is milliseconds.
gboolean
process_file_needs_read (const FileTask *task,
                         ConsumerData    *consumer_data)
{
    if (!task->has_stat) return TRUE;
    if (!S_ISREG (task->st.st_mode) || task->st.st_size == 0) return FALSE;
    if (strlen (task->path) > consumer_data->db_data->max_key_size) return FALSE;  // the worker reports it

    const Mode mode = consumer_data->config_data->mode;
    if (mode == MODE_ADD) return TRUE;

    FileEntryData stored;
    switch (lookup_entry (consumer_data->db_data, task->path, &stored)) {
        case LOOKUP_ERROR:
            return FALSE;                 // the worker will report it
        case LOOKUP_NOT_FOUND:
            return mode == MODE_UPDATE;   // CHECK just reports it as missing in the DB
        case LOOKUP_FOUND:
            break;
    }

    FileEntryData fresh = make_entry (&task->st, 0);
    return !(consumer_data->config_data->quick_check && stat_unchanged (&stored, &fresh) &&
             !scrub_selected (consumer_data, task->path));
}


void
process_file (const FileTask *task,
              ConsumerData    *consumer_data)
{
    SummaryData *summary_data = consumer_data->summary_data;
    const Mode mode = consumer_data->config_data->mode;
    const gchar *file_path = task->path;

    // The run is doomed (e.g. database full): do not waste time reading files
    if (g_atomic_int_get (&consumer_data->file_queue_data->abort_requested)) return;

    if (!file_path || !*file_path) {
        g_warning ("Ignoring an empty file path");
        return;
    }

    // LMDB rejects longer keys. Without this check the writer would lose its whole batch.
    if (strlen (file_path) > consumer_data->db_data->max_key_size) {
        g_critical ("Path is longer than the database key limit (%zu bytes), it cannot be tracked: %s",
                    consumer_data->db_data->max_key_size, file_path);
        summary_record_error (summary_data);
        return;
    }

    // The stat() used for the quick check and the hash decision (the entry itself is built from the
    // fstat() of the file we really read, see below). It is taken before reading the content, so any
    // later modification changes mtime/ctime and is caught by the next run. In HDD mode the scanner
    // already did it, in inode order. lstat: a symlink swapped in after the scan is not followed.
    struct stat st;
    if (task->has_stat) {
        st = task->st;
    } else if (lstat (file_path, &st) != 0) {
        report_file_error (summary_data, "stat", file_path, errno);
        return;
    }
    // Not a regular file any more (replaced by a symlink/directory since the scan): leave it out of
    // the seen-set so the missing-file pass notices that the tracked file is gone.
    if (!S_ISREG (st.st_mode)) return;

    // One database lookup per file (ADD overwrites blindly, so it does not need one)
    FileEntryData stored;
    gboolean have_stored = FALSE;
    if (mode != MODE_ADD) {
        switch (lookup_entry (consumer_data->db_data, file_path, &stored)) {
            case LOOKUP_ERROR:
                summary_record_error (summary_data);
                return;
            case LOOKUP_FOUND:
                have_stored = TRUE;
                break;
            case LOOKUP_NOT_FOUND:
                break;
        }
    }

    // CHECK of a file that is not in the database: there is nothing to compare the hash
    // with, so do not read the whole file just to throw the result away.
    if (mode == MODE_CHECK && !have_stored) {
        record_change (summary_data, file_path, CHANGE_MISSING_IN_DB);
        seen_set_add (consumer_data->seen, file_path);
        summary_increment_processed (summary_data, 1);
        return;
    }

    FileEntryData fresh = make_entry (&st, 0);

    if (have_stored && consumer_data->config_data->quick_check && stat_unchanged (&stored, &fresh) &&
        !scrub_selected (consumer_data, file_path)) {
        // Content is assumed unchanged: reuse the stored hash, but the stat-derived fields
        // (inode, links, blocks) are still compared below. Files in the current scrub slice
        // are not trusted and take the full read below.
        fresh.hash = stored.hash;
        seen_set_add (consumer_data->seen, file_path);
    } else {
        int err = 0;
        guint64 hash = 0;
        struct stat read_st;
        gboolean changed_while_reading;
        if (!compute_hash (file_path, consumer_data->config_data->max_ram_per_thread,
                           consumer_data->config_data->hdd_mode, &hash, &read_st, &changed_while_reading, &err)) {
            // Not added to the seen-set: if the file vanished (ENOENT) the missing-file pass must see it
            report_file_error (summary_data, "read", file_path, err);
            return;
        }
        seen_set_add (consumer_data->seen, file_path);
        if (changed_while_reading) {
            // The hash is not a snapshot of any version of the file; the next run sees the new mtime
            g_debug ("File changed while it was being read, skipping it this time: %s", file_path);
            summary_increment_processed (summary_data, 1);
            return;
        }
        // From here on the entry describes what we hashed: the fd's own stat, not the scanner's older
        // one. A file edited between the scan and the read is then a normal modification, not "corruption".
        fresh = make_entry (&read_st, hash);
    }

    switch (mode) {
        case MODE_ADD:
            db_writer_push (consumer_data->db_writer, file_path, &fresh, sizeof (FileEntryData));
            break;

        case MODE_UPDATE:
            if (have_stored && fresh.hash != stored.hash && stat_unchanged (&stored, &fresh)) {
                // The content differs but size, mtime and ctime do not: nobody edited this file,
                // the data rotted. Never "update" that away; keep the old entry so check still flags it.
                g_critical ("Possible silent corruption (content changed, metadata did not): %s", file_path);
                summary_record_error (summary_data);
                break;
            }
            if (!have_stored || !entry_equal (&fresh, &stored)) {
                db_writer_push (consumer_data->db_writer, file_path, &fresh, sizeof (FileEntryData));
            }
            break;

        case MODE_CHECK:
            if (fresh.hash != stored.hash) {
                record_change (summary_data, file_path, CHANGE_HASH);
            }
            if (fresh.inode != stored.inode) {
                record_change (summary_data, file_path, CHANGE_INODE);
            }
            if (fresh.link_count != stored.link_count) {
                record_change (summary_data, file_path, CHANGE_LINKS);
            }
            if (fresh.block_count != stored.block_count) {
                record_change (summary_data, file_path, CHANGE_BLOCKS);
            }
            break;

        default:
            break;
    }

    summary_increment_processed (summary_data, 1);
}
