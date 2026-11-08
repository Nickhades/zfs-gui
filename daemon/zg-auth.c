/* zg-auth.c - 管理员密码二次验证实现（PAM）
 *
 * 设计要点
 * --------
 * 1. 使用 PAM 而不是自己比对密码哈希：这样能兼容系统上已经配置的
 *    pam_unix / pam_krb5 / pam_ssh 等认证后端。
 * 2. PAM 服务名优先 "zfs-gui"（安装时提供 /etc/pam.d/zfs-gui），
 *    若系统没有该配置则回退到 "system"。
 * 3. 密码只在本函数栈上保留一份临时副本，用完 explicit_bzero 清除；
 *    不写日志、不落盘、不回显。
 * 4. 失败限速：按 uid 记录失败次数与时间，超过阈值后锁定一段时间，
 *    防止通过图形界面暴力猜测管理员密码。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-auth.h"

#include <errno.h>
#include <fcntl.h>
#include <security/pam_appl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "zg-log.h"
#include "zg-util.h"

/* 认证失败记录的文本行上限与文件权限 */
#define ZG_AUTH_LINE_MAX 128
#define ZG_AUTH_SECRET_FILE_MODE 0600

typedef struct
{
    const gchar *password;
} ZgPamData;

/* ------------------------------------------------------------------------- */
/* PAM 会话                                                                   */
/* ------------------------------------------------------------------------- */

static int
pam_conversation(int num_msg,
                 const struct pam_message **msg,
                 struct pam_response **resp,
                 void *appdata)
{
    ZgPamData *data = appdata;
    struct pam_response *responses;
    int i;

    if (num_msg <= 0 || msg == NULL || resp == NULL)
        return PAM_CONV_ERR;

    responses = calloc((size_t)num_msg, sizeof(struct pam_response));

    if (responses == NULL)
        return PAM_BUF_ERR;

    for (i = 0; i < num_msg; i++)
    {
        switch (msg[i]->msg_style)
        {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON:
            responses[i].resp = strdup(data->password != NULL ? data->password : "");
            responses[i].resp_retcode = 0;

            if (responses[i].resp == NULL)
            {
                int j;

                for (j = 0; j < i; j++)
                    free(responses[j].resp);

                free(responses);
                return PAM_BUF_ERR;
            }
            break;
        default:
            responses[i].resp = NULL;
            responses[i].resp_retcode = 0;
            break;
        }
    }

    *resp = responses;

    return PAM_SUCCESS;
}

static const gchar *
pam_status_text(int status)
{
    switch (status)
    {
    case PAM_AUTH_ERR:
        return "密码错误";
    case PAM_CRED_INSUFFICIENT:
        return "凭据不足";
    case PAM_AUTHINFO_UNAVAIL:
        return "无法获取认证信息";
    case PAM_USER_UNKNOWN:
        return "账号不存在";
    case PAM_MAXTRIES:
        return "尝试次数过多";
    case PAM_ACCT_EXPIRED:
        return "账号已过期";
    case PAM_PERM_DENIED:
        return "权限不足";
    case PAM_SYSTEM_ERR:
        return "PAM 系统错误";
    case PAM_BUF_ERR:
        return "PAM 内存错误";
    case PAM_CONV_ERR:
        return "PAM 会话错误";
    default:
        return "认证失败";
    }
}

/* 对单个 PAM 服务尝试认证；返回 PAM 状态码 */
static int
pam_try_service(const gchar *service,
                const gchar *username,
                const gchar *secret)
{
    ZgPamData data;
    struct pam_conv conversation;
    pam_handle_t *handle = NULL;
    int status;

    data.password = secret;
    conversation.conv = pam_conversation;
    conversation.appdata_ptr = &data;

    status = pam_start(service, username, &conversation, &handle);

    if (status != PAM_SUCCESS)
        return status;

    status = pam_authenticate(handle, 0);

    if (status == PAM_SUCCESS)
        status = pam_acct_mgmt(handle, 0);

    pam_end(handle, status);

    return status;
}

/* ------------------------------------------------------------------------- */
/* 失败限速                                                                   */
/* ------------------------------------------------------------------------- */

static gchar *
lockout_path(const ZgPolicy *policy,
             uid_t uid)
{
    gchar *socket_path = g_strdup(zg_policy_socket_path(policy));
    gchar *directory = g_path_get_dirname(socket_path);
    gchar *path = g_strdup_printf("%s/.authfail-%u", directory, (guint)uid);

    g_free(socket_path);
    g_free(directory);

    return path;
}

/* 返回 0 表示未锁定，否则返回剩余锁定秒数 */
static guint
lockout_remaining(const ZgPolicy *policy,
                  uid_t uid)
{
    gchar *path;
    gchar buffer[ZG_AUTH_LINE_MAX];
    int fd;
    gint64 now = (gint64)time(NULL);
    guint failures = 0;
    gint64 last = 0;
    guint remaining = 0;

    path = lockout_path(policy, uid);
    fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
    {
        g_free(path);
        return 0;
    }

    {
        const ssize_t read_bytes = read(fd, buffer, sizeof(buffer) - 1);

        if (read_bytes > 0)
        {
            buffer[read_bytes] = '\0';

            if (sscanf(buffer, "%u %" G_GINT64_FORMAT, &failures, &last) != 2)
            {
                failures = 0;
                last = 0;
            }
        }
    }

    close(fd);
    g_free(path);

    if (failures < policy->auth_max_attempts || policy->auth_lockout == 0)
        return 0;

    if (now - last >= (gint64)policy->auth_lockout)
        return 0;

    remaining = (guint)((gint64)policy->auth_lockout - (now - last));

    return remaining;
}

static guint
record_failure(const ZgPolicy *policy,
               uid_t uid)
{
    gchar *path = lockout_path(policy, uid);
    int fd;
    guint failures = 0;
    gint64 last = 0;
    gint64 now = (gint64)time(NULL);
    gchar buffer[ZG_AUTH_LINE_MAX];

    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, ZG_AUTH_SECRET_FILE_MODE);

    if (fd < 0)
    {
        g_free(path);
        return 0;
    }

    if (flock(fd, LOCK_EX) == 0)
    {
        gchar read_buffer[ZG_AUTH_LINE_MAX];
        const ssize_t read_bytes = read(fd, read_buffer, sizeof(read_buffer) - 1);

        if (read_bytes > 0)
        {
            read_buffer[read_bytes] = '\0';

            if (sscanf(read_buffer, "%u %" G_GINT64_FORMAT, &failures, &last) != 2)
            {
                failures = 0;
                last = 0;
            }
        }

        if (last != 0 && now - last >= (gint64)policy->auth_lockout)
            failures = 0;

        failures++;

        g_snprintf(buffer, sizeof(buffer), "%u %" G_GINT64_FORMAT "\n",
                   failures, now);
        lseek(fd, 0, SEEK_SET);
        ftruncate(fd, 0);

        if (write(fd, buffer, strlen(buffer)) < 0)
            zg_log_warning("写入认证失败计数失败：%s", g_strerror(errno));

        flock(fd, LOCK_UN);
    }

    close(fd);
    g_free(path);

    return failures;
}

static void
clear_failures(const ZgPolicy *policy,
               uid_t uid)
{
    gchar *path = lockout_path(policy, uid);

    unlink(path);
    g_free(path);
}

/* ------------------------------------------------------------------------- */
/* 对外接口                                                                   */
/* ------------------------------------------------------------------------- */

const gchar *
zg_auth_mode(const ZgPolicy *policy)
{
    return zg_policy_auth_mode(policy);
}

ZgAuthStatus
zg_auth_verify(const ZgPolicy *policy,
               uid_t peer_uid,
               const gchar *username,
               const gchar *password,
               gchar **message)
{
    ZgAuthStatus status = ZG_AUTH_BAD_CREDENTIALS;
    guint remaining;
    gchar *secret;
    int pam_status = PAM_SUCCESS;

    if (message != NULL)
        *message = NULL;

    /* 非 root 的守护进程无法读取系统密码库，PAM 必然失败。
     * 直接给出可操作的诊断，而不是误报"密码错误"。 */
    if (geteuid() != 0)
    {
        if (message != NULL)
            *message = g_strdup_printf(
                    "后台服务未以 root 身份运行（euid=%u）。\n"
                    "非 root 进程无法读取系统密码库，因此管理员密码无法验证。\n\n"
                    "请以 root 启动服务：\n"
                    "    service zfsguid start\n"
                    "或前台调试：\n"
                    "    su -c 'zfs-guid -f -c /usr/local/etc/zfs-gui.conf'",
                    (guint)geteuid());

        zg_log_warning("收到认证请求，但守护进程 euid=%u，无法验证密码",
                       (guint)geteuid());

        return ZG_AUTH_UNAVAILABLE;
    }

    if (policy == NULL || zg_str_is_empty(username) ||
        password == NULL || password[0] == '\0')
    {
        if (message != NULL)
            *message = g_strdup("身份验证需要管理员账号与密码");
        return ZG_AUTH_BAD_CREDENTIALS;
    }

    remaining = lockout_remaining(policy, peer_uid);

    if (remaining > 0)
    {
        if (message != NULL)
            *message = g_strdup_printf("身份验证失败次数过多，请 %u 秒后重试", remaining);

        zg_log_warning("认证被限速：uid=%u user=%s 剩余 %u 秒",
                       (guint)peer_uid, username, remaining);

        return ZG_AUTH_LOCKED;
    }

    /* 复制一份可变副本，便于用完后清零 */
    secret = g_strdup(password);

    pam_status = pam_try_service("zfs-gui", username, secret);

    /* 服务配置缺失时回退到系统默认策略 */
    if (pam_status == PAM_SYSTEM_ERR || pam_status == PAM_ABORT ||
        pam_status == PAM_SERVICE_ERR)
        pam_status = pam_try_service("system", username, secret);

    explicit_bzero(secret, strlen(secret));
    g_free(secret);

    if (pam_status == PAM_MAXTRIES)
    {
        status = ZG_AUTH_LOCKED;

        if (message != NULL)
            *message = g_strdup("认证失败次数过多，已被临时锁定");

        record_failure(policy, peer_uid);
    }
    else if (pam_status != PAM_SUCCESS)
    {
        const guint failures = record_failure(policy, peer_uid);
        const guint attempts_left = (policy->auth_max_attempts > failures)
                                            ? policy->auth_max_attempts - failures
                                            : 0;

        status = ZG_AUTH_BAD_CREDENTIALS;

        if (message != NULL)
        {
            gchar *hint = (attempts_left > 0)
                                  ? g_strdup_printf("还可尝试 %u 次", attempts_left)
                                  : g_strdup("已达到失败上限，将暂时锁定");

            *message = g_strdup_printf("身份验证失败：%s（账号 %s）。%s",
                                       pam_status_text(pam_status), username, hint);
            g_free(hint);
        }

        zg_log_warning("认证失败：uid=%u user=%s（%s）",
                       (guint)peer_uid, username, pam_status_text(pam_status));
    }
    else if (!zg_policy_user_is_admin(policy, username))
    {
        status = ZG_AUTH_NOT_ADMIN;

        if (message != NULL)
        {
            *message = g_strdup_printf(
                    "账号 %s 不是管理员。\n"
                    "请使用 root，或属于 allowed_groups（默认 wheel）的账号。",
                    username);
        }

        record_failure(policy, peer_uid);

        zg_log_warning("认证通过但非管理员：uid=%u user=%s", (guint)peer_uid, username);
    }
    else
    {
        status = ZG_AUTH_OK;

        clear_failures(policy, peer_uid);

        if (message != NULL)
            *message = g_strdup("身份验证通过");

        zg_log_info("认证通过：uid=%u user=%s", (guint)peer_uid, username);
    }

    if (pam_status == PAM_SYSTEM_ERR || pam_status == PAM_ABORT)
    {
        status = ZG_AUTH_UNAVAILABLE;

        if (message != NULL)
        {
            g_free(*message);
            *message = g_strdup("系统 PAM 不可用，无法验证管理员密码。\n"
                                "请确认 /etc/pam.d/zfs-gui 或 /etc/pam.d/system 存在。");
        }
    }

    return status;
}
