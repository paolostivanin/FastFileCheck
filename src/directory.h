#pragma once

#include "queue.h"

void process_directories (gchar         **dirs,
                          guint           max_depth,
                          FileQueueData  *file_queue_data);