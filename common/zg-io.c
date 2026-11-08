/* zg-io.c - Unix socket 帧读写实现
 *
 * 帧格式：4 字节大端无符号长度 N + N 字节 UTF-8 JSON 载荷。
 * 不使用文本分隔符，避免载荷中出现分隔符导致的截断与转义问题。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-io.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "zg-protocol.h"

GQuark
zg_io_error_quark(void)
{
    static GQuark quark = 0;

    if (quark == 0)
        quark = g_quark_from_static_string("zg-io-error");

    return quark;
}

gboolean
zg_io_set_timeouts(int fd,
                   guint receive_seconds,
                   guint send_seconds)
{
    struct timeval timeout;
    gboolean ok = TRUE;

    if (receive_seconds > 0)
    {
        timeout.tv_sec = (time_t)receive_seconds;
        timeout.tv_usec = 0;

        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
            ok = FALSE;
    }

    if (send_seconds > 0)
    {
        timeout.tv_sec = (time_t)send_seconds;
        timeout.tv_usec = 0;

        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
            ok = FALSE;
    }

    return ok;
}

static gboolean
write_all(int fd,
          const char *data,
          gsize length,
          GError **error)
{
    gsize offset = 0;

    while (offset < length)
    {
        const ssize_t written = write(fd, data + offset, length - offset);

        if (written < 0)
        {
            if (errno == EINTR)
                continue;

            g_set_error(error, ZG_IO_ERROR, ZG_IO_ERROR_SYSTEM,
                        "写入 socket 失败：%s", g_strerror(errno));
            return FALSE;
        }

        if (written == 0)
        {
            g_set_error_literal(error, ZG_IO_ERROR, ZG_IO_ERROR_CLOSED,
                                "写入 socket 时连接已关闭");
            return FALSE;
        }

        offset += (gsize)written;
    }

    return TRUE;
}

static gboolean
read_all(int fd,
         char *data,
         gsize length,
         GError **error)
{
    gsize offset = 0;

    while (offset < length)
    {
        const ssize_t received = read(fd, data + offset, length - offset);

        if (received < 0)
        {
            if (errno == EINTR)
                continue;

            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                g_set_error_literal(error, ZG_IO_ERROR, ZG_IO_ERROR_SYSTEM,
                                    "等待数据超时");
                return FALSE;
            }

            g_set_error(error, ZG_IO_ERROR, ZG_IO_ERROR_SYSTEM,
                        "读取 socket 失败：%s", g_strerror(errno));
            return FALSE;
        }

        if (received == 0)
        {
            g_set_error_literal(error, ZG_IO_ERROR, ZG_IO_ERROR_CLOSED,
                                "连接被对端关闭");
            return FALSE;
        }

        offset += (gsize)received;
    }

    return TRUE;
}

gboolean
zg_io_write_frame(int fd,
                  const char *payload,
                  gsize length,
                  GError **error)
{
    guchar header[4];

    if (length > ZG_MAX_FRAME)
    {
        g_set_error(error, ZG_IO_ERROR, ZG_IO_ERROR_TOO_LARGE,
                    "帧长度 %" G_GSIZE_FORMAT " 超过上限 %u",
                    length, ZG_MAX_FRAME);
        return FALSE;
    }

    header[0] = (guchar)((length >> 24) & 0xff);
    header[1] = (guchar)((length >> 16) & 0xff);
    header[2] = (guchar)((length >> 8) & 0xff);
    header[3] = (guchar)(length & 0xff);

    if (!write_all(fd, (const char *)header, sizeof(header), error))
        return FALSE;

    if (length == 0)
        return TRUE;

    return write_all(fd, payload, length, error);
}

char *
zg_io_read_frame(int fd,
                 gsize *length_out,
                 GError **error)
{
    guchar header[4];
    gsize length;
    char *payload;

    if (!read_all(fd, (char *)header, sizeof(header), error))
        return NULL;

    length = ((gsize)header[0] << 24) |
             ((gsize)header[1] << 16) |
             ((gsize)header[2] << 8) |
             ((gsize)header[3]);

    if (length > ZG_MAX_FRAME)
    {
        g_set_error(error, ZG_IO_ERROR, ZG_IO_ERROR_TOO_LARGE,
                    "对端声明的帧长度 %" G_GSIZE_FORMAT " 超过上限 %u",
                    length, ZG_MAX_FRAME);
        return NULL;
    }

    payload = g_malloc(length + 1);

    if (length > 0 && !read_all(fd, payload, length, error))
    {
        g_free(payload);
        return NULL;
    }

    payload[length] = '\0';

    if (length_out != NULL)
        *length_out = length;

    return payload;
}
