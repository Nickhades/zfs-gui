/* zg-auth.h - 管理员密码二次验证（PAM）
 *
 * 危险操作在执行前必须由调用方提供「管理员账号 + 密码」，
 * 守护进程通过 PAM 验证，并确认该账号确实属于管理员集合
 * （root / allowed_users / allowed_groups）。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_AUTH_H
#define ZG_AUTH_H

#include <glib.h>
#include <sys/types.h>

#include "zg-policy.h"

G_BEGIN_DECLS

typedef enum
{
    ZG_AUTH_OK = 0,
    ZG_AUTH_BAD_CREDENTIALS, /* 密码错误 */
    ZG_AUTH_NOT_ADMIN,       /* 密码正确但账号不是管理员 */
    ZG_AUTH_LOCKED,          /* 失败次数过多 */
    ZG_AUTH_UNAVAILABLE,     /* PAM 不可用或内部错误 */
} ZgAuthStatus;

/* 校验管理员身份。message 返回面向用户的中文说明（调用方 g_free）。
 * 实现内部带失败限速，密码只在本函数栈上短暂存在。 */
ZgAuthStatus zg_auth_verify(const ZgPolicy *policy,
                            uid_t peer_uid,
                            const gchar *username,
                            const gchar *password,
                            gchar **message);

/* 供前端预告：该守护进程的认证策略（"dangerous"/"all"/"none"） */
const gchar *zg_auth_mode(const ZgPolicy *policy);

G_END_DECLS

#endif /* ZG_AUTH_H */
