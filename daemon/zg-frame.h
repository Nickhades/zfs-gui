/* zg-frame.h - 非阻塞长度前缀帧缓冲
 *
 * 帧格式：4 字节大端长度 + JSON 文本。
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
    ZG_FRAME_AGAIN = 0,
    ZG_FRAME_EOF = 1,
    ZG_FRAME_ERROR = 2,
} ZgFrameReadResult;

typedef struct
{
    GByteArray *data;
    gsize max_frame;
} ZgFrameBuffer;

ZgFrameBuffer *zg_frame_buffer_new(gsize max_frame);
void zg_frame_buffer_free(ZgFrameBuffer *buffer);

/* 非阻塞读取；读到 EAGAIN 返回 AGAIN，EOF 返回 EOF，错误返回 ERROR */
ZgFrameReadResult zg_frame_read(ZgFrameBuffer *buffer,
                                int fd,
                                GError **error);

/* 1 = 取出一帧（*out 需 g_free），0 = 数据不足，-1 = 长度非法/超限 */
gint zg_frame_pop(ZgFrameBuffer *buffer,
                  gchar **out,
                  GError **error);

typedef struct
{
    GByteArray *data;
    gsize offset;
} ZgOutBuffer;

ZgOutBuffer *zg_out_buffer_new(void);
void zg_out_buffer_free(ZgOutBuffer *buffer);
gboolean zg_out_buffer_queue(ZgOutBuffer *buffer,
                             const gchar *text,
                             GError **error);
gboolean zg_out_buffer_queue_frame(ZgOutBuffer *buffer,
                                   const gchar *text,
                                   GError **error);

/* TRUE = 全部写完；FALSE = 仍有剩余（EAGAIN）或出错 */
gboolean zg_out_buffer_flush(ZgOutBuffer *buffer,
                             int fd,
                             GError **error);

G_END_DECLS
