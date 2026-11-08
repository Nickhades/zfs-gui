/* zg-server.c - Unix socket 服务端实现
 *
 * 并发模型：accept 后 fork 一个子进程处理该连接，父进程立即回到 accept。
 * 这样长耗时操作（scrub、send/receive）不会阻塞其他客户端。
 *
 * 鉴权：使用 getpeereid() 取得对端 uid/gid，再依据 ZgPolicy 判定。
 * socket 文件权限作为第一道防线（默认 root:wheel 0660）。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-server.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "zg-argv.h"
#include "zg-auth.h"
#include "zg-exec.h"
#include "zg-io.h"
#include "zg-json.h"
#include "zg-log.h"
#include "zg-opdef.h"
#include "zg-ops.h"
#include "zg-protocol.h"
#include "zg-util.h"

#define ZG_MAX_CHILDREN 64
#define ZG_REQUEST_TIMEOUT 60

static int zg_signal_pipe[2] = {-1, -1};
static volatile sig_atomic_t zg_terminate_requested = 0;

/* ------------------------------------------------------------------------- */
/* 信号处理                                                                   */
/* ------------------------------------------------------------------------- */

static void
signal_handler(int signo)
{
    gchar byte = (gchar)signo;
    ssize_t ignored;

    if (signo == SIGTERM || signo == SIGINT)
        zg_terminate_requested = 1;

    ignored = write(zg_signal_pipe[1], &byte, 1);
    (void)ignored;
}

static gboolean
install_signal_handlers(void)
{
    struct sigaction action;

    if (pipe(zg_signal_pipe) < 0)
        return FALSE;

    /* 自管道必须是非阻塞的：否则排空时 read() 会因为没有数据而永久阻塞。
     * 读端非阻塞用于排空，写端非阻塞保证信号处理器里不会卡住。 */
    fcntl(zg_signal_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(zg_signal_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(zg_signal_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(zg_signal_pipe[1], F_SETFL, O_NONBLOCK);

    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;

    if (sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGCHLD, &action, NULL) < 0)
        return FALSE;

    signal(SIGPIPE, SIG_IGN);

    return TRUE;
}

static void
reap_children(guint *active_children)
{
    for (;;)
    {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid <= 0)
            break;

        if (*active_children > 0)
            (*active_children)--;
    }
}

/* ------------------------------------------------------------------------- */
/* 监听 socket                                                                */
/* ------------------------------------------------------------------------- */

/* socket 目录权限：0711 允许普通用户“进入”目录以连接 socket，
 * 但不允许列出目录内容；真正的访问控制由 socket 的 0660 与
 * allowed_users / allowed_groups 完成。 */
#define ZG_SOCKET_DIR_MODE 0711

static int
create_listen_socket(const ZgPolicy *policy)
{
    const gchar *path = zg_policy_socket_path(policy);
    struct sockaddr_un address;
    gchar *directory;
    struct stat info;
    int fd;
    gid_t gid;

    if (strlen(path) >= sizeof(address.sun_path))
    {
        zg_log_error("socket 路径过长：%s", path);
        return -1;
    }

    directory = g_path_get_dirname(path);

    if (stat(directory, &info) < 0)
    {
        if (g_mkdir_with_parents(directory, ZG_SOCKET_DIR_MODE) < 0)
        {
            zg_log_error("无法创建目录 %s：%s", directory, g_strerror(errno));
            g_free(directory);
            return -1;
        }
    }

    /* 已存在的旧目录也要纠正权限，升级后无需手工 chmod */
    if (chmod(directory, ZG_SOCKET_DIR_MODE) < 0)
        zg_log_warning("无法设置目录权限 %s：%s", directory, g_strerror(errno));

    g_free(directory);

    unlink(path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
    {
        zg_log_error("创建 socket 失败：%s", g_strerror(errno));
        return -1;
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path, sizeof(address.sun_path));

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        zg_log_error("绑定 %s 失败：%s", path, g_strerror(errno));
        close(fd);
        return -1;
    }

    gid = zg_policy_socket_gid(policy);

    if (gid != (gid_t)-1)
        chown(path, 0, gid);

    chmod(path, policy->socket_mode);

    if (listen(fd, 16) < 0)
    {
        zg_log_error("listen 失败：%s", g_strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }

    return fd;
}

/* ------------------------------------------------------------------------- */
/* 应答                                                                       */
/* ------------------------------------------------------------------------- */

static void
send_response(int fd,
              gboolean ok,
              gint code,
              const gchar *stdout_text,
              const gchar *stderr_text,
              const gchar *data_json,
              const gchar *message)
{
    ZgJsonWriter *writer;
    gchar *payload;
    gsize length = 0;
    GError *error = NULL;

    writer = zg_json_writer_new();
    zg_json_writer_begin_object(writer, NULL);
    zg_json_writer_set_int(writer, ZG_KEY_VERSION, ZG_PROTOCOL_VERSION);
    zg_json_writer_set_bool(writer, ZG_KEY_OK, ok);
    zg_json_writer_set_int(writer, ZG_KEY_CODE, code);
    zg_json_writer_set_string(writer, ZG_KEY_STDOUT, stdout_text != NULL ? stdout_text : "");
    zg_json_writer_set_string(writer, ZG_KEY_STDERR, stderr_text != NULL ? stderr_text : "");
    zg_json_writer_set_string(writer, ZG_KEY_MESSAGE, message != NULL ? message : "");

    if (data_json != NULL && data_json[0] != '\0')
        zg_json_writer_set_raw(writer, ZG_KEY_DATA, data_json);

    zg_json_writer_end_object(writer);

    payload = zg_json_writer_to_string(writer, &length);

    if (!zg_io_write_frame(fd, payload, length, &error))
    {
        zg_log_warning("回写应答失败：%s", error != NULL ? error->message : "未知错误");
        g_clear_error(&error);
    }

    g_free(payload);
}

static void
send_error(int fd,
           gint code,
           const gchar *message)
{
    send_response(fd, FALSE, code, "", "", NULL, message);
}

/* ------------------------------------------------------------------------- */
/* 单连接处理                                                                 */
/* ------------------------------------------------------------------------- */

static void
handle_client(int fd,
              const ZgPolicy *policy,
              uid_t peer_uid)
{
    GError *error = NULL;
    gchar *payload = NULL;
    gsize payload_length = 0;
    ZgJson *root = NULL;
    const gchar *op_id;
    const ZgOpDef *op;
    ZgOpRequest *request = NULL;
    ZgOpOutcome *outcome = NULL;
    const ZgJson *version_node;

    zg_io_set_timeouts(fd, ZG_REQUEST_TIMEOUT, ZG_REQUEST_TIMEOUT);

    payload = zg_io_read_frame(fd, &payload_length, &error);

    if (payload == NULL)
    {
        send_error(fd, -1, error != NULL ? error->message : "读取请求失败");
        goto out;
    }

    root = zg_json_parse(payload, (gssize)payload_length, &error);

    if (root == NULL)
    {
        send_error(fd, -1, error != NULL ? error->message : "请求不是合法 JSON");
        goto out;
    }

    if (zg_json_get_type(root) != ZG_JSON_TYPE_OBJECT)
    {
        send_error(fd, -1, "请求必须是 JSON 对象");
        goto out;
    }

    version_node = zg_json_object_get(root, ZG_KEY_VERSION);

    if (zg_json_get_int(version_node, -1) != ZG_PROTOCOL_VERSION)
    {
        gchar *message = g_strdup_printf(
                "协议版本不匹配：前端 v%" G_GINT64_FORMAT "，守护进程 v%d，请同步升级两端。",
                zg_json_get_int(version_node, -1), ZG_PROTOCOL_VERSION);

        send_error(fd, -1, message);
        g_free(message);
        goto out;
    }

    op_id = zg_json_object_get_string(root, ZG_KEY_OP);

    if (zg_str_is_empty(op_id))
    {
        send_error(fd, -1, "请求缺少 op 字段");
        goto out;
    }

    op = zg_op_lookup(op_id);

    if (op == NULL)
    {
        gchar *message = g_strdup_printf("未授权的操作：%s", op_id);

        zg_log_warning("uid=%u 请求未登记的操作 %s，已拒绝",
                       (guint)peer_uid, op_id);
        send_error(fd, -1, message);
        g_free(message);
        goto out;
    }

    request = zg_op_request_new();

    if (!zg_op_request_parse(zg_json_object_get(root, ZG_KEY_ARGS),
                             request, &error))
    {
        send_error(fd, -1, error != NULL ? error->message : "参数解析失败");
        goto out;
    }

    if (!zg_op_request_validate(op, request, &error))
    {
        gchar *message = g_strdup_printf("参数校验失败：%s",
                                         error != NULL ? error->message : "未知原因");

        zg_log_warning("uid=%u 操作 %s 参数校验失败", (guint)peer_uid, op->id);
        send_error(fd, -1, message);
        g_free(message);
        goto out;
    }

    /* 危险操作：必须提供管理员密码，由 PAM 重新验证 */
    if (zg_op_requires_password(op, zg_policy_auth_mode(policy)))
    {
        const ZgJson *auth = zg_json_object_get(root, ZG_KEY_AUTH);
        const gchar *auth_user = zg_json_object_get_string(auth, ZG_KEY_AUTH_USER);
        const gchar *auth_pass = zg_json_object_get_string(auth, ZG_KEY_AUTH_PASS);
        gchar *auth_message = NULL;
        ZgAuthStatus auth_status;

        if (zg_str_is_empty(auth_user) || auth_pass == NULL || auth_pass[0] == '\0')
        {
            zg_log_warning("uid=%u 请求危险操作 %s 但未提供密码", (guint)peer_uid, op->id);
            send_error(fd, ZG_ERR_AUTH_REQUIRED,
                       "该操作属于危险操作，需要管理员密码重新验证。");
            goto out;
        }

        auth_status = zg_auth_verify(policy, peer_uid, auth_user, auth_pass,
                                     &auth_message);

        if (auth_status != ZG_AUTH_OK)
        {
            gint code;

            if (auth_status == ZG_AUTH_LOCKED)
                code = ZG_ERR_AUTH_LOCKED;
            else if (auth_status == ZG_AUTH_UNAVAILABLE)
                code = ZG_ERR_AUTH_UNAVAILABLE;
            else
                code = ZG_ERR_AUTH_FAILED;

            send_error(fd, code,
                       auth_message != NULL ? auth_message : "身份验证失败");
            g_free(auth_message);
            goto out;
        }

        g_free(auth_message);
    }

    {
        gchar *description = zg_op_describe(op, request);

        zg_log_info("uid=%u 执行 %s", (guint)peer_uid, description);
        g_free(description);
    }

    outcome = zg_op_execute(op, request, policy);

    if (outcome == NULL)
    {
        send_error(fd, -1, "操作执行失败");
        goto out;
    }

    if (outcome->ok)
        zg_log_info("操作 %s 完成（%d ms）", op->id, (int)outcome->duration_ms);
    else
        zg_log_warning("操作 %s 失败：%s", op->id,
                       outcome->message != NULL ? outcome->message : "未知原因");

    send_response(fd,
                  outcome->ok,
                  outcome->code,
                  outcome->stdout_text,
                  outcome->stderr_text,
                  outcome->data_json,
                  outcome->message);

out:
    zg_op_outcome_free(outcome);
    zg_op_request_free(request);
    zg_json_free(root);
    g_clear_error(&error);

    /* 请求载荷可能包含管理员密码，释放前先清零 */
    if (payload != NULL)
    {
        explicit_bzero(payload, payload_length);
        g_free(payload);
    }
}

/* ------------------------------------------------------------------------- */
/* 主循环                                                                     */
/* ------------------------------------------------------------------------- */

int zg_server_run(const ZgPolicy *policy)
{
    int server_fd;
    guint active_children = 0;
    gint exit_status = 0;

    if (!install_signal_handlers())
    {
        zg_log_error("安装信号处理器失败：%s", g_strerror(errno));
        return -1;
    }

    server_fd = create_listen_socket(policy);

    if (server_fd < 0)
        return -1;

    zg_log_info("守护进程已启动，监听 %s（用户 %s / 组 %s）",
                zg_policy_socket_path(policy),
                policy->allowed_users != NULL && policy->allowed_users[0] != NULL
                        ? policy->allowed_users[0]
                        : "-",
                policy->allowed_groups != NULL && policy->allowed_groups[0] != NULL
                        ? policy->allowed_groups[0]
                        : "-");

    while (!zg_terminate_requested)
    {
        struct pollfd fds[2];
        int ready;

        fds[0].fd = server_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        fds[1].fd = zg_signal_pipe[0];
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        ready = poll(fds, 2, -1);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            zg_log_error("poll 失败：%s", g_strerror(errno));
            exit_status = -1;
            break;
        }

        if (fds[1].revents & POLLIN)
        {
            gchar buffer[64];

            while (read(zg_signal_pipe[0], buffer, sizeof(buffer)) > 0)
                ;

            reap_children(&active_children);
        }

        if (zg_terminate_requested)
            break;

        if (fds[0].revents & POLLIN)
        {
            struct sockaddr_un peer;
            socklen_t peer_length = sizeof(peer);
            uid_t uid = (uid_t)-1;
            gid_t gid = (gid_t)-1;
            int client_fd;
            gchar *denied_message = NULL;
            pid_t pid;

            client_fd = accept(server_fd, (struct sockaddr *)&peer, &peer_length);

            if (client_fd < 0)
            {
                if (errno != EINTR && errno != ECONNABORTED)
                    zg_log_warning("accept 失败：%s", g_strerror(errno));
                continue;
            }

            fcntl(client_fd, F_SETFD, FD_CLOEXEC);

            if (getpeereid(client_fd, &uid, &gid) < 0)
            {
                zg_log_warning("无法获取对端身份：%s", g_strerror(errno));
                send_error(client_fd, -1, "无法确认客户端身份，连接已拒绝");
                close(client_fd);
                continue;
            }

            if (!zg_policy_authorize(policy, uid, gid, &denied_message))
            {
                gchar *message = g_strdup_printf("拒绝访问：%s",
                                                 denied_message != NULL ? denied_message
                                                                        : "权限不足");

                zg_log_warning("拒绝 uid=%u gid=%u 的连接", (guint)uid, (guint)gid);
                send_error(client_fd, -1, message);
                g_free(message);
                g_free(denied_message);
                close(client_fd);
                continue;
            }

            g_free(denied_message);

            if (active_children >= ZG_MAX_CHILDREN)
            {
                send_error(client_fd, -4, "后台服务繁忙，请稍后重试");
                close(client_fd);
                continue;
            }

            pid = fork();

            if (pid < 0)
            {
                zg_log_error("fork 失败：%s", g_strerror(errno));
                send_error(client_fd, -4, "后台服务无法创建处理进程");
                close(client_fd);
                continue;
            }

            if (pid == 0)
            {
                /* 子进程：只处理这一个连接 */
                close(server_fd);
                close(zg_signal_pipe[0]);
                close(zg_signal_pipe[1]);

                signal(SIGTERM, SIG_DFL);
                signal(SIGINT, SIG_DFL);
                signal(SIGCHLD, SIG_DFL);
                signal(SIGPIPE, SIG_IGN);

                handle_client(client_fd, policy, uid);

                close(client_fd);
                _exit(0);
            }

            close(client_fd);
            active_children++;
        }
    }

    zg_log_info("守护进程正在退出");

    close(server_fd);
    unlink(zg_policy_socket_path(policy));

    return exit_status;
}
