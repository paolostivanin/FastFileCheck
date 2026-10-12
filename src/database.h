#pragma once

#include <lmdb.h>
#include <glib.h>
#include "config.h"

// On-disk format version. Bump it whenever the key or value layout changes.
//   1: (never recorded) keys were NUL-terminated paths, values embedded a heap pointer
//   2: keys are raw paths, values are FileEntryData (see process_file.c)
#define DB_FORMAT_VERSION   2
#define DB_MAGIC            0x31434646u  // "FFC1"

// Reserved key that stores DbMeta. Real keys are file paths and never start with a NUL byte.
#define DB_META_KEY         "\0ffc:meta"
#define DB_META_KEY_LEN     (sizeof (DB_META_KEY) - 1)

typedef struct db_meta_t {
    guint32 magic;
    guint32 format_version;
    guint64 scrub_cursor;   // where the next scrub slice starts on the 64-bit path-hash ring
    gint64  last_run_us;    // wall clock (us since the epoch) of the last completed run
} DbMeta;

typedef struct database_t {
    MDB_env *env;
    MDB_dbi dbi;
    gsize max_key_size;     // LMDB rejects longer keys (511 bytes by default): such paths cannot be tracked
} DatabaseData;

DatabaseData *init_db        (ConfigData   *config_data);

void free_db                 (DatabaseData *db_data);

gboolean print_db_stats      (DatabaseData *db_data);

gboolean db_meta_load        (DatabaseData *db_data,
                              DbMeta       *meta);

gboolean db_meta_store       (DatabaseData *db_data,
                              const DbMeta *meta);

// TRUE for the reserved metadata key (and anything else that starts with a NUL byte)
gboolean db_is_meta_key      (const MDB_val *key);
