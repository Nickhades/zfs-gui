#pragma once
#include <glib.h>
#include "zg-ops.h"
G_BEGIN_DECLS
ZgOpOutcome *zg_task_send_stream(int client_fd, int helper_fd,
                                 const gchar *snapshot, gint file_fd,
                                 guint32 req_id);
ZgOpOutcome *zg_task_receive_stream(int client_fd, int helper_fd,
                                    const gchar *target, gint file_fd,
                                    gboolean force, guint32 req_id);
G_END_DECLS
