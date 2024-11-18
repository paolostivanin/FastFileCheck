#pragma once

#include <lmdb.h>

#define DEFAULT_CONFIG_PATH         "/etc/ffc.conf"
#define DEFAULT_DB_PATH             "/var/lib/ffc/ffc.db"
#define DEFAULT_LOG_PATH            "/var/log/ffc/ffc.log"
#define DEFAULT_DB_SIZE_IN_BYTES    (15 * 1024 * 1024)
#define DEFAULT_RAM_USAGE_PERCENT   70

typedef enum {
    MODE_ADD = 1,
    MODE_CHECK = 2
} Mode;

typedef struct config_t {
    unsigned int threads;
    unsigned long usable_ram;
    unsigned int db_size_bytes;
    char *db_path;
    char *log_path;

    Mode mode;
} ConfigData;

ConfigData *load_config (const char *config_path);

void free_config (ConfigData *config);