/* zg-protocol.h - zfs-gui 前端与守护进程之间的通信协议
 *
 * 传输层：AF_UNIX / SOCK_STREAM
 * 帧格式：4 字节大端长度 + UTF-8 JSON 载荷
 *
 * 请求示例：
 *   {
 *     "v": 1,
 *     "op": "zpool.create",
 *     "args": {
 *       "positional": ["tank", "mirror", "/dev/ada1", "/dev/ada2"],
 *       "flags": ["-f"],
 *       "options": [ { "key": "o", "value": "ashift=12" } ]
 *     }
 *   }
 *
 * 应答示例：
 *   {
 *     "v": 1,
 *     "ok": true,
 *     "code": 0,
 *     "stdout": "...",
 *     "stderr": "",
 *     "data": { }             // 可选的结构化数据
 *   }
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_PROTOCOL_H
#define ZG_PROTOCOL_H

/* 协议版本，前后端不一致时拒绝通信 */
#define ZG_PROTOCOL_VERSION 1

/* 运行期 socket 目录（仅 root 可写，避免 /tmp 符号链接攻击） */
#define ZG_RUN_DIR_DEFAULT "/var/run/zfs-gui"
#define ZG_SOCKET_NAME "zfs-gui.sock"
#define ZG_SOCKET_PATH_DEFAULT ZG_RUN_DIR_DEFAULT "/" ZG_SOCKET_NAME

/* 守护进程配置 */
#define ZG_CONFIG_PATH_DEFAULT "/usr/local/etc/zfs-gui.conf"
#define ZG_PID_PATH_DEFAULT "/var/run/zfs-gui.pid"

/* 单帧最大长度：zfs get all 之类的输出可能较大 */
#define ZG_MAX_FRAME (32u * 1024u * 1024u)

/* 命令输出上限，防止失控子进程耗尽内存 */
#define ZG_MAX_OUTPUT (64u * 1024u * 1024u)

/* 默认等待超时（秒），0 表示不限 */
#define ZG_IO_TIMEOUT_SECONDS 600
#define ZG_LONG_OP_TIMEOUT_SECONDS 0

/* 请求/应答 JSON 中的字段名 */
#define ZG_KEY_VERSION "v"
#define ZG_KEY_OP "op"
#define ZG_KEY_ARGS "args"
#define ZG_KEY_POSITIONAL "positional"
#define ZG_KEY_FLAGS "flags"
#define ZG_KEY_OPTIONS "options"
#define ZG_KEY_KEY "key"
#define ZG_KEY_VALUE "value"
#define ZG_KEY_OK "ok"
#define ZG_KEY_CODE "code"
#define ZG_KEY_STDOUT "stdout"
#define ZG_KEY_STDERR "stderr"
#define ZG_KEY_DATA "data"
#define ZG_KEY_MESSAGE "message"
#define ZG_KEY_AUTH "auth"
#define ZG_KEY_AUTH_USER "user"
#define ZG_KEY_AUTH_PASS "password"

/* 应答码：认证相关（负值，与子进程退出码区分） */
#define ZG_ERR_AUTH_REQUIRED (-10)    /* 需要管理员密码 */
#define ZG_ERR_AUTH_FAILED (-11)      /* 密码错误或账号非管理员 */
#define ZG_ERR_AUTH_LOCKED (-12)      /* 失败次数过多，暂时锁定 */
#define ZG_ERR_AUTH_UNAVAILABLE (-13) /* 守护进程无法验证（例如非 root） */
#define ZG_ERR_UNKNOWN_OP (-1)
#define ZG_ERR_VALIDATION (-2)

#endif /* ZG_PROTOCOL_H */
