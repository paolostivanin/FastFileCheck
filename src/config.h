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

typedef enum mode_t {
    MODE_ADD = 1,
    MODE_CHECK = 2,
    MODE_UPDATE = 3
} Mode;

typedef struct config_t {
    guint threads_count;
    guint64 usable_ram;
    guint64 max_ram_per_thread;

    gchar *db_path;
    guint db_size_bytes;
    // LMDB performance/durability toggles
    gboolean db_nosync;      // Reduce fsync frequency (unsafe on power loss)
    gboolean db_nometasync;  // Skip metadata syncs (unsafe on power loss)
    gboolean db_mapasync;    // Allow OS to flush asynchronously (unsafe on crash)
    gboolean db_writemap;    // Use writeable memory map (faster, but riskier with multiple processes)

    gboolean logging_enabled;
    gchar *log_path;

    guint max_recursion_depth;
    gchar *directories;
    gboolean exclude_hidden;
    gchar *exclude_directories;
    gchar *exclude_extensions;

    gboolean verbose; // enable verbose console output and debug logs

    Mode mode;
} ConfigData;

ConfigData *load_config (const char *config_path);

void free_config        (ConfigData *config);