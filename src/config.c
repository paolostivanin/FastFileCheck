#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include "config.h"


static size_t
get_free_memory (void)
{
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long pagesize = sysconf(_SC_PAGE_SIZE);

    if (pages == -1 || pagesize == -1) {
        g_log(NULL, G_LOG_LEVEL_INFO, "Warning: sysconf failed, using default memory value");
        return (1024 * 1024 * 1024);
    }

    // Check for potential overflow before multiplying
    if ((size_t)pages > G_MAXSIZE / (size_t)pagesize) {
        return G_MAXSIZE;
    }

    return (size_t)pages * (size_t)pagesize;
}


static guint
get_usable_threads (void)
{
    long num_processors = sysconf (_SC_NPROCESSORS_ONLN);
    if (num_processors <= 0) {
        g_print("Warning: Could not determine number of processors, using 1\n");
        return 1;
    }

    return (guint)(num_processors - 1);
}


static gboolean
validate_dir_path (const gchar *path)
{
    if (!g_file_test (path, G_FILE_TEST_IS_DIR)) {
        if (g_mkdir_with_parents (path, 0755) != 0) {
            g_log (NULL, G_LOG_LEVEL_ERROR, "Unable to create the directory: %s\n", path);
            return FALSE;
        }
    }

    if (g_access (path, W_OK) != 0) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Directory is not writable: %s\n", path);
        return FALSE;
    }

    return TRUE;
}


ConfigData *
load_config (const char *config_path)
{
    ConfigData *config_data = g_try_new0 (ConfigData, 1);
    if (!config_data) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Failed to allocate memory for config_data");
        return NULL;
    }

    // If no config was specified, we use the default one
    config_path = (config_path == NULL) ? DEFAULT_CONFIG_PATH : config_path;

    GKeyFile *key_file = g_key_file_new ();
    if (!g_key_file_load_from_file (key_file, config_path, G_KEY_FILE_NONE, NULL)) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Failed to load config file: %s", config_path);
        g_free (config_data);
        return NULL;
    }

    GError *config_error = NULL;
    gint t_val = g_key_file_get_integer (key_file, "settings", "threads_count", &config_error);
    guint usable_threads = get_usable_threads ();
    if ((config_error != NULL && config_error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND) || t_val < 0 || t_val > (gint)(usable_threads + 1)) {
        g_log (NULL, G_LOG_LEVEL_WARNING, "Invalid threads_count value: %d. Using the default value instead.", t_val);
        g_clear_error (&config_error);
    }
    // use 'usable_threads' when config is missing/wrong or when t_val == 0
    config_data->threads_count = usable_threads;
    if (t_val > 0 && t_val <= (gint)usable_threads + 1) {
        config_data->threads_count = (guint)t_val;
    }
    // Reserve one thread for the queue-consumer thread
    config_data->threads_count = (config_data->threads_count > 2) ? config_data->threads_count-1 : config_data->threads_count;

    t_val = g_key_file_get_integer (key_file, "settings", "ram_usage_percent", &config_error);
    if ((config_error != NULL && config_error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND) || t_val < 10 || t_val > 90) {
        g_log (NULL, G_LOG_LEVEL_WARNING, "Invalid ram_usage_percent value: %u. Using the default value instead.", t_val);
        t_val = DEFAULT_RAM_USAGE_PERCENT;
        g_clear_error (&config_error);
    }
    config_data->usable_ram = get_free_memory () * t_val / 100;
    config_data->max_ram_per_thread = config_data->usable_ram / config_data->threads_count;

    t_val = g_key_file_get_integer (key_file, "database", "db_size_mb", NULL);
    if (t_val < 5) {
        g_log (NULL, G_LOG_LEVEL_WARNING, "Invalid db_size_mb value: %u. Using the default value instead.", t_val);
        t_val = DEFAULT_DB_SIZE_IN_MB;
    }
    config_data->db_size_bytes = t_val * 1024 * 1024;

    gchar *t_str = g_key_file_get_string (key_file, "database", "db_path", NULL);
    config_data->db_path = (t_str != NULL) ? g_strdup (t_str) : g_strdup (DEFAULT_DB_PATH);
    g_free (t_str);
    if (!validate_dir_path (config_data->db_path)) {
        // If the database directory cannot be created or is not writable, we must fail and exit
        return NULL;
    }

    gboolean t_val_bool = g_key_file_get_boolean (key_file, "logging", "log_to_file_enabled", &config_error);
    if (!t_val_bool && (config_error != NULL && (config_error->code == G_KEY_FILE_ERROR_INVALID_VALUE || config_error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND))) {
        g_log (NULL, G_LOG_LEVEL_WARNING, "Couldn't get the value for log_to_file_enabled. Setting it to the default one.");
        t_val_bool = DEFAULT_LOG_TO_FILE;
        g_clear_error (&config_error);
    }
    config_data->logging_enabled = t_val_bool;

    if (config_data->logging_enabled) {
        t_str = g_key_file_get_string(key_file, "logging", "log_path", NULL);
        config_data->log_path = (t_str != NULL)
                                    ? g_strdup (t_str)
                                    : g_strdup (DEFAULT_LOG_PATH);
        g_free (t_str);

        if (!validate_dir_path (config_data->log_path)) {
            g_free (config_data->log_path);
            config_data->log_path = NULL;
            config_data->logging_enabled = FALSE;
        } else {
            char *dir_path = config_data->log_path;
            config_data->log_path = g_build_filename (dir_path, "/ffc.log", NULL);
            g_free (dir_path);
        }
    }

    t_val = g_key_file_get_integer (key_file, "scanning", "max_recursion_depth", &config_error);
    if ((config_error != NULL && config_error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND) || t_val < 0 || t_val > 64) {
        g_log (NULL, G_LOG_LEVEL_WARNING, "Invalid max_recursion_depth value: %u. Using the default value instead.", t_val);
        t_val = DEFAULT_MAX_RECURSION_DEPTH;
        g_clear_error (&config_error);
    }
    config_data->max_recursion_depth = t_val;

    t_val_bool = g_key_file_get_boolean (key_file, "scanning", "exclude_hidden", &config_error);
    if (!t_val_bool && (config_error != NULL && (config_error->code == G_KEY_FILE_ERROR_INVALID_VALUE || config_error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND))) {
        g_log (NULL, G_LOG_LEVEL_WARNING,"Couldn't get the value for exclude_hidden. Setting it to the default one.");
        t_val_bool = DEFAULT_EXCLUDE_HIDDEN;
        g_clear_error (&config_error);
    }
    config_data->exclude_hidden = t_val_bool;

    t_str = g_key_file_get_string (key_file, "scanning", "directories", NULL);
    if (!t_str || g_utf8_strlen (t_str, -1) < 1) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Couldn't get the value for which directories to scan, exiting.");
        return NULL;
    }
    config_data->directories = g_strdup (t_str);
    g_free (t_str);

    t_str = g_key_file_get_string (key_file, "scanning", "exclude_directories", NULL);
    if (!t_str || g_utf8_strlen (t_str, -1) < 1) {
        g_log (NULL, G_LOG_LEVEL_INFO, "No directories configured to be excluded.");
    }
    if (g_utf8_strlen (t_str, -1) > 0) config_data->exclude_directories = g_strdup (t_str);
    g_free (t_str);

    t_str = g_key_file_get_string (key_file, "scanning", "exclude_extensions", NULL);
    if (!t_str || g_utf8_strlen (t_str, -1) < 1) {
        g_log (NULL, G_LOG_LEVEL_INFO, "No file extensions configured to be excluded.");
    }
    if (g_utf8_strlen (t_str, -1) > 0) config_data->exclude_extensions = g_strdup (t_str);
    g_free (t_str);

    g_key_file_free (key_file);

    return config_data;
}


void
free_config (ConfigData *config)
{
    g_free (config->db_path);
    g_free (config->log_path);
    g_free (config->directories);
    g_free (config->exclude_directories);
    g_free (config->exclude_extensions);
    g_free (config);
}
