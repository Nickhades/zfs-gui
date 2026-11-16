/* zfs-helper.c - 特权 Helper（主守护进程的能力模式伙伴）
 *
 * 只做极少数需要挂载/卸载的事情：zfs mount / zfs unmount。
 * 主进程通过 Unix socket + JSON 请求调用；Helper 校验 peer uid 必须为 0。
 *
 * 协议（4 字节大端长度 + JSON）：
 *   请求：{"v":1,"op":"mount"|"unmount","dataset":"tank/data",
 *          "force":false,"options":""}
 *   应答：{"ok":true|false,"code":0|1,"message":"..."}
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "zg-argv.h"
#include "zg-json.h"
#include "zg-util.h"

#define ZG_HELPER_SOCKET_DEFAULT "/var/run/zfs-gui/zfs-helper.sock"

static gboolean
read_full(int fd,
          void *buffer,
          gsize length)
{
    guchar *p = buffer;

    while (length > 0)
    {
        ssize_t n = read(fd, p, length);

        if (n <= 0)
            return FALSE;

        p += n;
        length -= (gsize)n;
    }

    return TRUE;
}

static gboolean
write_full(int fd,
           const void *buffer,
           gsize length)
{
    const guchar *p = buffer;

    while (length > 0)
    {
        ssize_t n = write(fd, p, length);

        if (n <= 0)
            return FALSE;

        p += n;
        length -= (gsize)n;
    }

    return TRUE;
}

static gboolean
send_response(int fd,
              gboolean ok,
              const gchar *message)
{
    ZgJsonWriter *writer = zg_json_writer_new();
    gchar *text;
    guint32 length;
    gboolean result;

    zg_json_writer_begin_object(writer, NULL);
    zg_json_writer_set_bool(writer, "ok", ok);
    zg_json_writer_set_int(writer, "code", ok ? 0 : 1);
    zg_json_writer_set_string(writer, "message", message != NULL ? message : "");
    zg_json_writer_end_object(writer);
    text = zg_json_writer_to_string(writer, NULL);

    length = g_htonl((guint32)strlen(text));
    result = write_full(fd, &length, sizeof(length)) &&
             write_full(fd, text, strlen(text));

    g_free(text);
    return result;
}

static gchar *
run_zfs(const gchar *op,
        const gchar *dataset,
        const gchar *options,
        gboolean force)
{
    int pipefd[2];
    pid_t pid;
    GPtrArray *args;
    gchar *output = NULL;
    gchar buffer[512];
    gsize used = 0;
    int status = 0;

    if (pipe(pipefd) != 0)
        return g_strdup(g_strerror(errno));

    args = g_ptr_array_new();
    g_ptr_array_add(args, (gpointer) "/sbin/zfs");
    g_ptr_array_add(args, (gpointer)op);

    if (force)
        g_ptr_array_add(args, (gpointer) "-f");

    if (options != NULL && options[0] != '\0')
    {
        g_ptr_array_add(args, (gpointer) "-o");
        g_ptr_array_add(args, (gpointer)options);
    }

    g_ptr_array_add(args, (gpointer)dataset);
    g_ptr_array_add(args, NULL);

    pid = fork();

    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        g_ptr_array_free(args, TRUE);
        return g_strdup(g_strerror(errno));
    }

    if (pid == 0)
    {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execv("/sbin/zfs", (char *const *)args->pdata);
        _exit(127);
    }

    close(pipefd[1]);
    g_ptr_array_free(args, TRUE);

    while (1)
    {
        ssize_t n = read(pipefd[0], buffer, sizeof(buffer));

        if (n <= 0)
            break;

        output = g_realloc(output, used + (gsize)n + 1);
        memcpy(output + used, buffer, (gsize)n);
        used += (gsize)n;
    }

    close(pipefd[0]);
    waitpid(pid, &status, 0);

    if (output == NULL)
        output = g_strdup("");

    output[used] = '\0';

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        gchar *message;

        if (used == 0)
            message = g_strdup_printf("zfs %s 失败（exit=%d）", op,
                                      WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        else
            message = g_strdup_printf("%s", output);

        g_free(output);
        return message;
    }

    g_free(output);
    return NULL;
}

static int
recv_stream_fd(int sock)
{
    struct msghdr msg;
    struct iovec iov;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;
    char byte = 0;
    int fd = -1;

    memset(&msg, 0, sizeof(msg));
    memset(cbuf, 0, sizeof(cbuf));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    if (recvmsg(sock, &msg, MSG_CMSG_CLOEXEC) != 1)
        return -1;

    if (msg.msg_flags & MSG_CTRUNC)
        return -1;

    cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
        return -1;

    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return fd;
}

static gchar *
run_send_stream(int control_fd,
                const gchar *snapshot,
                int out_fd,
                gboolean *canceled)
{
    pid_t pid;
    int status = 0;
    gboolean stop = FALSE;

    *canceled = FALSE;

    pid = fork();
    if (pid < 0)
        return g_strdup(g_strerror(errno));

    if (pid == 0)
    {
        if (dup2(out_fd, STDOUT_FILENO) < 0)
            _exit(126);

        close(out_fd);
        execl("/sbin/zfs", "zfs", "send", snapshot, (char *)NULL);
        _exit(127);
    }

    close(out_fd);

    while (!stop)
    {
        struct pollfd pfd;
        int ready;
        pid_t done;

        pfd.fd = control_fd;
        pfd.events = POLLIN | POLLHUP;
        pfd.revents = 0;

        ready = poll(&pfd, 1, 100);

        if (ready > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)))
        {
            char buffer[64];

            /* 客户端发来 CANCEL 或直接断开：两者都终止发送 */
            (void)recv(control_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
            *canceled = TRUE;
            stop = TRUE;
        }

        done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            break;

        if (stop)
        {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            break;
        }
    }

    if (*canceled)
        return g_strdup("zfs send 已取消");

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return g_strdup_printf("zfs send 失败（exit=%d）",
                               WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    return NULL;
}

static void
handle_client(int fd)
{
    guint32 length;

    while (read_full(fd, &length, sizeof(length)))
    {
        gchar *text;
        ZgJson *root = NULL;
        GError *error = NULL;
        const gchar *op;
        const gchar *dataset;
        const gchar *snapshot;
        const gchar *options = "";
        gboolean force;
        gchar *message;

        length = g_ntohl(length);

        if (length == 0 || length > 64 * 1024)
            return;

        text = g_malloc0((gsize)length + 1);
        if (!read_full(fd, text, length))
        {
            g_free(text);
            return;
        }

        root = zg_json_parse(text, -1, &error);
        g_free(text);

        if (root == NULL)
        {
            send_response(fd, FALSE, error != NULL ? error->message : "JSON 解析失败");
            g_clear_error(&error);
            continue;
        }

        op = zg_json_object_get_string(root, "op");
        dataset = zg_json_object_get_string(root, "dataset");
        snapshot = zg_json_object_get_string(root, "snapshot");
        options = zg_json_object_get_string(root, "options");
        force = zg_json_object_get_bool(root, "force", FALSE);

        if (op == NULL)
        {
            send_response(fd, FALSE, "请求参数非法");
            zg_json_free(root);
            continue;
        }

        if (g_strcmp0(op, "send_stream") == 0)
        {
            int out_fd;

            if (snapshot == NULL || !zg_snapshot_name_valid(snapshot))
            {
                send_response(fd, FALSE, "快照名非法");
                zg_json_free(root);
                continue;
            }

            out_fd = recv_stream_fd(fd);
            if (out_fd < 0)
            {
                send_response(fd, FALSE, "未收到有效的流文件 fd");
                zg_json_free(root);
                continue;
            }

            {
                gboolean canceled = FALSE;

                message = run_send_stream(fd, snapshot, out_fd, &canceled);
                send_response(fd, message == NULL, message);
                if (canceled)
                    break;
            }
            g_free(message);
            zg_json_free(root);
            continue;
        }

        if (dataset == NULL ||
            (g_strcmp0(op, "mount") != 0 && g_strcmp0(op, "unmount") != 0) ||
            !zg_dataset_name_valid(dataset))
        {
            send_response(fd, FALSE, "请求参数非法");
            zg_json_free(root);
            continue;
        }

        if (options == NULL)
            options = "";

        message = run_zfs(op, dataset, options, force);

        {
            gboolean ok = message == NULL;

            if (ok)
                message = g_strdup(g_strcmp0(op, "mount") == 0 ? "挂载成功"
                                                               : "卸载成功");
            send_response(fd, ok, message);
        }
        g_free(message);
        zg_json_free(root);
    }
}

int main(int argc,
         char **argv)
{
    const gchar *socket_path = g_getenv("ZFS_GUI_HELPER_SOCKET");
    struct sockaddr_un addr;
    int listen_fd;
    int opt;

    while ((opt = getopt(argc, argv, "s:")) != -1)
    {
        if (opt == 's')
            socket_path = optarg;
    }

    if (socket_path == NULL)
        socket_path = ZG_HELPER_SOCKET_DEFAULT;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));
    unlink(socket_path);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        chmod(socket_path, 0600) != 0 || listen(listen_fd, 8) != 0)
    {
        perror("bind/listen");
        close(listen_fd);
        return 1;
    }

    while (1)
    {
        int client_fd = accept(listen_fd, NULL, NULL);
        uid_t uid;
        gid_t gid;

        if (client_fd < 0)
        {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }

        if (getpeereid(client_fd, &uid, &gid) != 0 || uid != 0)
        {
            close(client_fd);
            continue;
        }

        {
            pid_t pid = fork();

            if (pid == 0)
            {
                close(listen_fd);
                handle_client(client_fd);
                close(client_fd);
                _exit(0);
            }

            close(client_fd);
        }
    }

    close(listen_fd);
    unlink(socket_path);
    return 0;
}
