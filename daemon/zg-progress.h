#pragma once
#include <glib.h>
G_BEGIN_DECLS
void zg_progress_begin(int fd);
void zg_progress_report(guint64 bytes);
void zg_progress_end(void);
int zg_progress_fd(void);
G_END_DECLS
