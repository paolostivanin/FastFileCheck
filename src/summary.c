#include <glib.h>
#include <stdio.h>
#include <string.h>
#include "summary.h"

#include "config.h"


static const gchar *
change_type_to_string (ChangeType type)
{
    switch (type) {
        case CHANGE_HASH:           return "Hash mismatch";
        case CHANGE_INODE:          return "Inode changed";
        case CHANGE_LINKS:          return "Link count changed";
        case CHANGE_BLOCKS:         return "Block count changed";
        case CHANGE_MISSING_IN_DB:  return "File is missing in the database";
        case CHANGE_MISSING_IN_FS:  return "File is missing from the file system";
        default:                    return "Unknown change";
    }
}


static const gchar *
change_type_to_json_key (ChangeType type)
{
    switch (type) {
        case CHANGE_HASH:           return "hash_mismatch";
        case CHANGE_INODE:          return "inode_changed";
        case CHANGE_LINKS:          return "link_count_changed";
        case CHANGE_BLOCKS:         return "block_count_changed";
        case CHANGE_MISSING_IN_DB:  return "missing_in_db";
        case CHANGE_MISSING_IN_FS:  return "missing_in_fs";
        default:                    return "unknown";
    }
}


SummaryData *
summary_new (void)
{
    SummaryData *summary_data = g_try_new0 (SummaryData, 1);
    if (!summary_data) {
        g_critical ("Failed to allocate memory for SummaryData");
        return NULL;
    }
    g_mutex_init (&summary_data->mutex);
    summary_data->changed_files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_array_unref);
    return summary_data;
}


void
record_change (SummaryData *summary_data,
               const char  *filepath,
               ChangeType   change)
{
    g_mutex_lock (&summary_data->mutex);

    GArray *changes = g_hash_table_lookup (summary_data->changed_files, filepath);
    if (!changes) {
        changes = g_array_new( FALSE, FALSE, sizeof(ChangeType));
        g_hash_table_insert (summary_data->changed_files, g_strdup (filepath), changes);
        summary_data->files_with_changes++;
    }
    g_array_append_val (changes, change);

    switch (change) {
        case CHANGE_HASH: summary_data->hash_mismatches++; break;
        case CHANGE_INODE: summary_data->inode_changes++; break;
        case CHANGE_LINKS: summary_data->link_changes++; break;
        case CHANGE_BLOCKS: summary_data->block_changes++; break;
        case CHANGE_MISSING_IN_DB: summary_data->missing_files_in_db++; break;
        case CHANGE_MISSING_IN_FS: summary_data->missing_files_in_fs++; break;
    }

    g_mutex_unlock (&summary_data->mutex);
}


void
summary_increment_processed (SummaryData *summary_data,
                             guint        delta)
{
    g_atomic_int_add ((volatile gint*)&summary_data->total_files_processed, (gint)delta);
}


guint
summary_get_processed (SummaryData *summary_data)
{
    return (guint)g_atomic_int_get ((volatile gint*)&summary_data->total_files_processed);
}


void
summary_record_error (SummaryData *summary_data)
{
    g_atomic_int_inc ((volatile gint*)&summary_data->errors);
}


guint
summary_get_errors (SummaryData *summary_data)
{
    return (guint)g_atomic_int_get ((volatile gint*)&summary_data->errors);
}


void
print_summary (SummaryData *summary_data, Mode mode)
{
    g_print ("\n=== Summary ===\n");
    g_print ("Total files processed: %u\n", summary_data->total_files_processed);
    if (summary_data->errors > 0) {
        g_print ("Errors: %u (see the messages above or the log file; the exit code will be 2)\n", summary_data->errors);
    }

    if (mode == MODE_CHECK) {
        if (summary_data->files_with_changes > 0) {
            g_print ("Files with changes: %u\n", summary_data->files_with_changes);
            g_print ("\nChanges breakdown:\n");
            g_print ("- Hash mismatches: %u\n", summary_data->hash_mismatches);
            g_print ("- Inode changes: %u\n", summary_data->inode_changes);
            g_print ("- Link count changes: %u\n", summary_data->link_changes);
            g_print ("- Block count changes: %u\n", summary_data->block_changes);
            g_print ("- Missing files in the database (e.g. renamed, created): %u\n", summary_data->missing_files_in_db);
            g_print ("- Missing files from the file system (e.g. deleted, moved): %u\n", summary_data->missing_files_in_fs);
            g_print ("\nAffected files:\n");
            GHashTableIter iter;
            gpointer key, value;
            g_hash_table_iter_init (&iter, summary_data->changed_files);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                const char *filepath = key;
                GArray *changes = value;
                g_print ("%s:\n", filepath);
                for (guint i = 0; i < changes->len; i++) {
                    ChangeType change = g_array_index (changes, ChangeType, i);
                    g_print("  - %s\n", change_type_to_string (change));
                }
            }
            g_print ("\n");
        } else {
            g_print ("No changes detected.\n");
        }
    } else {
        if (summary_data->errors > 0) {
            g_print ("Database %s finished WITH ERRORS.\n", mode == MODE_ADD ? "addition" : "update");
        } else {
            g_print ("Database %s completed successfully.\n", mode == MODE_ADD ? "addition" : "update");
        }
    }
}


static void
json_escape_string (const gchar *input, FILE *out)
{
    fputc ('"', out);
    for (const gchar *p = input; *p; p++) {
        switch (*p) {
            case '"':  fputs ("\\\"", out); break;
            case '\\': fputs ("\\\\", out); break;
            case '\b': fputs ("\\b", out); break;
            case '\f': fputs ("\\f", out); break;
            case '\n': fputs ("\\n", out); break;
            case '\r': fputs ("\\r", out); break;
            case '\t': fputs ("\\t", out); break;
            default:
                if ((unsigned char)*p < 0x20) {
                    fprintf (out, "\\u%04x", (unsigned char)*p);
                } else {
                    fputc (*p, out);
                }
        }
    }
    fputc ('"', out);
}


void
print_summary_json (SummaryData *summary_data,
                    Mode         mode,
                    gdouble      elapsed_sec)
{
    const gchar *mode_str;
    switch (mode) {
        case MODE_ADD:    mode_str = "add"; break;
        case MODE_CHECK:  mode_str = "check"; break;
        case MODE_UPDATE: mode_str = "update"; break;
        default:          mode_str = "unknown"; break;
    }

    fprintf (stdout, "{\n");
    fprintf (stdout, "  \"mode\": \"%s\",\n", mode_str);
    fprintf (stdout, "  \"total_processed\": %u,\n", summary_data->total_files_processed);
    fprintf (stdout, "  \"files_with_changes\": %u,\n", summary_data->files_with_changes);
    fprintf (stdout, "  \"errors\": %u,\n", summary_data->errors);
    fprintf (stdout, "  \"duration_seconds\": %.2f", elapsed_sec);

    if (summary_data->files_with_changes > 0) {
        fprintf (stdout, ",\n  \"changes\": {\n");
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init (&iter, summary_data->changed_files);
        gboolean first_file = TRUE;
        while (g_hash_table_iter_next (&iter, &key, &value)) {
            if (!first_file) fprintf (stdout, ",\n");
            first_file = FALSE;
            fprintf (stdout, "    ");
            json_escape_string ((const gchar *)key, stdout);
            fprintf (stdout, ": [");
            GArray *changes = value;
            for (guint i = 0; i < changes->len; i++) {
                if (i > 0) fprintf (stdout, ", ");
                ChangeType change = g_array_index (changes, ChangeType, i);
                fprintf (stdout, "\"%s\"", change_type_to_json_key (change));
            }
            fprintf (stdout, "]");
        }
        fprintf (stdout, "\n  }");
    }

    fprintf (stdout, "\n}\n");
    fflush (stdout);
}


void
free_summary (SummaryData *summary_data)
{
    if (summary_data) {
        g_hash_table_destroy (summary_data->changed_files);
        g_mutex_clear (&summary_data->mutex);
        g_free (summary_data);
    }
}
