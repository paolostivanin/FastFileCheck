#pragma once

#include "queue.h"

void process_directory (const gchar   *dir_path,
                        guint          max_depth,
                        FileQueueData *file_queue_data);