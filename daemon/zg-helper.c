/* zg-helper.c - 主进程侧的 Helper 客户端
 *
 * 协议：4 字节大端长度 + JSON，Unix socket 默认
 * /var/run/zfs-gui/zfs-helper.sock（可用 ZFS_GUI_HELPER_SOCKET 覆盖）。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-helper.h"

#include <errno.h>
#include <string.h>
#include <sys/capsicum.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "zg-json.h"

#define ZG_HELPER_SOCKET_DEFAULT "/var/run/zfs-gui/zfs-helper.sock"

static int helper_fds[ZG_HELPER_POOL_SIZE];
static guint helper_count;
static gint helper_slot = -1;

static ZgOpOutcome *
helper_outcome(gboolean ok,
               const gchar *message)
{
    ZgOpOutcome *outcome = g_new0(ZgOpOutcome, 1);

    outcome->ok = ok;
    outcome->code = ok ? 0 : 1;
    outcome->message = g_strdup(message != NULL ? message
                                                : (ok ? "Helper 操作完成" : "Helper 操作失败"));
    outcome->stdout_text = g_strdup("");
    outcome->stderr_text = g_strdup(ok ? "" : (message != NULL ? message : ""));
    outcome->data_json = g_strdup("");

    return outcome;
}

gboolean
zg_helper_pool_open(guint count,
                    GError **error)
{
    const gchar *socket_path = g_getenv("ZFS_GUI_HELPER_SOCKET");
    guint i;

    if (count == 0 || count > ZG_HELPER_POOL_SIZE)
    {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "Helper 连接数非法：%u", count);
        return FALSE;
    }

    if (helper_count > 0)
        return TRUE;

    for (i = 0; i < count; i++)
    {
        struct sockaddr_un addr;
        cap_rights_t rights;
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);

        if (fd < 0)
        {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "socket 失败：%s", g_strerror(errno));
            zg_helper_pool_close();
            return FALSE;
        }

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        g_strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "连接 Helper 失败：%s", g_strerror(errno));
            close(fd);
            zg_helper_pool_close();
            return FALSE;
        }

        cap_rights_init(&rights, CAP_READ, CAP_WRITE);
        if (cap_rights_limit(fd, &rights) != 0)
        {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "限制 Helper fd 权利失败：%s", g_strerror(errno));
            close(fd);
            zg_helper_pool_close();
            return FALSE;
        }

        helper_fds[i] = fd;
        helper_count++;
    }

    return TRUE;
}

void zg_helper_pool_close(void)
{
    guint i;

    for (i = 0; i < helper_count; i++)
        close(helper_fds[i]);

    helper_count = 0;
    helper_slot = -1;
}

void zg_helper_set_slot(gint slot)
{
    helper_slot = slot;
}

gboolean
zg_helper_supports(const gchar *op_id)
{
    return g_strcmp0(op_id, "zfs.mount") == 0 ||
           g_strcmp0(op_id, "zfs.unmount") == 0;
}

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

ZgOpOutcome *
zg_helper_call(const ZgOpDef *op,
               const ZgOpRequest *request)
{
    const gchar *dataset = NULL;
    const gchar *options = "";
    const gchar *op_name;
    ZgJsonWriter *writer;
    gchar *text = NULL;
    guint32 length;
    int fd = -1;
    guint i;
    gboolean force = FALSE;
    ZgOpOutcome *outcome = NULL;

    if (request->positional->len > 0)
        dataset = g_ptr_array_index(request->positional, 0);

    for (i = 0; i < request->flags->len; i++)
    {
        if (g_strcmp0(g_ptr_array_index(request->flags, i), "-f") == 0)
            force = TRUE;
    }

    for (i = 0; i < request->options->len; i++)
    {
        const ZgOpOption *option = g_ptr_array_index(request->options, i);

        if (g_strcmp0(option->key, "o") == 0)
            options = option->value;
    }

    if (dataset == NULL)
        return helper_outcome(FALSE, "Helper 需要明确的数据集名称");

    op_name = g_strcmp0(op->id, "zfs.unmount") == 0 ? "unmount" : "mount";

    writer = zg_json_writer_new();
    zg_json_writer_begin_object(writer, NULL);
    zg_json_writer_set_int(writer, "v", 1);
    zg_json_writer_set_string(writer, "op", op_name);
    zg_json_writer_set_string(writer, "dataset", dataset);
    zg_json_writer_set_string(writer, "options", options);
    zg_json_writer_set_bool(writer, "force", force);
    zg_json_writer_end_object(writer);
    text = zg_json_writer_to_string(writer, NULL);

    if (helper_count == 0 || helper_slot < 0 ||
        (guint)helper_slot >= helper_count)
    {
        outcome = helper_outcome(FALSE, "特权 Helper 未预连接");
        goto out;
    }

    fd = helper_fds[helper_slot];

    length = g_htonl((guint32)strlen(text));
    if (!write_full(fd, &length, sizeof(length)) ||
        !write_full(fd, text, strlen(text)) ||
        !read_full(fd, &length, sizeof(length)))
    {
        outcome = helper_outcome(FALSE, "与 Helper 通信失败");
        goto out;
    }

    length = g_ntohl(length);
    if (length == 0 || length > 64 * 1024)
    {
        outcome = helper_outcome(FALSE, "Helper 应答非法");
        goto out;
    }

    {
        gchar *response = g_malloc0((gsize)length + 1);
        GError *error = NULL;
        ZgJson *root;

        if (!read_full(fd, response, length))
        {
            g_free(response);
            outcome = helper_outcome(FALSE, "读取 Helper 应答失败");
            goto out;
        }

        root = zg_json_parse(response, -1, &error);
        g_free(response);

        if (root == NULL)
        {
            outcome = helper_outcome(FALSE,
                                     error != NULL ? error->message : "Helper 应答解析失败");
            g_clear_error(&error);
            goto out;
        }

        outcome = helper_outcome(
                zg_json_object_get_bool(root, "ok", FALSE),
                zg_json_object_get_string(root, "message"));
        zg_json_free(root);
    }

out:
    g_free(text);
    return outcome;
}
