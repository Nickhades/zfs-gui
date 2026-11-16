/* zg-frame.c - 非阻塞长度前缀帧缓冲
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-frame.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

ZgFrameBuffer *
zg_frame_buffer_new(gsize max_frame)
{
    ZgFrameBuffer *buffer = g_new0(ZgFrameBuffer, 1);

    buffer->data = g_byte_array_new();
    buffer->max_frame = max_frame > 0 ? max_frame : (1024 * 1024);

    return buffer;
}

void zg_frame_buffer_free(ZgFrameBuffer *buffer)
{
    if (buffer == NULL)
        return;

    g_byte_array_free(buffer->data, TRUE);
    g_free(buffer);
}

ZgFrameReadResult
zg_frame_read(ZgFrameBuffer *buffer,
              int fd,
              GError **error)
{
    guchar temp[8192];

    for (;;)
    {
        ssize_t n = read(fd, temp, sizeof(temp));

        if (n > 0)
        {
            g_byte_array_append(buffer->data, temp, (guint)n);
            continue;
        }

        if (n == 0)
            return ZG_FRAME_EOF;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return ZG_FRAME_AGAIN;

        if (errno == EINTR)
            continue;

        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "read 失败：%s", g_strerror(errno));
        return ZG_FRAME_ERROR;
    }
}

gint zg_frame_pop(ZgFrameBuffer *buffer,
                  gchar **out,
                  GError **error)
{
    guint32 length;
    gchar *text;

    *out = NULL;

    if (buffer->data->len < sizeof(length))
        return 0;

    memcpy(&length, buffer->data->data, sizeof(length));
    length = g_ntohl(length);

    if (length == 0 || length > buffer->max_frame)
    {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "帧长度非法：%u（上限 %" G_GSIZE_FORMAT "）",
                    length, buffer->max_frame);
        return -1;
    }

    if (buffer->data->len < sizeof(length) + length)
        return 0;

    text = g_malloc0((gsize)length + 1);
    memcpy(text, buffer->data->data + sizeof(length), length);
    g_byte_array_remove_range(buffer->data, 0, sizeof(length) + length);

    *out = text;
    return 1;
}

ZgOutBuffer *
zg_out_buffer_new(void)
{
    ZgOutBuffer *buffer = g_new0(ZgOutBuffer, 1);

    buffer->data = g_byte_array_new();
    return buffer;
}

void zg_out_buffer_free(ZgOutBuffer *buffer)
{
    if (buffer == NULL)
        return;

    g_byte_array_free(buffer->data, TRUE);
    g_free(buffer);
}

gboolean
zg_out_buffer_queue(ZgOutBuffer *buffer,
                    const gchar *text,
                    GError **error)
{
    if (text == NULL)
        return TRUE;

    if (buffer->data->len > 4 * 1024 * 1024)
    {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOSPC,
                    "发送缓冲区过大");
        return FALSE;
    }

    g_byte_array_append(buffer->data, (const guint8 *)text, (guint)strlen(text));
    return TRUE;
}

gboolean
zg_out_buffer_queue_frame(ZgOutBuffer *buffer,
                          const gchar *text,
                          GError **error)
{
    guint32 length = g_htonl((guint32)strlen(text));

    if (buffer->data->len > 4 * 1024 * 1024)
    {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOSPC,
                    "发送缓冲区过大");
        return FALSE;
    }

    g_byte_array_append(buffer->data, (const guint8 *)&length, sizeof(length));
    return zg_out_buffer_queue(buffer, text, error);
}

gboolean
zg_out_buffer_flush(ZgOutBuffer *buffer,
                    int fd,
                    GError **error)
{
    while (buffer->offset < buffer->data->len)
    {
        ssize_t n = write(fd, buffer->data->data + buffer->offset,
                          buffer->data->len - buffer->offset);

        if (n > 0)
        {
            buffer->offset += (gsize)n;
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return FALSE;

        if (n < 0 && errno == EINTR)
            continue;

        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "write 失败：%s", g_strerror(errno));
        return FALSE;
    }

    g_byte_array_set_size(buffer->data, 0);
    buffer->offset = 0;
    return TRUE;
}
