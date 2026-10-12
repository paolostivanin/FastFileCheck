#pragma once

#include <lmdb.h>
#include <glib.h>

#define DEFAULT_CONFIG_PATH         "/etc/ffc.conf"
#define DEFAULT_DB_PATH             "/var/lib/ffc/ffc.db"
#define DEFAULT_LOG_PATH            "/var/log/ffc/ffc.log"
#define DEFAULT_DB_SIZE_IN_MB       15
#define DEFAULT_RAM_USAGE_PERCENT   70
#define DEFAULT_MAX_RECURSION_DEPTH 10
#define DEFAULT_LOG_TO_FILE         TRUE
#define DEFAULT_EXCLUDE_HIDDEN      TRUE
#define DEFAULT_DB_BATCH_SIZE       500
#define DEFAULT_SCRUB_DAYS          30

typedef enum mode_t {
    MODE_ADD = 1,
    MODE_CHECK = 2,
    MODE_UPDATE = 3,
    MODE_STATS = 4
} Mode;

typedef enum storage_kind_t {
    STORAGE_AUTO = 0,   // detect: HDD mode if any scanned directory is on a rotational device
    STORAGE_HDD,
    STORAGE_SSD
} StorageKind;

typedef struct config_t {
    guint threads_count;   // size of the hashing worker pool (after io_threads cap)
    guint64 usable_ram;

    gchar *db_path;
    guint64 db_size_bytes;
    // LMDB performance/durability toggles
    gboolean db_nosync;      // Reduce fsync frequency (unsafe on power loss)
    gboolean db_nometasync;  // Skip metadata syncs (unsafe on power loss)
    gboolean db_mapasync;    // Allow OS to flush asynchronously (unsafe on crash)
    gboolean db_writemap;    // Use writeable memory map (faster, but riskier with multiple processes)
    guint db_batch_size;     // Number of DB writes per transaction batch

    gboolean logging_enabled;
    gchar *log_path;

    guint max_recursion_depth;
    gchar *directories;
    gboolean exclude_hidden;
    gchar *exclude_directories;
    gchar *exclude_extensions;

    gboolean verbose;      // enable verbose console output and debug logs
    gboolean json_output;  // emit JSON summary instead of text
    gboolean quick_check;  // skip hash if mtime+size unchanged
    guint io_threads;      // I/O concurrency limit (0 = same as threads_count)

    StorageKind storage;   // [settings] storage = auto|hdd|ssd
    gboolean hdd_mode;     // resolved from `storage` + detection (set in main, not read from the file)
    guint prefetch_mb;     // HDD mode: look-ahead budget in MB (0 = auto)
    gboolean low_priority; // run with idle I/O priority
    guint scrub_days;      // quick_check: every file is re-hashed at least this often (0 = never)

    Mode mode;
} ConfigData;

ConfigData *load_config (const char *config_path);

void free_config        (ConfigData *config);
