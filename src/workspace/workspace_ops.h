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

/* 兑换码自动化链路：账号完成 OAuth 拿到 access_token 后，对每个目标工作区
 * 依次执行「上车（加入外部目标工作区）+ 推送 aether（按该工作区标记一次）」。
 * target_workspace_ids 为外部共享工作区 ID 数组，每个都会被上车并单独推送一次。
 * 返回 JSON 结果（ok/join_success/join_failed/push_success/push_failed/details）。
 * 调用方负责 free。 */
char *workspace_onboard_and_push_json(sqlite3 *db, const char *email,
                                      const char *access_token,
                                      const char *refresh_token,
                                      const char *external_account_id,
                                      const char *const *target_workspace_ids,
                                      size_t target_count,
                                      const char *pool_type);

#endif
