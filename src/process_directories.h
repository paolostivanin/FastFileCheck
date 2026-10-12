#pragma once

#include "queue.h"

// `empty_roots` (element type gchar *, owned by the caller) receives every configured directory
// that could not be opened or contained no regular file at all: a missing mount looks exactly
// like that, and the update pass must not purge the database entries below such a directory.

void process_directories (gchar         **dirs,
                          guint           max_depth,
                          FileQueueData  *file_queue_data,
                          ConfigData     *config_data,
                          SummaryData    *summary_data,
                          GPtrArray      *empty_roots);