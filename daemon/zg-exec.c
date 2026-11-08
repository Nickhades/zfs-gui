/* zg-exec.c - 子进程执行封装实现
 *
 * 说明
 * ----
 * - 一律使用 fork + execve，绝不调用 shell，因此不存在命令注入面。
 * - 子进程使用独立进程组，超时可整组终止（含 zfs 派生的子进程）。
 * - 环境固定为 LC_ALL=C，保证 ZFS 输出格式稳定（数字、月份名可解析）。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-exec.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "zg-protocol.h"

/* 执行器常量：避免散落的魔法数字 */
#define ZG_MS_PER_SECOND 1000
#define ZG_NS_PER_MS 1000000
#define ZG_EXEC_OUTPUT_CHUNK 65536
#define ZG_EXEC_FILE_MODE 0600
#define ZG_EXEC_EXIT_NOT_FOUND 127
#define ZG_EXEC_POLL_MS 1000
#define ZG_EXEC_FINISH_GRACE_MS 2000
#define ZG_EXEC_KILL_GRACE_US 50000


static gint64
monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;

    return (gint64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const gchar *const zg_child_environment[] = {
        "PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin",
        "LC_ALL=C",
        "LANG=C",
        "TZ=UTC",
        NULL,
};

gchar *
zg_exec_resolve_tool(const gchar *name)
{
    static const gchar *const directories[] = {
            "/sbin",
            "/usr/sbin",
            "/usr/local/sbin",
            "/bin",
            "/usr/bin",
            "/usr/local/bin",
    };
    guint i;

    if (name == NULL || name[0] == '\0')
        return NULL;

    if (strchr(name, '/') != NULL)
    {
        if (g_file_test(name, G_FILE_TEST_IS_EXECUTABLE))
            return g_strdup(name);

        return NULL;
    }

    for (i = 0; i < G_N_ELEMENTS(directories); i++)
    {
        gchar *candidate = g_strdup_printf("%s/%s", directories[i], name);

        if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
            return candidate;

        g_free(candidate);
    }

    return g_find_program_in_path(name);
}

static void
reap_child(pid_t pid,
           ZgExecResult *result)
{
    int status = 0;

    if (waitpid(pid, &status, 0) < 0)
        return;

    if (WIFEXITED(status))
        result->exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result->term_signal = WTERMSIG(status);
}

/* 读取 fd 中的全部数据，返回 FALSE 表示已到 EOF 或出错 */
static gboolean
drain_fd(int fd,
         GString *buffer,
         gboolean *truncated)
{
    gchar chunk[ZG_EXEC_OUTPUT_CHUNK];

    for (;;)
    {
        const ssize_t received = read(fd, chunk, sizeof(chunk));

        if (received < 0)
        {
            if (errno == EINTR)
                continue;

            return FALSE;
        }

        if (received == 0)
            return FALSE;

        if (buffer->len + (gsize)received > ZG_MAX_OUTPUT)
        {
            const gsize room = (buffer->len < ZG_MAX_OUTPUT)
                                       ? ZG_MAX_OUTPUT - buffer->len
                                       : 0;
            gsize i;

            for (i = 0; i < room; i++)
                g_string_append_c(buffer, chunk[i]);

            if (truncated != NULL)
                *truncated = TRUE;

            continue; /* 继续读取并丢弃，避免子进程因管道写满而阻塞 */
        }

        g_string_append_len(buffer, chunk, received);
    }
}

ZgExecResult *
zg_exec_run(char *const argv[],
            const gchar *stdout_path,
            const gchar *stdin_path,
            guint timeout_seconds)
{
    ZgExecResult *result;
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int stdout_target = -1;
    int stdin_source = -1;
    pid_t pid;
    gboolean out_open = FALSE;
    gboolean err_open = FALSE;
    gint64 deadline = 0;

    g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);

    result = g_new0(ZgExecResult, 1);
    result->stdout_text = g_string_new(NULL);
    result->stderr_text = g_string_new(NULL);
    result->exit_code = -1;

    /* stdout：重定向到文件或使用管道 */
    if (stdout_path != NULL)
    {
        stdout_target = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, ZG_EXEC_FILE_MODE);

        if (stdout_target < 0)
        {
            result->error_message = g_strdup_printf("无法打开输出文件 %s：%s",
                                                    stdout_path, g_strerror(errno));
            return result;
        }
    }
    else if (pipe(out_pipe) < 0)
    {
        result->error_message = g_strdup_printf("创建管道失败：%s", g_strerror(errno));
        return result;
    }

    if (pipe(err_pipe) < 0)
    {
        result->error_message = g_strdup_printf("创建管道失败：%s", g_strerror(errno));

        if (stdout_target >= 0)
            close(stdout_target);
        if (out_pipe[0] >= 0)
        {
            close(out_pipe[0]);
            close(out_pipe[1]);
        }

        return result;
    }

    /* stdin：重定向自文件或 /dev/null */
    if (stdin_path != NULL)
    {
        stdin_source = open(stdin_path, O_RDONLY);

        if (stdin_source < 0)
        {
            result->error_message = g_strdup_printf("无法读取输入文件 %s：%s",
                                                    stdin_path, g_strerror(errno));
            goto cleanup;
        }
    }
    else
    {
        stdin_source = open("/dev/null", O_RDONLY);

        if (stdin_source < 0)
            stdin_source = -1;
    }

    pid = fork();

    if (pid < 0)
    {
        result->error_message = g_strdup_printf("fork 失败：%s", g_strerror(errno));
        goto cleanup;
    }

    if (pid == 0)
    {
        /* ---- 子进程 ---- */
        sigset_t empty;

        setpgid(0, 0);

        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        if (stdout_target >= 0)
        {
            dup2(stdout_target, STDOUT_FILENO);
            close(stdout_target);
        }
        else
        {
            dup2(out_pipe[1], STDOUT_FILENO);
            close(out_pipe[0]);
            close(out_pipe[1]);
        }

        dup2(err_pipe[1], STDERR_FILENO);
        close(err_pipe[0]);
        close(err_pipe[1]);

        if (stdin_source >= 0)
        {
            dup2(stdin_source, STDIN_FILENO);
            close(stdin_source);
        }

        execve(argv[0], argv, (char *const *)zg_child_environment);

        _exit(ZG_EXEC_EXIT_NOT_FOUND);
    }

    /* ---- 父进程 ---- */
    setpgid(pid, pid); /* 忽略失败（子进程可能已自行设置） */

    if (stdout_target >= 0)
    {
        close(stdout_target);
        stdout_target = -1;
    }
    else
    {
        close(out_pipe[1]);
        out_pipe[1] = -1;
        out_open = TRUE;
    }

    close(err_pipe[1]);
    err_pipe[1] = -1;
    err_open = TRUE;

    if (stdin_source >= 0)
    {
        close(stdin_source);
        stdin_source = -1;
    }

    if (timeout_seconds > 0)
        deadline = monotonic_ms() + (gint64)timeout_seconds * 1000;

    while (out_open || err_open)
    {
        struct pollfd fds[2];
        int nfds = 0;
        int out_index = -1;
        int err_index = -1;
        int wait_ms = ZG_EXEC_POLL_MS;
        int ready;

        if (out_open)
        {
            fds[nfds].fd = out_pipe[0];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            out_index = nfds++;
        }

        if (err_open)
        {
            fds[nfds].fd = err_pipe[0];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            err_index = nfds++;
        }

        if (deadline > 0)
        {
            const gint64 remaining = deadline - monotonic_ms();

            if (remaining <= 0)
            {
                result->timed_out = TRUE;
                break;
            }

            if (remaining < wait_ms)
                wait_ms = (int)remaining;
        }

        ready = poll(fds, (nfds_t)nfds, wait_ms);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            result->error_message = g_strdup_printf("poll 失败：%s", g_strerror(errno));
            break;
        }

        if (ready == 0)
            continue;

        if (out_index >= 0 && (fds[out_index].revents & (POLLIN | POLLHUP | POLLERR)))
        {
            if (!drain_fd(out_pipe[0], result->stdout_text, &result->truncated))
            {
                close(out_pipe[0]);
                out_pipe[0] = -1;
                out_open = FALSE;
            }
        }

        if (err_index >= 0 && (fds[err_index].revents & (POLLIN | POLLHUP | POLLERR)))
        {
            if (!drain_fd(err_pipe[0], result->stderr_text, &result->truncated))
            {
                close(err_pipe[0]);
                err_pipe[0] = -1;
                err_open = FALSE;
            }
        }
    }

    if (out_open)
    {
        close(out_pipe[0]);
        out_pipe[0] = -1;
        out_open = FALSE;
    }

    if (err_open)
    {
        close(err_pipe[0]);
        err_pipe[0] = -1;
        err_open = FALSE;
    }

    if (result->timed_out)
    {
        gint64 give_up;

        kill(-pid, SIGTERM);

        give_up = monotonic_ms() + ZG_EXEC_FINISH_GRACE_MS;

        for (;;)
        {
            int status = 0;
            const pid_t reaped = waitpid(pid, &status, WNOHANG);

            if (reaped == pid)
            {
                if (WIFEXITED(status))
                    result->exit_code = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    result->term_signal = WTERMSIG(status);
                break;
            }

            if (reaped < 0 && errno != EINTR)
                break;

            if (monotonic_ms() > give_up)
            {
                kill(-pid, SIGKILL);
                reap_child(pid, result);
                break;
            }

            g_usleep(ZG_EXEC_KILL_GRACE_US);
        }

        if (result->error_message == NULL)
        {
            result->error_message = g_strdup_printf(
                    "操作超过 %u 秒仍未完成，已强制终止。"
                    "长耗时操作请到「任务」页查看或在终端中执行。",
                    timeout_seconds);
        }
    }
    else
    {
        reap_child(pid, result);
    }

cleanup:
    if (stdout_target >= 0)
        close(stdout_target);

    if (stdin_source >= 0)
        close(stdin_source);

    if (out_pipe[0] >= 0)
        close(out_pipe[0]);
    if (out_pipe[1] >= 0)
        close(out_pipe[1]);
    if (err_pipe[0] >= 0)
        close(err_pipe[0]);
    if (err_pipe[1] >= 0)
        close(err_pipe[1]);

    return result;
}

void zg_exec_result_free(ZgExecResult *result)
{
    if (result == NULL)
        return;

    if (result->stdout_text != NULL)
        g_string_free(result->stdout_text, TRUE);

    if (result->stderr_text != NULL)
        g_string_free(result->stderr_text, TRUE);

    g_free(result->error_message);
    g_free(result);
}
