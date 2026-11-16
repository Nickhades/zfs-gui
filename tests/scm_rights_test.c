/* scm_rights_test.c - SCM_RIGHTS 底层机制单元测试
 *
 * 覆盖：
 *   1. 通过 Unix socket 传递 /etc/passwd fd，子进程 fstat/read 验证
 *   2. MSG_CMSG_CLOEXEC 生效（接收端 fd 带 FD_CLOEXEC）
 *   3. MSG_CTRUNC 检测：控制缓冲区不足时必须置位
 *   4. 100 次传递后 fd 不泄漏
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <sys/socket.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void
check(int condition,
      const char *message)
{
    if (condition)
    {
        printf("PASS: %s\n", message);
    }
    else
    {
        printf("FAIL: %s (%s)\n", message, strerror(errno));
        failures++;
    }
}

static int
send_fd(int sock,
        int fd,
        const void *data,
        size_t length)
{
    struct msghdr msg;
    struct iovec iov;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;

    memset(&msg, 0, sizeof(msg));
    memset(cbuf, 0, sizeof(cbuf));

    iov.iov_base = (void *)data;
    iov.iov_len = length;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

    return sendmsg(sock, &msg, MSG_NOSIGNAL) == (ssize_t)length ? 0 : -1;
}

static int
recv_fd(int sock,
        int *fd,
        void *data,
        size_t length,
        bool *truncated)
{
    struct msghdr msg;
    struct iovec iov;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;
    ssize_t n;

    memset(&msg, 0, sizeof(msg));
    memset(cbuf, 0, sizeof(cbuf));

    iov.iov_base = data;
    iov.iov_len = length;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    if (n < 0)
        return -1;

    *truncated = (msg.msg_flags & MSG_CTRUNC) != 0;

    cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
        return -1;

    memcpy(fd, CMSG_DATA(cmsg), sizeof(*fd));
    return (int)n;
}

static int
count_open_fds(void)
{
    int count = 0;
    int fd;

    for (fd = 0; fd < 256; fd++)
    {
        if (fcntl(fd, F_GETFD) != -1)
            count++;
    }

    return count;
}

static void
test_transfer_and_cloexec(void)
{
    int sv[2];
    int fd;
    int child_status = 0;
    char byte = 'F';
    pid_t pid;

    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

    fd = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
    check(fd >= 0, "open /etc/passwd");

    pid = fork();
    if (pid == 0)
    {
        int received = -1;
        bool truncated = false;
        char data[1];
        struct stat st;

        close(sv[1]);

        if (recv_fd(sv[0], &received, data, sizeof(data), &truncated) < 0)
            _exit(2);

        if (truncated || received < 0)
            _exit(3);

        if (fstat(received, &st) != 0 || st.st_size <= 0)
            _exit(4);

        if ((fcntl(received, F_GETFD) & FD_CLOEXEC) == 0)
            _exit(5);

        if (read(received, data, 1) != 1)
            _exit(6);

        printf("PASS: child fstat size=%lld, FD_CLOEXEC set, first byte='%c'\n",
               (long long)st.st_size, data[0]);
        close(received);
        close(sv[0]);
        _exit(0);
    }

    close(sv[0]);
    check(send_fd(sv[1], fd, &byte, sizeof(byte)) == 0, "sendmsg fd");
    close(fd);
    close(sv[1]);

    waitpid(pid, &child_status, 0);
    check(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
          "child received fd and read file");
    if (WIFEXITED(child_status) && WEXITSTATUS(child_status) != 0)
        printf("  child exit=%d\n", WEXITSTATUS(child_status));
}

static void
test_truncation_flag(void)
{
    int sv[2];
    int fd;
    struct msghdr msg;
    struct iovec iov;
    char cbuf[8];
    char byte = 'T';

    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "truncation socketpair");
    fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    check(fd >= 0, "open truncation fd");

    check(send_fd(sv[0], fd, &byte, sizeof(byte)) == 0, "sendmsg fd for truncation");
    close(fd);

    memset(&msg, 0, sizeof(msg));
    memset(cbuf, 0, sizeof(cbuf));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    check(recvmsg(sv[1], &msg, MSG_CMSG_CLOEXEC) == 1, "recvmsg with short cmsg buffer");
    check((msg.msg_flags & MSG_CTRUNC) != 0, "MSG_CTRUNC set for short cmsg buffer");

    close(sv[0]);
    close(sv[1]);
}

static void
test_fd_leak(void)
{
    int sv[2];
    int before;
    int after;
    int i;

    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "leak socketpair");
    before = count_open_fds();

    for (i = 0; i < 100; i++)
    {
        int fd = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
        int received = -1;
        bool truncated = false;
        char byte = 'L';
        char data[1];

        if (fd < 0 || send_fd(sv[0], fd, &byte, sizeof(byte)) != 0)
        {
            check(0, "leak loop send");
            break;
        }

        close(fd);

        if (recv_fd(sv[1], &received, data, sizeof(data), &truncated) != 1 ||
            truncated || received < 0)
        {
            check(0, "leak loop recv");
            break;
        }

        close(received);
    }

    after = count_open_fds();
    printf("fd count: before=%d after=%d\n", before, after);
    check(before == after, "no fd leak after 100 transfers");

    close(sv[0]);
    close(sv[1]);
}

int
main(void)
{
    test_transfer_and_cloexec();
    test_truncation_flag();
    test_fd_leak();

    printf(failures == 0 ? "SCM_RIGHTS: ALL PASS\n" : "SCM_RIGHTS: FAILURES=%d\n",
           failures);

    return failures == 0 ? 0 : 1;
}
