#pragma once

#include <glib.h>
#include "queue.h"
#include "seen_set.h"

#include "file_task.h"

void process_file                 (const FileTask *task,
                                   ConsumerData   *consumer_data);

// FALSE when process_file() will not need to read the file's content (see process_file.c)
gboolean process_file_needs_read  (const FileTask *task,
                                   ConsumerData   *consumer_data);

// open(O_RDONLY | O_NOATIME), retrying without O_NOATIME when that is not permitted
int open_noatime                  (const char *filepath);

// Reports (CHECK) or removes (UPDATE) database entries whose file no longer exists.
// `seen` is the set of paths the workers processed in this run; entries in it are known to
// exist and are not stat()ed again.
//
// `empty_roots` (see process_directories.h): in UPDATE mode the entries below those directories are
// kept and reported as an error, because an unmounted disk would otherwise wipe its whole history.
void handle_missing_files_from_fs (DatabaseData *db_data,
                                   SummaryData  *summary_data,
                                   SeenSet      *seen,
                                   const GPtrArray *empty_roots,
                                   gboolean      delete_file_from_db);
