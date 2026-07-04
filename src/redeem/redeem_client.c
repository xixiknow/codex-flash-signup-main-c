#include "redeem/redeem_client.h"

#include "http_client/http_client.h"
#include "mongoose.h"

#include <ctype.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_BASE_URL "https://sms.paymesh.cn"

static char *trim(char *s) {
  char *end;
  while (*s && isspace((unsigned char) *s)) s++;
  end = s + strlen(s);
  while (end > s && isspace((unsigned char) end[-1])) *--end = '\0';
  return s;
}

static int upsert_setting(sqlite3 *db, const char *key, const char *value) {
  sqlite3_stmt *stmt = NULL;
  const char *sql =
      "INSERT INTO mail_settings(key,value,updated_at) VALUES(?,?,unixepoch()) "
      "ON CONFLICT(key) DO UPDATE SET value=excluded.value,"
      "updated_at=unixepoch()";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, value ? value : "", -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

static char *setting_value(sqlite3 *db, const char *key, const char *fallback) {
  sqlite3_stmt *stmt = NULL;
  char *value = NULL;
  if (sqlite3_prepare_v2(db, "SELECT value FROM mail_settings WHERE key=?", -1,
                         &stmt, NULL) != SQLITE_OK) {
    return strdup(fallback ? fallback : "");
  }
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *s = sqlite3_column_text(stmt, 0);
    value = strdup(s == NULL ? "" : (const char *) s);
  }
  sqlite3_finalize(stmt);
  if (value == NULL) value = strdup(fallback ? fallback : "");
  return value;
}

char *redeem_config_json(sqlite3 *db) {
  char *base_url = setting_value(db, "redeem_base_url", DEFAULT_BASE_URL);
  struct mg_iobuf io = {0, 0, 0, 256};

  mg_xprintf(mg_pfn_iobuf, &io, "{%m:%m}",
             MG_ESC("base_url"),
             MG_ESC(base_url && base_url[0] ? base_url : DEFAULT_BASE_URL));
  mg_iobuf_add(&io, io.len, "", 1);
  free(base_url);
  return (char *) io.buf;
}

int redeem_save_config(sqlite3 *db, const char *base_url) {
  char base_buf[512], *base;
  int rc;
  mg_snprintf(base_buf, sizeof(base_buf), "%s",
              base_url != NULL && base_url[0] != '\0' ? base_url
                                                      : DEFAULT_BASE_URL);
  base = trim(base_buf);
  while (strlen(base) > 1 && base[strlen(base) - 1] == '/') {
    base[strlen(base) - 1] = '\0';
  }
  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  rc = upsert_setting(db, "redeem_base_url", base);
  sqlite3_exec(db, rc == 0 ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
  return rc;
}

static void set_error(char *error, size_t error_len, const char *message) {
  if (error_len == 0) return;
  mg_snprintf(error, error_len, "%s", message ? message : "兑换请求失败");
}

static bool copy_json_string_field(const char *json, const char *path,
                                   char *out, size_t out_len) {
  char *value;
  if (json == NULL || path == NULL || out == NULL || out_len == 0) return false;
  value = mg_json_get_str(mg_str(json), path);
  if (value == NULL || value[0] == '\0') {
    mg_free(value);
    return false;
  }
  mg_snprintf(out, out_len, "%s", value);
  mg_free(value);
  return true;
}

int redeem_apply(sqlite3 *db, const char *code, struct redeem_result *out,
                 char *error, size_t error_len) {
  char *base_url = setting_value(db, "redeem_base_url", DEFAULT_BASE_URL);
  char url[1024];
  char body[256];
  struct http_client_header headers[] = {{"Content-Type", "application/json"}};
  struct http_client_request req;
  struct http_client_response res;
  int result = -1;
  long api_code;

  if (out != NULL) memset(out, 0, sizeof(*out));
  if (code == NULL || code[0] == '\0') {
    set_error(error, error_len, "兑换码为空");
    goto done;
  }
  mg_snprintf(url, sizeof(url), "%s/api/v1/redeem", base_url);
  mg_snprintf(body, sizeof(body), "{\"code\":\"%s\"}", code);

  memset(&req, 0, sizeof(req));
  req.method = "POST";
  req.url = url;
  req.body = body;
  req.body_len = strlen(body);
  req.timeout_ms = 15000;
  req.headers = headers;
  req.num_headers = 1;

  if (http_client_perform(&req, &res) != 0) {
    set_error(error, error_len, res.error);
    goto done;
  }
  if (res.status_code < 200 || res.status_code >= 300) {
    mg_snprintf(error, error_len, "兑换接口 HTTP %ld", res.status_code);
    http_client_response_free(&res);
    goto done;
  }

  api_code = mg_json_get_long(mg_str(res.body ? res.body : ""), "$.code", -1);
  if (api_code != 0) {
    char *msg = mg_json_get_str(mg_str(res.body ? res.body : ""), "$.msg");
    mg_snprintf(error, error_len, "兑换失败: %s",
                msg && msg[0] ? msg : "接口返回错误");
    mg_free(msg);
    http_client_response_free(&res);
    goto done;
  }

  if (out != NULL) {
    const char *json = res.body ? res.body : "";
    if (!copy_json_string_field(json, "$.data.emailAddress", out->email,
                                sizeof(out->email))) {
      copy_json_string_field(json, "$.data.phone", out->email,
                             sizeof(out->email));
    }
    copy_json_string_field(json, "$.data.phone", out->phone,
                           sizeof(out->phone));
    copy_json_string_field(json, "$.data.type", out->type, sizeof(out->type));
    copy_json_string_field(json, "$.data.productName", out->product_name,
                           sizeof(out->product_name));
    copy_json_string_field(json, "$.data.endTime", out->end_time,
                           sizeof(out->end_time));
    out->card_id = mg_json_get_long(mg_str(json), "$.data.cardId", 0);
    out->session_id = mg_json_get_long(mg_str(json), "$.data.sessionId", 0);
    if (out->email[0] == '\0') {
      set_error(error, error_len, "兑换成功但未返回邮箱地址");
      http_client_response_free(&res);
      goto done;
    }
  }
  result = 0;
  http_client_response_free(&res);

done:
  free(base_url);
  return result;
}

int redeem_lookup_code(sqlite3 *db, const char *code, char *out_code,
                       size_t out_len, char *error, size_t error_len) {
  char *base_url = setting_value(db, "redeem_base_url", DEFAULT_BASE_URL);
  char url[1024];
  char code_enc[256];
  struct http_client_request req;
  struct http_client_response res;
  int result = -1;

  if (out_code != NULL && out_len > 0) out_code[0] = '\0';
  if (code == NULL || code[0] == '\0') {
    set_error(error, error_len, "兑换码为空");
    goto done;
  }
  if (mg_url_encode(code, strlen(code), code_enc, sizeof(code_enc)) >=
      sizeof(code_enc)) {
    set_error(error, error_len, "兑换码编码失败");
    goto done;
  }
  mg_snprintf(url, sizeof(url), "%s/api/v1/order/lookup?code=%s&poll=true",
              base_url, code_enc);

  memset(&req, 0, sizeof(req));
  req.method = "GET";
  req.url = url;
  req.timeout_ms = 8000;

  if (http_client_perform(&req, &res) != 0) {
    set_error(error, error_len, res.error);
    goto done;
  }
  if (res.status_code < 200 || res.status_code >= 300) {
    mg_snprintf(error, error_len, "查询接口 HTTP %ld", res.status_code);
    http_client_response_free(&res);
    goto done;
  }

  {
    const char *json = res.body ? res.body : "";
    char path[96];
    bool found = false;
    /* codes 数组按接收顺序排列，取最新（末尾）一条；逐个探测下标。 */
    for (int i = 0; i < 32; i++) {
      char probe[32];
      mg_snprintf(path, sizeof(path), "$.data.email.codes[%d].code", i);
      if (copy_json_string_field(json, path, probe, sizeof(probe))) {
        if (out_code != NULL) {
          mg_snprintf(out_code, out_len, "%s", probe);
        }
        found = true;
      } else {
        break;
      }
    }
    result = found ? 1 : 0;
  }
  http_client_response_free(&res);

done:
  free(base_url);
  return result;
}
