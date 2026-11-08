/* zg-io.h - Unix socket 帧读写（4 字节大端长度 + 载荷）
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_IO_H
#define ZG_IO_H

#include <glib.h>

G_BEGIN_DECLS

#define ZG_IO_ERROR (zg_io_error_quark())
GQuark zg_io_error_quark(void);

typedef enum
{
    ZG_IO_ERROR_CLOSED,
    ZG_IO_ERROR_PROTOCOL,
    ZG_IO_ERROR_TOO_LARGE,
    ZG_IO_ERROR_SYSTEM,
} ZgIoError;

gboolean zg_io_write_frame(int fd,
                           const char *payload,
                           gsize length,
                           GError **error);

char *zg_io_read_frame(int fd,
                       gsize *length_out,
                       GError **error);

gboolean zg_io_set_timeouts(int fd,
                            guint receive_seconds,
                            guint send_seconds);

G_END_DECLS

#endif /* ZG_IO_H */
