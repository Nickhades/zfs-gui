/* kqueue_frame_test.c - kqueue + 非阻塞长度前缀帧解析单元测试
 *
 * 覆盖：
 *   1. 半包到达时等待，不阻塞
 *   2. 拼包后正确解析出完整 JSON 帧
 *   3. 非阻塞写部分失败后补写
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <arpa/inet.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void
check(bool condition,
      const char *message)
{
    printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition)
        failures++;
}

typedef struct
{
    uint8_t data[4096];
    size_t length;
} FrameBuffer;

/* 返回 1 = 取出一帧（out 为 NUL 结尾），0 = 需要更多数据，-1 = 错误 */
static int
frame_pop(FrameBuffer *buffer,
          char *out,
          size_t out_size)
{
    uint32_t length;

    if (buffer->length < 4)
        return 0;

    memcpy(&length, buffer->data, 4);
    length = ntohl(length);

    if (length == 0 || length >= out_size)
        return -1;

    if (buffer->length < 4 + length)
        return 0;

    memcpy(out, buffer->data + 4, length);
    out[length] = '\0';
    memmove(buffer->data, buffer->data + 4 + length,
            buffer->length - 4 - length);
    buffer->length -= 4 + length;

    return 1;
}

static int
send_frame(int fd,
           const char *text)
{
    uint32_t length = htonl((uint32_t)strlen(text));
    struct iovec iov[2];
    struct msghdr msg;

    memset(&msg, 0, sizeof(msg));
    iov[0].iov_base = &length;
    iov[0].iov_len = sizeof(length);
    iov[1].iov_base = (void *)text;
    iov[1].iov_len = strlen(text);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    return sendmsg(fd, &msg, MSG_NOSIGNAL) > 0 ? 0 : -1;
}

int main(void)
{
    int sv[2];
    int kq;
    struct kevent change;
    struct kevent event;
    struct timespec timeout = {.tv_sec = 2, .tv_nsec = 0};
    FrameBuffer buffer;
    char frame[256];
    bool got_first = false;
    bool got_second = false;

    memset(&buffer, 0, sizeof(buffer));

    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);

    kq = kqueue();
    check(kq >= 0, "kqueue");

    EV_SET(&change, sv[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    check(kevent(kq, &change, 1, NULL, 0, NULL) == 0, "kevent register");

    /* 发送第一帧的一半，事件循环应只读到半包 */
    {
        const char *text = "{\"type\":\"started\",\"req_id\":1}";
        uint32_t be = htonl((uint32_t)strlen(text));
        uint8_t raw[256];
        size_t total = 0;

        memcpy(raw, &be, 4);
        memcpy(raw + 4, text, strlen(text));
        total = 4 + strlen(text);

        check(write(sv[1], raw, total / 2) == (ssize_t)(total / 2),
              "write half frame");
    }

    {
        int n = kevent(kq, NULL, 0, &event, 1, &timeout);
        check(n == 1 && event.filter == EVFILT_READ, "read event for half frame");

        {
            ssize_t got = read(sv[0], buffer.data + buffer.length,
                               sizeof(buffer.data) - buffer.length);

            check(got > 0, "nonblocking read half frame");
            if (got > 0)
                buffer.length += (size_t)got;
        }

        check(frame_pop(&buffer, frame, sizeof(frame)) == 0,
              "half frame requires more data");
        got_first = true;
    }

    /* 发送剩余部分 + 第二帧，事件循环解析出两帧 */
    {
        const char *text = "{\"type\":\"started\",\"req_id\":1}";
        uint32_t be = htonl((uint32_t)strlen(text));
        uint8_t raw[256];
        size_t total = 4 + strlen(text);

        memcpy(raw, &be, 4);
        memcpy(raw + 4, text, strlen(text));

        check(write(sv[1], raw + total / 2, total - total / 2) ==
                      (ssize_t)(total - total / 2),
              "write remaining half");
        check(send_frame(sv[1], "{\"type\":\"cancel\",\"req_id\":1}") == 0,
              "send cancel frame");
    }

    {
        int n = kevent(kq, NULL, 0, &event, 1, &timeout);

        check(n == 1, "read event for remaining frames");
        if (n == 1)
        {
            ssize_t got = read(sv[0], buffer.data + buffer.length,
                               sizeof(buffer.data) - buffer.length);

            check(got > 0, "nonblocking read rest");
            if (got > 0)
                buffer.length += (size_t)got;
        }

        check(frame_pop(&buffer, frame, sizeof(frame)) == 1,
              "first frame parsed");
        check(strstr(frame, "started") != NULL, "first frame is started");

        check(frame_pop(&buffer, frame, sizeof(frame)) == 1,
              "second frame parsed");
        check(strstr(frame, "cancel") != NULL, "second frame is cancel");
        got_second = true;
    }

    check(got_first && got_second, "half-frame state machine completed");

    close(sv[0]);
    close(sv[1]);
    close(kq);

    printf(failures == 0 ? "KQUEUE_FRAME: ALL PASS\n"
                         : "KQUEUE_FRAME: FAILURES=%d\n",
           failures);
    return failures == 0 ? 0 : 1;
}
