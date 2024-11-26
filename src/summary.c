#include <glib.h>
#include "summary.h"


static const gchar *
change_type_to_string (ChangeType type)
{
    switch (type) {
        case CHANGE_HASH:   return "Hash mismatch";
        case CHANGE_INODE:  return "Inode changed";
        case CHANGE_LINKS:  return "Link count changed";
        case CHANGE_BLOCKS: return "Block count changed";
        default:            return "Unknown change";
    }
}


SummaryData *
summary_new (void)
{
    SummaryData *summary_data = g_try_new0 (SummaryData, 1);
    if (!summary_data) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Failed to allocate memory for SummaryData");
        return NULL;
    }
    summary_data->changed_files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_array_unref);
    return summary_data;
}


void
record_change (SummaryData *summary_data,
               const char  *filepath,
               ChangeType   change)
{
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
    }
}


void
print_summary (SummaryData *summary_data)
{
    g_print ("\n=== Summary ===\n");
    g_print ("Total files processed: %u\n", summary_data->total_files_processed);
    g_print ("Files with changes: %u\n", summary_data->files_with_changes);
    g_print ("\nChanges breakdown:\n");
    g_print ("- Hash mismatches: %u\n", summary_data->hash_mismatches);
    g_print ("- Inode changes: %u\n", summary_data->inode_changes);
    g_print ("- Link count changes: %u\n", summary_data->link_changes);
    g_print ("- Block count changes: %u\n", summary_data->block_changes);

    if (summary_data->files_with_changes > 0) {
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
    }
}


void
free_summary (SummaryData *summary_data)
{
    if (summary_data) {
        g_hash_table_destroy (summary_data->changed_files);
        g_free (summary_data);
    }
}