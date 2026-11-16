/* zg-libzfs.h - 可选的 libzfs 只读适配层
 *
 * 目标：GUI 直接通过 libzfs 读取存储池/数据集列表（只读），
 * 失败或不具备权限时自动回退到守护进程的 zpool.list / zfs.list。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#pragma once

#include <glib.h>

#include "zg-model.h"

G_BEGIN_DECLS

/* 探测 /dev/zfs 与 libzfs 是否可用（首次调用时初始化，进程内缓存） */
gboolean zg_libzfs_available(void);

/* 成功返回 ZgPool* / ZgDataset* 数组（调用方用 g_ptr_array_free 释放），
 * 失败返回 NULL，调用方应回退到守护进程。 */
GPtrArray *zg_libzfs_pools(void);
GPtrArray *zg_libzfs_datasets(void);

/* 用 libzfs 配置合成 zpool status 等价数据（含 vdev 树/错误/scan） */
GPtrArray *zg_libzfs_pool_statuses(void);

/* 读取全部快照（ZgSnapshot* 数组，失败返回 NULL） */
GPtrArray *zg_libzfs_snapshots(void);

/* 读取数据集的全部属性（ZgProperty* 数组，失败返回 NULL） */
GPtrArray *zg_libzfs_dataset_props(const gchar *dataset);

G_END_DECLS
