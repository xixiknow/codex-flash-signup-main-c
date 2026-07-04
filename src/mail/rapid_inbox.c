#include "mail/rapid_inbox.h"

#include "http_client/http_client.h"
#include "mongoose.h"

#include <ctype.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define DEFAULT_BASE_URL "http://127.0.0.1:8000"

struct domain_rule {
  char pattern[256];
  char base_domain[256];
  int wildcard_depth;
};

static char *trim(char *s) {
  char *end;
  while (*s && isspace((unsigned char) *s)) s++;
  end = s + strlen(s);
  while (end > s && isspace((unsigned char) end[-1])) *--end = '\0';
  return s;
}

static void lower_copy(char *dst, size_t dstlen, const char *src) {
  size_t i = 0;
  if (dstlen == 0) return;
  for (; src != NULL && src[i] && i + 1 < dstlen; i++) {
    dst[i] = (char) tolower((unsigned char) src[i]);
  }
  dst[i] = '\0';
}

static bool valid_domain_label(const char *label, size_t len) {
  if (len == 0 || len > 63) return false;
  if (label[0] == '-' || label[len - 1] == '-') return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char) label[i];
    if (!isalnum(ch) && ch != '-') return false;
  }
  return true;
}

static bool validate_base_domain(const char *domain) {
  const char *p = domain;
  int labels = 0;
  if (domain == NULL || domain[0] == '\0' || strlen(domain) > 253) return false;
  while (*p) {
    const char *dot = strchr(p, '.');
    size_t len = dot == NULL ? strlen(p) : (size_t) (dot - p);
    if (!valid_domain_label(p, len)) return false;
    labels++;
    if (dot == NULL) break;
    p = dot + 1;
  }
  return labels >= 2;
}

static bool parse_domain_rule(const char *pattern, struct domain_rule *rule,
                              char *error, size_t error_len) {
  char buf[256], *p, *base;
  int wildcard_depth = 0;

  if (rule == NULL) return false;
  memset(rule, 0, sizeof(*rule));
  if (pattern == NULL) {
    mg_snprintf(error, error_len, "missing pattern");
    return false;
  }
  lower_copy(buf, sizeof(buf), pattern);
  p = trim(buf);
  if (*p == '\0') {
    mg_snprintf(error, error_len, "domain pattern is empty");
    return false;
  }

  while (strncmp(p, "*.", 2) == 0) {
    wildcard_depth++;
    p += 2;
  }
  if (strchr(p, '*') != NULL || p[0] == '.') {
    mg_snprintf(error, error_len, "wildcard must be leading labels");
    return false;
  }
  base = p;
  if (!validate_base_domain(base)) {
    mg_snprintf(error, error_len, "invalid domain");
    return false;
  }

  rule->wildcard_depth = wildcard_depth;
  mg_snprintf(rule->base_domain, sizeof(rule->base_domain), "%s", base);
  if (wildcard_depth > 0) {
    struct mg_iobuf io = {0, 0, 0, 64};
    for (int i = 0; i < wildcard_depth; i++) mg_xprintf(mg_pfn_iobuf, &io, "*.");
    mg_xprintf(mg_pfn_iobuf, &io, "%s", base);
    mg_iobuf_add(&io, io.len, "", 1);
    mg_snprintf(rule->pattern, sizeof(rule->pattern), "%s", (char *) io.buf);
    mg_iobuf_free(&io);
  } else {
    mg_snprintf(rule->pattern, sizeof(rule->pattern), "%s", base);
  }
  return true;
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

static void api_key_preview(const char *key, char *out, size_t outlen) {
  size_t len;
  if (outlen == 0) return;
  out[0] = '\0';
  if (key == NULL || key[0] == '\0') return;
  len = strlen(key);
  if (len <= 4) {
    mg_snprintf(out, outlen, "****");
  } else {
    mg_snprintf(out, outlen, "****%s", key + len - 4);
  }
}

static const char *column_text(sqlite3_stmt *stmt, int col) {
  const unsigned char *s = sqlite3_column_text(stmt, col);
  return s == NULL ? "" : (const char *) s;
}

static void append_domain_rules(sqlite3 *db, struct mg_iobuf *io) {
  sqlite3_stmt *stmt = NULL;
  bool first = true;
  const char *sql =
      "SELECT id,pattern,base_domain,wildcard_depth,is_active,created_at,"
      "updated_at FROM mail_domain_rules ORDER BY id DESC";

  mg_xprintf(mg_pfn_iobuf, io, "[");
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      if (!first) mg_xprintf(mg_pfn_iobuf, io, ",");
      first = false;
      mg_xprintf(mg_pfn_iobuf, io,
                 "{%m:%lld,%m:%m,%m:%m,%m:%d,%m:%d,%m:%lld,%m:%lld}",
                 MG_ESC("id"), sqlite3_column_int64(stmt, 0),
                 MG_ESC("pattern"), MG_ESC(column_text(stmt, 1)),
                 MG_ESC("base_domain"), MG_ESC(column_text(stmt, 2)),
                 MG_ESC("wildcard_depth"), sqlite3_column_int(stmt, 3),
                 MG_ESC("is_active"), sqlite3_column_int(stmt, 4),
                 MG_ESC("created_at"), sqlite3_column_int64(stmt, 5),
                 MG_ESC("updated_at"), sqlite3_column_int64(stmt, 6));
    }
  }
  sqlite3_finalize(stmt);
  mg_xprintf(mg_pfn_iobuf, io, "]");
}

char *rapid_inbox_config_json(sqlite3 *db) {
  char *base_url = setting_value(db, "rapid_inbox_base_url", DEFAULT_BASE_URL);
  char *api_key = setting_value(db, "rapid_inbox_api_key", "");
  char preview[32];
  struct mg_iobuf io = {0, 0, 0, 512};

  api_key_preview(api_key, preview, sizeof(preview));
  mg_xprintf(mg_pfn_iobuf, &io,
             "{%m:%m,%m:%d,%m:%m,%m:",
             MG_ESC("base_url"), MG_ESC(base_url ? base_url : DEFAULT_BASE_URL),
             MG_ESC("has_api_key"), api_key != NULL && api_key[0] != '\0',
             MG_ESC("api_key_preview"), MG_ESC(preview),
             MG_ESC("domains"));
  append_domain_rules(db, &io);
  mg_xprintf(mg_pfn_iobuf, &io, "}");
  mg_iobuf_add(&io, io.len, "", 1);
  free(base_url);
  free(api_key);
  return (char *) io.buf;
}

int rapid_inbox_save_config(sqlite3 *db, const char *base_url,
                            const char *api_key) {
  char base_buf[512], key_buf[1024], *base, *key;
  int rc = 0;
  mg_snprintf(base_buf, sizeof(base_buf), "%s",
              base_url != NULL && base_url[0] != '\0' ? base_url : DEFAULT_BASE_URL);
  base = trim(base_buf);
  while (strlen(base) > 1 && base[strlen(base) - 1] == '/') {
    base[strlen(base) - 1] = '\0';
  }
  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  if (upsert_setting(db, "rapid_inbox_base_url", base) != 0) rc = -1;
  if (rc == 0 && api_key != NULL) {
    mg_snprintf(key_buf, sizeof(key_buf), "%s", api_key);
    key = trim(key_buf);
    if (upsert_setting(db, "rapid_inbox_api_key", key) != 0) rc = -1;
  }
  sqlite3_exec(db, rc == 0 ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
  return rc;
}

char *rapid_inbox_add_domain_json(sqlite3 *db, const char *pattern) {
  struct domain_rule rule;
  sqlite3_stmt *stmt = NULL;
  char error[160] = "";
  struct mg_iobuf io = {0, 0, 0, 256};
  int rc;

  if (!parse_domain_rule(pattern, &rule, error, sizeof(error))) {
    mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:%m}",
               MG_ESC("ok"), 0, MG_ESC("error"), MG_ESC(error));
    mg_iobuf_add(&io, io.len, "", 1);
    return (char *) io.buf;
  }

  const char *sql =
      "INSERT INTO mail_domain_rules(pattern,base_domain,wildcard_depth,"
      "is_active,created_at,updated_at) VALUES(?,?,?,1,unixepoch(),unixepoch()) "
      "ON CONFLICT(pattern) DO UPDATE SET is_active=1,base_domain=excluded.base_domain,"
      "wildcard_depth=excluded.wildcard_depth,updated_at=unixepoch()";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:%m}",
               MG_ESC("ok"), 0, MG_ESC("error"), MG_ESC(sqlite3_errmsg(db)));
    mg_iobuf_add(&io, io.len, "", 1);
    return (char *) io.buf;
  }
  sqlite3_bind_text(stmt, 1, rule.pattern, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, rule.base_domain, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, rule.wildcard_depth);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc == SQLITE_DONE) {
    mg_xprintf(mg_pfn_iobuf, &io,
               "{%m:%d,%m:%m,%m:%m,%m:%d}",
               MG_ESC("ok"), 1,
               MG_ESC("pattern"), MG_ESC(rule.pattern),
               MG_ESC("base_domain"), MG_ESC(rule.base_domain),
               MG_ESC("wildcard_depth"), rule.wildcard_depth);
  } else {
    mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:%m}",
               MG_ESC("ok"), 0, MG_ESC("error"), MG_ESC(sqlite3_errmsg(db)));
  }
  mg_iobuf_add(&io, io.len, "", 1);
  return (char *) io.buf;
}

int rapid_inbox_delete_domain_ids(sqlite3 *db, const long *ids, size_t count) {
  sqlite3_stmt *stmt = NULL;
  int deleted = 0;
  if (db == NULL || ids == NULL || count == 0) return 0;
  if (sqlite3_prepare_v2(db, "DELETE FROM mail_domain_rules WHERE id=?", -1,
                         &stmt, NULL) != SQLITE_OK) {
    return -1;
  }
  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  for (size_t i = 0; i < count; i++) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int64(stmt, 1, ids[i]);
    if (sqlite3_step(stmt) == SQLITE_DONE) deleted += sqlite3_changes(db);
  }
  sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  sqlite3_finalize(stmt);
  return deleted;
}

static bool build_public_url(const char *base_url, const char *mailbox,
                             const char *action, const char *delivery_id,
                             long limit, char *url, size_t url_len) {
  char mailbox_enc[512], delivery_enc[256];
  if (base_url == NULL || mailbox == NULL || action == NULL || url_len == 0) {
    return false;
  }
  if (mg_url_encode(mailbox, strlen(mailbox), mailbox_enc, sizeof(mailbox_enc)) >=
      sizeof(mailbox_enc)) {
    return false;
  }

  if (strcmp(action, "codes") == 0) {
    mg_snprintf(url, url_len,
                "%s/api/v1/public/mailboxes/%s/verification-codes?limit=%ld",
                base_url, mailbox_enc, limit > 0 ? limit : 20);
    return true;
  }
  if (strcmp(action, "messages") == 0) {
    mg_snprintf(url, url_len,
                "%s/api/v1/public/mailboxes/%s/messages?limit=%ld",
                base_url, mailbox_enc, limit > 0 ? limit : 20);
    return true;
  }
  if ((strcmp(action, "message") == 0 || strcmp(action, "message-code") == 0) &&
      delivery_id != NULL && delivery_id[0] != '\0') {
    if (mg_url_encode(delivery_id, strlen(delivery_id), delivery_enc,
                      sizeof(delivery_enc)) >= sizeof(delivery_enc)) {
      return false;
    }
    mg_snprintf(url, url_len, "%s/api/v1/public/mailboxes/%s/messages/%s%s",
                base_url, mailbox_enc, delivery_enc,
                strcmp(action, "message-code") == 0 ? "/verification-code" : "");
    return true;
  }
  return false;
}

char *rapid_inbox_fetch_json(sqlite3 *db, const char *mailbox,
                             const char *action, const char *delivery_id,
                             long limit) {
  char *base_url = setting_value(db, "rapid_inbox_base_url", DEFAULT_BASE_URL);
  char *api_key = setting_value(db, "rapid_inbox_api_key", "");
  char url[1200];
  struct http_client_header headers[] = {{"X-API-Key", api_key}};
  struct http_client_request req;
  struct http_client_response res;
  struct mg_iobuf io = {0, 0, 0, 512};

  if (api_key == NULL || api_key[0] == '\0') {
    mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:%m}",
               MG_ESC("ok"), 0, MG_ESC("error"), MG_ESC("Rapid-Inbox API Key 未配置"));
    goto done;
  }
  if (mailbox == NULL || strchr(mailbox, '@') == NULL) {
    mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:%m}",
               MG_ESC("ok"), 0, MG_ESC("error"), MG_ESC("邮箱地址不完整"));
    goto done;
  }
  if (!build_public_url(base_url, mailbox, action, delivery_id, limit, url,
                        sizeof(url))) {
    mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:%m}",
               MG_ESC("ok"), 0, MG_ESC("error"), MG_ESC("请求类型或 delivery_id 无效"));
    goto done;
  }

  memset(&req, 0, sizeof(req));
  req.method = "GET";
  req.url = url;
  req.timeout_ms = 15000;
  req.headers = headers;
  req.num_headers = 1;

  if (http_client_perform(&req, &res) != 0) {
    mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:%m,%m:%m}",
               MG_ESC("ok"), 0,
               MG_ESC("endpoint"), MG_ESC(url),
               MG_ESC("error"), MG_ESC(res.error));
    goto done;
  }

  mg_xprintf(mg_pfn_iobuf, &io,
             "{%m:%d,%m:%ld,%m:%m,%m:%m}",
             MG_ESC("ok"), res.status_code >= 200 && res.status_code < 300,
             MG_ESC("status_code"), res.status_code,
             MG_ESC("endpoint"), MG_ESC(url),
             MG_ESC("body"), MG_ESC(res.body ? res.body : ""));
  http_client_response_free(&res);

done:
  mg_iobuf_add(&io, io.len, "", 1);
  free(base_url);
  free(api_key);
  return (char *) io.buf;
}

static void set_fetch_error(char *error, size_t error_len,
                            const char *message) {
  if (error_len == 0) return;
  mg_snprintf(error, error_len, "%s", message ? message : "Rapid-Inbox 请求失败");
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

static long parse_rfc3339_epoch(const char *value) {
  struct tm tm_value;
  char *end;
  long epoch;

  if (value == NULL || value[0] == '\0') return 0;
  memset(&tm_value, 0, sizeof(tm_value));
  end = strptime(value, "%Y-%m-%dT%H:%M:%S", &tm_value);
  if (end == NULL) return 0;
  epoch = (long) timegm(&tm_value);
  return epoch > 0 ? epoch : 0;
}

static bool copy_code_from_item(const char *json, int index, char *code,
                                size_t code_len) {
  char path[96];
  char text[512];
  mg_snprintf(path, sizeof(path), "$.items[%d].verification_code", index);
  if (copy_json_string_field(json, path, code, code_len)) return true;
  mg_snprintf(path, sizeof(path), "$.items[%d].code", index);
  if (copy_json_string_field(json, path, code, code_len)) return true;
  mg_snprintf(path, sizeof(path), "$.items[%d].subject", index);
  if (copy_json_string_field(json, path, text, sizeof(text))) {
    size_t run = 0;
    const char *start = NULL;
    for (const char *p = text; ; p++) {
      if (isdigit((unsigned char) *p)) {
        if (run == 0) start = p;
        run++;
      } else {
        if (run == 6 && code_len > 6) {
          memcpy(code, start, 6);
          code[6] = '\0';
          return true;
        }
        run = 0;
        start = NULL;
      }
      if (*p == '\0') break;
    }
  }
  return false;
}

static long item_received_at(const char *json, int index) {
  char path[96];
  char value[96];

  mg_snprintf(path, sizeof(path), "$.items[%d].received_at", index);
  if (copy_json_string_field(json, path, value, sizeof(value))) {
    return parse_rfc3339_epoch(value);
  }
  mg_snprintf(path, sizeof(path), "$.items[%d].created_at", index);
  if (copy_json_string_field(json, path, value, sizeof(value))) {
    return parse_rfc3339_epoch(value);
  }
  return 0;
}

static int fetch_latest_code_impl(sqlite3 *db, const char *mailbox,
                                  long min_received_at, char *code,
                                  size_t code_len, char *error,
                                  size_t error_len) {
  char *base_url = setting_value(db, "rapid_inbox_base_url", DEFAULT_BASE_URL);
  char *api_key = setting_value(db, "rapid_inbox_api_key", "");
  char url[1200];
  struct http_client_header headers[] = {{"X-API-Key", api_key}};
  struct http_client_request req;
  struct http_client_response res;
  int result = -1;

  if (code != NULL && code_len > 0) code[0] = '\0';
  if (api_key == NULL || api_key[0] == '\0') {
    set_fetch_error(error, error_len, "Rapid-Inbox API Key 未配置");
    goto done;
  }
  if (mailbox == NULL || strchr(mailbox, '@') == NULL) {
    set_fetch_error(error, error_len, "邮箱地址不完整");
    goto done;
  }
  if (!build_public_url(base_url, mailbox, "codes", NULL, 20, url,
                        sizeof(url))) {
    set_fetch_error(error, error_len, "Rapid-Inbox 验证码接口构造失败");
    goto done;
  }

  memset(&req, 0, sizeof(req));
  req.method = "GET";
  req.url = url;
  req.timeout_ms = 5000;
  req.headers = headers;
  req.num_headers = 1;

  if (http_client_perform(&req, &res) != 0) {
    set_fetch_error(error, error_len, res.error);
    goto done;
  }
  if (res.status_code < 200 || res.status_code >= 300) {
    mg_snprintf(error, error_len, "Rapid-Inbox HTTP %ld", res.status_code);
    http_client_response_free(&res);
    goto done;
  }

  if (min_received_at > 0) {
    for (int i = 0; i < 20; i++) {
      long received_at = item_received_at(res.body, i);
      if (received_at == 0) break;
      if (received_at + 10 < min_received_at) continue;
      if (copy_code_from_item(res.body, i, code, code_len)) {
        result = 1;
        break;
      }
    }
    if (result < 0) result = 0;
  } else if (copy_code_from_item(res.body, 0, code, code_len) ||
             copy_json_string_field(res.body, "$.verification_code", code,
                                    code_len) ||
             copy_json_string_field(res.body, "$.code", code, code_len)) {
    result = 1;
  } else {
    result = 0;
  }
  http_client_response_free(&res);

done:
  free(base_url);
  free(api_key);
  return result;
}

int rapid_inbox_fetch_latest_code(sqlite3 *db, const char *mailbox,
                                  char *code, size_t code_len,
                                  char *error, size_t error_len) {
  return fetch_latest_code_impl(db, mailbox, 0, code, code_len, error,
                                error_len);
}

int rapid_inbox_fetch_latest_code_since(sqlite3 *db, const char *mailbox,
                                        long min_received_at, char *code,
                                        size_t code_len, char *error,
                                        size_t error_len) {
  return fetch_latest_code_impl(db, mailbox, min_received_at, code, code_len,
                                error, error_len);
}
