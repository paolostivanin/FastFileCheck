#include <lmdb.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "config.h"
#include "database.h"


gboolean
db_is_meta_key (const MDB_val *key)
{
    return key->mv_size > 0 && ((const char *)key->mv_data)[0] == '\0';
}


void
free_db (DatabaseData *db_data)
{
    if (db_data) {
        if (db_data->env && db_data->dbi) mdb_dbi_close (db_data->env, db_data->dbi);
        if (db_data->env) mdb_env_close (db_data->env);
        g_free (db_data);
    }
}


static guint64
get_total_ram (void)
{
    long pages = sysconf (_SC_PHYS_PAGES);
    long pagesize = sysconf (_SC_PAGE_SIZE);
    if (pages <= 0 || pagesize <= 0) return 0;
    return (guint64)pages * (guint64)pagesize;
}


// Size of the existing data.mdb (0 if there is none yet)
static guint64
get_db_file_size (const gchar *db_path)
{
    gchar *data_file = g_build_filename (db_path, "data.mdb", NULL);
    struct stat st;
    guint64 size = (stat (data_file, &st) == 0) ? (guint64)st.st_size : 0;
    g_free (data_file);
    return size;
}


// Ask the kernel to pull data.mdb into the page cache with large sequential reads, so the
// per-file lookups that follow are RAM hits instead of thousands of random 4K page faults.
static void
prewarm_db_file (const gchar *db_path)
{
    gchar *data_file = g_build_filename (db_path, "data.mdb", NULL);
    int fd = open (data_file, O_RDONLY | O_CLOEXEC);
    g_free (data_file);
    if (fd < 0) return;
#ifdef POSIX_FADV_WILLNEED
    (void)posix_fadvise (fd, 0, 0, POSIX_FADV_WILLNEED);
#endif
    close (fd);
}


DatabaseData *
init_db (ConfigData *config_data)
{
    if (!config_data || !config_data->db_path) {
        g_critical ("Invalid configuration data");
        return NULL;
    }

    DatabaseData *db_data = g_try_new0 (DatabaseData, 1);
    if (!db_data) {
        g_critical ("Failed to allocate memory for DatabaseData");
        return NULL;
    }

    int rc = mdb_env_create (&db_data->env);
    if (rc != 0) {
        g_critical ("Error in mdb_env_create: %s", mdb_strerror (rc));
        g_free (db_data);
        return NULL;
    }

    rc = mdb_env_set_mapsize (db_data->env, config_data->db_size_bytes);
    if (rc != 0) {
        g_critical ("Error in mdb_env_set_mapsize: %s", mdb_strerror (rc));
        goto fail_env;
    }

    unsigned int env_flags = 0;
    if (config_data->db_writemap)   env_flags |= MDB_WRITEMAP;
    if (config_data->db_mapasync)   env_flags |= MDB_MAPASYNC;
    if (config_data->db_nosync)     env_flags |= MDB_NOSYNC;
    if (config_data->db_nometasync) env_flags |= MDB_NOMETASYNC;

    // A DB that is bigger than half the RAM cannot be cached: lookups are random, so
    // kernel readahead on the mmap just wastes disk bandwidth.
    guint64 db_file_size = get_db_file_size (config_data->db_path);
    gboolean db_fits_in_ram = db_file_size <= get_total_ram () / 4;
#ifdef MDB_NORDAHEAD
    if (db_file_size > get_total_ram () / 2) env_flags |= MDB_NORDAHEAD;
#endif

    rc = mdb_env_open (db_data->env, config_data->db_path, env_flags, 0644);
    if (rc != 0) {
        g_critical ("Error in mdb_env_open: %s", mdb_strerror (rc));
        goto fail_env;
    }

    db_data->max_key_size = (gsize)mdb_env_get_maxkeysize (db_data->env);

    if (config_data->mode != MODE_STATS && db_file_size > 0 && db_fits_in_ram) {
        prewarm_db_file (config_data->db_path);
    }

    MDB_txn *txn;
    rc = mdb_txn_begin (db_data->env, NULL, 0, &txn);
    if (rc != 0) {
        g_critical ("Error in mdb_txn_begin: %s", mdb_strerror (rc));
        goto fail_env;
    }

    rc = mdb_dbi_open (txn, NULL, 0, &db_data->dbi);
    if (rc != 0) {
        mdb_txn_abort (txn);
        g_critical ("Error in mdb_dbi_open: %s", mdb_strerror (rc));
        goto fail_env;
    }

    // Format check: refuse to touch a DB written by an incompatible version
    // (it would silently look empty/"missing" for every file).
    MDB_val meta_key = { .mv_size = DB_META_KEY_LEN, .mv_data = (void *)DB_META_KEY };
    MDB_val meta_val;
    rc = mdb_get (txn, db_data->dbi, &meta_key, &meta_val);
    if (rc == 0) {
        // Older records may be shorter (fields are only ever appended): missing ones read as 0
        DbMeta meta = { 0 };
        memcpy (&meta, meta_val.mv_data, MIN (meta_val.mv_size, sizeof (DbMeta)));
        if (meta.magic != DB_MAGIC || meta.format_version != DB_FORMAT_VERSION) {
            mdb_txn_abort (txn);
            g_critical ("Database %s has format version %u, this build needs version %u. "
                        "Remove the database directory and run 'add' again.",
                        config_data->db_path, meta.format_version, (guint)DB_FORMAT_VERSION);
            goto fail_env;
        }
    } else if (rc == MDB_NOTFOUND) {
        MDB_stat stat;
        rc = mdb_stat (txn, db_data->dbi, &stat);
        if (rc != 0) {
            mdb_txn_abort (txn);
            g_critical ("Error in mdb_stat: %s", mdb_strerror (rc));
            goto fail_env;
        }
        if (stat.ms_entries > 0) {
            mdb_txn_abort (txn);
            g_critical ("Database %s was created by an older, incompatible version (no format record). "
                        "Remove the database directory and run 'add' again.", config_data->db_path);
            goto fail_env;
        }
        DbMeta meta = { .magic = DB_MAGIC, .format_version = DB_FORMAT_VERSION, .last_run_us = g_get_real_time () };
        meta_val.mv_size = sizeof (DbMeta);
        meta_val.mv_data = &meta;
        rc = mdb_put (txn, db_data->dbi, &meta_key, &meta_val, 0);
        if (rc != 0) {
            mdb_txn_abort (txn);
            g_critical ("Error writing the database format record: %s", mdb_strerror (rc));
            goto fail_env;
        }
    } else {
        mdb_txn_abort (txn);
        g_critical ("Error reading the database format record: %s", mdb_strerror (rc));
        goto fail_env;
    }

    rc = mdb_txn_commit (txn);
    if (rc != 0) {
        g_critical ("Error in mdb_txn_commit: %s", mdb_strerror (rc));
        goto fail_env;
    }

    return db_data;

fail_env:
    mdb_env_close (db_data->env);
    g_free (db_data);
    return NULL;
}


gboolean
db_meta_load (DatabaseData *db_data,
              DbMeta       *meta)
{
    MDB_txn *txn;
    int rc = mdb_txn_begin (db_data->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0) {
        g_critical ("Error starting read transaction: %s", mdb_strerror (rc));
        return FALSE;
    }

    MDB_val key = { .mv_size = DB_META_KEY_LEN, .mv_data = (void *)DB_META_KEY };
    MDB_val val;
    rc = mdb_get (txn, db_data->dbi, &key, &val);
    if (rc == 0) {
        memset (meta, 0, sizeof (DbMeta));
        memcpy (meta, val.mv_data, MIN (val.mv_size, sizeof (DbMeta)));
    } else {
        g_critical ("Error reading the database metadata: %s", mdb_strerror (rc));
    }
    mdb_txn_abort (txn);
    return rc == 0;
}


gboolean
db_meta_store (DatabaseData *db_data,
               const DbMeta *meta)
{
    MDB_txn *txn;
    int rc = mdb_txn_begin (db_data->env, NULL, 0, &txn);
    if (rc != 0) {
        g_critical ("Error starting write transaction: %s", mdb_strerror (rc));
        return FALSE;
    }

    MDB_val key = { .mv_size = DB_META_KEY_LEN, .mv_data = (void *)DB_META_KEY };
    MDB_val val = { .mv_size = sizeof (DbMeta), .mv_data = (void *)meta };
    rc = mdb_put (txn, db_data->dbi, &key, &val, 0);
    if (rc != 0) {
        g_critical ("Error writing the database metadata: %s", mdb_strerror (rc));
        mdb_txn_abort (txn);
        return FALSE;
    }

    rc = mdb_txn_commit (txn);
    if (rc != 0) {
        g_critical ("Error committing the database metadata: %s", mdb_strerror (rc));
        return FALSE;
    }
    return TRUE;
}


gboolean
print_db_stats (DatabaseData *db_data)
{
    if (!db_data || !db_data->env) {
        g_critical ("Database not initialized.");
        return FALSE;
    }

    MDB_envinfo env_info;
    int rc = mdb_env_info (db_data->env, &env_info);
    if (rc != 0) {
        g_critical ("Error reading env info: %s", mdb_strerror (rc));
        return FALSE;
    }

    MDB_txn *txn;
    rc = mdb_txn_begin (db_data->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0) {
        g_critical ("Error starting read transaction: %s", mdb_strerror (rc));
        return FALSE;
    }

    MDB_stat stat;
    rc = mdb_stat (txn, db_data->dbi, &stat);
    mdb_txn_abort (txn);
    if (rc != 0) {
        g_critical ("Error reading DB stats: %s", mdb_strerror (rc));
        return FALSE;
    }

    // The format record is bookkeeping, not a file entry
    size_t files = (stat.ms_entries > 0) ? stat.ms_entries - 1 : 0;

    g_print ("=== Database Statistics ===\n");
    g_print ("Format version:     %u\n", (guint)DB_FORMAT_VERSION);
    g_print ("Number of entries:  %zu\n", files);
    g_print ("Page size:          %u bytes\n", stat.ms_psize);
    g_print ("B-tree depth:       %u\n", stat.ms_depth);
    g_print ("Branch pages:       %zu\n", stat.ms_branch_pages);
    g_print ("Leaf pages:         %zu\n", stat.ms_leaf_pages);
    g_print ("Overflow pages:     %zu\n", stat.ms_overflow_pages);
    g_print ("Map size:           %zu bytes (%.1f MB)\n", env_info.me_mapsize, env_info.me_mapsize / (1024.0 * 1024.0));
    return TRUE;
}
