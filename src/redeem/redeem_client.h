#ifndef APP_REDEEM_CLIENT_H
#define APP_REDEEM_CLIENT_H

#include <sqlite3.h>
#include <stddef.h>

#define REDEEM_EMAIL_LEN 320
#define REDEEM_TEXT_LEN 128
#define REDEEM_TIME_LEN 48

struct redeem_result {
  char type[REDEEM_TEXT_LEN];
  char email[REDEEM_EMAIL_LEN];
  char phone[REDEEM_EMAIL_LEN];
  char product_name[REDEEM_TEXT_LEN];
  char end_time[REDEEM_TIME_LEN];
  long card_id;
  long session_id;
};

/* 读取/保存 paymesh 兑换接口地址（存 mail_settings，key=redeem_base_url）。 */
char *redeem_config_json(sqlite3 *db);
int redeem_save_config(sqlite3 *db, const char *base_url);

/* POST {base}/api/v1/redeem，成功返回 0 并填充 out；失败返回 -1 并写入 error。 */
int redeem_apply(sqlite3 *db, const char *code, struct redeem_result *out,
                 char *error, size_t error_len);

/* GET {base}/api/v1/order/lookup?code=..&poll=true 取最新验证码。
 * 返回 1 已取到，0 暂未收到，-1 出错（与 rapid_inbox_fetch_latest_code 约定一致）。 */
int redeem_lookup_code(sqlite3 *db, const char *code, char *out_code,
                       size_t out_len, char *error, size_t error_len);

#endif
