#include "workspace/workspace_ops.h"

#include "http_client/http_client.h"
#include "upload/aether_upload.h"
#include "mongoose.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define CHATGPT_BASE_URL "https://chatgpt.com"

struct account_creds {
  long id;
  char email[320];
  char access_token[4096];
  char refresh_token[4096];
  char external_account_id[160];
  char workspace_id[160];
  char chatgpt_user_id[160];
};

/* 解码 JWT payload（第二段 base64url），返回 malloc 的 JSON 字符串。 */
static char *decode_jwt_payload(const char *token) {
  const char *first, *second, *end;
  size_t enc_len, i;
  char *enc, *out;
  size_t out_cap;

  if (token == NULL) return NULL;
  first = strchr(token, '.');
  if (first == NULL) return NULL;
  second = strchr(first + 1, '.');
  end = second != NULL ? second : first + 1 + strlen(first + 1);
  enc_len = (size_t) (end - (first + 1));
  if (enc_len == 0) return NULL;

  enc = (char *) malloc(enc_len + 4);
  if (enc == NULL) return NULL;
  memcpy(enc, first + 1, enc_len);
  enc[enc_len] = '\0';
  /* base64url -> base64 */
  for (i = 0; i < enc_len; i++) {
    if (enc[i] == '-') enc[i] = '+';
    else if (enc[i] == '_') enc[i] = '/';
  }
  while (enc_len % 4 != 0) {
    enc[enc_len++] = '=';
    enc[enc_len] = '\0';
  }

  out_cap = enc_len; /* decoded 一定小于编码长度 */
  out = (char *) malloc(out_cap + 1);
  if (out == NULL) {
    free(enc);
    return NULL;
  }
  size_t n = mg_base64_decode(enc, enc_len, out, out_cap + 1);
  free(enc);
  if (n == 0) {
    free(out);
    return NULL;
  }
  out[n] = '\0';
  return out;
}

/* 从 JSON 文本中按键名抽取字符串值。mongoose 的 JSON path 用 `.` 分隔，
 * 而 access_token 的 auth claim 键名 "https://api.openai.com/auth" 含点和斜杠，
 * 无法用 path 导航，这里直接按 "key":"value" 手动匹配。 */
static bool copy_json_string_key(const char *json, const char *key, char *out,
                                 size_t out_len) {
  const char *p, *value, *end;
  char needle[128];

  if (json == NULL || key == NULL || out == NULL || out_len == 0) return false;
  mg_snprintf(needle, sizeof(needle), "\"%s\"", key);
  p = strstr(json, needle);
  if (p == NULL) return false;
  p += strlen(needle);
  while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  if (*p != ':') return false;
  p++;
  while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  if (*p != '"') return false;
  value = ++p;
  while (*p && (*p != '"' || p[-1] == '\\')) p++;
  end = p;
  if (end <= value) return false;
  mg_snprintf(out, out_len, "%.*s", (int) (end - value), value);
  return true;
}

static void extract_user_id(struct account_creds *creds) {
  char *payload;
  if (creds->chatgpt_user_id[0] != '\0') return;
  payload = decode_jwt_payload(creds->access_token);
  if (payload == NULL) return;
  if (!copy_json_string_key(payload, "chatgpt_user_id", creds->chatgpt_user_id,
                            sizeof(creds->chatgpt_user_id))) {
    copy_json_string_key(payload, "user_id", creds->chatgpt_user_id,
                         sizeof(creds->chatgpt_user_id));
  }
  free(payload);
}

static int load_account_creds(sqlite3 *db, long id,
                              struct account_creds *creds) {
  sqlite3_stmt *stmt = NULL;
  const char *sql =
      "SELECT a.email,COALESCE(s.access_token,''),COALESCE(s.refresh_token,''),"
      "COALESCE(s.external_account_id,''),COALESCE(s.workspace_id,'') "
      "FROM accounts a LEFT JOIN account_secrets s ON s.account_id=a.id "
      "WHERE a.id=?";
  int rc = -1;
  if (creds == NULL) return -1;
  memset(creds, 0, sizeof(*creds));
  creds->id = id;
  if (db == NULL || id <= 0) return -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int64(stmt, 1, id);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *email = sqlite3_column_text(stmt, 0);
    const unsigned char *at = sqlite3_column_text(stmt, 1);
    const unsigned char *rt = sqlite3_column_text(stmt, 2);
    const unsigned char *ext = sqlite3_column_text(stmt, 3);
    const unsigned char *ws = sqlite3_column_text(stmt, 4);
    mg_snprintf(creds->email, sizeof(creds->email), "%s",
                email ? (const char *) email : "");
    mg_snprintf(creds->access_token, sizeof(creds->access_token), "%s",
                at ? (const char *) at : "");
    mg_snprintf(creds->refresh_token, sizeof(creds->refresh_token), "%s",
                rt ? (const char *) rt : "");
    mg_snprintf(creds->external_account_id, sizeof(creds->external_account_id),
                "%s", ext ? (const char *) ext : "");
    mg_snprintf(creds->workspace_id, sizeof(creds->workspace_id), "%s",
                ws ? (const char *) ws : "");
    rc = 0;
  }
  sqlite3_finalize(stmt);
  return rc;
}

static int workspace_request(const char *method, const char *url,
                             const char *bearer, const char *body,
                             long *status_out, char *error, size_t error_len) {
  char auth_header[4200];
  struct http_client_header headers[] = {
      {"Authorization", auth_header},
      {"Accept", "*/*"},
      {"Content-Type", "application/json"},
  };
  struct http_client_request req;
  struct http_client_response res;

  if (status_out != NULL) *status_out = 0;
  mg_snprintf(auth_header, sizeof(auth_header), "Bearer %s", bearer ? bearer : "");
  memset(&req, 0, sizeof(req));
  req.method = method;
  req.url = url;
  req.timeout_ms = 15000;
  req.headers = headers;
  req.num_headers = body != NULL ? 3 : 2;
  if (body != NULL) {
    req.body = body;
    req.body_len = strlen(body);
  }
  if (http_client_perform(&req, &res) != 0) {
    if (error != NULL) mg_snprintf(error, error_len, "%s", res.error);
    return -1;
  }
  if (status_out != NULL) *status_out = res.status_code;
  if (res.status_code < 200 || res.status_code >= 300) {
    if (error != NULL) {
      mg_snprintf(error, error_len, "HTTP %ld", res.status_code);
    }
    http_client_response_free(&res);
    return -1;
  }
  http_client_response_free(&res);
  return 0;
}

char *workspace_join_json(sqlite3 *db, const long *ids, size_t count) {
  struct mg_iobuf io = {0, 0, 0, 512};
  int success = 0, failed = 0;
  bool first = true;

  mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:[", MG_ESC("ok"), 1,
             MG_ESC("details"));
  for (size_t i = 0; i < count; i++) {
    struct account_creds creds;
    char url[512];
    char error[256] = "";
    long status = 0;
    if (!first) mg_xprintf(mg_pfn_iobuf, &io, ",");
    first = false;
    if (load_account_creds(db, ids[i], &creds) != 0 ||
        creds.access_token[0] == '\0' || creds.external_account_id[0] == '\0') {
      failed++;
      mg_xprintf(mg_pfn_iobuf, &io, "{%m:%ld,%m:%d,%m:%m}", MG_ESC("id"),
                 ids[i], MG_ESC("ok"), 0, MG_ESC("error"),
                 MG_ESC("账号缺少 Access Token 或 Account ID"));
      continue;
    }
    mg_snprintf(url, sizeof(url),
                CHATGPT_BASE_URL "/backend-api/accounts/%s/invites/request",
                creds.external_account_id);
    if (workspace_request("POST", url, creds.access_token, "{}", &status, error,
                          sizeof(error)) == 0) {
      success++;
      mg_xprintf(mg_pfn_iobuf, &io, "{%m:%ld,%m:%d,%m:%ld}", MG_ESC("id"),
                 ids[i], MG_ESC("ok"), 1, MG_ESC("http_status"), status);
    } else {
      failed++;
      mg_xprintf(mg_pfn_iobuf, &io, "{%m:%ld,%m:%d,%m:%m,%m:%ld}", MG_ESC("id"),
                 ids[i], MG_ESC("ok"), 0, MG_ESC("error"), MG_ESC(error),
                 MG_ESC("http_status"), status);
    }
  }
  mg_xprintf(mg_pfn_iobuf, &io, "],%m:%d,%m:%d}", MG_ESC("success_count"),
             success, MG_ESC("failed_count"), failed);
  mg_iobuf_add(&io, io.len, "", 1);
  return (char *) io.buf;
}

char *workspace_leave_json(sqlite3 *db, const long *ids, size_t count) {
  struct mg_iobuf io = {0, 0, 0, 512};
  int success = 0, failed = 0;
  bool first = true;

  mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:[", MG_ESC("ok"), 1,
             MG_ESC("details"));
  for (size_t i = 0; i < count; i++) {
    struct account_creds creds;
    char url[640];
    char error[256] = "";
    long status = 0;
    if (!first) mg_xprintf(mg_pfn_iobuf, &io, ",");
    first = false;
    if (load_account_creds(db, ids[i], &creds) != 0 ||
        creds.access_token[0] == '\0' || creds.external_account_id[0] == '\0') {
      failed++;
      mg_xprintf(mg_pfn_iobuf, &io, "{%m:%ld,%m:%d,%m:%m}", MG_ESC("id"),
                 ids[i], MG_ESC("ok"), 0, MG_ESC("error"),
                 MG_ESC("账号缺少 Access Token 或 Account ID"));
      continue;
    }
    extract_user_id(&creds);
    if (creds.chatgpt_user_id[0] == '\0') {
      failed++;
      mg_xprintf(mg_pfn_iobuf, &io, "{%m:%ld,%m:%d,%m:%m}", MG_ESC("id"),
                 ids[i], MG_ESC("ok"), 0, MG_ESC("error"),
                 MG_ESC("无法从 Access Token 解析用户 ID，无法退出工作区"));
      continue;
    }
    mg_snprintf(url, sizeof(url),
                CHATGPT_BASE_URL "/backend-api/accounts/%s/users/%s",
                creds.external_account_id, creds.chatgpt_user_id);
    if (workspace_request("DELETE", url, creds.access_token, NULL, &status,
                          error, sizeof(error)) == 0) {
      success++;
      mg_xprintf(mg_pfn_iobuf, &io, "{%m:%ld,%m:%d,%m:%ld}", MG_ESC("id"),
                 ids[i], MG_ESC("ok"), 1, MG_ESC("http_status"), status);
    } else {
      failed++;
      mg_xprintf(mg_pfn_iobuf, &io, "{%m:%ld,%m:%d,%m:%m,%m:%ld}", MG_ESC("id"),
                 ids[i], MG_ESC("ok"), 0, MG_ESC("error"), MG_ESC(error),
                 MG_ESC("http_status"), status);
    }
  }
  mg_xprintf(mg_pfn_iobuf, &io, "],%m:%d,%m:%d}", MG_ESC("success_count"),
             success, MG_ESC("failed_count"), failed);
  mg_iobuf_add(&io, io.len, "", 1);
  return (char *) io.buf;
}

/* 单账号的三种凭证对象，写入 io（不含数组包裹）。 */
static void append_codex(struct mg_iobuf *io, const struct account_creds *c) {
  mg_xprintf(mg_pfn_iobuf, io,
             "{%m:%m,%m:null,%m:{%m:%m,%m:%m,%m:%m,%m:%m},%m:%m}",
             MG_ESC("auth_mode"), MG_ESC("chatgpt"), MG_ESC("OPENAI_API_KEY"),
             MG_ESC("tokens"), MG_ESC("id_token"), MG_ESC(""),
             MG_ESC("access_token"), MG_ESC(c->access_token),
             MG_ESC("refresh_token"), MG_ESC(c->refresh_token),
             MG_ESC("account_id"), MG_ESC(c->external_account_id),
             MG_ESC("last_refresh"), MG_ESC(""));
}

static void append_cpa(struct mg_iobuf *io, const struct account_creds *c) {
  mg_xprintf(mg_pfn_iobuf, io,
             "{%m:%m,%m:%m,%m:%m,%m:%m,%m:%m,%m:%s,%m:%m,%m:%m,%m:%m}",
             MG_ESC("type"), MG_ESC("codex"), MG_ESC("email"),
             MG_ESC(c->email), MG_ESC("id_token"), MG_ESC(""),
             MG_ESC("account_id"), MG_ESC(c->external_account_id),
             MG_ESC("access_token"), MG_ESC(c->access_token),
             MG_ESC("disabled"), "false", MG_ESC("session_token"), MG_ESC(""),
             MG_ESC("refresh_token"), MG_ESC(c->refresh_token),
             MG_ESC("last_refresh"), MG_ESC(""));
}

static void append_sub2api_account(struct mg_iobuf *io,
                                   const struct account_creds *c) {
  mg_xprintf(mg_pfn_iobuf, io,
             "{%m:%m,%m:%m,%m:%m,%m:{%m:%m,%m:%m,%m:%m,%m:%m,%m:%m,%m:%m},"
             "%m:%d,%m:%d}",
             MG_ESC("name"), MG_ESC(c->email), MG_ESC("platform"),
             MG_ESC("openai"), MG_ESC("type"), MG_ESC("oauth"),
             MG_ESC("credentials"), MG_ESC("access_token"),
             MG_ESC(c->access_token), MG_ESC("chatgpt_account_id"),
             MG_ESC(c->external_account_id), MG_ESC("chatgpt_user_id"),
             MG_ESC(c->chatgpt_user_id), MG_ESC("email"), MG_ESC(c->email),
             MG_ESC("refresh_token"), MG_ESC(c->refresh_token),
             MG_ESC("id_token"), MG_ESC(""), MG_ESC("concurrency"), 10,
             MG_ESC("priority"), 1);
}

char *workspace_export_json(sqlite3 *db, const long *ids, size_t count,
                            const char *format) {
  struct mg_iobuf io = {0, 0, 0, 1024};
  struct mg_iobuf content = {0, 0, 0, 1024};
  const char *fmt = format != NULL ? format : "codex";
  const char *filename;
  bool is_sub = strcmp(fmt, "sub2api") == 0 || strcmp(fmt, "sub") == 0;
  bool is_cpa = strcmp(fmt, "cpa") == 0;
  bool first = true;
  int exported = 0;

  if (is_sub) {
    mg_xprintf(mg_pfn_iobuf, &content, "{%m:%m,%m:[],%m:[",
               MG_ESC("exported_at"), MG_ESC(""), MG_ESC("proxies"),
               MG_ESC("accounts"));
  } else {
    mg_xprintf(mg_pfn_iobuf, &content, "[");
  }

  for (size_t i = 0; i < count; i++) {
    struct account_creds creds;
    if (load_account_creds(db, ids[i], &creds) != 0 ||
        creds.access_token[0] == '\0') {
      continue;
    }
    extract_user_id(&creds);
    if (!first) mg_xprintf(mg_pfn_iobuf, &content, ",");
    first = false;
    if (is_sub) {
      append_sub2api_account(&content, &creds);
    } else if (is_cpa) {
      append_cpa(&content, &creds);
    } else {
      append_codex(&content, &creds);
    }
    exported++;
  }

  if (is_sub) {
    mg_xprintf(mg_pfn_iobuf, &content, "]}");
    filename = "sub2api_bundle.json";
  } else {
    mg_xprintf(mg_pfn_iobuf, &content, "]");
    filename = is_cpa ? "cpa_credentials.json" : "codex_auth.json";
  }
  mg_iobuf_add(&content, content.len, "", 1);

  mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:%m,%m:%d,%m:%m,%m:%m}", MG_ESC("ok"),
             exported > 0, MG_ESC("format"),
             MG_ESC(is_sub ? "sub2api" : is_cpa ? "cpa" : "codex"),
             MG_ESC("exported"), exported, MG_ESC("filename"),
             MG_ESC(filename), MG_ESC("content"),
             MG_ESC((char *) content.buf));
  mg_iobuf_free(&content);
  mg_iobuf_add(&io, io.len, "", 1);
  return (char *) io.buf;
}

char *workspace_onboard_and_push_json(sqlite3 *db, const char *email,
                                      const char *access_token,
                                      const char *refresh_token,
                                      const char *external_account_id,
                                      const char *const *target_workspace_ids,
                                      size_t target_count,
                                      const char *pool_type) {
  struct mg_iobuf io = {0, 0, 0, 512};
  int join_success = 0, join_failed = 0;
  int push_success = 0, push_failed = 0;
  bool first = true;

  mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:[", MG_ESC("ok"), 1,
             MG_ESC("details"));

  if (access_token == NULL || access_token[0] == '\0' ||
      external_account_id == NULL || external_account_id[0] == '\0') {
    mg_xprintf(mg_pfn_iobuf, &io, "],%m:%d,%m:%d,%m:%d,%m:%d,%m:%d,%m:%m}",
               MG_ESC("join_success"), 0, MG_ESC("join_failed"), 0,
               MG_ESC("push_success"), 0, MG_ESC("push_failed"), 0,
               MG_ESC("ok"), 0, MG_ESC("error"),
               MG_ESC("缺少 Access Token 或 Account ID，无法执行上车与推送"));
    mg_iobuf_add(&io, io.len, "", 1);
    return (char *) io.buf;
  }

  for (size_t i = 0; i < target_count; i++) {
    const char *ws = target_workspace_ids ? target_workspace_ids[i] : NULL;
    char url[512];
    char join_error[256] = "";
    long join_status = 0;
    bool joined;

    if (ws == NULL || ws[0] == '\0') continue;
    if (!first) mg_xprintf(mg_pfn_iobuf, &io, ",");
    first = false;

    /* 上车：加入外部目标工作区（target workspace 作为 URL 中的 account id）。 */
    mg_snprintf(url, sizeof(url),
                CHATGPT_BASE_URL "/backend-api/accounts/%s/invites/request", ws);
    joined = workspace_request("POST", url, access_token, "{}", &join_status,
                               join_error, sizeof(join_error)) == 0;
    if (joined) {
      join_success++;
    } else {
      join_failed++;
    }

    mg_xprintf(mg_pfn_iobuf, &io,
               "{%m:%m,%m:%d,%m:%ld", MG_ESC("workspace_id"), MG_ESC(ws),
               MG_ESC("join_ok"), joined ? 1 : 0, MG_ESC("join_http_status"),
               join_status);
    if (!joined) {
      mg_xprintf(mg_pfn_iobuf, &io, ",%m:%m", MG_ESC("join_error"),
                 MG_ESC(join_error));
    }

    /* 推送 aether：即便上车失败也尝试推送（工作区可能已加入过），
     * 每个目标工作区都用其自身 workspace_id 标记推送一次。 */
    {
      struct aether_push_credential cred;
      char *push_json;
      bool push_ok = false;
      char push_err[256] = "";

      memset(&cred, 0, sizeof(cred));
      cred.email = email;
      cred.access_token = access_token;
      cred.refresh_token = refresh_token;
      cred.external_account_id = external_account_id;
      cred.workspace_id = ws;
      push_json = aether_upload_credential_json(db, &cred, pool_type);
      if (push_json != NULL) {
        struct mg_str pb = mg_str(push_json);
        push_ok = mg_json_get_long(pb, "$.success", 0) == 1;
        if (!push_ok) {
          char *e = mg_json_get_str(pb, "$.error");
          if (e != NULL) {
            mg_snprintf(push_err, sizeof(push_err), "%s", e);
            mg_free(e);
          }
        }
        free(push_json);
      } else {
        mg_snprintf(push_err, sizeof(push_err), "%s", "Aether 推送失败");
      }
      if (push_ok) {
        push_success++;
      } else {
        push_failed++;
      }
      mg_xprintf(mg_pfn_iobuf, &io, ",%m:%d", MG_ESC("push_ok"),
                 push_ok ? 1 : 0);
      if (!push_ok) {
        mg_xprintf(mg_pfn_iobuf, &io, ",%m:%m", MG_ESC("push_error"),
                   MG_ESC(push_err));
      }
    }

    mg_xprintf(mg_pfn_iobuf, &io, "}");
  }

  mg_xprintf(mg_pfn_iobuf, &io,
             "],%m:%d,%m:%d,%m:%d,%m:%d}", MG_ESC("join_success"), join_success,
             MG_ESC("join_failed"), join_failed, MG_ESC("push_success"),
             push_success, MG_ESC("push_failed"), push_failed);
  mg_iobuf_add(&io, io.len, "", 1);
  return (char *) io.buf;
}
