/* zg-model.c - ZFS 输出解析实现
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-model.h"

#include <stdlib.h>
#include <string.h>

#include "zg-util.h"

/* ------------------------------------------------------------------------- */
/* 生命周期                                                                   */
/* ------------------------------------------------------------------------- */

static void
pool_free(gpointer data)
{
    ZgPool *pool = data;

    if (pool == NULL)
        return;

    g_free(pool->name);
    g_free(pool->health);
    g_free(pool->altroot);
    g_free(pool->guid);
    g_free(pool);
}

static void
dataset_free(gpointer data)
{
    ZgDataset *dataset = data;

    if (dataset == NULL)
        return;

    g_free(dataset->name);
    g_free(dataset->type);
    g_free(dataset->mountpoint);
    g_free(dataset);
}

static void
snapshot_free(gpointer data)
{
    ZgSnapshot *snapshot = data;

    if (snapshot == NULL)
        return;

    g_free(snapshot->name);
    g_free(snapshot->dataset);
    g_free(snapshot->snap);
    g_free(snapshot);
}

static void
property_free(gpointer data)
{
    ZgProperty *property = data;

    if (property == NULL)
        return;

    g_free(property->name);
    g_free(property->property);
    g_free(property->value);
    g_free(property->source);
    g_free(property);
}

void zg_model_vdev_free(ZgVdev *vdev)
{
    if (vdev == NULL)
        return;

    g_free(vdev->name);
    g_free(vdev->type);
    g_free(vdev->state);
    g_free(vdev->vdev_class);
    g_free(vdev->path);
    g_free(vdev->alloc_space);
    g_free(vdev->total_space);

    if (vdev->children != NULL)
        g_ptr_array_free(vdev->children, TRUE);

    g_free(vdev);
}

void zg_model_status_free(ZgPoolStatus *status)
{
    if (status == NULL)
        return;

    g_free(status->name);
    g_free(status->state);
    g_free(status->scan);
    g_free(status->errors);
    g_free(status->txg);
    g_free(status->spa_version);
    g_free(status->status_note);
    zg_model_vdev_free(status->root);
    g_free(status);
}

void zg_model_pool_free(ZgPool *pool)
{
    pool_free(pool);
}

void zg_model_dataset_free(ZgDataset *dataset)
{
    dataset_free(dataset);
}

void zg_model_snapshot_free(ZgSnapshot *snapshot)
{
    snapshot_free(snapshot);
}

void zg_model_property_free(ZgProperty *property)
{
    property_free(property);
}

GPtrArray *
zg_model_pool_array_new(void)
{
    return g_ptr_array_new_with_free_func(pool_free);
}

GPtrArray *
zg_model_dataset_array_new(void)
{
    return g_ptr_array_new_with_free_func(dataset_free);
}

GPtrArray *
zg_model_snapshot_array_new(void)
{
    return g_ptr_array_new_with_free_func(snapshot_free);
}

GPtrArray *
zg_model_property_array_new(void)
{
    return g_ptr_array_new_with_free_func(property_free);
}

GPtrArray *
zg_model_status_array_new(void)
{
    return g_ptr_array_new_with_free_func((GDestroyNotify)zg_model_status_free);
}

/* ------------------------------------------------------------------------- */
/* 基础解析                                                                   */
/* ------------------------------------------------------------------------- */

gboolean
zg_model_parse_size(const gchar *text,
                    guint64 *bytes_out)
{
    static const gchar units[] = "KMGTPEZ";
    gchar *end = NULL;
    double value;

    if (text == NULL || text[0] == '\0' || g_strcmp0(text, "-") == 0)
        return FALSE;

    value = g_ascii_strtod(text, &end);

    if (end == text)
        return FALSE;

    while (*end == ' ')
        end++;

    if (*end != '\0')
    {
        const gchar *unit = strchr(units, g_ascii_toupper(*end));

        if (unit == NULL)
            return FALSE;

        {
            const gint power = (gint)(unit - units) + 1;
            gint i;

            for (i = 0; i < power; i++)
                value *= 1024.0;
        }
    }

    if (bytes_out != NULL)
        *bytes_out = (guint64)value;

    return TRUE;
}

static guint64
parse_u64_or_zero(const gchar *text)
{
    guint64 value = 0;

    if (text == NULL || text[0] == '\0' || g_strcmp0(text, "-") == 0)
        return 0;

    if (!zg_parse_u64(text, &value))
    {
        if (!zg_model_parse_size(text, &value))
            return 0;
    }

    return value;
}

static gint
parse_percent(const gchar *text)
{
    if (text == NULL || g_strcmp0(text, "-") == 0 || text[0] == '\0')
        return -1;

    return (gint)strtol(text, NULL, 10);
}

const gchar *
zg_model_state_css(const gchar *state)
{
    if (state == NULL)
        return "zg-state-unknown";

    if (g_strcmp0(state, "ONLINE") == 0 || g_strcmp0(state, "AVAIL") == 0)
        return "zg-state-online";

    if (g_strcmp0(state, "DEGRADED") == 0 || g_strcmp0(state, "INUSE") == 0)
        return "zg-state-degraded";

    if (g_strcmp0(state, "FAULTED") == 0 ||
        g_strcmp0(state, "UNAVAIL") == 0 ||
        g_strcmp0(state, "REMOVED") == 0)
        return "zg-state-faulted";

    if (g_strcmp0(state, "OFFLINE") == 0)
        return "zg-state-offline";

    return "zg-state-unknown";
}

gboolean
zg_model_state_is_healthy(const gchar *state)
{
    return g_strcmp0(state, "ONLINE") == 0;
}

/* ------------------------------------------------------------------------- */
/* 列表解析（-H -p）                                                          */
/* ------------------------------------------------------------------------- */

static gchar **
split_tsv(const gchar *line)
{
    return g_strsplit(line, "\t", -1);
}

GPtrArray *
zg_model_pools_from_tsv(const gchar *text)
{
    GPtrArray *pools = zg_model_pool_array_new();
    gchar **lines;
    guint i;

    if (text == NULL)
        return pools;

    lines = g_strsplit(text, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
    {
        gchar **cols;
        guint count;
        ZgPool *pool;

        if (lines[i][0] == '\0')
            continue;

        cols = split_tsv(lines[i]);
        count = g_strv_length(cols);

        if (count < 10)
        {
            g_strfreev(cols);
            continue;
        }

        pool = g_new0(ZgPool, 1);
        pool->name = g_strdup(cols[0]);
        pool->size = parse_u64_or_zero(cols[1]);
        pool->alloc = parse_u64_or_zero(cols[2]);
        pool->free = parse_u64_or_zero(cols[3]);
        pool->checkpoint = parse_u64_or_zero(cols[4]);
        pool->fragmentation = parse_percent(cols[6]);
        pool->capacity = parse_percent(cols[7]);
        pool->dedup_ratio = g_ascii_strtod(cols[8], NULL);
        pool->health = g_strdup(cols[9]);
        pool->altroot = g_strdup(count > 10 ? cols[10] : "-");

        g_ptr_array_add(pools, pool);
        g_strfreev(cols);
    }

    g_strfreev(lines);

    return pools;
}

GPtrArray *
zg_model_datasets_from_tsv(const gchar *text)
{
    GPtrArray *datasets = zg_model_dataset_array_new();
    gchar **lines;
    guint i;

    if (text == NULL)
        return datasets;

    lines = g_strsplit(text, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
    {
        gchar **cols;
        guint count;
        ZgDataset *dataset;

        if (lines[i][0] == '\0')
            continue;

        cols = split_tsv(lines[i]);
        count = g_strv_length(cols);

        if (count < 7)
        {
            g_strfreev(cols);
            continue;
        }

        dataset = g_new0(ZgDataset, 1);
        dataset->name = g_strdup(cols[0]);
        dataset->used = parse_u64_or_zero(cols[1]);
        dataset->avail = parse_u64_or_zero(cols[2]);
        dataset->refer = parse_u64_or_zero(cols[3]);
        dataset->mountpoint = g_strdup(count > 4 ? cols[4] : "-");
        dataset->type = g_strdup(count > 5 ? cols[5] : "filesystem");
        dataset->creation = (gint64)parse_u64_or_zero(count > 6 ? cols[6] : "0");
        dataset->mounted = count > 7 && g_strcmp0(cols[7], "yes") == 0;

        g_ptr_array_add(datasets, dataset);
        g_strfreev(cols);
    }

    g_strfreev(lines);

    return datasets;
}

GPtrArray *
zg_model_snapshots_from_tsv(const gchar *text)
{
    GPtrArray *snapshots = zg_model_snapshot_array_new();
    gchar **lines;
    guint i;

    if (text == NULL)
        return snapshots;

    lines = g_strsplit(text, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
    {
        gchar **cols;
        guint count;
        ZgSnapshot *snapshot;
        gchar *at;

        if (lines[i][0] == '\0')
            continue;

        cols = split_tsv(lines[i]);
        count = g_strv_length(cols);

        if (count < 4)
        {
            g_strfreev(cols);
            continue;
        }

        snapshot = g_new0(ZgSnapshot, 1);
        snapshot->name = g_strdup(cols[0]);
        snapshot->used = parse_u64_or_zero(cols[1]);
        snapshot->refer = parse_u64_or_zero(cols[2]);
        snapshot->creation = (gint64)parse_u64_or_zero(cols[3]);

        at = strchr(snapshot->name, '@');
        if (at != NULL)
        {
            snapshot->dataset = g_strndup(snapshot->name, (gsize)(at - snapshot->name));
            snapshot->snap = g_strdup(at + 1);
        }
        else
        {
            snapshot->dataset = g_strdup(snapshot->name);
            snapshot->snap = g_strdup("");
        }

        g_ptr_array_add(snapshots, snapshot);
        g_strfreev(cols);
    }

    g_strfreev(lines);

    return snapshots;
}

/* 以字节为单位的属性：界面显示前转成人类可读形式 */
static gboolean
property_is_size(const gchar *name)
{
    static const gchar *const sizes[] = {
            "used", "available", "referenced", "usedbychildren", "usedbydataset",
            "usedbysnapshots", "usedbyrefreservation", "logicalused",
            "logicalreferenced", "quota", "reservation", "refreservation",
            "refquota", "volsize", "recordsize", "volblocksize", "written",
            "freespace", "size", "allocated", "free", "checkpoint", "expandsize",
            "leaked", "def_space", "total_space", "alloc_space",
            NULL};
    guint i;

    for (i = 0; sizes[i] != NULL; i++)
    {
        if (g_strcmp0(sizes[i], name) == 0)
            return TRUE;
    }

    return FALSE;
}

static gboolean
property_zero_means_none(const gchar *name)
{
    static const gchar *const names[] = {
            "quota", "reservation", "refreservation", "refquota", "volsize",
            "checkpoint", "expandsize",
            NULL};
    guint i;

    for (i = 0; names[i] != NULL; i++)
    {
        if (g_strcmp0(names[i], name) == 0)
            return TRUE;
    }

    return FALSE;
}

static gchar *
format_property_value(const gchar *name,
                      const gchar *value)
{
    guint64 number = 0;

    if (value == NULL || value[0] == '\0')
        return g_strdup("—");

    if (g_strcmp0(name, "creation") == 0 && zg_parse_u64(value, &number))
        return zg_format_epoch_local((gint64)number);

    if (g_strcmp0(name, "capacity") == 0 ||
        g_strcmp0(name, "fragmentation") == 0)
    {
        if (zg_parse_u64(value, &number))
            return g_strdup_printf("%" G_GUINT64_FORMAT "%%", number);

        return g_strdup(value);
    }

    if (property_is_size(name) && zg_parse_u64(value, &number))
    {
        if (number == 0 && property_zero_means_none(name))
            return g_strdup("none");

        return zg_format_bytes(number);
    }

    return g_strdup(value);
}

GPtrArray *
zg_model_properties_from_tsv(const gchar *text)
{
    GPtrArray *properties = zg_model_property_array_new();
    gchar **lines;
    guint i;

    if (text == NULL)
        return properties;

    lines = g_strsplit(text, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
    {
        gchar **cols;
        guint count;
        ZgProperty *property;

        if (lines[i][0] == '\0')
            continue;

        cols = split_tsv(lines[i]);
        count = g_strv_length(cols);

        if (count < 4)
        {
            g_strfreev(cols);
            continue;
        }

        property = g_new0(ZgProperty, 1);
        property->name = g_strdup(cols[0]);
        property->property = g_strdup(cols[1]);
        property->value = format_property_value(cols[1], cols[2]);
        property->source = g_strdup(cols[3]);

        g_ptr_array_add(properties, property);
        g_strfreev(cols);
    }

    g_strfreev(lines);

    return properties;
}

GPtrArray *
zg_model_arcstats_from_text(const gchar *text)
{
    GPtrArray *properties = zg_model_property_array_new();
    gchar **lines;
    guint i;

    if (text == NULL)
        return properties;

    lines = g_strsplit(text, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
    {
        gchar *colon;
        gchar *name;
        gchar *value;
        ZgProperty *property;

        if (lines[i][0] == '\0')
            continue;

        colon = strchr(lines[i], ':');

        if (colon == NULL)
            continue;

        name = g_strndup(lines[i], (gsize)(colon - lines[i]));
        value = zg_str_trim(colon + 1);

        property = g_new0(ZgProperty, 1);
        property->name = g_strdup("ARC");
        property->property = g_strstrip(name);
        property->value = value;
        property->source = g_strdup("kstat");

        g_ptr_array_add(properties, property);
    }

    g_strfreev(lines);

    return properties;
}

/* ------------------------------------------------------------------------- */
/* zpool status -j                                                            */
/* ------------------------------------------------------------------------- */

static guint64
json_u64(const ZgJson *node,
         const gchar *key)
{
    const gchar *text = zg_json_object_get_string(node, key);

    return parse_u64_or_zero(text);
}

static ZgVdev *
vdev_from_json(const ZgJson *node,
               guint depth)
{
    ZgVdev *vdev;
    const ZgJson *children;
    guint i;

    if (node == NULL || zg_json_get_type(node) != ZG_JSON_TYPE_OBJECT)
        return NULL;

    vdev = g_new0(ZgVdev, 1);
    vdev->name = g_strdup(zg_json_object_get_string(node, "name"));
    vdev->type = g_strdup(zg_json_object_get_string(node, "vdev_type"));
    vdev->state = g_strdup(zg_json_object_get_string(node, "state"));
    vdev->vdev_class = g_strdup(zg_json_object_get_string(node, "class"));
    vdev->path = g_strdup(zg_json_object_get_string(node, "path"));
    vdev->alloc_space = g_strdup(zg_json_object_get_string(node, "alloc_space"));
    vdev->total_space = g_strdup(zg_json_object_get_string(node, "total_space"));
    vdev->read_errors = json_u64(node, "read_errors");
    vdev->write_errors = json_u64(node, "write_errors");
    vdev->checksum_errors = json_u64(node, "checksum_errors");
    vdev->depth = depth;
    vdev->children = g_ptr_array_new_with_free_func((GDestroyNotify)zg_model_vdev_free);

    children = zg_json_object_get(node, "vdevs");

    for (i = 0; i < zg_json_object_size(children); i++)
    {
        ZgVdev *child = vdev_from_json(zg_json_object_value_at(children, i), depth + 1);

        if (child != NULL)
            g_ptr_array_add(vdev->children, child);
    }

    return vdev;
}

GPtrArray *
zg_model_statuses_from_json(const gchar *text,
                            GError **error)
{
    GPtrArray *statuses = zg_model_status_array_new();
    ZgJson *root;
    const ZgJson *pools;
    guint i;

    root = zg_json_parse(text, -1, error);

    if (root == NULL)
        return statuses;

    pools = zg_json_object_get(root, "pools");

    for (i = 0; i < zg_json_object_size(pools); i++)
    {
        const ZgJson *pool_json = zg_json_object_value_at(pools, i);
        const gchar *key = zg_json_object_key_at(pools, i);
        const ZgJson *vdevs;
        ZgPoolStatus *status;

        if (zg_json_get_type(pool_json) != ZG_JSON_TYPE_OBJECT)
            continue;

        status = g_new0(ZgPoolStatus, 1);
        status->name = g_strdup(zg_json_object_get_string_or(pool_json, "name", key));
        status->state = g_strdup(zg_json_object_get_string(pool_json, "state"));
        status->txg = g_strdup(zg_json_object_get_string(pool_json, "txg"));
        status->spa_version = g_strdup(zg_json_object_get_string(pool_json, "spa_version"));

        vdevs = zg_json_object_get(pool_json, "vdevs");

        if (zg_json_object_size(vdevs) > 0)
            status->root = vdev_from_json(zg_json_object_value_at(vdevs, 0), 0);

        g_ptr_array_add(statuses, status);
    }

    zg_json_free(root);

    return statuses;
}

/* ------------------------------------------------------------------------- */
/* zpool status（传统文本，按缩进还原层级）                                     */
/* ------------------------------------------------------------------------- */

static gint
line_indent(const gchar *line)
{
    gint indent = 0;

    while (*line == ' ' || *line == '\t')
    {
        indent += (*line == '\t') ? 8 : 1;
        line++;
    }

    return indent;
}

static gboolean
is_section_marker(const gchar *name,
                  gchar **class_out)
{
    static const struct
    {
        const gchar *name;
        const gchar *klass;
    } sections[] = {
            {"logs", "log"},
            {"cache", "cache"},
            {"spares", "spare"},
            {"special", "special"},
            {"dedup", "dedup"},
            {"indirect", "indirect"},
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(sections); i++)
    {
        if (g_strcmp0(name, sections[i].name) == 0)
        {
            if (class_out != NULL)
                *class_out = g_strdup(sections[i].klass);
            return TRUE;
        }
    }

    return FALSE;
}

GPtrArray *
zg_model_statuses_from_text(const gchar *text)
{
    GPtrArray *statuses = zg_model_status_array_new();
    gchar **lines;
    guint i;
    ZgPoolStatus *current = NULL;
    gboolean in_config = FALSE;
    gboolean in_errors = FALSE;
    GArray *indent_stack = NULL;
    GPtrArray *node_stack = NULL;
    gchar *section_class = NULL;

    if (text == NULL)
        return statuses;

    lines = g_strsplit(text, "\n", -1);
    indent_stack = g_array_new(FALSE, FALSE, sizeof(gint));
    node_stack = g_ptr_array_new();

    for (i = 0; lines[i] != NULL; i++)
    {
        const gchar *line = lines[i];
        gchar *trimmed = zg_str_trim(line);

        if (trimmed[0] == '\0')
        {
            if (in_errors)
                in_errors = FALSE;
            g_free(trimmed);
            continue;
        }

        if (g_str_has_prefix(trimmed, "pool:"))
        {
            current = g_new0(ZgPoolStatus, 1);
            current->name = zg_str_trim(trimmed + 5);
            g_ptr_array_add(statuses, current);

            in_config = FALSE;
            in_errors = FALSE;
            g_array_set_size(indent_stack, 0);
            g_ptr_array_set_size(node_stack, 0);
            g_free(section_class);
            section_class = NULL;

            g_free(trimmed);
            continue;
        }

        if (current == NULL)
        {
            g_free(trimmed);
            continue;
        }

        if (g_str_has_prefix(trimmed, "state:"))
        {
            g_free(current->state);
            current->state = zg_str_trim(trimmed + 6);
            g_free(trimmed);
            continue;
        }

        if (g_str_has_prefix(trimmed, "scan:"))
        {
            g_free(current->scan);
            current->scan = zg_str_trim(trimmed + 5);
            in_config = FALSE;
            g_free(trimmed);
            continue;
        }

        if (g_str_has_prefix(trimmed, "status:"))
        {
            g_free(current->status_note);
            current->status_note = zg_str_trim(trimmed + 7);
            in_config = FALSE;
            g_free(trimmed);
            continue;
        }

        if (g_str_has_prefix(trimmed, "action:") ||
            g_str_has_prefix(trimmed, "see:") ||
            g_str_has_prefix(trimmed, "removed-") ||
            g_str_has_prefix(trimmed, "supplemental"))
        {
            in_config = FALSE;
            g_free(trimmed);
            continue;
        }

        if (g_strcmp0(trimmed, "config:") == 0)
        {
            in_config = TRUE;
            in_errors = FALSE;
            g_array_set_size(indent_stack, 0);
            g_ptr_array_set_size(node_stack, 0);
            g_free(trimmed);
            continue;
        }

        if (g_str_has_prefix(trimmed, "errors:"))
        {
            g_free(current->errors);
            current->errors = zg_str_trim(trimmed + 7);
            in_config = FALSE;
            in_errors = TRUE;
            g_free(trimmed);
            continue;
        }

        if (in_errors)
        {
            gchar *combined = g_strdup_printf("%s %s",
                                              current->errors != NULL ? current->errors : "",
                                              trimmed);

            g_free(current->errors);
            current->errors = combined;
            g_free(trimmed);
            continue;
        }

        if (in_config)
        {
            gchar **tokens = g_strsplit_set(trimmed, " \t", -1);
            GPtrArray *parts = g_ptr_array_new();
            guint j;

            for (j = 0; tokens[j] != NULL; j++)
            {
                if (tokens[j][0] != '\0')
                    g_ptr_array_add(parts, tokens[j]);
            }

            if (parts->len >= 2)
            {
                const gchar *name = g_ptr_array_index(parts, 0);

                if (g_strcmp0(name, "NAME") != 0)
                {
                    ZgVdev *node = g_new0(ZgVdev, 1);
                    const gint indent = line_indent(line);

                    node->name = g_strdup(name);
                    node->state = g_strdup(g_ptr_array_index(parts, 1));
                    node->read_errors = parts->len > 2 ? parse_u64_or_zero(g_ptr_array_index(parts, 2)) : 0;
                    node->write_errors = parts->len > 3 ? parse_u64_or_zero(g_ptr_array_index(parts, 3)) : 0;
                    node->checksum_errors = parts->len > 4 ? parse_u64_or_zero(g_ptr_array_index(parts, 4)) : 0;
                    node->children = g_ptr_array_new_with_free_func((GDestroyNotify)zg_model_vdev_free);

                    while (indent_stack->len > 0 &&
                           indent <= g_array_index(indent_stack, gint, indent_stack->len - 1))
                    {
                        g_array_set_size(indent_stack, indent_stack->len - 1);
                        g_ptr_array_set_size(node_stack, node_stack->len - 1);
                    }

                    if (node_stack->len == 0)
                    {
                        if (current->root == NULL)
                        {
                            node->depth = 0;
                            current->root = node;
                        }
                        else
                        {
                            node->depth = 1;
                            g_ptr_array_add(current->root->children, node);
                        }
                    }
                    else
                    {
                        ZgVdev *parent = g_ptr_array_index(node_stack, node_stack->len - 1);

                        node->depth = parent->depth + 1;
                        g_ptr_array_add(parent->children, node);

                        if (parent->depth == 0 && section_class != NULL)
                            node->vdev_class = g_strdup(section_class);
                    }

                    g_array_append_val(indent_stack, indent);
                    g_ptr_array_add(node_stack, node);
                }

                g_ptr_array_free(parts, TRUE);
            }
            else if (parts->len == 1)
            {
                gchar *klass = NULL;

                if (is_section_marker(g_ptr_array_index(parts, 0), &klass))
                {
                    g_free(section_class);
                    section_class = klass;
                }

                /* 其余单列行（如 "was /dev/ada1"）忽略 */
                g_ptr_array_free(parts, TRUE);
            }
            else
            {
                g_ptr_array_free(parts, TRUE);
            }

            g_strfreev(tokens);
            g_free(trimmed);
            continue;
        }

        g_free(trimmed);
    }

    g_array_free(indent_stack, TRUE);
    g_ptr_array_free(node_stack, TRUE);
    g_free(section_class);
    g_strfreev(lines);

    return statuses;
}

/* ------------------------------------------------------------------------- */
/* 查找                                                                       */
/* ------------------------------------------------------------------------- */

ZgPool *
zg_model_pool_find(GPtrArray *pools,
                   const gchar *name)
{
    guint i;

    for (i = 0; pools != NULL && i < pools->len; i++)
    {
        ZgPool *pool = g_ptr_array_index(pools, i);

        if (g_strcmp0(pool->name, name) == 0)
            return pool;
    }

    return NULL;
}

ZgDataset *
zg_model_dataset_find(GPtrArray *datasets,
                      const gchar *name)
{
    guint i;

    for (i = 0; datasets != NULL && i < datasets->len; i++)
    {
        ZgDataset *dataset = g_ptr_array_index(datasets, i);

        if (g_strcmp0(dataset->name, name) == 0)
            return dataset;
    }

    return NULL;
}

ZgPoolStatus *
zg_model_status_find(GPtrArray *statuses,
                     const gchar *name)
{
    guint i;

    for (i = 0; statuses != NULL && i < statuses->len; i++)
    {
        ZgPoolStatus *status = g_ptr_array_index(statuses, i);

        if (g_strcmp0(status->name, name) == 0)
            return status;
    }

    return NULL;
}

/* ------------------------------------------------------------------------- */
/* 磁盘清单（geom disk list）                                                 */
/* ------------------------------------------------------------------------- */

void zg_model_disk_free(ZgDisk *disk)
{
    if (disk == NULL)
        return;

    g_free(disk->name);
    g_free(disk->descr);
    g_free(disk->ident);
    g_free(disk);
}

void zg_model_importable_free(ZgImportablePool *pool)
{
    if (pool == NULL)
        return;

    g_free(pool->pool);
    g_free(pool->state);
    g_free(pool->note);
    g_free(pool);
}

GPtrArray *
zg_model_disk_array_new(void)
{
    return g_ptr_array_new_with_free_func((GDestroyNotify)zg_model_disk_free);
}

GPtrArray *
zg_model_importable_array_new(void)
{
    return g_ptr_array_new_with_free_func((GDestroyNotify)zg_model_importable_free);
}

GPtrArray *
zg_model_disks_from_text(const gchar *text)
{
    GPtrArray *disks = zg_model_disk_array_new();
    gchar **lines;
    guint i;
    ZgDisk *current = NULL;

    if (text == NULL)
        return disks;

    lines = g_strsplit(text, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
    {
        gchar *line = zg_str_trim(lines[i]);

        if (g_str_has_prefix(line, "Geom name:"))
        {
            current = g_new0(ZgDisk, 1);
            current->name = zg_str_trim(line + strlen("Geom name:"));
            current->sectorsize = 512;
            g_ptr_array_add(disks, current);
        }
        else if (current != NULL && g_str_has_prefix(line, "descr:"))
        {
            g_free(current->descr);
            current->descr = zg_str_trim(line + strlen("descr:"));
        }
        else if (current != NULL && g_str_has_prefix(line, "ident:"))
        {
            g_free(current->ident);
            current->ident = zg_str_trim(line + strlen("ident:"));
        }
        else if (current != NULL && g_str_has_prefix(line, "Mediasize:"))
        {
            gchar *value = zg_str_trim(line + strlen("Mediasize:"));
            gchar *space = strchr(value, ' ');

            if (space != NULL)
                *space = '\0';

            zg_parse_u64(value, &current->size);
            g_free(value);
        }
        else if (current != NULL && g_str_has_prefix(line, "Sectorsize:"))
        {
            gchar *value = zg_str_trim(line + strlen("Sectorsize:"));
            guint64 number = 0;

            if (zg_parse_u64(value, &number))
                current->sectorsize = (gint)number;

            g_free(value);
        }

        g_free(line);
    }

    g_strfreev(lines);

    return disks;
}

ZgDisk *
zg_model_disk_find(GPtrArray *disks,
                   const gchar *name)
{
    guint i;

    for (i = 0; disks != NULL && i < disks->len; i++)
    {
        ZgDisk *disk = g_ptr_array_index(disks, i);

        if (g_strcmp0(disk->name, name) == 0)
            return disk;
    }

    return NULL;
}

/* ------------------------------------------------------------------------- */
/* 可导入池清单（zpool import 无参数输出）                                     */
/* ------------------------------------------------------------------------- */

GPtrArray *
zg_model_importable_from_text(const gchar *text)
{
    GPtrArray *pools = zg_model_importable_array_new();
    gchar **lines;
    guint i;
    ZgImportablePool *current = NULL;

    if (text == NULL)
        return pools;

    lines = g_strsplit(text, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
    {
        gchar *line = zg_str_trim(lines[i]);

        if (g_str_has_prefix(line, "pool:"))
        {
            current = g_new0(ZgImportablePool, 1);
            current->pool = zg_str_trim(line + strlen("pool:"));
            g_ptr_array_add(pools, current);
        }
        else if (current != NULL && g_str_has_prefix(line, "state:"))
        {
            g_free(current->state);
            current->state = zg_str_trim(line + strlen("state:"));
        }
        else if (current != NULL && g_str_has_prefix(line, "action:"))
        {
            g_free(current->note);
            current->note = zg_str_trim(line + strlen("action:"));
        }

        g_free(line);
    }

    g_strfreev(lines);

    return pools;
}
