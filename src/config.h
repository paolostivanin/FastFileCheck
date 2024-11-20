#pragma once

#include <lmdb.h>
#include <glib.h>

#define DEFAULT_CONFIG_PATH         "/etc/ffc.conf"
#define DEFAULT_DB_PATH             "/var/lib/ffc/ffc.db"
#define DEFAULT_LOG_PATH            "/var/log/ffc/ffc.log"
#define DEFAULT_DB_SIZE_IN_MB       15
#define DEFAULT_RAM_USAGE_PERCENT   70

typedef enum {
    MODE_ADD = 1,
    MODE_CHECK = 2,
    MODE_UPDATE = 3
} Mode;

typedef struct config_t {
    guint threads_count;
    guint64 usable_ram;
    guint db_size_bytes;
    gchar *db_path;
    gchar *log_path;

    Mode mode;
} ConfigData;

ConfigData *load_config (const char *config_path);

void free_config (ConfigData *config);