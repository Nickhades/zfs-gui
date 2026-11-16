/* zg-log.h - 守护进程日志（前台输出 / syslog 双模式）
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_LOG_H
#define ZG_LOG_H

#include <glib.h>

G_BEGIN_DECLS

void zg_log_open(gboolean foreground,
                 const gchar *ident);
void zg_log_close(void);
void zg_log_info(const gchar *format, ...) G_GNUC_PRINTF(1, 2);
void zg_log_warning(const gchar *format, ...) G_GNUC_PRINTF(1, 2);
void zg_log_error(const gchar *format, ...) G_GNUC_PRINTF(1, 2);

G_END_DECLS

#endif /* ZG_LOG_H */
