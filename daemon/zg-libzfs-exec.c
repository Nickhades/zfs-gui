/* zg-libzfs-exec.c - 可选的 libzfs 执行层（阶段 B1.5 / B2）
 *
 * 通过运行时 dlopen 加载 libzfs/libnvpair，不产生硬链接依赖；默认关闭
 * （ZFS_GUI_LIBZFS_EXEC=1 或 rc.conf 的 zfsguid_libzfs_exec=YES）。
 * 未实现或不支持的参数组合返回 FALSE，调用方回退到 execv 路径。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-libzfs-exec.h"

#ifdef HAVE_LIBZFS

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/capsicum.h>
#include <libzfs.h>
#include <string.h>
#include <unistd.h>

#include "zg-log.h"

typedef struct
{
    void *dl;
    libzfs_handle_t *(*init)(void);
    const char *(*error_action)(libzfs_handle_t *);
    const char *(*error_description)(libzfs_handle_t *);
    zfs_handle_t *(*open)(libzfs_handle_t *, const char *, int);
    void (*close)(zfs_handle_t *);
    const char *(*get_name)(const zfs_handle_t *);
    zfs_type_t (*get_type)(const zfs_handle_t *);
    int (*iter_children)(zfs_handle_t *, zfs_iter_f, void *);
    int (*iter_snapshots)(zfs_handle_t *, boolean_t, zfs_iter_f, void *, uint64_t,
                          uint64_t);
    int (*create)(libzfs_handle_t *, const char *, zfs_type_t, nvlist_t *);
    int (*create_ancestors)(libzfs_handle_t *, const char *);
    int (*destroy)(zfs_handle_t *, boolean_t);
    int (*rename)(zfs_handle_t *, const char *, renameflags_t);
    int (*prop_set)(zfs_handle_t *, const char *, const char *);
    int (*snapshot)(libzfs_handle_t *, const char *, boolean_t, nvlist_t *);
    int (*rollback)(zfs_handle_t *, zfs_handle_t *, boolean_t);
    int (*clone)(zfs_handle_t *, const char *, nvlist_t *);
    libzfs_handle_t *(*init_fd)(int);
    int (*zpool_iter)(libzfs_handle_t *, zpool_iter_f, void *);
    int (*send)(zfs_handle_t *, const char *, const char *, sendflags_t *, int,
                snapfilter_cb_t *, void *, nvlist_t **);
    int (*receive)(libzfs_handle_t *, const char *, nvlist_t *, recvflags_t *, int,
                   avl_tree_t *);
    int (*crypto_load_key)(zfs_handle_t *, boolean_t, const char *);
    int (*crypto_unload_key)(zfs_handle_t *);
    int (*crypto_rewrap)(zfs_handle_t *, nvlist_t *, boolean_t);

    zpool_handle_t *(*pool_open)(libzfs_handle_t *, const char *);
    void (*pool_close)(zpool_handle_t *);
    int (*pool_destroy)(zpool_handle_t *, const char *);
    int (*pool_export)(zpool_handle_t *, boolean_t, const char *);
    int (*pool_scan)(zpool_handle_t *, pool_scan_func_t, pool_scrub_cmd_t);
    int (*pool_clear)(zpool_handle_t *, const char *, nvlist_t *);
    int (*pool_trim)(zpool_handle_t *, pool_trim_func_t, nvlist_t *, trimflags_t *);
    int (*vdev_online)(zpool_handle_t *, const char *, int, vdev_state_t *);
    int (*vdev_offline)(zpool_handle_t *, const char *, boolean_t);
    int (*pool_create)(libzfs_handle_t *, const char *, nvlist_t *, nvlist_t *,
                       nvlist_t *);
    int (*pool_add)(zpool_handle_t *, nvlist_t *, boolean_t);
    int (*vdev_remove)(zpool_handle_t *, const char *);
} LzExec;

typedef struct
{
    void *dl;
    int (*alloc)(nvlist_t **, int, int);
    int (*add_string)(nvlist_t *, const char *, const char *);
    int (*add_uint64)(nvlist_t *, const char *, uint64_t);
    int (*add_nvlist)(nvlist_t *, const char *, nvlist_t *);
    int (*add_nvlist_array)(nvlist_t *, const char *, nvlist_t **, uint_t);
    void (*free)(nvlist_t *);
} LzNv;

static LzExec lz;
static LzNv nv;
static libzfs_handle_t *lz_handle;
static int injected_fd = -1;
static gboolean lz_loaded;
static gboolean lz_ok;
static gboolean nv_loaded;
static gboolean nv_ok;

static gboolean
exec_enabled(void)
{
    const gchar *value = g_getenv("ZFS_GUI_LIBZFS_EXEC");

    if (value == NULL || value[0] == '\0')
        return FALSE;

    return g_ascii_strcasecmp(value, "0") != 0 &&
           g_ascii_strcasecmp(value, "no") != 0 &&
           g_ascii_strcasecmp(value, "false") != 0 &&
           g_ascii_strcasecmp(value, "off") != 0;
}

static gboolean
lz_load(void)
{
    if (lz_loaded)
        return lz_ok;

    lz_loaded = TRUE;
    lz.dl = dlopen("libzfs.so.7", RTLD_NOW | RTLD_LOCAL);

    if (lz.dl == NULL)
    {
        zg_log_warning("libzfs: dlopen 失败：%s", dlerror());
        return FALSE;
    }

#define LZ_SYM(field, symbol)                              \
    do                                                     \
    {                                                      \
        *(void **)(&lz.field) = dlsym(lz.dl, symbol);      \
        if (lz.field == NULL)                              \
        {                                                  \
            zg_log_warning("libzfs: 缺少符号 %s", symbol); \
            dlclose(lz.dl);                                \
            memset(&lz, 0, sizeof(lz));                    \
            return FALSE;                                  \
        }                                                  \
    } while (0)

    LZ_SYM(init, "libzfs_init");
    LZ_SYM(error_action, "libzfs_error_action");
    LZ_SYM(error_description, "libzfs_error_description");
    LZ_SYM(open, "zfs_open");
    LZ_SYM(close, "zfs_close");
    LZ_SYM(get_name, "zfs_get_name");
    LZ_SYM(get_type, "zfs_get_type");
    LZ_SYM(iter_children, "zfs_iter_children");
    LZ_SYM(iter_snapshots, "zfs_iter_snapshots");
    LZ_SYM(create, "zfs_create");
    LZ_SYM(create_ancestors, "zfs_create_ancestors");
    LZ_SYM(destroy, "zfs_destroy");
    LZ_SYM(rename, "zfs_rename");
    LZ_SYM(prop_set, "zfs_prop_set");
    LZ_SYM(snapshot, "zfs_snapshot");
    LZ_SYM(rollback, "zfs_rollback");
    LZ_SYM(clone, "zfs_clone");
    LZ_SYM(send, "zfs_send");
    LZ_SYM(receive, "zfs_receive");
    LZ_SYM(crypto_load_key, "zfs_crypto_load_key");
    LZ_SYM(crypto_unload_key, "zfs_crypto_unload_key");
    LZ_SYM(crypto_rewrap, "zfs_crypto_rewrap");
    LZ_SYM(pool_open, "zpool_open");
    LZ_SYM(pool_close, "zpool_close");
    LZ_SYM(pool_destroy, "zpool_destroy");
    LZ_SYM(pool_export, "zpool_export");
    LZ_SYM(pool_scan, "zpool_scan");
    LZ_SYM(pool_clear, "zpool_clear");
    LZ_SYM(pool_trim, "zpool_trim");
    LZ_SYM(vdev_online, "zpool_vdev_online");
    LZ_SYM(vdev_offline, "zpool_vdev_offline");

    *(void **)(&lz.init_fd) = dlsym(lz.dl, "libzfs_init_fd");
    LZ_SYM(zpool_iter, "zpool_iter");
    LZ_SYM(pool_create, "zpool_create");
    LZ_SYM(pool_add, "zpool_add");
    LZ_SYM(vdev_remove, "zpool_vdev_remove");

#undef LZ_SYM

    if (injected_fd >= 0)
    {
        if (lz.init_fd == NULL)
        {
            zg_log_warning("libzfs: 缺少 libzfs_init_fd，无法进入能力模式");
            dlclose(lz.dl);
            memset(&lz, 0, sizeof(lz));
            return FALSE;
        }

        lz_handle = lz.init_fd(injected_fd);
    }
    else
    {
        lz_handle = lz.init();
    }
    if (lz_handle == NULL)
    {
        zg_log_warning("libzfs: 初始化失败（/dev/zfs 不可用？）");
        dlclose(lz.dl);
        memset(&lz, 0, sizeof(lz));
        return FALSE;
    }

    lz_ok = TRUE;
    return TRUE;
}

static gboolean
nv_load(void)
{
    if (nv_loaded)
        return nv_ok;

    nv_loaded = TRUE;
    nv.dl = dlopen("libnvpair.so.3", RTLD_NOW | RTLD_LOCAL);

    if (nv.dl == NULL)
        return FALSE;

#define NV_SYM(field, symbol)                         \
    do                                                \
    {                                                 \
        *(void **)(&nv.field) = dlsym(nv.dl, symbol); \
        if (nv.field == NULL)                         \
        {                                             \
            dlclose(nv.dl);                           \
            memset(&nv, 0, sizeof(nv));               \
            return FALSE;                             \
        }                                             \
    } while (0)

    NV_SYM(alloc, "nvlist_alloc");
    NV_SYM(add_string, "nvlist_add_string");
    NV_SYM(add_uint64, "nvlist_add_uint64");
    NV_SYM(add_nvlist, "nvlist_add_nvlist");
    NV_SYM(add_nvlist_array, "nvlist_add_nvlist_array");
    NV_SYM(free, "nvlist_free");

#undef NV_SYM

    nv_ok = TRUE;
    return TRUE;
}

static ZgOpOutcome *
outcome_make(gboolean ok,
             const gchar *message)
{
    ZgOpOutcome *outcome = g_new0(ZgOpOutcome, 1);

    outcome->ok = ok;
    outcome->code = ok ? 0 : 1;
    outcome->message = g_strdup(message != NULL ? message : (ok ? "操作完成" : "操作失败"));
    outcome->stdout_text = g_strdup("");
    outcome->stderr_text = g_strdup(ok ? "" : (message != NULL ? message : ""));
    outcome->data_json = g_strdup("");

    return outcome;
}

static ZgOpOutcome *
outcome_from_libzfs(void)
{
    const gchar *action = lz.error_action(lz_handle);
    const gchar *description = lz.error_description(lz_handle);
    gchar *message = g_strdup_printf("%s%s%s",
                                     action != NULL ? action : "libzfs",
                                     description != NULL ? "：" : "",
                                     description != NULL ? description : "");
    ZgOpOutcome *outcome = outcome_make(FALSE, message);

    g_free(message);
    return outcome;
}

static gboolean
has_flag(const ZgOpRequest *request,
         const gchar *flag)
{
    guint i;

    for (i = 0; i < request->flags->len; i++)
    {
        if (g_strcmp0(g_ptr_array_index(request->flags, i), flag) == 0)
            return TRUE;
    }

    return FALSE;
}

static gboolean
flags_subset(const ZgOpRequest *request,
             const gchar *const *allowed,
             guint n_allowed)
{
    guint i;
    guint j;

    for (i = 0; i < request->flags->len; i++)
    {
        const gchar *flag = g_ptr_array_index(request->flags, i);
        gboolean ok = FALSE;

        for (j = 0; j < n_allowed; j++)
        {
            if (g_strcmp0(flag, allowed[j]) == 0)
            {
                ok = TRUE;
                break;
            }
        }

        if (!ok)
            return FALSE;
    }

    return TRUE;
}

static const gchar *
positional_at(const ZgOpRequest *request,
              guint index)
{
    if (index >= request->positional->len)
        return NULL;

    return g_ptr_array_index(request->positional, index);
}

static zfs_handle_t *
open_dataset(const gchar *name)
{
    return lz.open(lz_handle, name, ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME);
}

/* 把 -o property=value 选项转换成 nvlist；无选项时 *props=NULL。
 * 返回 FALSE 表示需要回退（选项非法或 nvpair 不可用）。 */
static gboolean
props_from_options(const ZgOpRequest *request,
                   nvlist_t **props)
{
    guint i;

    *props = NULL;

    if (request->options->len == 0)
        return TRUE;

    if (!nv_load())
        return FALSE;

    if (nv.alloc(props, NV_UNIQUE_NAME, 0) != 0)
        return FALSE;

    for (i = 0; i < request->options->len; i++)
    {
        const ZgOpOption *option = g_ptr_array_index(request->options, i);
        const gchar *equals;
        gchar *key;

        if (g_strcmp0(option->key, "o") != 0)
        {
            nv.free(*props);
            *props = NULL;
            return FALSE;
        }

        equals = strchr(option->value, '=');
        if (equals == NULL || equals == option->value)
        {
            nv.free(*props);
            *props = NULL;
            return FALSE;
        }

        key = g_strndup(option->value, (gsize)(equals - option->value));

        if (nv.add_string(*props, key, equals + 1) != 0)
        {
            g_free(key);
            nv.free(*props);
            *props = NULL;
            return FALSE;
        }

        g_free(key);
    }

    return TRUE;
}

/* ---- 数据集操作 ------------------------------------------------------ */

static gboolean
try_snapshot(const ZgOpRequest *request,
             ZgOpOutcome **outcome)
{
    const gchar *name = positional_at(request, 0);
    static const gchar *const allowed[] = {"-r"};
    nvlist_t *props;
    int rc;

    if (request->positional->len != 1 ||
        !flags_subset(request, allowed, G_N_ELEMENTS(allowed)) ||
        !props_from_options(request, &props))
        return FALSE;

    rc = lz.snapshot(lz_handle, name, has_flag(request, "-r"), props);

    if (props != NULL)
        nv.free(props);

    if (rc != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "快照已创建（libzfs）");

    return TRUE;
}

static gboolean
try_create(const ZgOpRequest *request,
           ZgOpOutcome **outcome)
{
    const gchar *name = positional_at(request, 0);
    static const gchar *const allowed[] = {"-p"};
    nvlist_t *props;
    int rc;

    if (request->positional->len != 1 ||
        !flags_subset(request, allowed, G_N_ELEMENTS(allowed)) ||
        !props_from_options(request, &props))
        return FALSE;

    if (has_flag(request, "-p"))
        (void)lz.create_ancestors(lz_handle, name);

    rc = lz.create(lz_handle, name, ZFS_TYPE_FILESYSTEM, props);

    if (props != NULL)
        nv.free(props);

    if (rc != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "数据集已创建（libzfs）");

    return TRUE;
}

static int
collect_child(zfs_handle_t *zh,
              void *data)
{
    GPtrArray *names = data;
    zfs_type_t type = lz.get_type(zh);

    if (type == ZFS_TYPE_FILESYSTEM || type == ZFS_TYPE_VOLUME)
        g_ptr_array_add(names, g_strdup(lz.get_name(zh)));

    lz.close(zh);
    return 0;
}

static int
collect_snapshot(zfs_handle_t *zh,
                 void *data)
{
    GPtrArray *names = data;

    g_ptr_array_add(names, g_strdup(lz.get_name(zh)));
    lz.close(zh);
    return 0;
}

static gboolean
destroy_snapshots(const gchar *dataset,
                  ZgOpOutcome **outcome)
{
    zfs_handle_t *zh = open_dataset(dataset);
    GPtrArray *snaps;
    guint i;
    gboolean ok = TRUE;

    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return FALSE;
    }

    snaps = g_ptr_array_new_with_free_func(g_free);
    lz.iter_snapshots(zh, B_FALSE, collect_snapshot, snaps, 0, 0);
    lz.close(zh);

    for (i = 0; i < snaps->len; i++)
    {
        zfs_handle_t *snap = lz.open(lz_handle,
                                     g_ptr_array_index(snaps, i),
                                     ZFS_TYPE_SNAPSHOT);

        if (snap == NULL || lz.destroy(snap, B_FALSE) != 0)
        {
            if (snap != NULL)
                lz.close(snap);
            *outcome = outcome_from_libzfs();
            ok = FALSE;
            break;
        }

        lz.close(snap);
    }

    g_ptr_array_free(snaps, TRUE);
    return ok;
}

static gboolean
destroy_tree(const gchar *name,
             ZgOpOutcome **outcome)
{
    zfs_handle_t *zh;
    GPtrArray *children;
    guint i;

    if (!destroy_snapshots(name, outcome))
        return FALSE;

    zh = open_dataset(name);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return FALSE;
    }

    children = g_ptr_array_new_with_free_func(g_free);
    lz.iter_children(zh, collect_child, children);
    lz.close(zh);

    for (i = 0; i < children->len; i++)
    {
        if (!destroy_tree(g_ptr_array_index(children, i), outcome))
        {
            g_ptr_array_free(children, TRUE);
            return FALSE;
        }
    }

    g_ptr_array_free(children, TRUE);

    zh = open_dataset(name);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return FALSE;
    }

    if (lz.destroy(zh, B_FALSE) != 0)
    {
        lz.close(zh);
        *outcome = outcome_from_libzfs();
        return FALSE;
    }

    lz.close(zh);
    return TRUE;
}

static gboolean
try_destroy(const ZgOpRequest *request,
            ZgOpOutcome **outcome)
{
    const gchar *name = positional_at(request, 0);
    static const gchar *const allowed[] = {"-r"};
    zfs_handle_t *zh;

    if (request->positional->len != 1 ||
        !flags_subset(request, allowed, G_N_ELEMENTS(allowed)))
        return FALSE;

    if (has_flag(request, "-r") && strchr(name, '@') == NULL)
    {
        if (!destroy_tree(name, outcome))
            return TRUE;

        *outcome = outcome_make(TRUE, "已递归销毁（libzfs）");
        return TRUE;
    }

    if (has_flag(request, "-r"))
        return FALSE;

    zh = lz.open(lz_handle, name,
                 ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME | ZFS_TYPE_SNAPSHOT);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    if (lz.destroy(zh, B_FALSE) != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "已销毁（libzfs）");

    lz.close(zh);
    return TRUE;
}

static gboolean
try_rename(const ZgOpRequest *request,
           ZgOpOutcome **outcome)
{
    const gchar *old_name = positional_at(request, 0);
    const gchar *new_name = positional_at(request, 1);
    static const gchar *const allowed[] = {"-f", "-p", "-u", "-r"};
    renameflags_t flags = {0};
    zfs_handle_t *zh;

    if (request->positional->len != 2 ||
        !flags_subset(request, allowed, G_N_ELEMENTS(allowed)))
        return FALSE;

    flags.recursive = has_flag(request, "-r");
    flags.nounmount = has_flag(request, "-u");
    flags.forceunmount = has_flag(request, "-f");

    if (has_flag(request, "-p"))
    {
        if (strchr(old_name, '@') != NULL || strchr(new_name, '@') != NULL)
            return FALSE;

        (void)lz.create_ancestors(lz_handle, new_name);
    }

    zh = lz.open(lz_handle, old_name,
                 ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME | ZFS_TYPE_SNAPSHOT);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    if (lz.rename(zh, new_name, flags) != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "重命名完成（libzfs）");

    lz.close(zh);
    return TRUE;
}

static gboolean
try_set(const ZgOpRequest *request,
        ZgOpOutcome **outcome)
{
    const gchar *dataset;
    zfs_handle_t *zh;
    guint i;

    if (request->positional->len != 2 || request->flags->len > 0)
        return FALSE;

    dataset = positional_at(request, 1);

    zh = open_dataset(dataset);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    for (i = 0; i < 1; i++)
    {
        const gchar *pair = positional_at(request, 0);
        const gchar *equals = strchr(pair, '=');
        gchar *key;

        if (equals == NULL || equals == pair)
        {
            lz.close(zh);
            *outcome = outcome_make(FALSE, "属性格式非法");
            return TRUE;
        }

        key = g_strndup(pair, (gsize)(equals - pair));

        if (lz.prop_set(zh, key, equals + 1) != 0)
        {
            g_free(key);
            lz.close(zh);
            *outcome = outcome_from_libzfs();
            return TRUE;
        }

        g_free(key);
    }

    lz.close(zh);
    *outcome = outcome_make(TRUE, "属性已设置（libzfs）");
    return TRUE;
}

static gboolean
try_rollback(const ZgOpRequest *request,
             ZgOpOutcome **outcome)
{
    const gchar *name = positional_at(request, 0);
    const gchar *at;
    gchar *dataset;
    zfs_handle_t *zh_dataset;
    zfs_handle_t *zh_snap;

    if (request->positional->len != 1 || request->flags->len > 0 || name == NULL)
        return FALSE;

    at = strchr(name, '@');
    if (at == NULL)
        return FALSE;

    dataset = g_strndup(name, (gsize)(at - name));
    zh_dataset = open_dataset(dataset);
    zh_snap = lz.open(lz_handle, name, ZFS_TYPE_SNAPSHOT);

    if (zh_dataset == NULL || zh_snap == NULL)
    {
        if (zh_dataset != NULL)
            lz.close(zh_dataset);
        if (zh_snap != NULL)
            lz.close(zh_snap);
        g_free(dataset);
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    if (lz.rollback(zh_dataset, zh_snap, B_FALSE) != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "回滚完成（libzfs）");

    lz.close(zh_dataset);
    lz.close(zh_snap);
    g_free(dataset);
    return TRUE;
}

static gboolean
try_clone(const ZgOpRequest *request,
          ZgOpOutcome **outcome)
{
    const gchar *snapshot = positional_at(request, 0);
    const gchar *target = positional_at(request, 1);
    static const gchar *const allowed[] = {"-p"};
    nvlist_t *props;
    zfs_handle_t *zh;
    int rc;

    if (request->positional->len != 2 ||
        !flags_subset(request, allowed, G_N_ELEMENTS(allowed)) ||
        !props_from_options(request, &props))
        return FALSE;

    if (has_flag(request, "-p"))
        (void)lz.create_ancestors(lz_handle, target);

    zh = lz.open(lz_handle, snapshot, ZFS_TYPE_SNAPSHOT);
    if (zh == NULL)
    {
        if (props != NULL)
            nv.free(props);
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    rc = lz.clone(zh, target, props);
    lz.close(zh);

    if (props != NULL)
        nv.free(props);

    if (rc != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "克隆完成（libzfs）");

    return TRUE;
}

/* 打开流文件：拒绝符号链接，发送端截断，接收端只读 */
static gchar *
stream_open_error(const gchar *path,
                  int error)
{
    if (error == ELOOP)
        return g_strdup_printf("拒绝符号链接：%s", path);

    return g_strdup_printf("无法打开 %s：%s", path, g_strerror(error));
}

static gboolean
try_send(const ZgOpRequest *request,
         ZgOpOutcome **outcome)
{
    const gchar *snapshot = positional_at(request, 0);
    static const gchar *const allowed[] = {"-R", "-c", "-h", "-p", "-P", "-v"};
    const gchar *fromsnap = NULL;
    gboolean doall = FALSE;
    const gchar *at;
    gchar *dataset = NULL;
    gchar *tosnap = NULL;
    sendflags_t flags = {0};
    zfs_handle_t *zh;
    int fd;
    int rc;
    guint i;

    if (request->positional->len != 1 ||
        !flags_subset(request, allowed, G_N_ELEMENTS(allowed)) ||
        request->io_path == NULL || request->io_path[0] != '/')
        return FALSE;

    at = snapshot != NULL ? strchr(snapshot, '@') : NULL;
    if (at == NULL)
        return FALSE;

    dataset = g_strndup(snapshot, (gsize)(at - snapshot));
    tosnap = g_strdup(at + 1);

    for (i = 0; i < request->options->len; i++)
    {
        const ZgOpOption *option = g_ptr_array_index(request->options, i);
        const gchar *option_at;

        if ((g_strcmp0(option->key, "i") != 0 &&
             g_strcmp0(option->key, "I") != 0) ||
            fromsnap != NULL)
        {
            g_free(dataset);
            g_free(tosnap);
            return FALSE;
        }

        doall = g_strcmp0(option->key, "I") == 0;
        option_at = strchr(option->value, '@');
        if (option_at == NULL)
            fromsnap = option->value;
        else if (g_str_has_prefix(option->value, dataset) &&
                 option_at == option->value + strlen(dataset))
            fromsnap = option_at + 1;
        else
        {
            g_free(dataset);
            g_free(tosnap);
            return FALSE;
        }
    }

    fd = open(request->io_path,
              O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        *outcome = outcome_make(FALSE,
                                stream_open_error(request->io_path, errno));
        return TRUE;
    }

    flags.replicate = has_flag(request, "-R");
    flags.compress = has_flag(request, "-c");
    flags.holds = has_flag(request, "-h");
    flags.props = has_flag(request, "-p");
    flags.parsable = has_flag(request, "-P");
    flags.verbosity = has_flag(request, "-v") ? 1 : 0;
    flags.doall = doall;

    zh = lz.open(lz_handle, dataset, ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME);
    if (zh == NULL)
    {
        close(fd);
        g_free(dataset);
        g_free(tosnap);
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    rc = lz.send(zh, fromsnap, tosnap, &flags, fd, NULL, NULL, NULL);

    lz.close(zh);
    close(fd);

    if (rc != 0)
        *outcome = outcome_from_libzfs();
    else
    {
        gchar *message = g_strdup_printf("已写入 %s（libzfs）", request->io_path);

        *outcome = outcome_make(TRUE, message);
        g_free(message);
    }

    g_free(dataset);
    g_free(tosnap);

    return TRUE;
}

static gboolean
try_receive(const ZgOpRequest *request,
            ZgOpOutcome **outcome)
{
    const gchar *target = positional_at(request, 0);
    static const gchar *const allowed[] = {"-F", "-d", "-e", "-n", "-u", "-v"};
    recvflags_t flags = {0};
    nvlist_t *props = NULL;
    int fd;
    int rc;
    guint i;

    if (request->positional->len != 1 ||
        !flags_subset(request, allowed, G_N_ELEMENTS(allowed)) ||
        request->io_path == NULL || request->io_path[0] != '/')
        return FALSE;

    for (i = 0; i < request->options->len; i++)
    {
        const ZgOpOption *option = g_ptr_array_index(request->options, i);

        if (g_strcmp0(option->key, "o") != 0)
            return FALSE;
    }

    if (request->options->len > 0 && !nv_load())
        return FALSE;

    fd = open(request->io_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
    {
        *outcome = outcome_make(FALSE,
                                stream_open_error(request->io_path, errno));
        return TRUE;
    }

    flags.verbose = has_flag(request, "-v");
    flags.force = has_flag(request, "-F");
    flags.dryrun = has_flag(request, "-n");
    flags.nomount = has_flag(request, "-u");
    flags.isprefix = has_flag(request, "-d");
    flags.istail = has_flag(request, "-e");

    if (request->options->len > 0)
    {
        if (nv.alloc(&props, NV_UNIQUE_NAME, 0) != 0)
        {
            close(fd);
            return FALSE;
        }

        for (i = 0; i < request->options->len; i++)
        {
            const ZgOpOption *option = g_ptr_array_index(request->options, i);
            const gchar *equals = strchr(option->value, '=');
            gchar *key;

            if (equals == NULL || equals == option->value)
            {
                nv.free(props);
                close(fd);
                return FALSE;
            }

            key = g_strndup(option->value, (gsize)(equals - option->value));
            if (nv.add_string(props, key, equals + 1) != 0)
            {
                g_free(key);
                nv.free(props);
                close(fd);
                return FALSE;
            }
            g_free(key);
        }
    }

    rc = lz.receive(lz_handle, target, props, &flags, fd, NULL);
    close(fd);

    if (props != NULL)
        nv.free(props);

    if (rc != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "接收完成（libzfs）");

    return TRUE;
}

/* ---- 加密 ------------------------------------------------------------ */

static gboolean
try_load_key(const ZgOpRequest *request,
             ZgOpOutcome **outcome)
{
    const gchar *dataset = positional_at(request, 0);
    static const gchar *const allowed[] = {"-n"};
    const gchar *location = NULL;
    zfs_handle_t *zh;
    guint i;
    int rc;

    if (request->positional->len != 1 ||
        !flags_subset(request, allowed, G_N_ELEMENTS(allowed)))
        return FALSE;

    for (i = 0; i < request->options->len; i++)
    {
        const ZgOpOption *option = g_ptr_array_index(request->options, i);

        if (g_strcmp0(option->key, "L") != 0 || location != NULL)
            return FALSE;

        location = option->value;
    }

    zh = open_dataset(dataset);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    rc = lz.crypto_load_key(zh, has_flag(request, "-n"), location);
    lz.close(zh);

    if (rc != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "密钥已加载（libzfs）");

    return TRUE;
}

static gboolean
try_unload_key(const ZgOpRequest *request,
               ZgOpOutcome **outcome)
{
    const gchar *dataset = positional_at(request, 0);
    zfs_handle_t *zh;
    int rc;

    if (request->positional->len != 1 || request->flags->len > 0 ||
        request->options->len > 0)
        return FALSE;

    zh = open_dataset(dataset);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    rc = lz.crypto_unload_key(zh);
    lz.close(zh);

    if (rc != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "密钥已卸载（libzfs）");

    return TRUE;
}

static gboolean
try_change_key(const ZgOpRequest *request,
               ZgOpOutcome **outcome)
{
    const gchar *dataset = positional_at(request, 0);
    static const gchar *const allowed[] = {"-i"};
    nvlist_t *props = NULL;
    zfs_handle_t *zh;
    guint i;
    int rc;

    if (request->positional->len != 1 ||
        !flags_subset(request, allowed, G_N_ELEMENTS(allowed)))
        return FALSE;

    if (request->options->len > 0)
    {
        if (!nv_load() || nv.alloc(&props, NV_UNIQUE_NAME, 0) != 0)
            return FALSE;

        for (i = 0; i < request->options->len; i++)
        {
            const ZgOpOption *option = g_ptr_array_index(request->options, i);
            const gchar *equals;
            gchar *key;

            if (g_strcmp0(option->key, "o") != 0)
            {
                nv.free(props);
                return FALSE;
            }

            equals = strchr(option->value, '=');
            if (equals == NULL || equals == option->value)
            {
                nv.free(props);
                return FALSE;
            }

            key = g_strndup(option->value, (gsize)(equals - option->value));
            if (nv.add_string(props, key, equals + 1) != 0)
            {
                g_free(key);
                nv.free(props);
                return FALSE;
            }
            g_free(key);
        }
    }

    zh = open_dataset(dataset);
    if (zh == NULL)
    {
        if (props != NULL)
            nv.free(props);
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    rc = lz.crypto_rewrap(zh, props, B_FALSE);
    lz.close(zh);
    if (props != NULL)
        nv.free(props);

    if (rc != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "密钥已更换（libzfs）");

    return TRUE;
}

/* ---- 存储池操作（B2 子集） ------------------------------------------- */

static const gchar *const zg_topologies[] = {"mirror", "raidz1", "raidz2", "raidz3"};

static gboolean
parse_ashift(const gchar *value,
             guint64 *ashift)
{
    const gchar *equals = strchr(value, '=');

    if (equals == NULL || g_str_has_prefix(value, "ashift=") == FALSE)
        return FALSE;

    *ashift = g_ascii_strtoull(equals + 1, NULL, 10);

    return *ashift >= 9 && *ashift <= 16;
}

/* 把位置参数里的设备加入 parent 的 children；ashift 写到每个 disk vdev */
static gboolean
add_disk_children(nvlist_t *parent,
                  const ZgOpRequest *request,
                  guint first,
                  guint64 ashift)
{
    guint n = request->positional->len - first;
    nvlist_t **disks;
    guint i;

    if (n == 0)
        return FALSE;

    disks = g_new0(nvlist_t *, n);

    for (i = 0; i < n; i++)
    {
        const gchar *path = positional_at(request, first + i);

        if (nv.alloc(&disks[i], NV_UNIQUE_NAME, 0) != 0)
            return FALSE;

        nv.add_string(disks[i], "type", "disk");
        nv.add_string(disks[i], "path", path);

        if (ashift != 0)
            nv.add_uint64(disks[i], "ashift", ashift);
    }

    nv.add_nvlist_array(parent, "children", disks, n);
    g_free(disks);
    return TRUE;
}

/* 构建 zpool_create/zpool_add 的 nvroot；返回 FALSE 表示回退 */
static gboolean
build_vdev_root(const ZgOpRequest *request,
                guint first,
                guint64 ashift,
                nvlist_t **root_out)
{
    const gchar *token = positional_at(request, first);
    const gchar *group_type = NULL;
    guint nparity = 0;
    guint dev_start = first;
    nvlist_t *root;
    guint i;

    *root_out = NULL;

    for (i = 0; i < G_N_ELEMENTS(zg_topologies); i++)
    {
        if (g_strcmp0(token, zg_topologies[i]) == 0)
        {
            if (i == 0)
            {
                group_type = "mirror";
            }
            else
            {
                group_type = "raidz";
                nparity = i;
            }

            dev_start = first + 1;
            break;
        }
    }

    if (nv.alloc(&root, NV_UNIQUE_NAME, 0) != 0)
        return FALSE;

    nv.add_string(root, "type", "root");

    if (group_type == NULL)
    {
        if (!add_disk_children(root, request, dev_start, ashift))
        {
            nv.free(root);
            return FALSE;
        }
    }
    else
    {
        nvlist_t *group;

        if (nv.alloc(&group, NV_UNIQUE_NAME, 0) != 0)
        {
            nv.free(root);
            return FALSE;
        }

        nv.add_string(group, "type", group_type);

        if (nparity != 0)
            nv.add_uint64(group, "nparity", nparity);

        if (!add_disk_children(group, request, dev_start, ashift))
        {
            nv.free(group);
            nv.free(root);
            return FALSE;
        }

        {
            nvlist_t *array[1] = {group};

            nv.add_nvlist_array(root, "children", array, 1);
        }
    }

    *root_out = root;
    return TRUE;
}

/* zpool.create 的属性 nvlist；ashift 由调用方写到 vdev */
static gboolean
build_pool_props(const ZgOpRequest *request,
                 guint64 *ashift,
                 nvlist_t **props_out,
                 nvlist_t **fsprops_out)
{
    guint i;
    gboolean any = FALSE;
    gboolean any_fs = FALSE;

    *props_out = NULL;
    *fsprops_out = NULL;
    *ashift = 0;

    if (request->options->len == 0)
        return TRUE;

    if (!nv_load())
        return FALSE;

    if (nv.alloc(props_out, NV_UNIQUE_NAME, 0) != 0)
        return FALSE;

    if (nv.alloc(fsprops_out, NV_UNIQUE_NAME, 0) != 0)
    {
        nv.free(*props_out);
        *props_out = NULL;
        return FALSE;
    }

    for (i = 0; i < request->options->len; i++)
    {
        const ZgOpOption *option = g_ptr_array_index(request->options, i);
        const gchar *equals;
        gchar *key;
        const gchar *value;

        if (g_strcmp0(option->key, "m") == 0)
        {
            if (nv.add_string(*fsprops_out, "mountpoint", option->value) != 0)
            {
                nv.free(*props_out);
                nv.free(*fsprops_out);
                *props_out = NULL;
                *fsprops_out = NULL;
                return FALSE;
            }

            any_fs = TRUE;
            continue;
        }

        equals = strchr(option->value, '=');

        if (equals == NULL || equals == option->value)
        {
            nv.free(*props_out);
            nv.free(*fsprops_out);
            *props_out = NULL;
            *fsprops_out = NULL;
            return FALSE;
        }

        key = g_strndup(option->value, (gsize)(equals - option->value));
        value = equals + 1;

        if (g_strcmp0(option->key, "o") == 0 && g_strcmp0(key, "ashift") == 0)
        {
            if (!parse_ashift(option->value, ashift))
            {
                g_free(key);
                nv.free(*props_out);
                *props_out = NULL;
                return FALSE;
            }
        }
        else if (g_strcmp0(option->key, "o") == 0)
        {
            if (nv.add_string(*props_out, key, value) != 0)
            {
                g_free(key);
                nv.free(*props_out);
                nv.free(*fsprops_out);
                *props_out = NULL;
                *fsprops_out = NULL;
                return FALSE;
            }

            any = TRUE;
        }
        else if (g_strcmp0(option->key, "O") == 0)
        {
            if (nv.add_string(*fsprops_out, key, value) != 0)
            {
                g_free(key);
                nv.free(*props_out);
                nv.free(*fsprops_out);
                *props_out = NULL;
                *fsprops_out = NULL;
                return FALSE;
            }

            any_fs = TRUE;
        }
        else
        {
            g_free(key);
            nv.free(*props_out);
            nv.free(*fsprops_out);
            *props_out = NULL;
            *fsprops_out = NULL;
            return FALSE;
        }

        g_free(key);
    }

    if (!any)
    {
        nv.free(*props_out);
        *props_out = NULL;
    }

    if (!any_fs)
    {
        nv.free(*fsprops_out);
        *fsprops_out = NULL;
    }

    return TRUE;
}

static gboolean
try_pool_destroy(const ZgOpRequest *request,
                 ZgOpOutcome **outcome)
{
    const gchar *pool = positional_at(request, 0);
    zpool_handle_t *zh;

    if (request->positional->len != 1 || request->flags->len > 0)
        return FALSE;

    zh = lz.pool_open(lz_handle, pool);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    if (lz.pool_destroy(zh, NULL) != 0)
    {
        lz.pool_close(zh);
        *outcome = outcome_from_libzfs();
    }
    else
    {
        *outcome = outcome_make(TRUE, "存储池已销毁（libzfs）");
    }

    return TRUE;
}

static gboolean
try_pool_export(const ZgOpRequest *request,
                ZgOpOutcome **outcome)
{
    const gchar *pool = positional_at(request, 0);
    static const gchar *const allowed[] = {"-f"};
    zpool_handle_t *zh;

    if (request->positional->len != 1 ||
        !flags_subset(request, allowed, G_N_ELEMENTS(allowed)))
        return FALSE;

    zh = lz.pool_open(lz_handle, pool);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    if (lz.pool_export(zh, has_flag(request, "-f"), NULL) != 0)
    {
        lz.pool_close(zh);
        *outcome = outcome_from_libzfs();
    }
    else
    {
        *outcome = outcome_make(TRUE, "存储池已导出（libzfs）");
    }

    return TRUE;
}

static gboolean
try_pool_scrub(const ZgOpRequest *request,
               ZgOpOutcome **outcome)
{
    const gchar *pool = positional_at(request, 0);
    static const gchar *const allowed[] = {"-s", "-p"};
    zpool_handle_t *zh;
    pool_scan_func_t func;
    pool_scrub_cmd_t cmd;
    int rc;

    if (request->positional->len != 1 || request->options->len > 0 ||
        !flags_subset(request, allowed, G_N_ELEMENTS(allowed)))
        return FALSE;

    if (has_flag(request, "-s"))
    {
        func = POOL_SCAN_NONE;
        cmd = POOL_SCRUB_NORMAL;
    }
    else
    {
        func = POOL_SCAN_SCRUB;
        cmd = has_flag(request, "-p") ? POOL_SCRUB_PAUSE : POOL_SCRUB_NORMAL;
    }

    zh = lz.pool_open(lz_handle, pool);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    rc = lz.pool_scan(zh, func, cmd);
    lz.pool_close(zh);

    if (rc != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "scrub 请求已提交（libzfs）");

    return TRUE;
}

static gboolean
try_pool_trim(const ZgOpRequest *request,
              ZgOpOutcome **outcome)
{
    const gchar *pool = positional_at(request, 0);
    static const gchar *const allowed[] = {"-c", "-d", "-s", "-w"};
    pool_trim_func_t func;
    trimflags_t flags = {0};
    zpool_handle_t *zh;
    int rc;
    guint i;

    if (request->positional->len != 1 ||
        !flags_subset(request, allowed, G_N_ELEMENTS(allowed)))
        return FALSE;

    for (i = 0; i < request->options->len; i++)
    {
        const ZgOpOption *option = g_ptr_array_index(request->options, i);

        if (g_strcmp0(option->key, "r") != 0)
            return FALSE;

        flags.rate = g_ascii_strtoull(option->value, NULL, 10);
        if (flags.rate == 0)
            return FALSE;
    }

    if (has_flag(request, "-c"))
        func = POOL_TRIM_CANCEL;
    else if (has_flag(request, "-s"))
        func = POOL_TRIM_SUSPEND;
    else
        func = POOL_TRIM_START;

    flags.fullpool = B_TRUE;
    flags.secure = has_flag(request, "-d");
    flags.wait = has_flag(request, "-w");

    zh = lz.pool_open(lz_handle, pool);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    rc = lz.pool_trim(zh, func, NULL, &flags);
    lz.pool_close(zh);

    if (rc != 0)
        *outcome = outcome_from_libzfs();
    else if (func == POOL_TRIM_CANCEL)
        *outcome = outcome_make(TRUE, "TRIM 已取消（libzfs）");
    else if (func == POOL_TRIM_SUSPEND)
        *outcome = outcome_make(TRUE, "TRIM 已暂停（libzfs）");
    else
        *outcome = outcome_make(TRUE, "TRIM 请求已提交（libzfs）");

    return TRUE;
}

static gboolean
try_pool_online(const ZgOpRequest *request,
                ZgOpOutcome **outcome)
{
    const gchar *pool = positional_at(request, 0);
    const gchar *device = positional_at(request, 1);
    zpool_handle_t *zh;
    vdev_state_t state;

    if (request->positional->len != 2 || request->flags->len > 0 ||
        request->options->len > 0)
        return FALSE;

    zh = lz.pool_open(lz_handle, pool);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    if (lz.vdev_online(zh, device, 0, &state) != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "设备已上线（libzfs）");

    lz.pool_close(zh);
    return TRUE;
}

static gboolean
try_pool_offline(const ZgOpRequest *request,
                 ZgOpOutcome **outcome)
{
    const gchar *pool = positional_at(request, 0);
    const gchar *device = positional_at(request, 1);
    static const gchar *const allowed[] = {"-t"};
    zpool_handle_t *zh;

    if (request->positional->len != 2 ||
        !flags_subset(request, allowed, G_N_ELEMENTS(allowed)) ||
        request->options->len > 0)
        return FALSE;

    zh = lz.pool_open(lz_handle, pool);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    if (lz.vdev_offline(zh, device, has_flag(request, "-t")) != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "设备已下线（libzfs）");

    lz.pool_close(zh);
    return TRUE;
}

static gboolean
try_pool_create(const ZgOpRequest *request,
                ZgOpOutcome **outcome)
{
    const gchar *pool = positional_at(request, 0);
    guint64 ashift = 0;
    nvlist_t *root = NULL;
    nvlist_t *props = NULL;
    nvlist_t *fsprops = NULL;
    int rc;

    if (request->positional->len < 2 || request->flags->len > 0)
        return FALSE;

    /* -O/-m 等文件系统属性暂由 CLI 处理，避免语义差异 */
    {
        guint i;

        for (i = 0; i < request->options->len; i++)
        {
            const ZgOpOption *option = g_ptr_array_index(request->options, i);

            if (g_strcmp0(option->key, "o") != 0)
                return FALSE;
        }
    }

    if (!nv_load() || !build_pool_props(request, &ashift, &props, &fsprops))
        return FALSE;

    if (!build_vdev_root(request, 1, ashift, &root))
    {
        if (props != NULL)
            nv.free(props);
        if (fsprops != NULL)
            nv.free(fsprops);
        return FALSE;
    }

    rc = lz.pool_create(lz_handle, pool, root, props, fsprops);

    nv.free(root);
    if (props != NULL)
        nv.free(props);
    if (fsprops != NULL)
        nv.free(fsprops);

    if (rc != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "存储池已创建（libzfs）");

    return TRUE;
}

static gboolean
try_pool_add(const ZgOpRequest *request,
             ZgOpOutcome **outcome)
{
    const gchar *pool = positional_at(request, 0);
    guint64 ashift = 0;
    nvlist_t *root = NULL;
    zpool_handle_t *zh;
    int rc;

    if (request->positional->len < 2 || request->flags->len > 0)
        return FALSE;

    if (!nv_load())
        return FALSE;

    if (request->options->len > 0)
    {
        /* zpool add 只支持 -o ashift=... */
        guint i;

        for (i = 0; i < request->options->len; i++)
        {
            const ZgOpOption *option = g_ptr_array_index(request->options, i);

            if (g_strcmp0(option->key, "o") != 0 ||
                !g_str_has_prefix(option->value, "ashift=") ||
                !parse_ashift(option->value, &ashift))
                return FALSE;
        }
    }

    if (!build_vdev_root(request, 1, ashift, &root))
        return FALSE;

    zh = lz.pool_open(lz_handle, pool);
    if (zh == NULL)
    {
        nv.free(root);
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    rc = lz.pool_add(zh, root, B_FALSE);
    lz.pool_close(zh);
    nv.free(root);

    if (rc != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "vdev 已添加（libzfs）");

    return TRUE;
}

static gboolean
try_pool_remove(const ZgOpRequest *request,
                ZgOpOutcome **outcome)
{
    const gchar *pool = positional_at(request, 0);
    const gchar *device = positional_at(request, 1);
    zpool_handle_t *zh;

    if (request->positional->len != 2 || request->flags->len > 0 ||
        request->options->len > 0)
        return FALSE;

    zh = lz.pool_open(lz_handle, pool);
    if (zh == NULL)
    {
        *outcome = outcome_from_libzfs();
        return TRUE;
    }

    if (lz.vdev_remove(zh, device) != 0)
        *outcome = outcome_from_libzfs();
    else
        *outcome = outcome_make(TRUE, "设备已移除（libzfs）");

    lz.pool_close(zh);
    return TRUE;
}

static int
capsicum_warmup_cb(zpool_handle_t *zh,
                   void *data)
{
    (void)data;
    lz.pool_close(zh);
    return 1;
}

gboolean
zg_libzfs_exec_capsicum_prepare(void)
{
    cap_rights_t rights;
    int fd;

    if (injected_fd >= 0)
        return TRUE;

    fd = open("/dev/zfs", O_RDWR | O_EXCL | O_CLOEXEC);
    if (fd < 0)
    {
        zg_log_warning("capsicum: 打开 /dev/zfs 失败：%s", g_strerror(errno));
        return FALSE;
    }

    cap_rights_init(&rights, CAP_IOCTL, CAP_FSTAT, CAP_EVENT);
    if (cap_rights_limit(fd, &rights) != 0)
    {
        zg_log_warning("capsicum: cap_rights_limit 失败：%s", g_strerror(errno));
        close(fd);
        return FALSE;
    }

    injected_fd = fd;

    if (!lz_load())
    {
        close(fd);
        injected_fd = -1;
        return FALSE;
    }

    /* 预热 ioctl：libzfs_core 会读取 vfs.zfs.version.ioctl（sysctl） */
    if (lz.zpool_iter(lz_handle, capsicum_warmup_cb, NULL) < 0)
    {
        zg_log_warning("capsicum: libzfs 预热失败");
        return FALSE;
    }

    zg_log_info("capsicum: libzfs fd 注入完成，/dev/zfs fd=%d", fd);
    return TRUE;
}

gboolean
zg_libzfs_exec_capsicum_supports(const gchar *op_id)
{
    static const gchar *const allowed[] = {
            "zfs.create",
            "zfs.destroy",
            "zfs.rename",
            "zfs.set",
            "zfs.snapshot",
            "zfs.rollback",
            "zfs.clone",
            "zpool.destroy",
            "zpool.export",
            "zpool.scrub",
            "zpool.trim",
            "zpool.online",
            "zpool.offline",
            "zpool.remove",
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(allowed); i++)
    {
        if (g_strcmp0(op_id, allowed[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

gboolean
zg_libzfs_exec_try(const ZgOpDef *op,
                   const ZgOpRequest *request,
                   ZgOpOutcome **outcome)
{
    if (outcome == NULL)
        return FALSE;

    *outcome = NULL;

    if (op == NULL || request == NULL || !exec_enabled())
        return FALSE;

    if (!lz_load())
        return FALSE;

    if (g_strcmp0(op->id, "zfs.snapshot") == 0)
        return try_snapshot(request, outcome);
    if (g_strcmp0(op->id, "zfs.create") == 0)
        return try_create(request, outcome);
    if (g_strcmp0(op->id, "zfs.destroy") == 0)
        return try_destroy(request, outcome);
    if (g_strcmp0(op->id, "zfs.rename") == 0)
        return try_rename(request, outcome);
    if (g_strcmp0(op->id, "zfs.set") == 0)
        return try_set(request, outcome);
    if (g_strcmp0(op->id, "zfs.rollback") == 0)
        return try_rollback(request, outcome);
    if (g_strcmp0(op->id, "zfs.clone") == 0)
        return try_clone(request, outcome);
    if (g_strcmp0(op->id, "zfs.load-key") == 0)
        return try_load_key(request, outcome);
    if (g_strcmp0(op->id, "zfs.unload-key") == 0)
        return try_unload_key(request, outcome);
    if (g_strcmp0(op->id, "zfs.change-key") == 0)
        return try_change_key(request, outcome);
    if (g_strcmp0(op->id, "zfs.send") == 0)
        return try_send(request, outcome);
    if (g_strcmp0(op->id, "zfs.receive") == 0)
        return try_receive(request, outcome);

    if (g_strcmp0(op->id, "zpool.create") == 0)
        return try_pool_create(request, outcome);
    if (g_strcmp0(op->id, "zpool.add") == 0)
        return try_pool_add(request, outcome);
    if (g_strcmp0(op->id, "zpool.remove") == 0)
        return try_pool_remove(request, outcome);
    if (g_strcmp0(op->id, "zpool.destroy") == 0)
        return try_pool_destroy(request, outcome);
    if (g_strcmp0(op->id, "zpool.export") == 0)
        return try_pool_export(request, outcome);
    if (g_strcmp0(op->id, "zpool.scrub") == 0)
        return try_pool_scrub(request, outcome);
    if (g_strcmp0(op->id, "zpool.trim") == 0)
        return try_pool_trim(request, outcome);
    if (g_strcmp0(op->id, "zpool.online") == 0)
        return try_pool_online(request, outcome);
    if (g_strcmp0(op->id, "zpool.offline") == 0)
        return try_pool_offline(request, outcome);

    return FALSE;
}

#else /* !HAVE_LIBZFS */

gboolean
zg_libzfs_exec_capsicum_prepare(void)
{
    return FALSE;
}

gboolean
zg_libzfs_exec_capsicum_supports(const gchar *op_id)
{
    (void)op_id;
    return FALSE;
}

gboolean
zg_libzfs_exec_try(const ZgOpDef *op,
                   const ZgOpRequest *request,
                   ZgOpOutcome **outcome)
{
    (void)op;
    (void)request;
    (void)outcome;
    return FALSE;
}

#endif
