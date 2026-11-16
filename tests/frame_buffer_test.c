/* frame_buffer_test.c - zg-frame 非阻塞帧缓冲测试 */

#include <arpa/inet.h>
#include <sys/socket.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "zg-frame.h"

static int failures;

static void
check(int ok,
      const char *message)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok)
        failures++;
}

static gboolean
send_all(int fd,
         const void *data,
         size_t length)
{
    const unsigned char *p = data;

    while (length > 0)
    {
        ssize_t n = write(fd, p, length);

        if (n <= 0)
            return FALSE;
        p += n;
        length -= (size_t)n;
    }

    return TRUE;
}

static gboolean
send_text_frame(int fd,
                const char *text)
{
    guint32 length = htonl((guint32)strlen(text));

    return send_all(fd, &length, sizeof(length)) &&
           send_all(fd, text, strlen(text));
}

int
main(void)
{
    int sv[2];
    ZgFrameBuffer *buffer;
    ZgOutBuffer *out;
    gchar *frame = NULL;
    GError *error = NULL;

    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);

    buffer = zg_frame_buffer_new(1024);
    check(buffer != NULL, "frame buffer new");

    /* 半包 */
    {
        const char *text = "{\"type\":\"started\"}";
        guint32 length = htonl((guint32)strlen(text));
        unsigned char raw[128];
        size_t total = sizeof(length) + strlen(text);

        memcpy(raw, &length, sizeof(length));
        memcpy(raw + sizeof(length), text, strlen(text));
        check(send_all(sv[1], raw, total / 2), "write half frame");

        check(zg_frame_read(buffer, sv[0], &error) == ZG_FRAME_AGAIN,
              "read returns AGAIN");
        check(zg_frame_pop(buffer, &frame, &error) == 0,
              "half frame waits");
        check(send_all(sv[1], raw + total / 2, total - total / 2),
              "write remaining half");
        check(send_text_frame(sv[1], "{\"type\":\"cancel\"}"),
              "write cancel frame");

        check(zg_frame_read(buffer, sv[0], &error) == ZG_FRAME_AGAIN,
              "read rest");
        check(zg_frame_pop(buffer, &frame, &error) == 1, "first frame");
        check(strstr(frame, "started") != NULL, "first frame content");
        g_free(frame);

        check(zg_frame_pop(buffer, &frame, &error) == 1, "second frame");
        check(strstr(frame, "cancel") != NULL, "second frame content");
        g_free(frame);
    }

    /* 超长帧被拒绝 */
    {
        guint32 evil = htonl(4096);

        check(send_all(sv[1], &evil, sizeof(evil)), "write oversize length");
        check(g_byte_array_set_size(buffer->data, 0) == buffer->data,
              "clear buffer");
        check(zg_frame_read(buffer, sv[0], &error) == ZG_FRAME_AGAIN,
              "read oversize length");
        check(zg_frame_pop(buffer, &frame, &error) == -1,
              "oversize frame rejected");
        g_clear_error(&error);
    }

    /* 输出缓冲 */
    out = zg_out_buffer_new();
    check(zg_out_buffer_queue_frame(out, "{\"ok\":true}", &error),
          "queue output frame");
    check(zg_out_buffer_flush(out, sv[1], &error), "flush output");

    {
        char raw[64];
        ssize_t n = read(sv[0], raw, sizeof(raw));

        check(n > 4, "peer received output frame");
    }

    zg_out_buffer_free(out);
    zg_frame_buffer_free(buffer);
    close(sv[0]);
    close(sv[1]);

    printf(failures == 0 ? "FRAME_BUFFER: ALL PASS\n"
                          : "FRAME_BUFFER: FAILURES=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
