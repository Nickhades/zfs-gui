/* zg-server.h - Unix socket 服务端
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_SERVER_H
#define ZG_SERVER_H

#include "zg-policy.h"

G_BEGIN_DECLS

/* 阻塞运行服务循环，直到收到 SIGTERM / SIGINT。
 * 返回 0 表示正常退出，负值表示启动失败。 */
int zg_server_run(const ZgPolicy *policy);

G_END_DECLS

#endif /* ZG_SERVER_H */
