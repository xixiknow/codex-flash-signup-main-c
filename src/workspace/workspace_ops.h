#ifndef APP_WORKSPACE_OPS_H
#define APP_WORKSPACE_OPS_H

#include <sqlite3.h>
#include <stddef.h>

/* 加入工作区：对每个账号 POST /backend-api/accounts/{acct}/invites/request。
 * 返回 JSON 结果（ok/success_count/failed_count/details）。 */
char *workspace_join_json(sqlite3 *db, const long *ids, size_t count);

/* 退出工作区：对每个账号 DELETE /backend-api/accounts/{acct}/users/{user_id}。 */
char *workspace_leave_json(sqlite3 *db, const long *ids, size_t count);

/* 导出凭证：format = "codex" | "cpa" | "sub2api"。
 * 返回 JSON：{ok, format, filename, content}，content 为可直接落地的 JSON 文本。 */
char *workspace_export_json(sqlite3 *db, const long *ids, size_t count,
                            const char *format);

#endif
