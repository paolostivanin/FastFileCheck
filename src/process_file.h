#pragma once

#include <glib.h>
#include "queue.h"

void process_file                 (const gchar  *file_path,
                                   ConsumerData *consumer_data);

void handle_missing_files_from_fs (DatabaseData *db_data,
                                   SummaryData  *summary_data,
                                   gboolean      delete_file_from_db);