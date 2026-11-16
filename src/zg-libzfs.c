/* zg-libzfs.c - 可选的 libzfs 只读适配层（运行时 dlopen）
 *
 * 不在二进制中直接依赖 libzfs：启动后尝试 dlopen("libzfs.so.7")，
 * 成功且符号齐全才启用；否则回退守护进程。这样打包出的 GUI 可以在
 * 没有 openzfs 开发库的机器上直接运行。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-libzfs.h"

#include "zg-util.h"

#ifdef HAVE_LIBZFS

#include <dlfcn.h>
#include <libzfs.h>
#include <string.h>

typedef struct
{
    void *dl;
    libzfs_handle_t *(*init)(void);
    void (*fini)(libzfs_handle_t *);
    int (*zpool_iter)(libzfs_handle_t *, zpool_iter_f, void *);
    void (*zpool_close)(zpool_handle_t *);
    const char *(*zpool_get_name)(zpool_handle_t *);
    const char *(*zpool_get_state_str)(zpool_handle_t *);
    uint64_t (*zpool_get_prop_int)(zpool_handle_t *, zpool_prop_t, zprop_source_t *);
    int (*zpool_get_prop)(zpool_handle_t *, zpool_prop_t, char *, size_t,
                          zprop_source_t *, boolean_t);
    zfs_handle_t *(*zfs_open)(libzfs_handle_t *, const char *, int);
    int (*zfs_iter_root)(libzfs_handle_t *, zfs_iter_f, void *);
    int (*zfs_iter_children)(zfs_handle_t *, zfs_iter_f, void *);
    int (*zfs_iter_snapshots)(zfs_handle_t *, boolean_t, zfs_iter_f, void *, uint64_t,
                              uint64_t);
    void (*zfs_close)(zfs_handle_t *);
    const char *(*zfs_get_name)(const zfs_handle_t *);
    zfs_type_t (*zfs_get_type)(const zfs_handle_t *);
    uint64_t (*zfs_prop_get_int)(zfs_handle_t *, zfs_prop_t);
    int (*zfs_prop_get)(zfs_handle_t *, zfs_prop_t, char *, size_t,
                        zprop_source_t *, char *, size_t, boolean_t);
    nvlist_t *(*zpool_get_config)(zpool_handle_t *, nvlist_t **);
    nvlist_t *(*zfs_get_all_props)(zfs_handle_t *);
    int (*zprop_iter)(zprop_func, void *, boolean_t, boolean_t, zfs_type_t);
    const char *(*zfs_prop_to_name)(zfs_prop_t);
    nvlist_t *(*zfs_get_user_props)(zfs_handle_t *);
    int (*zprop_get_list)(libzfs_handle_t *, char *, zprop_list_t **, zfs_type_t);
    int (*zfs_expand_proplist)(zfs_handle_t *, zprop_list_t **, boolean_t,
                               boolean_t);
    void (*zprop_free_list)(zprop_list_t *);
    zpool_status_t (*zpool_get_status)(zpool_handle_t *, const char **, int *);
} LzApi;

static LzApi lz;
static libzfs_handle_t *zg_handle;
static gboolean zg_probed;
static gboolean zg_usable;

static gboolean
lz_load(void)
{
    const gchar *override = g_getenv("ZFS_GUI_LIBZFS");
    const gchar *candidates[] = {"libzfs.so.7", NULL};
    guint i;

    if (lz.dl != NULL)
        return TRUE;

    for (i = 0; override != NULL ? i == 0 : candidates[i] != NULL; i++)
    {
        const gchar *name = override != NULL ? override : candidates[i];

        lz.dl = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (lz.dl != NULL)
            break;
    }

    if (lz.dl == NULL)
        return FALSE;

#define LZ_SYM(field, symbol)                         \
    do                                                \
    {                                                 \
        *(void **)(&lz.field) = dlsym(lz.dl, symbol); \
        if (lz.field == NULL)                         \
        {                                             \
            dlclose(lz.dl);                           \
            memset(&lz, 0, sizeof(lz));               \
            return FALSE;                             \
        }                                             \
    } while (0)

    LZ_SYM(init, "libzfs_init");
    LZ_SYM(fini, "libzfs_fini");
    LZ_SYM(zpool_iter, "zpool_iter");
    LZ_SYM(zpool_close, "zpool_close");
    LZ_SYM(zpool_get_name, "zpool_get_name");
    LZ_SYM(zpool_get_state_str, "zpool_get_state_str");
    LZ_SYM(zpool_get_prop_int, "zpool_get_prop_int");
    LZ_SYM(zpool_get_prop, "zpool_get_prop");
    LZ_SYM(zfs_open, "zfs_open");
    LZ_SYM(zfs_iter_root, "zfs_iter_root");
    LZ_SYM(zfs_iter_children, "zfs_iter_children");
    LZ_SYM(zfs_iter_snapshots, "zfs_iter_snapshots");
    LZ_SYM(zfs_close, "zfs_close");
    LZ_SYM(zfs_get_name, "zfs_get_name");
    LZ_SYM(zfs_get_type, "zfs_get_type");
    LZ_SYM(zfs_prop_get_int, "zfs_prop_get_int");
    LZ_SYM(zfs_prop_get, "zfs_prop_get");
    LZ_SYM(zpool_get_config, "zpool_get_config");
    LZ_SYM(zfs_get_all_props, "zfs_get_all_props");
    LZ_SYM(zprop_iter, "zprop_iter");
    LZ_SYM(zfs_prop_to_name, "zfs_prop_to_name");
    LZ_SYM(zfs_get_user_props, "zfs_get_user_props");
    LZ_SYM(zprop_get_list, "zprop_get_list");
    LZ_SYM(zfs_expand_proplist, "zfs_expand_proplist");
    LZ_SYM(zprop_free_list, "zprop_free_list");
    LZ_SYM(zpool_get_status, "zpool_get_status");

#undef LZ_SYM

    return TRUE;
}

static int
probe_cb(zpool_handle_t *zh,
         void *data)
{
    (void)data;
    lz.zpool_close(zh);
    return 1;
}

gboolean
zg_libzfs_available(void)
{
    if (zg_probed)
        return zg_usable;

    zg_probed = TRUE;

    if (g_getenv("ZFS_GUI_NO_LIBZFS") != NULL)
        return FALSE;

    if (!lz_load())
        return FALSE;

    zg_handle = lz.init();
    if (zg_handle == NULL)
        return FALSE;

    if (lz.zpool_iter(zg_handle, probe_cb, NULL) < 0)
    {
        lz.fini(zg_handle);
        zg_handle = NULL;
        return FALSE;
    }

    zg_usable = TRUE;
    return TRUE;
}

static int
pool_add_cb(zpool_handle_t *zh,
            void *data)
{
    GPtrArray *pools = data;
    ZgPool *pool = g_new0(ZgPool, 1);
    gchar buf[ZFS_MAXPROPLEN];

    pool->name = g_strdup(lz.zpool_get_name(zh));
    pool->size = lz.zpool_get_prop_int(zh, ZPOOL_PROP_SIZE, NULL);
    pool->alloc = lz.zpool_get_prop_int(zh, ZPOOL_PROP_ALLOCATED, NULL);
    pool->free = lz.zpool_get_prop_int(zh, ZPOOL_PROP_FREE, NULL);
    pool->checkpoint = lz.zpool_get_prop_int(zh, ZPOOL_PROP_CHECKPOINT, NULL);
    pool->capacity = (gint)lz.zpool_get_prop_int(zh, ZPOOL_PROP_CAPACITY, NULL);
    pool->fragmentation =
            (gint)lz.zpool_get_prop_int(zh, ZPOOL_PROP_FRAGMENTATION, NULL);
    pool->health = g_strdup(lz.zpool_get_state_str(zh));
    pool->dedup_ratio = 1.0;

    if (lz.zpool_get_prop(zh, ZPOOL_PROP_DEDUPRATIO, buf, sizeof(buf), NULL,
                          B_FALSE) == 0)
        sscanf(buf, "%lf", &pool->dedup_ratio);

    if (lz.zpool_get_prop(zh, ZPOOL_PROP_ALTROOT, buf, sizeof(buf), NULL,
                          B_FALSE) == 0)
        pool->altroot = g_strdup(buf);

    pool->guid = g_strdup_printf(
            "%" G_GUINT64_FORMAT,
            lz.zpool_get_prop_int(zh, ZPOOL_PROP_GUID, NULL));

    g_ptr_array_add(pools, pool);
    lz.zpool_close(zh);
    return 0;
}

GPtrArray *
zg_libzfs_pools(void)
{
    GPtrArray *pools;

    if (!zg_libzfs_available())
        return NULL;

    pools = g_ptr_array_new_with_free_func((GDestroyNotify)zg_model_pool_free);

    if (lz.zpool_iter(zg_handle, pool_add_cb, pools) != 0)
    {
        g_ptr_array_free(pools, TRUE);
        return NULL;
    }

    return pools;
}

static void
dataset_add(zfs_handle_t *zh,
            GPtrArray *datasets)
{
    ZgDataset *dataset = g_new0(ZgDataset, 1);
    gchar buf[ZFS_MAXPROPLEN];

    dataset->name = g_strdup(lz.zfs_get_name(zh));
    dataset->type = g_strdup(lz.zfs_get_type(zh) == ZFS_TYPE_VOLUME ? "volume"
                                                                    : "filesystem");
    dataset->used = lz.zfs_prop_get_int(zh, ZFS_PROP_USED);
    dataset->avail = lz.zfs_prop_get_int(zh, ZFS_PROP_AVAILABLE);
    dataset->refer = lz.zfs_prop_get_int(zh, ZFS_PROP_REFERENCED);
    dataset->creation = (gint64)lz.zfs_prop_get_int(zh, ZFS_PROP_CREATION);
    dataset->mounted = lz.zfs_prop_get_int(zh, ZFS_PROP_MOUNTED) != 0;

    if (lz.zfs_prop_get(zh, ZFS_PROP_MOUNTPOINT, buf, sizeof(buf), NULL, NULL,
                        0, B_TRUE) == 0)
        dataset->mountpoint = g_strdup(buf);
    else
        dataset->mountpoint = g_strdup("none");

    g_ptr_array_add(datasets, dataset);
}

static int
dataset_iter_cb(zfs_handle_t *zh,
                void *data)
{
    GPtrArray *datasets = data;
    zfs_type_t type = lz.zfs_get_type(zh);

    /* zfs_iter_children 同时返回快照，快照由快照页管理 */
    if (type == ZFS_TYPE_FILESYSTEM || type == ZFS_TYPE_VOLUME)
        dataset_add(zh, datasets);

    if (type == ZFS_TYPE_FILESYSTEM)
        lz.zfs_iter_children(zh, dataset_iter_cb, datasets);

    lz.zfs_close(zh);
    return 0;
}

GPtrArray *
zg_libzfs_datasets(void)
{
    GPtrArray *datasets;

    if (!zg_libzfs_available())
        return NULL;

    datasets = g_ptr_array_new_with_free_func(
            (GDestroyNotify)zg_model_dataset_free);

    if (lz.zfs_iter_root(zg_handle, dataset_iter_cb, datasets) != 0)
    {
        g_ptr_array_free(datasets, TRUE);
        return NULL;
    }

    return datasets;
}

typedef struct
{
    void *dl;
    int (*lookup_nvlist)(nvlist_t *, const char *, nvlist_t **);
    int (*lookup_nvlist_array)(nvlist_t *, const char *, nvlist_t ***, uint_t *);
    int (*lookup_uint64)(nvlist_t *, const char *, uint64_t *);
    int (*lookup_uint64_array)(nvlist_t *, const char *, uint64_t **, uint_t *);
    int (*lookup_string)(nvlist_t *, const char *, const char **);
    nvpair_t *(*next_nvpair)(nvlist_t *, const nvpair_t *);
    const char *(*nvpair_name)(const nvpair_t *);
    data_type_t (*nvpair_type)(const nvpair_t *);
    int (*nvpair_value_nvlist)(const nvpair_t *, nvlist_t **);
    int (*nvpair_value_string)(const nvpair_t *, const char **);
} LzLookup;

static LzLookup nvl;
static gboolean nvl_loaded;
static gboolean nvl_ok;

static gboolean
nvl_load(void)
{
    if (nvl_loaded)
        return nvl_ok;

    nvl_loaded = TRUE;
    nvl.dl = dlopen("libnvpair.so.3", RTLD_NOW | RTLD_LOCAL);

    if (nvl.dl == NULL)
        return FALSE;

#define NVL_SYM(field, symbol)                          \
    do                                                  \
    {                                                   \
        *(void **)(&nvl.field) = dlsym(nvl.dl, symbol); \
        if (nvl.field == NULL)                          \
        {                                               \
            dlclose(nvl.dl);                            \
            memset(&nvl, 0, sizeof(nvl));               \
            return FALSE;                               \
        }                                               \
    } while (0)

    NVL_SYM(lookup_nvlist, "nvlist_lookup_nvlist");
    NVL_SYM(lookup_nvlist_array, "nvlist_lookup_nvlist_array");
    NVL_SYM(lookup_uint64, "nvlist_lookup_uint64");
    NVL_SYM(lookup_uint64_array, "nvlist_lookup_uint64_array");
    NVL_SYM(lookup_string, "nvlist_lookup_string");
    NVL_SYM(next_nvpair, "nvlist_next_nvpair");
    NVL_SYM(nvpair_name, "nvpair_name");
    NVL_SYM(nvpair_type, "nvpair_type");
    NVL_SYM(nvpair_value_nvlist, "nvpair_value_nvlist");
    NVL_SYM(nvpair_value_string, "nvpair_value_string");

#undef NVL_SYM

    nvl_ok = TRUE;
    return TRUE;
}

static const gchar *
vdev_state_name(uint64_t state)
{
    switch ((vdev_state_t)state)
    {
    case VDEV_STATE_CLOSED:
        return "CLOSED";
    case VDEV_STATE_OFFLINE:
        return "OFFLINE";
    case VDEV_STATE_REMOVED:
        return "REMOVED";
    case VDEV_STATE_CANT_OPEN:
        return "UNAVAIL";
    case VDEV_STATE_FAULTED:
        return "FAULTED";
    case VDEV_STATE_DEGRADED:
        return "DEGRADED";
    case VDEV_STATE_HEALTHY:
        return "ONLINE";
    default:
        return "UNKNOWN";
    }
}

static const gchar *
scan_func_name(uint64_t func)
{
    switch ((pool_scan_func_t)func)
    {
    case POOL_SCAN_SCRUB:
        return "scrub";
    case POOL_SCAN_RESILVER:
        return "resilver";
    case POOL_SCAN_ERRORSCRUB:
        return "error scrub";
    default:
        return "scan";
    }
}

static gchar *
format_time(uint64_t when)
{
    time_t t = (time_t)when;
    struct tm tm;
    gchar buf[64];

    if (when == 0 || localtime_r(&t, &tm) == NULL)
        return g_strdup("—");

    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return g_strdup(buf);
}

static gchar *
format_scan(nvlist_t *cfg)
{
    uint64_t *stats;
    uint_t count;
    pool_scan_stat_t scan;
    gchar *examined;
    gchar *total;
    gchar *message;

    if (cfg == NULL || nvl.lookup_uint64_array(cfg, "scan_stats", &stats, &count) != 0 ||
        count * sizeof(uint64_t) < sizeof(scan))
        return g_strdup("—");

    memcpy(&scan, stats, sizeof(scan));

    if (scan.pss_state == DSS_NONE)
        return g_strdup("—");

    examined = zg_format_bytes(scan.pss_examined);
    total = zg_format_bytes(scan.pss_to_examine);

    if (scan.pss_state == DSS_SCANNING)
    {
        message = g_strdup_printf("%s 进行中：已检查 %s / %s",
                                  scan_func_name(scan.pss_func), examined, total);
    }
    else if (scan.pss_state == DSS_FINISHED)
    {
        gchar *ended = format_time(scan.pss_end_time);

        message = g_strdup_printf("%s 完成：检查 %s，错误 %" G_GUINT64_FORMAT "，结束于 %s",
                                  scan_func_name(scan.pss_func), examined,
                                  scan.pss_errors, ended);
        g_free(ended);
    }
    else if (scan.pss_state == DSS_CANCELED)
    {
        message = g_strdup_printf("%s 已取消", scan_func_name(scan.pss_func));
    }
    else
    {
        message = g_strdup("—");
    }

    g_free(examined);
    g_free(total);
    return message;
}

static gchar *
vdev_child_name(nvlist_t *parent,
                guint index,
                const gchar *pool)
{
    nvlist_t **children;
    uint_t count;
    const gchar *type = NULL;
    const gchar *path = NULL;
    guint same = 0;
    guint i;
    uint64_t nparity = 0;

    if (parent == NULL ||
        nvl.lookup_nvlist_array(parent, "children", &children, &count) != 0 ||
        index >= count)
        return g_strdup(pool);

    (void)nvl.lookup_string(children[index], "type", &type);
    (void)nvl.lookup_string(children[index], "path", &path);

    for (i = 0; i < index; i++)
    {
        const gchar *other = NULL;

        if (nvl.lookup_string(children[i], "type", &other) == 0 &&
            g_strcmp0(other, type) == 0)
            same++;
    }

    if (type == NULL)
        return g_strdup(pool);

    if (g_strcmp0(type, "disk") == 0 || g_strcmp0(type, "file") == 0)
    {
        if (path != NULL)
            return g_path_get_basename(path);
        return g_strdup(type);
    }

    if (g_strcmp0(type, "mirror") == 0)
        return g_strdup_printf("mirror-%u", same);

    if (g_strcmp0(type, "raidz") == 0)
    {
        (void)nvl.lookup_uint64(children[index], "nparity", &nparity);
        return g_strdup_printf("raidz%" G_GUINT64_FORMAT "-%u", nparity, same);
    }

    if (g_strcmp0(type, "spare") == 0)
        return g_strdup_printf("spare-%u", same);

    if (path != NULL)
        return g_path_get_basename(path);

    return g_strdup(type);
}

static void vdev_parse(nvlist_t *vdev,
                       ZgVdev *node,
                       nvlist_t *parent,
                       guint index,
                       const gchar *pool,
                       guint depth);

static void
vdev_fill_stats(nvlist_t *vdev,
                ZgVdev *node)
{
    uint64_t *stats;
    uint_t count;
    vdev_stat_t vs;

    if (nvl.lookup_uint64_array(vdev, "vdev_stats", &stats, &count) != 0 ||
        count * sizeof(uint64_t) < sizeof(vs))
    {
        node->state = g_strdup("—");
        return;
    }

    memcpy(&vs, stats, sizeof(vs));

    node->state = g_strdup(vdev_state_name(vs.vs_state));
    node->read_errors = vs.vs_read_errors;
    node->write_errors = vs.vs_write_errors;
    node->checksum_errors = vs.vs_checksum_errors;
    node->alloc_space = zg_format_bytes(vs.vs_alloc);
    node->total_space = zg_format_bytes(vs.vs_space);
}

static void
vdev_parse(nvlist_t *vdev,
           ZgVdev *node,
           nvlist_t *parent,
           guint index,
           const gchar *pool,
           guint depth)
{
    nvlist_t **children;
    uint_t count;
    const gchar *type = NULL;
    const gchar *path = NULL;
    uint64_t is_log = 0;
    guint i;

    node->depth = depth;
    node->name = depth == 0 ? g_strdup(pool)
                            : vdev_child_name(parent, index, pool);
    node->children = g_ptr_array_new_with_free_func((GDestroyNotify)zg_model_vdev_free);

    if (nvl.lookup_string(vdev, "type", &type) == 0)
        node->type = g_strdup(type);

    if (nvl.lookup_string(vdev, "path", &path) == 0 && path != NULL)
        node->path = g_strdup(path);

    if (nvl.lookup_uint64(vdev, "is_log", &is_log) == 0 && is_log != 0)
        node->vdev_class = g_strdup("log");

    vdev_fill_stats(vdev, node);

    if (nvl.lookup_nvlist_array(vdev, "children", &children, &count) != 0)
        return;

    for (i = 0; i < count; i++)
    {
        ZgVdev *child = g_new0(ZgVdev, 1);

        vdev_parse(children[i], child, vdev, i, pool, depth + 1);
        g_ptr_array_add(node->children, child);
    }
}

static int
pool_status_add_cb(zpool_handle_t *zh,
                   void *data)
{
    GPtrArray *statuses = data;
    ZgPoolStatus *status = g_new0(ZgPoolStatus, 1);
    nvlist_t *cfg = lz.zpool_get_config(zh, NULL);
    nvlist_t *tree = NULL;
    const gchar *message = NULL;
    uint64_t value = 0;
    int error = 0;

    status->name = g_strdup(lz.zpool_get_name(zh));
    status->state = g_strdup(lz.zpool_get_state_str(zh));

    (void)lz.zpool_get_status(zh, &message, &error);

    if (message != NULL && message[0] != '\0')
        status->status_note = g_strdup(message);

    if (nvl.lookup_uint64(cfg, "txg", &value) == 0)
        status->txg = g_strdup_printf("%" G_GUINT64_FORMAT, value);

    if (nvl.lookup_uint64(cfg, "version", &value) == 0)
        status->spa_version = g_strdup_printf("%" G_GUINT64_FORMAT, value);

    status->scan = format_scan(cfg);

    if (nvl.lookup_nvlist(cfg, "vdev_tree", &tree) == 0)
    {
        ZgVdev *root = g_new0(ZgVdev, 1);

        vdev_parse(tree, root, NULL, 0, status->name, 0);
        status->root = root;

        if (root->read_errors != 0 || root->write_errors != 0 ||
            root->checksum_errors != 0)
            status->errors = g_strdup_printf(
                    "检测到 %" G_GUINT64_FORMAT " 个读/写/校验错误",
                    root->read_errors + root->write_errors + root->checksum_errors);
        else
            status->errors = g_strdup("无已知数据错误");
    }
    else
    {
        status->errors = g_strdup("—");
        status->root = g_new0(ZgVdev, 1);
        status->root->name = g_strdup(status->name);
        status->root->state = g_strdup(status->state);
        status->root->children = g_ptr_array_new_with_free_func(
                (GDestroyNotify)zg_model_vdev_free);
    }

    g_ptr_array_add(statuses, status);
    lz.zpool_close(zh);

    (void)error;
    return 0;
}

GPtrArray *
zg_libzfs_pool_statuses(void)
{
    GPtrArray *statuses;

    if (!zg_libzfs_available() || !nvl_load() || !nvl_ok)
        return NULL;

    statuses = g_ptr_array_new_with_free_func((GDestroyNotify)zg_model_status_free);

    if (lz.zpool_iter(zg_handle, pool_status_add_cb, statuses) != 0)
    {
        g_ptr_array_free(statuses, TRUE);
        return NULL;
    }

    return statuses;
}

static int
snapshot_add_cb(zfs_handle_t *zh,
                void *data)
{
    GPtrArray *snapshots = data;
    ZgSnapshot *snapshot = g_new0(ZgSnapshot, 1);
    const gchar *name = lz.zfs_get_name(zh);
    const gchar *at = strchr(name, '@');

    snapshot->name = g_strdup(name);
    if (at != NULL)
    {
        snapshot->dataset = g_strndup(name, (gsize)(at - name));
        snapshot->snap = g_strdup(at + 1);
    }
    snapshot->used = lz.zfs_prop_get_int(zh, ZFS_PROP_USED);
    snapshot->refer = lz.zfs_prop_get_int(zh, ZFS_PROP_REFERENCED);
    snapshot->creation = (gint64)lz.zfs_prop_get_int(zh, ZFS_PROP_CREATION);

    g_ptr_array_add(snapshots, snapshot);
    lz.zfs_close(zh);
    return 0;
}

static int
snapshot_walk_cb(zfs_handle_t *zh,
                 void *data)
{
    GPtrArray *snapshots = data;

    lz.zfs_iter_snapshots(zh, B_FALSE, snapshot_add_cb, snapshots, 0, 0);
    lz.zfs_iter_children(zh, snapshot_walk_cb, snapshots);
    lz.zfs_close(zh);
    return 0;
}

GPtrArray *
zg_libzfs_snapshots(void)
{
    GPtrArray *snapshots;

    if (!zg_libzfs_available())
        return NULL;

    snapshots = g_ptr_array_new_with_free_func(
            (GDestroyNotify)zg_model_snapshot_free);

    if (lz.zfs_iter_root(zg_handle, snapshot_walk_cb, snapshots) != 0)
    {
        g_ptr_array_free(snapshots, TRUE);
        return NULL;
    }

    return snapshots;
}

static int
prop_collect_cb(int prop,
                void *data)
{
    GArray *props = data;

    g_array_append_val(props, prop);
    return ZPROP_CONT;
}

static const gchar *
prop_source_name(zprop_source_t source)
{
    switch (source & ZPROP_SRC_ALL)
    {
    case ZPROP_SRC_LOCAL:
        return "local";
    case ZPROP_SRC_DEFAULT:
        return "default";
    case ZPROP_SRC_INHERITED:
        return "inherited";
    case ZPROP_SRC_TEMPORARY:
        return "temporary";
    case ZPROP_SRC_RECEIVED:
        return "received";
    default:
        return "-";
    }
}

GPtrArray *
zg_libzfs_dataset_props(const gchar *dataset)
{
    zfs_handle_t *zh;
    nvlist_t *user_props;
    nvpair_t *nvp = NULL;
    GPtrArray *properties;
    GArray *native;
    guint i;

    if (!zg_libzfs_available() || !nvl_load() || !nvl_ok)
        return NULL;

    zh = lz.zfs_open(zg_handle, dataset,
                     ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME | ZFS_TYPE_SNAPSHOT);
    if (zh == NULL)
        return NULL;

    native = g_array_new(FALSE, FALSE, sizeof(gint));
    {
        const int rc = lz.zprop_iter(prop_collect_cb, native, B_FALSE, B_FALSE,
                                     ZFS_TYPE_DATASET);

        if (rc != 0 && rc != ZPROP_CONT)
        {
            g_array_free(native, TRUE);
            lz.zfs_close(zh);
            return NULL;
        }
    }
    if (0)
    {
        g_array_free(native, TRUE);
        lz.zfs_close(zh);
        return NULL;
    }

    properties = g_ptr_array_new_with_free_func(
            (GDestroyNotify)zg_model_property_free);

    for (i = 0; i < native->len; i++)
    {
        const zfs_prop_t prop = (zfs_prop_t)g_array_index(native, gint, i);
        const gchar *name = lz.zfs_prop_to_name(prop);
        gchar buf[ZFS_MAXPROPLEN];
        zprop_source_t source = ZPROP_SRC_NONE;
        ZgProperty *property;

        if (name == NULL)
            continue;

        if (lz.zfs_prop_get(zh, prop, buf, sizeof(buf), &source, NULL, 0,
                            B_TRUE) != 0)
            continue;

        property = g_new0(ZgProperty, 1);
        property->name = g_strdup(dataset);
        property->property = g_strdup(name);
        property->value = g_strdup(buf);
        property->source = g_strdup(prop_source_name(source));
        g_ptr_array_add(properties, property);
    }

    g_array_free(native, TRUE);

    user_props = lz.zfs_get_user_props(zh);
    while (user_props != NULL && (nvp = nvl.next_nvpair(user_props, nvp)) != NULL)
    {
        const gchar *name = nvl.nvpair_name(nvp);
        const gchar *value = NULL;

        if (name == NULL ||
            nvl.nvpair_value_string(nvp, &value) != 0 || value == NULL)
            continue;

        {
            ZgProperty *property = g_new0(ZgProperty, 1);

            property->name = g_strdup(dataset);
            property->property = g_strdup(name);
            property->value = g_strdup(value);
            property->source = g_strdup("local");
            g_ptr_array_add(properties, property);
        }
    }

    lz.zfs_close(zh);
    return properties;
}

#else /* !HAVE_LIBZFS */

gboolean
zg_libzfs_available(void)
{
    return FALSE;
}

GPtrArray *
zg_libzfs_pools(void)
{
    return NULL;
}

GPtrArray *
zg_libzfs_datasets(void)
{
    return NULL;
}

GPtrArray *
zg_libzfs_pool_statuses(void)
{
    return NULL;
}

GPtrArray *
zg_libzfs_snapshots(void)
{
    return NULL;
}

GPtrArray *
zg_libzfs_dataset_props(const gchar *dataset)
{
    return NULL;
}

#endif
