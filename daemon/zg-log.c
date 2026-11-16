/* zg-log.c - 守护进程日志实现
 *
 * 前台运行时输出到 stderr（带时间戳，便于调试）；
 * 后台运行时写入 syslog，facility 为 LOG_AUTHPRIV。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

static gboolean zg_log_foreground = TRUE;

void zg_log_open(gboolean foreground,
                 const gchar *ident)
{
    zg_log_foreground = foreground;

    if (!foreground)
        openlog(ident != NULL ? ident : "zfs-guid", LOG_PID, LOG_AUTHPRIV);
}

void zg_log_close(void)
{
    if (!zg_log_foreground)
        closelog();
}

static void G_GNUC_PRINTF(2, 0)
        log_write(int priority,
                  const gchar *format,
                  va_list args)
{
    if (zg_log_foreground)
    {
        gchar *message;
        struct tm tm;
        time_t now;
        gchar stamp[32];

        message = g_strdup_vprintf(format, args);
        now = time(NULL);

        if (localtime_r(&now, &tm) != NULL &&
            strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm) > 0)
            g_printerr("[%s] %s\n", stamp, message);
        else
            g_printerr("%s\n", message);

        g_free(message);
    }
    else
    {
        vsyslog(priority, format, args);
    }
}

void zg_log_info(const gchar *format, ...)
{
    va_list args;

    va_start(args, format);
    log_write(LOG_INFO, format, args);
    va_end(args);
}

void zg_log_warning(const gchar *format, ...)
{
    va_list args;

    va_start(args, format);
    log_write(LOG_WARNING, format, args);
    va_end(args);
}

void zg_log_error(const gchar *format, ...)
{
    va_list args;

    va_start(args, format);
    log_write(LOG_ERR, format, args);
    va_end(args);
}
