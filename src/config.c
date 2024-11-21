#include <glib.h>
#include <unistd.h>
#include "config.h"


static size_t
get_free_memory (void)
{
    long pages = sysconf (_SC_AVPHYS_PAGES);
    long pagesize = sysconf (_SC_PAGE_SIZE);
    if (pages == -1 || pagesize == -1) {
        g_print ("Warning: Could not determine available memory, using 1GB default\n");
        return 1024 * 1024 * 1024; // Default to 1GB
    }

    return (size_t)(pages * pagesize);
}


ConfigData *
load_config (const char *config_path)
{
    ConfigData *config_data = g_new0 (ConfigData, 1);

    // If no config was specified, we use the default one
    config_path = (config_path == NULL) ? DEFAULT_CONFIG_PATH : config_path;

    GKeyFile *key_file = g_key_file_new ();
    if (!g_key_file_load_from_file (key_file, config_path, G_KEY_FILE_NONE, NULL)) {
        g_print ("Failed to load config file: %s", config_path);
        return NULL;
    }

    GError *config_error = NULL;
    gint t_val = g_key_file_get_integer (key_file, "settings", "threads_count", &config_error);
    guint usable_threads = sysconf(_SC_NPROCESSORS_ONLN) - 1;
    if ((config_error != NULL && config_error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND) || t_val < 0 || t_val > (gint)(usable_threads + 1)) {
        g_print ("Invalid threads_count value: %d. Using the default value instead.\n", t_val);
    }
    // use 'usable_threads' when config is missing/wrong or when t_val == 0
    config_data->threads_count = usable_threads;
    if (t_val > 0 && t_val <= (gint)usable_threads + 1) {
        config_data->threads_count = (guint)t_val;
    }
    // Reserve one thread for the queue-consumer thread
    config_data->threads_count = (config_data->threads_count > 2) ? config_data->threads_count-1 : config_data->threads_count;
    g_clear_error (&config_error);

    t_val = g_key_file_get_integer (key_file, "settings", "ram_usage_percent", &config_error);
    if ((config_error != NULL && config_error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND) || t_val < 10 || t_val > 90) {
        g_print ("Invalid ram_usage_percent value: %u. Using the default value instead.\n", t_val);
        t_val = DEFAULT_RAM_USAGE_PERCENT;
    }
    config_data->usable_ram = get_free_memory () * t_val / 100;
    config_data->max_ram_per_thread = config_data->usable_ram / config_data->threads_count;
    g_clear_error (&config_error);

    t_val = g_key_file_get_integer (key_file, "database", "db_size_mb", NULL);
    if (t_val < 5) {
        g_print ("Invalid db_size_mb value: %u. Using the default value instead.\n", t_val);
        t_val = DEFAULT_DB_SIZE_IN_MB;
    }
    config_data->db_size_bytes = t_val * 1024 * 1024;

    gchar *t_str;
    t_str = g_key_file_get_string (key_file, "database", "db_path", NULL);
    config_data->db_path = (t_str != NULL) ? g_strdup (t_str) : g_strdup (DEFAULT_DB_PATH);
    g_free (t_str);

    t_str = g_key_file_get_string (key_file, "logging", "log_path", NULL);
    config_data->log_path = (t_str != NULL) ? g_strdup (t_str) : g_strdup (DEFAULT_LOG_PATH);
    g_free (t_str);

    t_val = g_key_file_get_integer (key_file, "scanning", "max_recursion_depth", &config_error);
    if ((config_error != NULL && config_error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND) || t_val < 0 || t_val > 64) {
        g_print ("Invalid max_recursion_depth value: %u. Using the default value instead.\n", t_val);
        t_val = DEFAULT_MAX_RECURSION_DEPTH;
    }
    config_data->max_recursion_depth = t_val;
    g_clear_error (&config_error);

    g_key_file_free (key_file);

    return config_data;
}


void
free_config (ConfigData *config)
{
    g_free (config->db_path);
    g_free (config->log_path);
    g_free (config);
}