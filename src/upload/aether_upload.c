#include "upload/aether_upload.h"

#include "account/account_store.h"
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

#define AETHER_URL_LEN 768
#define AETHER_TEXT_LEN 256
#define AETHER_TOKEN_PREVIEW_LEN 20
#define AETHER_TIMEOUT_MS 90000L

struct aether_service {
  long id;
  char name[AETHER_TEXT_LEN];
  char api_url[512];
  char *management_token;
  char provider_id[AETHER_TEXT_LEN];
  char provider_name[AETHER_TEXT_LEN];
  char chatgpt_web_provider_id[AETHER_TEXT_LEN];
  char chatgpt_web_provider_name[AETHER_TEXT_LEN];
  char proxy_node_id[AETHER_TEXT_LEN];
  char proxy_node_name[AETHER_TEXT_LEN];
  int enabled;
  int priority;
};

struct upload_account {
  long id;
  char *email;
  char *access_token;
  char *refresh_token;
  char *external_account_id;
  char *workspace_id;
};

static const char *column_text(sqlite3_stmt *stmt, int col) {
  const unsigned char *s = sqlite3_column_text(stmt, col);
  return s == NULL ? "" : (const char *) s;
}

static char *str_dup(const char *s) {
  size_t len;
  char *copy;
  if (s == NULL) s = "";
  len = strlen(s);
  copy = (char *) malloc(len + 1);
  if (copy == NULL) return NULL;
  memcpy(copy, s, len + 1);
  return copy;
}

static char *trim_in_place(char *s) {
  char *end;
  if (s == NULL) return NULL;
  while (*s && isspace((unsigned char) *s)) s++;
  end = s + strlen(s);
  while (end > s && isspace((unsigned char) end[-1])) *--end = '\0';
  return s;
}

static char *json_get_trim(struct mg_str body, const char *path) {
  char *raw = mg_json_get_str(body, path);
  char *trimmed;
  char *copy;
  if (raw == NULL) return NULL;
  trimmed = trim_in_place(raw);
  copy = str_dup(trimmed);
  mg_free(raw);
  return copy;
}

static bool has_text(const char *s) {
  if (s == NULL) return false;
  while (*s) {
    if (!isspace((unsigned char) *s)) return true;
    s++;
  }
  return false;
}

static bool text_equals_ci(const char *a, const char *b) {
  if (a == NULL || b == NULL) return false;
  return strcasecmp(a, b) == 0;
}

static bool contains_ci(const char *haystack, const char *needle) {
  if (haystack == NULL || needle == NULL || *needle == '\0') return false;
  return strcasestr(haystack, needle) != NULL;
}

static bool is_already_exists_error(const char *message) {
  return contains_ci(message, "已存在") ||
         contains_ci(message, "already exists") ||
         contains_ci(message, "already exist") ||
         contains_ci(message, "duplicate");
}

static void free_service(struct aether_service *svc) {
  if (svc == NULL) return;
  free(svc->management_token);
  svc->management_token = NULL;
}

static void fill_service_from_stmt(struct aether_service *svc,
                                   sqlite3_stmt *stmt) {
  memset(svc, 0, sizeof(*svc));
  svc->id = (long) sqlite3_column_int64(stmt, 0);
  mg_snprintf(svc->name, sizeof(svc->name), "%s", column_text(stmt, 1));
  mg_snprintf(svc->api_url, sizeof(svc->api_url), "%s", column_text(stmt, 2));
  svc->management_token = str_dup(column_text(stmt, 3));
  mg_snprintf(svc->provider_id, sizeof(svc->provider_id), "%s",
              column_text(stmt, 4));
  mg_snprintf(svc->provider_name, sizeof(svc->provider_name), "%s",
              column_text(stmt, 5));
  mg_snprintf(svc->chatgpt_web_provider_id,
              sizeof(svc->chatgpt_web_provider_id), "%s",
              column_text(stmt, 6));
  mg_snprintf(svc->chatgpt_web_provider_name,
              sizeof(svc->chatgpt_web_provider_name), "%s",
              column_text(stmt, 7));
  mg_snprintf(svc->proxy_node_id, sizeof(svc->proxy_node_id), "%s",
              column_text(stmt, 8));
  mg_snprintf(svc->proxy_node_name, sizeof(svc->proxy_node_name), "%s",
              column_text(stmt, 9));
  svc->enabled = sqlite3_column_int(stmt, 10);
  svc->priority = sqlite3_column_int(stmt, 11);
}

static int load_service_by_id(sqlite3 *db, long id, struct aether_service *out) {
  sqlite3_stmt *stmt = NULL;
  const char *sql =
      "SELECT id,name,api_url,management_token,provider_id,provider_name,"
      "COALESCE(chatgpt_web_provider_id,''),"
      "COALESCE(chatgpt_web_provider_name,''),"
      "COALESCE(proxy_node_id,''),COALESCE(proxy_node_name,''),"
      "enabled,priority FROM aether_services WHERE id=?";
  int rc = -1;

  if (out != NULL) memset(out, 0, sizeof(*out));
  if (db == NULL || id <= 0 || out == NULL) return -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int64(stmt, 1, id);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    fill_service_from_stmt(out, stmt);
    rc = 0;
  }
  sqlite3_finalize(stmt);
  return rc;
}

static int load_default_service(sqlite3 *db, bool prefer_web_pool,
                                struct aether_service *out) {
  sqlite3_stmt *stmt = NULL;
  const char *sql =
      "SELECT id,name,api_url,management_token,provider_id,provider_name,"
      "COALESCE(chatgpt_web_provider_id,''),"
      "COALESCE(chatgpt_web_provider_name,''),"
      "COALESCE(proxy_node_id,''),COALESCE(proxy_node_name,''),"
      "enabled,priority FROM aether_services "
      "WHERE enabled=1 ORDER BY priority ASC,id ASC";
  int rc = -1;

  if (out != NULL) memset(out, 0, sizeof(*out));
  if (db == NULL || out == NULL) return -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    struct aether_service candidate;
    fill_service_from_stmt(&candidate, stmt);
    if (!prefer_web_pool || has_text(candidate.chatgpt_web_provider_id)) {
      *out = candidate;
      rc = 0;
      break;
    }
    free_service(&candidate);
  }
  sqlite3_finalize(stmt);
  return rc;
}

static void normalize_root_url(const char *api_url, char *out, size_t len) {
  static const char *suffixes[] = {
      "/openapi.json",
      "/redoc",
      "/docs",
      "/api/admin/provider-oauth",
      "/api/admin/providers",
      "/api/admin/pool",
      "/api/admin",
      "/api",
  };
  size_t n;

  if (out == NULL || len == 0) return;
  mg_snprintf(out, len, "%s", api_url ? api_url : "");
  n = strlen(out);
  while (n > 0 && out[n - 1] == '/') out[--n] = '\0';
  for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
    size_t suffix_len = strlen(suffixes[i]);
    if (n >= suffix_len &&
        strcasecmp(out + n - suffix_len, suffixes[i]) == 0) {
      out[n - suffix_len] = '\0';
      return;
    }
  }
}

static int build_aether_url(const char *api_url, const char *path, char *out,
                            size_t len) {
  char root[512];
  normalize_root_url(api_url, root, sizeof(root));
  if (!has_text(root) || path == NULL || out == NULL || len == 0) return -1;
  mg_snprintf(out, len, "%s%s%s", root, path[0] == '/' ? "" : "/", path);
  return 0;
}

static void append_json_field(struct mg_iobuf *io, bool *first, const char *key,
                              const char *value) {
  if (!has_text(value)) return;
  mg_xprintf(mg_pfn_iobuf, io, "%s%m:%m", *first ? "" : ",", MG_ESC(key),
             MG_ESC(value));
  *first = false;
}

static void append_detail(struct mg_iobuf *details, bool *first, long id,
                          const char *email, bool success, const char *message,
                          const char *error) {
  mg_xprintf(mg_pfn_iobuf, details, "%s{%m:%ld,%m:%m,%m:%d",
             *first ? "" : ",", MG_ESC("id"), id, MG_ESC("email"),
             MG_ESC(email ? email : ""), MG_ESC("success"), success);
  if (success) {
    mg_xprintf(mg_pfn_iobuf, details, ",%m:%m", MG_ESC("message"),
               MG_ESC(message ? message : "上传成功"));
  } else {
    mg_xprintf(mg_pfn_iobuf, details, ",%m:%m", MG_ESC("error"),
               MG_ESC(error ? error : "上传失败"));
  }
  mg_xprintf(mg_pfn_iobuf, details, "}");
  *first = false;
}

static void free_upload_account(struct upload_account *account) {
  if (account == NULL) return;
  free(account->email);
  free(account->access_token);
  free(account->refresh_token);
  free(account->external_account_id);
  free(account->workspace_id);
}

static int load_upload_account(sqlite3 *db, long id,
                               struct upload_account *out) {
  sqlite3_stmt *stmt = NULL;
  const char *sql =
      "SELECT a.id,a.email,COALESCE(s.access_token,''),"
      "COALESCE(s.refresh_token,''),COALESCE(s.external_account_id,''),"
      "COALESCE(s.workspace_id,'') "
      "FROM accounts a LEFT JOIN account_secrets s ON s.account_id=a.id "
      "WHERE a.id=?";

  memset(out, 0, sizeof(*out));
  if (db == NULL || id <= 0 || out == NULL) return -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int64(stmt, 1, id);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return -1;
  }
  out->id = (long) sqlite3_column_int64(stmt, 0);
  out->email = str_dup(column_text(stmt, 1));
  out->access_token = str_dup(column_text(stmt, 2));
  out->refresh_token = str_dup(column_text(stmt, 3));
  out->external_account_id = str_dup(column_text(stmt, 4));
  out->workspace_id = str_dup(column_text(stmt, 5));
  sqlite3_finalize(stmt);
  return out->email == NULL ? -1 : 0;
}

static void append_credential_entry(struct mg_iobuf *credentials,
                                    const struct upload_account *account,
                                    bool include_web_credentials) {
  bool first = true;
  mg_xprintf(mg_pfn_iobuf, credentials, "{");
  if (include_web_credentials) {
    append_json_field(credentials, &first, "accessToken",
                      account->access_token);
    append_json_field(credentials, &first, "email", account->email);
    append_json_field(credentials, &first, "accountId",
                      account->external_account_id);
    append_json_field(credentials, &first, "workspaceId",
                      account->workspace_id);
  } else {
    append_json_field(credentials, &first, "refresh_token",
                      account->refresh_token);
    append_json_field(credentials, &first, "access_token",
                      account->access_token);
    append_json_field(credentials, &first, "email", account->email);
    append_json_field(credentials, &first, "account_id",
                      account->external_account_id);
    append_json_field(credentials, &first, "workspace_id",
                      account->workspace_id);
  }
  mg_xprintf(mg_pfn_iobuf, credentials, "}");
}

static char *extract_error_message(const struct http_client_response *res,
                                   const char *fallback) {
  char *msg = NULL;
  if (res != NULL && res->body != NULL) {
    msg = mg_json_get_str(mg_str(res->body), "$.message");
    if (msg == NULL) msg = mg_json_get_str(mg_str(res->body), "$.detail");
    if (msg == NULL) msg = mg_json_get_str(mg_str(res->body), "$.error.message");
    if (msg == NULL) msg = mg_json_get_str(mg_str(res->body), "$.error");
  }
  if (msg == NULL) {
    char buf[320];
    if (res != NULL && res->status_code > 0) {
      mg_snprintf(buf, sizeof(buf), "%s: HTTP %ld", fallback ? fallback : "请求失败",
                  res->status_code);
    } else if (res != NULL && res->error[0] != '\0') {
      mg_snprintf(buf, sizeof(buf), "%s: %s", fallback ? fallback : "请求失败",
                  res->error);
    } else {
      mg_snprintf(buf, sizeof(buf), "%s", fallback ? fallback : "请求失败");
    }
    return str_dup(buf);
  }
  return msg;
}

static int aether_get_json(const char *api_url, const char *management_token,
                           const char *path,
                           struct http_client_response *res) {
  struct http_client_header headers[1];
  struct http_client_request req;
  char auth_header[1024];
  char url[AETHER_URL_LEN];

  if (build_aether_url(api_url, path, url, sizeof(url)) != 0) {
    if (res != NULL) memset(res, 0, sizeof(*res));
    return -1;
  }
  mg_snprintf(auth_header, sizeof(auth_header), "Bearer %s",
              management_token ? management_token : "");
  headers[0].name = "Authorization";
  headers[0].value = auth_header;

  memset(&req, 0, sizeof(req));
  req.method = "GET";
  req.url = url;
  req.timeout_ms = 15000L;
  req.headers = headers;
  req.num_headers = 1;
  return http_client_perform(&req, res);
}

static int post_batch_import(const struct aether_service *svc,
                             const char *provider_id,
                             const char *credentials_json,
                             struct http_client_response *res) {
  struct http_client_header headers[2];
  struct http_client_request req;
  struct mg_iobuf body = {0, 0, 0, 1024};
  char auth_header[1024];
  char root[512], url[AETHER_URL_LEN];
  int rc;

  normalize_root_url(svc->api_url, root, sizeof(root));
  if (!has_text(root)) {
    if (res != NULL) memset(res, 0, sizeof(*res));
    return -1;
  }
  mg_snprintf(url, sizeof(url),
              "%s/api/admin/provider-oauth/providers/%s/batch-import",
              root, provider_id);
  mg_snprintf(auth_header, sizeof(auth_header), "Bearer %s",
              svc->management_token ? svc->management_token : "");
  headers[0].name = "Authorization";
  headers[0].value = auth_header;
  headers[1].name = "Content-Type";
  headers[1].value = "application/json";

  mg_xprintf(mg_pfn_iobuf, &body, "{%m:%m", MG_ESC("credentials"),
             MG_ESC(credentials_json));
  if (has_text(svc->proxy_node_id)) {
    mg_xprintf(mg_pfn_iobuf, &body, ",%m:%m", MG_ESC("proxy_node_id"),
               MG_ESC(svc->proxy_node_id));
  }
  mg_xprintf(mg_pfn_iobuf, &body, "}");
  mg_iobuf_add(&body, body.len, "", 1);

  memset(&req, 0, sizeof(req));
  req.method = "POST";
  req.url = url;
  req.body = (char *) body.buf;
  req.body_len = body.len > 0 ? body.len - 1 : 0;
  req.timeout_ms = AETHER_TIMEOUT_MS;
  req.headers = headers;
  req.num_headers = 2;

  rc = http_client_perform(&req, res);
  free(body.buf);
  return rc;
}

static char *json_item_str(struct mg_str json, const char *array_path,
                           size_t index, const char *field) {
  char path[160];
  mg_snprintf(path, sizeof(path), "%s[%lu].%s", array_path,
              (unsigned long) index, field);
  return mg_json_get_str(json, path);
}

static long json_item_long(struct mg_str json, const char *array_path,
                           size_t index, const char *field, long fallback) {
  char path[160];
  int len = 0;
  mg_snprintf(path, sizeof(path), "%s[%lu].%s", array_path,
              (unsigned long) index, field);
  if (mg_json_get(json, path, &len) < 0) return fallback;
  return mg_json_get_long(json, path, fallback);
}

static bool json_item_bool(struct mg_str json, const char *array_path,
                           size_t index, const char *field, bool fallback) {
  char path[160];
  bool value = fallback;
  mg_snprintf(path, sizeof(path), "%s[%lu].%s", array_path,
              (unsigned long) index, field);
  mg_json_get_bool(json, path, &value);
  return value;
}

static void append_aether_pools(struct mg_iobuf *out, const char *body) {
  struct mg_str json = mg_str(body ? body : "");
  bool first = true;

  for (size_t i = 0; i < 2000; i++) {
    char *provider_id = json_item_str(json, "$.items", i, "provider_id");
    char *provider_name = NULL;
    char *provider_type = NULL;
    long total_keys, active_keys, cooldown_count;
    bool pool_enabled;

    if (provider_id == NULL) provider_id = json_item_str(json, "$.items", i, "id");
    if (provider_id == NULL) break;
    if (!has_text(provider_id)) {
      mg_free(provider_id);
      continue;
    }
    provider_name = json_item_str(json, "$.items", i, "provider_name");
    if (provider_name == NULL) provider_name = json_item_str(json, "$.items", i, "name");
    provider_type = json_item_str(json, "$.items", i, "provider_type");
    total_keys = json_item_long(json, "$.items", i, "total_keys", 0);
    active_keys = json_item_long(json, "$.items", i, "active_keys", 0);
    cooldown_count = json_item_long(json, "$.items", i, "cooldown_count", 0);
    pool_enabled = json_item_bool(json, "$.items", i, "pool_enabled", true);

    mg_xprintf(
        mg_pfn_iobuf, out,
        "%s{%m:%m,%m:%m,%m:%m,%m:%ld,%m:%ld,%m:%ld,%m:%d}",
        first ? "" : ",", MG_ESC("provider_id"), MG_ESC(provider_id),
        MG_ESC("provider_name"), MG_ESC(has_text(provider_name) ? provider_name : provider_id),
        MG_ESC("provider_type"), MG_ESC(provider_type ? provider_type : ""),
        MG_ESC("total_keys"), total_keys, MG_ESC("active_keys"), active_keys,
        MG_ESC("cooldown_count"), cooldown_count, MG_ESC("pool_enabled"),
        pool_enabled);
    first = false;
    mg_free(provider_id);
    mg_free(provider_name);
    mg_free(provider_type);
  }
}

static void append_aether_proxy_nodes(struct mg_iobuf *out, const char *body) {
  struct mg_str json = mg_str(body ? body : "");
  bool first = true;

  for (size_t i = 0; i < 2000; i++) {
    char *id = json_item_str(json, "$.items", i, "id");
    char *name = NULL;
    char *ip = NULL;
    char *region = NULL;
    char *status = NULL;
    long port;
    bool is_manual, tunnel_mode, tunnel_connected;

    if (id == NULL) id = json_item_str(json, "$.items", i, "node_id");
    if (id == NULL) break;
    if (!has_text(id)) {
      mg_free(id);
      continue;
    }
    name = json_item_str(json, "$.items", i, "name");
    ip = json_item_str(json, "$.items", i, "ip");
    region = json_item_str(json, "$.items", i, "region");
    status = json_item_str(json, "$.items", i, "status");
    port = json_item_long(json, "$.items", i, "port", 0);
    is_manual = json_item_bool(json, "$.items", i, "is_manual", false);
    tunnel_mode = json_item_bool(json, "$.items", i, "tunnel_mode", false);
    tunnel_connected = json_item_bool(json, "$.items", i, "tunnel_connected", false);

    mg_xprintf(
        mg_pfn_iobuf, out,
        "%s{%m:%m,%m:%m,%m:%m,%m:%ld,%m:%m,%m:%m,%m:%d,%m:%d,%m:%d}",
        first ? "" : ",", MG_ESC("id"), MG_ESC(id), MG_ESC("name"),
        MG_ESC(has_text(name) ? name : id), MG_ESC("ip"), MG_ESC(ip ? ip : ""),
        MG_ESC("port"), port, MG_ESC("region"), MG_ESC(region ? region : ""),
        MG_ESC("status"), MG_ESC(status ? status : ""), MG_ESC("is_manual"),
        is_manual, MG_ESC("tunnel_mode"), tunnel_mode,
        MG_ESC("tunnel_connected"), tunnel_connected);
    first = false;
    mg_free(id);
    mg_free(name);
    mg_free(ip);
    mg_free(region);
    mg_free(status);
  }
}

static long json_long_default(struct mg_str body, const char *path,
                              long fallback) {
  int len = 0;
  if (mg_json_get(body, path, &len) < 0) return fallback;
  return mg_json_get_long(body, path, fallback);
}

static char *json_result_string(struct mg_str body, size_t index,
                                const char *field) {
  char path[96];
  mg_snprintf(path, sizeof(path), "$.results[%lu].%s", (unsigned long) index,
              field);
  return mg_json_get_str(body, path);
}

static void update_upload_stats(sqlite3 *db, long attempted, long success,
                                long failed, long skipped,
                                const char *message) {
  sqlite3_stmt *stmt = NULL;
  const char *sql =
      "UPDATE aether_upload_stats SET "
      "total_attempted=total_attempted+?,"
      "success_count=success_count+?,"
      "failed_count=failed_count+?,"
      "skipped_count=skipped_count+?,"
      "last_success_at=CASE WHEN ?>0 THEN unixepoch() ELSE last_success_at END,"
      "last_failed_at=CASE WHEN ?>0 THEN unixepoch() ELSE last_failed_at END,"
      "last_message=?,updated_at=unixepoch() WHERE id=1";
  if (db == NULL) return;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
  sqlite3_bind_int64(stmt, 1, attempted);
  sqlite3_bind_int64(stmt, 2, success);
  sqlite3_bind_int64(stmt, 3, failed);
  sqlite3_bind_int64(stmt, 4, skipped);
  sqlite3_bind_int64(stmt, 5, success);
  sqlite3_bind_int64(stmt, 6, failed);
  sqlite3_bind_text(stmt, 7, message ? message : "", -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void append_upload_result_from_response(
    struct mg_iobuf *details, bool *first_detail,
    const struct upload_account *accounts, size_t count,
    const struct http_client_response *res, long *success_count,
    long *failed_count, long **success_ids, size_t *success_id_count) {
  struct mg_str body = mg_str(res->body ? res->body : "");
  long top_failed = json_long_default(body, "$.failed", -1);
  bool has_results = false;

  *success_ids = NULL;
  *success_id_count = 0;
  for (size_t i = 0; i < count; i++) {
    char *status = json_result_string(body, i, "status");
    char *message = json_result_string(body, i, "message");
    char *error = json_result_string(body, i, "error");
    bool success = false;
    const char *success_message = "上传成功";
    const char *failure_message = "Aether 返回结果不完整，无法确认该账号导入状态";

    if (status != NULL || message != NULL || error != NULL) has_results = true;
    if (status == NULL && message == NULL && error == NULL) {
      if (!has_results && top_failed == 0) {
        success = true;
      }
    } else if (text_equals_ci(status, "success") ||
               text_equals_ci(status, "ok") ||
               text_equals_ci(status, "uploaded")) {
      success = true;
      success_message = has_text(message) ? message : "上传成功";
    } else if (is_already_exists_error(error) || is_already_exists_error(message)) {
      success = true;
      success_message = "账号已存在于 Aether 号池，按上传成功处理";
    } else {
      failure_message = has_text(error) ? error :
                        (has_text(message) ? message : failure_message);
    }

    if (success) {
      long *next = (long *) realloc(*success_ids,
                                    (*success_id_count + 1) * sizeof(long));
      if (next != NULL) {
        *success_ids = next;
        (*success_ids)[(*success_id_count)++] = accounts[i].id;
      }
      (*success_count)++;
      append_detail(details, first_detail, accounts[i].id, accounts[i].email,
                    true, success_message, NULL);
    } else {
      (*failed_count)++;
      append_detail(details, first_detail, accounts[i].id, accounts[i].email,
                    false, NULL, failure_message);
    }

    mg_free(status);
    mg_free(message);
    mg_free(error);
  }
}

static int normalize_pool_type(const char *pool_type, bool *include_web) {
  if (include_web != NULL) *include_web = false;
  if (!has_text(pool_type) || text_equals_ci(pool_type, "oauth") ||
      text_equals_ci(pool_type, "normal") || text_equals_ci(pool_type, "default")) {
    return 0;
  }
  if (text_equals_ci(pool_type, "chatgpt_web") ||
      text_equals_ci(pool_type, "web") || text_equals_ci(pool_type, "chatgpt")) {
    if (include_web != NULL) *include_web = true;
    return 0;
  }
  return -1;
}

char *aether_upload_accounts_json(sqlite3 *db, const long *ids, size_t count,
                                  const char *pool_type) {
  struct aether_service svc;
  struct upload_account *valid = NULL;
  size_t valid_len = 0, valid_cap = 0;
  struct mg_iobuf credentials = {0, 0, 0, 2048};
  struct mg_iobuf details = {0, 0, 0, 2048};
  struct mg_iobuf out = {0, 0, 0, 2048};
  bool first_credential = true, first_detail = true;
  bool include_web = false;
  const char *provider_id;
  char first_error[512] = "";
  long success_count = 0, failed_count = 0, skipped_count = 0;
  long *success_ids = NULL;
  size_t success_id_count = 0;

  memset(&svc, 0, sizeof(svc));
  mg_xprintf(mg_pfn_iobuf, &details, "[");
  if (count == 0 || ids == NULL) {
    mg_xprintf(mg_pfn_iobuf, &out, "{%m:%d,%m:%m}", MG_ESC("ok"), 0,
               MG_ESC("error"), MG_ESC("请选择要上传的账号"));
    goto done;
  }
  if (normalize_pool_type(pool_type, &include_web) != 0) {
    mg_xprintf(mg_pfn_iobuf, &out, "{%m:%d,%m:%m}", MG_ESC("ok"), 0,
               MG_ESC("error"), MG_ESC("不支持的 Aether 上传号池类型"));
    goto done;
  }
  if (load_default_service(db, include_web, &svc) != 0) {
    mg_xprintf(mg_pfn_iobuf, &out, "{%m:%d,%m:%m}", MG_ESC("ok"), 0,
               MG_ESC("error"),
               MG_ESC(include_web ? "未找到配置 ChatGPT Web 号池的 Aether 上传服务"
                                  : "未找到已启用的 Aether 上传服务，请先配置"));
    goto done;
  }

  provider_id = include_web ? svc.chatgpt_web_provider_id : svc.provider_id;
  if (!has_text(svc.api_url) || !has_text(svc.management_token) ||
      !has_text(provider_id)) {
    mg_xprintf(mg_pfn_iobuf, &out, "{%m:%d,%m:%m}", MG_ESC("ok"), 0,
               MG_ESC("error"), MG_ESC("Aether 上传服务配置不完整"));
    goto done;
  }

  mg_xprintf(mg_pfn_iobuf, &credentials, "[");
  for (size_t i = 0; i < count; i++) {
    struct upload_account account;
    bool has_credentials;
    if (load_upload_account(db, ids[i], &account) != 0) {
      failed_count++;
      if (!has_text(first_error)) {
        mg_snprintf(first_error, sizeof(first_error), "%s", "账号不存在");
      }
      append_detail(&details, &first_detail, ids[i], "", false, NULL,
                    "账号不存在");
      continue;
    }
    has_credentials = include_web ? has_text(account.access_token)
                                  : (has_text(account.refresh_token) ||
                                     has_text(account.access_token));
    if (!has_credentials) {
      skipped_count++;
      append_detail(
          &details, &first_detail, account.id, account.email, false, NULL,
          include_web ? "缺少 ChatGPT Web 上传所需 Access Token"
                      : "缺少 refresh_token/access_token，无法上传到 Aether");
      free_upload_account(&account);
      continue;
    }
    if (valid_len == valid_cap) {
      struct upload_account *next;
      valid_cap = valid_cap == 0 ? 8 : valid_cap * 2;
      next = (struct upload_account *) realloc(valid,
                                               valid_cap * sizeof(*valid));
      if (next == NULL) {
        failed_count++;
        append_detail(&details, &first_detail, account.id, account.email, false,
                      NULL, "内存不足，无法构建上传任务");
        free_upload_account(&account);
        continue;
      }
      valid = next;
    }
    valid[valid_len++] = account;
    mg_xprintf(mg_pfn_iobuf, &credentials, "%s", first_credential ? "" : ",");
    append_credential_entry(&credentials, &account, include_web);
    first_credential = false;
  }
  mg_xprintf(mg_pfn_iobuf, &credentials, "]");
  mg_iobuf_add(&credentials, credentials.len, "", 1);

  if (valid_len > 0) {
    struct http_client_response res;
    memset(&res, 0, sizeof(res));
    int rc = post_batch_import(&svc, provider_id, (char *) credentials.buf, &res);
    if (rc != 0 || res.status_code < 200 || res.status_code >= 300) {
      char *message = extract_error_message(&res, "Aether 上传失败");
      for (size_t i = 0; i < valid_len; i++) {
        failed_count++;
        append_detail(&details, &first_detail, valid[i].id, valid[i].email,
                      false, NULL, message);
      }
      mg_snprintf(first_error, sizeof(first_error), "%s",
                  message ? message : "Aether 上传失败");
      mg_free(message);
    } else {
      append_upload_result_from_response(&details, &first_detail, valid,
                                         valid_len, &res, &success_count,
                                         &failed_count, &success_ids,
                                         &success_id_count);
    }
    http_client_response_free(&res);
  }

  if (success_id_count > 0) {
    account_set_upload_state(db, success_ids, success_id_count, 1);
  }
  update_upload_stats(db, (long) count, success_count, failed_count,
                      skipped_count,
                      success_count > 0 ? "上传完成" :
                      (has_text(first_error) ? first_error : "上传未成功"));

  mg_xprintf(mg_pfn_iobuf, &details, "]");
  mg_xprintf(mg_pfn_iobuf, &out,
             "{%m:%d,%m:%ld,%m:%ld,%m:%ld,%m:%ld,%m:%m,%m:%s}",
             MG_ESC("ok"), 1, MG_ESC("attempted"), (long) count,
             MG_ESC("success_count"), success_count, MG_ESC("failed_count"),
             failed_count, MG_ESC("skipped_count"), skipped_count,
             MG_ESC("pool_type"), MG_ESC(include_web ? "chatgpt_web" : "oauth"),
             MG_ESC("details"), (char *) details.buf);

done:
  mg_iobuf_add(&out, out.len, "", 1);
  for (size_t i = 0; i < valid_len; i++) free_upload_account(&valid[i]);
  free(valid);
  free(success_ids);
  free(credentials.buf);
  free(details.buf);
  free_service(&svc);
  return (char *) out.buf;
}

char *aether_config_json(sqlite3 *db) {
  sqlite3_stmt *stmt = NULL;
  struct mg_iobuf out = {0, 0, 0, 2048};
  sqlite3_int64 uploaded = 0, not_uploaded = 0;
  sqlite3_int64 attempted = 0, success = 0, failed = 0, skipped = 0;
  sqlite3_int64 last_success_at = 0, last_failed_at = 0, updated_at = 0;
  char last_message[AETHER_TEXT_LEN] = "";
  bool first = true;

  if (db != NULL &&
      sqlite3_prepare_v2(db,
                         "SELECT uploaded_count,not_uploaded_count FROM "
                         "account_stats WHERE id=1",
                         -1, &stmt, NULL) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW) {
    uploaded = sqlite3_column_int64(stmt, 0);
    not_uploaded = sqlite3_column_int64(stmt, 1);
  }
  sqlite3_finalize(stmt);
  stmt = NULL;

  if (db != NULL &&
      sqlite3_prepare_v2(db,
                         "SELECT total_attempted,success_count,failed_count,"
                         "skipped_count,COALESCE(last_success_at,0),"
                         "COALESCE(last_failed_at,0),COALESCE(last_message,''),"
                         "updated_at FROM aether_upload_stats WHERE id=1",
                         -1, &stmt, NULL) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW) {
    attempted = sqlite3_column_int64(stmt, 0);
    success = sqlite3_column_int64(stmt, 1);
    failed = sqlite3_column_int64(stmt, 2);
    skipped = sqlite3_column_int64(stmt, 3);
    last_success_at = sqlite3_column_int64(stmt, 4);
    last_failed_at = sqlite3_column_int64(stmt, 5);
    mg_snprintf(last_message, sizeof(last_message), "%s", column_text(stmt, 6));
    updated_at = sqlite3_column_int64(stmt, 7);
  }
  sqlite3_finalize(stmt);
  stmt = NULL;

  mg_xprintf(mg_pfn_iobuf, &out,
             "{%m:{%m:%lld,%m:%lld,%m:%lld,%m:%lld,%m:%lld,%m:%lld,%m:%lld,"
             "%m:%lld,%m:%m,%m:%lld},%m:[",
             MG_ESC("stats"), MG_ESC("account_uploaded"), uploaded,
             MG_ESC("account_not_uploaded"), not_uploaded,
             MG_ESC("total_attempted"), attempted, MG_ESC("success_count"),
             success, MG_ESC("failed_count"), failed, MG_ESC("skipped_count"),
             skipped, MG_ESC("last_success_at"), last_success_at,
             MG_ESC("last_failed_at"), last_failed_at, MG_ESC("last_message"),
             MG_ESC(last_message), MG_ESC("updated_at"), updated_at,
             MG_ESC("services"));

  if (db != NULL &&
      sqlite3_prepare_v2(
          db,
          "SELECT id,name,api_url,management_token,provider_id,provider_name,"
          "COALESCE(chatgpt_web_provider_id,''),"
          "COALESCE(chatgpt_web_provider_name,''),COALESCE(proxy_node_id,''),"
          "COALESCE(proxy_node_name,''),enabled,priority,created_at,updated_at "
          "FROM aether_services ORDER BY priority ASC,id ASC",
          -1, &stmt, NULL) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *token = column_text(stmt, 3);
      mg_xprintf(
          mg_pfn_iobuf, &out,
          "%s{%m:%lld,%m:%m,%m:%m,%m:%m,%m:%m,%m:%m,%m:%m,%m:%m,%m:%m,"
          "%m:%d,%m:%d,%m:%d,%m:%lld,%m:%lld}",
          first ? "" : ",", MG_ESC("id"), sqlite3_column_int64(stmt, 0),
          MG_ESC("name"), MG_ESC(column_text(stmt, 1)), MG_ESC("api_url"),
          MG_ESC(column_text(stmt, 2)), MG_ESC("provider_id"),
          MG_ESC(column_text(stmt, 4)), MG_ESC("provider_name"),
          MG_ESC(column_text(stmt, 5)), MG_ESC("chatgpt_web_provider_id"),
          MG_ESC(column_text(stmt, 6)), MG_ESC("chatgpt_web_provider_name"),
          MG_ESC(column_text(stmt, 7)), MG_ESC("proxy_node_id"),
          MG_ESC(column_text(stmt, 8)), MG_ESC("proxy_node_name"),
          MG_ESC(column_text(stmt, 9)), MG_ESC("has_management_token"),
          has_text(token), MG_ESC("enabled"), sqlite3_column_int(stmt, 10),
          MG_ESC("priority"), sqlite3_column_int(stmt, 11),
          MG_ESC("created_at"), sqlite3_column_int64(stmt, 12),
          MG_ESC("updated_at"), sqlite3_column_int64(stmt, 13));
      first = false;
    }
  }
  sqlite3_finalize(stmt);
  mg_xprintf(mg_pfn_iobuf, &out, "]}");
  mg_iobuf_add(&out, out.len, "", 1);
  return (char *) out.buf;
}

static char *ok_message_json(const char *message, long id) {
  struct mg_iobuf out = {0, 0, 0, 256};
  mg_xprintf(mg_pfn_iobuf, &out, "{%m:%d,%m:%ld,%m:%m}", MG_ESC("ok"), 1,
             MG_ESC("id"), id, MG_ESC("message"), MG_ESC(message));
  mg_iobuf_add(&out, out.len, "", 1);
  return (char *) out.buf;
}

static char *error_json(const char *message) {
  struct mg_iobuf out = {0, 0, 0, 256};
  mg_xprintf(mg_pfn_iobuf, &out, "{%m:%d,%m:%m}", MG_ESC("ok"), 0,
             MG_ESC("error"), MG_ESC(message ? message : "操作失败"));
  mg_iobuf_add(&out, out.len, "", 1);
  return (char *) out.buf;
}

char *aether_service_save_json(sqlite3 *db, struct mg_str body) {
  sqlite3_stmt *stmt = NULL;
  long id = mg_json_get_long(body, "$.id", 0);
  bool is_update = id > 0;
  char *name = json_get_trim(body, "$.name");
  char *api_url = json_get_trim(body, "$.api_url");
  char *management_token = json_get_trim(body, "$.management_token");
  char *provider_id = json_get_trim(body, "$.provider_id");
  char *provider_name = json_get_trim(body, "$.provider_name");
  char *web_provider_id = json_get_trim(body, "$.chatgpt_web_provider_id");
  char *web_provider_name = json_get_trim(body, "$.chatgpt_web_provider_name");
  char *proxy_node_id = json_get_trim(body, "$.proxy_node_id");
  char *proxy_node_name = json_get_trim(body, "$.proxy_node_name");
  bool enabled = true;
  long priority = mg_json_get_long(body, "$.priority", 0);
  int ok = 0;

  mg_json_get_bool(body, "$.enabled", &enabled);
  if (!has_text(name) || !has_text(api_url) || !has_text(provider_id)) {
    free(name); free(api_url); free(management_token); free(provider_id);
    free(provider_name); free(web_provider_id); free(web_provider_name);
    free(proxy_node_id); free(proxy_node_name);
    return error_json("请填写名称、Aether 地址和 OAuth Provider ID");
  }
  if (id <= 0 && !has_text(management_token)) {
    free(name); free(api_url); free(management_token); free(provider_id);
    free(provider_name); free(web_provider_id); free(web_provider_name);
    free(proxy_node_id); free(proxy_node_name);
    return error_json("请填写 Aether 管理员 Token");
  }

  if (id > 0) {
    struct aether_service existing;
    const char *sql_with_token =
        "UPDATE aether_services SET name=?,api_url=?,management_token=?,"
        "provider_id=?,provider_name=?,chatgpt_web_provider_id=?,"
        "chatgpt_web_provider_name=?,proxy_node_id=?,proxy_node_name=?,"
        "enabled=?,priority=?,updated_at=unixepoch() WHERE id=?";
    const char *sql_without_token =
        "UPDATE aether_services SET name=?,api_url=?,provider_id=?,"
        "provider_name=?,chatgpt_web_provider_id=?,"
        "chatgpt_web_provider_name=?,proxy_node_id=?,proxy_node_name=?,"
        "enabled=?,priority=?,updated_at=unixepoch() WHERE id=?";
    const bool update_token = has_text(management_token);
    const char *sql = update_token ? sql_with_token : sql_without_token;
    int bind = 1;
    if (load_service_by_id(db, id, &existing) != 0) {
      free(name); free(api_url); free(management_token); free(provider_id);
      free(provider_name); free(web_provider_id); free(web_provider_name);
      free(proxy_node_id); free(proxy_node_name);
      return error_json("Aether 上传服务不存在");
    }
    free_service(&existing);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, bind++, name, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, bind++, api_url, -1, SQLITE_TRANSIENT);
      if (update_token) {
        sqlite3_bind_text(stmt, bind++, management_token, -1, SQLITE_TRANSIENT);
      }
      sqlite3_bind_text(stmt, bind++, provider_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, bind++, provider_name ? provider_name : "", -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, bind++, web_provider_id ? web_provider_id : "",
                        -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, bind++, web_provider_name ? web_provider_name : "",
                        -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, bind++, proxy_node_id ? proxy_node_id : "", -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, bind++, proxy_node_name ? proxy_node_name : "",
                        -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, bind++, enabled ? 1 : 0);
      sqlite3_bind_int64(stmt, bind++, priority);
      sqlite3_bind_int64(stmt, bind++, id);
      ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
  } else {
    const char *sql =
        "INSERT INTO aether_services(name,api_url,management_token,provider_id,"
        "provider_name,chatgpt_web_provider_id,chatgpt_web_provider_name,"
        "proxy_node_id,proxy_node_name,enabled,priority,created_at,updated_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,unixepoch(),unixepoch())";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, api_url, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, management_token, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, provider_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 5, provider_name ? provider_name : "", -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 6, web_provider_id ? web_provider_id : "", -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 7, web_provider_name ? web_provider_name : "", -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 8, proxy_node_id ? proxy_node_id : "", -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 9, proxy_node_name ? proxy_node_name : "", -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 10, enabled ? 1 : 0);
      sqlite3_bind_int64(stmt, 11, priority);
      ok = sqlite3_step(stmt) == SQLITE_DONE;
      if (ok) id = (long) sqlite3_last_insert_rowid(db);
    }
  }

  sqlite3_finalize(stmt);
  free(name); free(api_url); free(management_token); free(provider_id);
  free(provider_name); free(web_provider_id); free(web_provider_name);
  free(proxy_node_id); free(proxy_node_name);
  if (!ok) return error_json("Aether 上传服务保存失败");
  return ok_message_json(is_update ? "Aether 上传服务已保存" : "Aether 上传服务已创建",
                         id);
}

char *aether_service_delete_json(sqlite3 *db, struct mg_str body) {
  sqlite3_stmt *stmt = NULL;
  long id = mg_json_get_long(body, "$.id", 0);
  int ok = 0;
  if (db == NULL || id <= 0) return error_json("请选择要删除的 Aether 服务");
  if (sqlite3_prepare_v2(db, "DELETE FROM aether_services WHERE id=?", -1,
                         &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, id);
    ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0;
  }
  sqlite3_finalize(stmt);
  return ok ? ok_message_json("Aether 上传服务已删除", id)
            : error_json("Aether 上传服务不存在或删除失败");
}

char *aether_options_json(sqlite3 *db, struct mg_str body) {
  struct aether_service svc;
  struct http_client_response pools_res;
  struct http_client_response nodes_res;
  struct mg_iobuf out = {0, 0, 0, 4096};
  char *api_url = json_get_trim(body, "$.api_url");
  char *management_token = json_get_trim(body, "$.management_token");
  long id = mg_json_get_long(body, "$.id", 0);
  char *pools_error = NULL;
  char *nodes_error = NULL;

  memset(&svc, 0, sizeof(svc));
  memset(&pools_res, 0, sizeof(pools_res));
  memset(&nodes_res, 0, sizeof(nodes_res));

  if (id > 0 && load_service_by_id(db, id, &svc) == 0) {
    if (!has_text(api_url)) {
      free(api_url);
      api_url = str_dup(svc.api_url);
    }
    if (!has_text(management_token)) {
      free(management_token);
      management_token = str_dup(svc.management_token);
    }
  }

  if (!has_text(api_url) || !has_text(management_token)) {
    free_service(&svc);
    free(api_url);
    free(management_token);
    return error_json("请先填写 Aether 地址和管理员 Token");
  }

  if (aether_get_json(api_url, management_token, "/api/admin/pool/overview",
                      &pools_res) != 0 ||
      pools_res.status_code != 200) {
    pools_error = extract_error_message(&pools_res, "读取 Aether Provider 失败");
    mg_xprintf(mg_pfn_iobuf, &out, "{%m:%d,%m:%m}", MG_ESC("ok"), 0,
               MG_ESC("error"), MG_ESC(pools_error));
    goto done;
  }

  if (aether_get_json(api_url, management_token,
                      "/api/admin/proxy-nodes?status=online&limit=1000",
                      &nodes_res) != 0 ||
      nodes_res.status_code != 200) {
    nodes_error = extract_error_message(&nodes_res, "读取 Aether 代理节点失败");
  }

  mg_xprintf(mg_pfn_iobuf, &out, "{%m:%d,%m:%m,%m:[", MG_ESC("ok"), 1,
             MG_ESC("message"), MG_ESC("Aether 选项已读取"),
             MG_ESC("pools"));
  append_aether_pools(&out, pools_res.body);
  mg_xprintf(mg_pfn_iobuf, &out, "],%m:[", MG_ESC("proxy_nodes"));
  if (nodes_error == NULL) append_aether_proxy_nodes(&out, nodes_res.body);
  mg_xprintf(mg_pfn_iobuf, &out, "],%m:%m}", MG_ESC("proxy_nodes_error"),
             MG_ESC(nodes_error ? nodes_error : ""));

done:
  mg_iobuf_add(&out, out.len, "", 1);
  mg_free(pools_error);
  mg_free(nodes_error);
  http_client_response_free(&pools_res);
  http_client_response_free(&nodes_res);
  free_service(&svc);
  free(api_url);
  free(management_token);
  return (char *) out.buf;
}

char *aether_service_test_json(sqlite3 *db, struct mg_str body) {
  struct aether_service svc;
  struct http_client_header headers[1];
  struct http_client_request req;
  struct http_client_response res;
  struct mg_iobuf out = {0, 0, 0, 512};
  char auth_header[1024], url[AETHER_URL_LEN], path[128];
  char *api_url = json_get_trim(body, "$.api_url");
  char *management_token = json_get_trim(body, "$.management_token");
  char *provider_id = json_get_trim(body, "$.provider_id");
  long id = mg_json_get_long(body, "$.id", 0);
  bool found = false;
  char provider_name[AETHER_TEXT_LEN] = "";

  memset(&svc, 0, sizeof(svc));
  if (id > 0 && load_service_by_id(db, id, &svc) == 0) {
    if (!has_text(api_url)) {
      free(api_url);
      api_url = str_dup(svc.api_url);
    }
    if (!has_text(management_token)) {
      free(management_token);
      management_token = str_dup(svc.management_token);
    }
    if (!has_text(provider_id)) {
      free(provider_id);
      provider_id = str_dup(svc.provider_id);
    }
  }
  if (!has_text(api_url) || !has_text(management_token) ||
      !has_text(provider_id)) {
    free_service(&svc);
    free(api_url); free(management_token); free(provider_id);
    return error_json("请填写 Aether 地址、管理员 Token 和 Provider ID 后再测试");
  }
  if (build_aether_url(api_url, "/api/admin/pool/overview", url,
                       sizeof(url)) != 0) {
    free_service(&svc);
    free(api_url); free(management_token); free(provider_id);
    return error_json("Aether 地址无效");
  }

  mg_snprintf(auth_header, sizeof(auth_header), "Bearer %s", management_token);
  headers[0].name = "Authorization";
  headers[0].value = auth_header;
  memset(&req, 0, sizeof(req));
  req.method = "GET";
  req.url = url;
  req.timeout_ms = 15000L;
  req.headers = headers;
  req.num_headers = 1;

  if (http_client_perform(&req, &res) != 0 || res.status_code != 200) {
    char *message = extract_error_message(&res, "Aether 连接测试失败");
    mg_xprintf(mg_pfn_iobuf, &out,
               "{%m:%d,%m:%d,%m:%ld,%m:%m}", MG_ESC("ok"), 1,
               MG_ESC("success"), 0, MG_ESC("http_status"), res.status_code,
               MG_ESC("message"), MG_ESC(message));
    mg_free(message);
    http_client_response_free(&res);
    goto done;
  }

  for (size_t i = 0; i < 1000; i++) {
    char *pid;
    char *pname;
    mg_snprintf(path, sizeof(path), "$.items[%lu].provider_id",
                (unsigned long) i);
    pid = mg_json_get_str(mg_str(res.body ? res.body : ""), path);
    if (pid == NULL) break;
    if (strcmp(pid, provider_id) == 0) {
      mg_snprintf(path, sizeof(path), "$.items[%lu].provider_name",
                  (unsigned long) i);
      pname = mg_json_get_str(mg_str(res.body ? res.body : ""), path);
      mg_snprintf(provider_name, sizeof(provider_name), "%s",
                  pname ? pname : provider_id);
      mg_free(pname);
      found = true;
      mg_free(pid);
      break;
    }
    mg_free(pid);
  }
  http_client_response_free(&res);
  mg_xprintf(mg_pfn_iobuf, &out,
             "{%m:%d,%m:%d,%m:%ld,%m:%m}", MG_ESC("ok"), 1,
             MG_ESC("success"), found, MG_ESC("http_status"), 200L,
             MG_ESC("message"),
             MG_ESC(found ? "Aether 连接成功，已找到目标 Provider"
                          : "连接成功，但未找到指定 Provider ID"));
  if (found) {
    out.len--;
    mg_xprintf(mg_pfn_iobuf, &out, ",%m:%m}", MG_ESC("provider_name"),
               MG_ESC(provider_name));
  }

done:
  mg_iobuf_add(&out, out.len, "", 1);
  free_service(&svc);
  free(api_url); free(management_token); free(provider_id);
  return (char *) out.buf;
}

char *aether_upload_credential_json(sqlite3 *db,
                                    const struct aether_push_credential *cred,
                                    const char *pool_type) {
  struct aether_service svc;
  struct upload_account account;
  struct mg_iobuf credentials = {0, 0, 0, 1024};
  struct mg_iobuf out = {0, 0, 0, 512};
  struct http_client_response res;
  bool include_web = false;
  const char *provider_id;
  int rc;
  bool success = false;
  char *message = NULL;

  memset(&svc, 0, sizeof(svc));
  memset(&account, 0, sizeof(account));
  memset(&res, 0, sizeof(res));

  if (cred == NULL || !has_text(cred->access_token)) {
    return error_json("缺少 Access Token，无法推送到 Aether");
  }
  if (normalize_pool_type(pool_type, &include_web) != 0) {
    return error_json("不支持的 Aether 上传号池类型");
  }
  if (load_default_service(db, include_web, &svc) != 0) {
    return error_json(include_web
                          ? "未找到配置 ChatGPT Web 号池的 Aether 上传服务"
                          : "未找到已启用的 Aether 上传服务，请先配置");
  }
  provider_id = include_web ? svc.chatgpt_web_provider_id : svc.provider_id;
  if (!has_text(svc.api_url) || !has_text(svc.management_token) ||
      !has_text(provider_id)) {
    free_service(&svc);
    return error_json("Aether 上传服务配置不完整");
  }

  /* upload_account 内部字段为 char*，此处指向传入常量的可写副本。 */
  account.email = str_dup(cred->email ? cred->email : "");
  account.access_token = str_dup(cred->access_token);
  account.refresh_token = str_dup(cred->refresh_token ? cred->refresh_token : "");
  account.external_account_id =
      str_dup(cred->external_account_id ? cred->external_account_id : "");
  account.workspace_id = str_dup(cred->workspace_id ? cred->workspace_id : "");

  mg_xprintf(mg_pfn_iobuf, &credentials, "[");
  append_credential_entry(&credentials, &account, include_web);
  mg_xprintf(mg_pfn_iobuf, &credentials, "]");
  mg_iobuf_add(&credentials, credentials.len, "", 1);

  rc = post_batch_import(&svc, provider_id, (char *) credentials.buf, &res);
  if (rc != 0 || res.status_code < 200 || res.status_code >= 300) {
    message = extract_error_message(&res, "Aether 上传失败");
  } else {
    struct mg_str rbody = mg_str(res.body ? res.body : "");
    char *status = json_result_string(rbody, 0, "status");
    char *rmessage = json_result_string(rbody, 0, "message");
    char *rerror = json_result_string(rbody, 0, "error");
    long top_failed = json_long_default(rbody, "$.failed", -1);
    if (text_equals_ci(status, "success") || text_equals_ci(status, "ok") ||
        text_equals_ci(status, "uploaded") ||
        is_already_exists_error(rerror) || is_already_exists_error(rmessage) ||
        (status == NULL && rmessage == NULL && rerror == NULL &&
         top_failed == 0)) {
      success = true;
    } else {
      message = str_dup(has_text(rerror) ? rerror
                        : has_text(rmessage) ? rmessage
                                             : "Aether 返回结果不完整");
    }
    mg_free(status);
    mg_free(rmessage);
    mg_free(rerror);
  }

  update_upload_stats(db, 1, success ? 1 : 0, success ? 0 : 1, 0,
                      success ? "上传完成" : (message ? message : "上传未成功"));

  mg_xprintf(mg_pfn_iobuf, &out, "{%m:%d,%m:%d,%m:%m,%m:%m}", MG_ESC("ok"),
             success ? 1 : 0, MG_ESC("success"), success ? 1 : 0,
             MG_ESC("workspace_id"),
             MG_ESC(cred->workspace_id ? cred->workspace_id : ""),
             MG_ESC(success ? "message" : "error"),
             MG_ESC(success ? "推送成功" : (message ? message : "推送失败")));
  mg_iobuf_add(&out, out.len, "", 1);

  free(message);
  free_upload_account(&account);
  free(credentials.buf);
  http_client_response_free(&res);
  free_service(&svc);
  return (char *) out.buf;
}
