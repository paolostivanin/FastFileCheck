#pragma once

#include <glib.h>
#include "config.h"

typedef struct summary_data_t {
    GHashTable *changed_files;  // filepath -> array of change types
    GMutex mutex;               // protects changed_files and files_with_changes
    guint total_files_processed;
    guint files_with_changes;
    guint hash_mismatches;
    guint inode_changes;
    guint link_changes;
    guint block_changes;
    guint missing_files_in_db;
    guint missing_files_in_fs;
} SummaryData;

typedef enum change_type_t {
    CHANGE_HASH,
    CHANGE_INODE,
    CHANGE_LINKS,
    CHANGE_BLOCKS,
    CHANGE_MISSING_IN_DB,
    CHANGE_MISSING_IN_FS
} ChangeType;

SummaryData *summary_new   (void);

void          free_summary  (SummaryData *summary);

void          record_change (SummaryData *summary,
                             const gchar *filepath,
                             ChangeType   change);

void          summary_increment_processed (SummaryData *summary,
                                          guint        delta);

guint         summary_get_processed (SummaryData *summary);

void          print_summary (SummaryData *summary,
                             Mode         mode);