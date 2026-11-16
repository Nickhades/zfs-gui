/* zg-window.h - 主窗口
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_WINDOW_H
#define ZG_WINDOW_H

#include <gtk/gtk.h>

#include "zg-app.h"

G_BEGIN_DECLS

GtkWidget *zg_window_new(ZgApp *app);

G_END_DECLS

#endif /* ZG_WINDOW_H */
