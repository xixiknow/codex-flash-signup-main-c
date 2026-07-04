#ifndef APP_REDEEM_STORE_H
#define APP_REDEEM_STORE_H

#include "redeem/redeem_client.h"

#include <sqlite3.h>
#include <stddef.h>

/* 列表（游标分页，仿 account_list_json）。status 为空表示全部。 */
char *redeem_list_json(sqlite3 *db, const char *status, long cursor,
                       long limit);

/* 批量导入兑换码文本（一行一个，去重）。返回 JSON 统计。 */
char *redeem_import_json(sqlite3 *db, const char *text);

/* 删除指定兑换码 ID。返回删除数量，出错返回 -1。 */
int redeem_delete_ids(sqlite3 *db, const long *ids, size_t count);

/* 对选中兑换码调用 paymesh 兑换接口并回写。返回 JSON 结果。 */
char *redeem_redeem_ids_json(sqlite3 *db, const long *ids, size_t count);

/* 读取一条兑换码记录的 code 与 email；返回 0 成功。 */
int redeem_load_code(sqlite3 *db, long id, char *code, size_t code_len,
                     char *email, size_t email_len);

/* 兑换成功后回写 email 等字段与状态。 */
int redeem_mark_redeemed(sqlite3 *db, long id, const struct redeem_result *r);
/* 注册成功后绑定账号并置为 registered。 */
int redeem_mark_registered(sqlite3 *db, long id, long account_id);
/* 标记失败并记录原因。 */
int redeem_mark_failed(sqlite3 *db, long id, const char *error);

#endif
