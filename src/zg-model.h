/* zg-model.h - ZFS 命令输出的数据模型与解析器
 *
 * 兼容性策略
 * ----------
 * - 列表/属性统一使用 `-H -p`（脚本模式 + 精确数值）：
 *   该组合在 OpenZFS 0.8 到 2.x 上均可用，且不受 locale 影响。
 * - `zpool status` 无法使用 -H，因此优先使用 `-j`（OpenZFS >= 2.2）获得
 *   vdev 树；不支持时回退到解析传统文本输出（按缩进还原层级）。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_MODEL_H
#define ZG_MODEL_H

#include <glib.h>

#include "zg-json.h"

G_BEGIN_DECLS

/* 界面固定的字段顺序，解析器与请求参数必须保持一致 */
#define ZG_MODEL_DATASET_FIELDS "name,used,avail,refer,mountpoint,type,creation,mounted"
#define ZG_MODEL_SNAPSHOT_FIELDS "name,used,refer,creation"

/* ------------------------------------------------------------------------- */
/* 结构                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct
{
    gchar *name;
    guint64 size;
    guint64 alloc;
    guint64 free;
    guint64 checkpoint;
    gint fragmentation; /* -1 表示未知 */
    gint capacity;
    gdouble dedup_ratio;
    gchar *health;
    gchar *altroot;
    gchar *guid;
} ZgPool;

typedef struct
{
    gchar *name;
    gchar *type; /* filesystem / volume / snapshot */
    guint64 used;
    guint64 avail;
    guint64 refer;
    gchar *mountpoint;
    gint64 creation;
    gboolean mounted;
} ZgDataset;

typedef struct
{
    gchar *name; /* dataset@snap */
    gchar *dataset;
    gchar *snap;
    guint64 used;
    guint64 refer;
    gint64 creation;
} ZgSnapshot;

typedef struct ZgVdev
{
    gchar *name;
    gchar *type; /* root / disk / mirror / raidz1 ... */
    gchar *state;
    gchar *vdev_class; /* normal / log / cache / spare / special / dedup */
    gchar *path;
    gchar *alloc_space;
    gchar *total_space;
    guint64 read_errors;
    guint64 write_errors;
    guint64 checksum_errors;
    guint depth;
    GPtrArray *children; /* ZgVdev * */
} ZgVdev;

typedef struct
{
    gchar *name;
    gchar *state;
    gchar *scan;
    gchar *errors;
    gchar *txg;
    gchar *spa_version;
    gchar *status_note;
    ZgVdev *root;
} ZgPoolStatus;

typedef struct
{
    gchar *name;  /* nda0 / ada0 */
    gchar *descr; /* 型号 */
    gchar *ident; /* 序列号 */
    guint64 size; /* 字节 */
    gint sectorsize;
} ZgDisk;

typedef struct
{
    gchar *pool;
    gchar *state;
    gchar *note;
} ZgImportablePool;

typedef struct
{
    gchar *name; /* 数据集 / 池名称 */
    gchar *property;
    gchar *value;
    gchar *source;
} ZgProperty;

/* ------------------------------------------------------------------------- */
/* 生命周期                                                                   */
/* ------------------------------------------------------------------------- */

GPtrArray *zg_model_pool_array_new(void);
GPtrArray *zg_model_dataset_array_new(void);
GPtrArray *zg_model_snapshot_array_new(void);
GPtrArray *zg_model_property_array_new(void);
GPtrArray *zg_model_status_array_new(void);
GPtrArray *zg_model_disk_array_new(void);
GPtrArray *zg_model_importable_array_new(void);

void zg_model_pool_free(ZgPool *pool);
void zg_model_dataset_free(ZgDataset *dataset);
void zg_model_snapshot_free(ZgSnapshot *snapshot);
void zg_model_property_free(ZgProperty *property);
void zg_model_vdev_free(ZgVdev *vdev);
void zg_model_status_free(ZgPoolStatus *status);
void zg_model_disk_free(ZgDisk *disk);
void zg_model_importable_free(ZgImportablePool *pool);

/* ------------------------------------------------------------------------- */
/* 解析                                                                       */
/* ------------------------------------------------------------------------- */

/* `zpool list -H -p`，列顺序固定为
 * name,size,alloc,free,ckpoint,expandsz,frag,cap,dedup,health,altroot */
GPtrArray *zg_model_pools_from_tsv(const gchar *text);

/* `zfs list -H -p -o name,used,avail,refer,mountpoint,type,creation` */
GPtrArray *zg_model_datasets_from_tsv(const gchar *text);

/* `zfs list -H -p -t snapshot -o name,used,refer,creation` */
GPtrArray *zg_model_snapshots_from_tsv(const gchar *text);

/* `zfs get|zpool get -H -p`：name,property,value,source */
GPtrArray *zg_model_properties_from_tsv(const gchar *text);

/* `zpool status -j`（OpenZFS >= 2.2） */
GPtrArray *zg_model_statuses_from_json(const gchar *text,
                                       GError **error);

/* `zpool status` 传统文本输出（按缩进还原 vdev 层级） */
GPtrArray *zg_model_statuses_from_text(const gchar *text);

/* `geom disk list`：磁盘清单 */
GPtrArray *zg_model_disks_from_text(const gchar *text);

/* `zpool import`（无参数时的可导入池清单） */
GPtrArray *zg_model_importable_from_text(const gchar *text);

/* `sysctl kstat.zfs.misc.arcstats`：name: value */
GPtrArray *zg_model_arcstats_from_text(const gchar *text);

/* ------------------------------------------------------------------------- */
/* 辅助                                                                       */
/* ------------------------------------------------------------------------- */

/* 解析 ZFS 的人类可读大小（如 37.9G / 24K / 1.10T），失败返回 FALSE */
gboolean zg_model_parse_size(const gchar *text,
                             guint64 *bytes_out);

/* 健康状态 -> GTK CSS 类名（用于着色） */
const gchar *zg_model_state_css(const gchar *state);

/* 状态是否属于正常 */
gboolean zg_model_state_is_healthy(const gchar *state);

/* 在数组中按名称查找 */
ZgPool *zg_model_pool_find(GPtrArray *pools,
                           const gchar *name);
ZgDataset *zg_model_dataset_find(GPtrArray *datasets,
                                 const gchar *name);
ZgPoolStatus *zg_model_status_find(GPtrArray *statuses,
                                   const gchar *name);
ZgDisk *zg_model_disk_find(GPtrArray *disks,
                           const gchar *name);

G_END_DECLS

#endif /* ZG_MODEL_H */
