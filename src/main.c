#include "mongoose.h"
#include "account/account_store.h"
#include "account/account_token_validator.h"
#include "auth/app_auth.h"
#include "flow/flow_impersonate.h"
#include "http_client/browser_profile.h"
#include "http_client/http_client.h"
#include "mail/rapid_inbox.h"
#include "proxy/proxy_pool.h"
#include "redeem/redeem_client.h"
#include "redeem/redeem_store.h"
#include "registration/registration_tasks.h"
#include "storage/app_db.h"
#include "system/system_monitor.h"
#include "upload/aether_upload.h"
#include "workspace/workspace_ops.h"

#include <signal.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define JSON_HEADERS "Content-Type: application/json\r\nCache-Control: no-store\r\n"

extern const struct mg_mem_file mg_packed_files[];

static volatile sig_atomic_t s_running = 1;
static uint64_t s_started_ms;
static sqlite3 *s_db;

static void signal_handler(int signo) {
  (void) signo;
  s_running = 0;
}

static void configure_mongoose_log(void) {
  const char *level = getenv("MONGOOSE_LOG");
  if (level == NULL || *level == '\0' || strcmp(level, "error") == 0) {
    mg_log_set(MG_LL_ERROR);
  } else if (strcmp(level, "none") == 0 || strcmp(level, "off") == 0) {
    mg_log_set(MG_LL_NONE);
  } else if (strcmp(level, "info") == 0) {
    mg_log_set(MG_LL_INFO);
  } else if (strcmp(level, "debug") == 0) {
    mg_log_set(MG_LL_DEBUG);
  } else if (strcmp(level, "verbose") == 0) {
    mg_log_set(MG_LL_VERBOSE);
  } else {
    mg_log_set(MG_LL_ERROR);
  }
}

static unsigned count_connections(struct mg_mgr *mgr) {
  unsigned count = 0;
  for (struct mg_connection *c = mgr->conns; c != NULL; c = c->next) count++;
  return count;
}

static bool uri_has_prefix(struct mg_str uri, const char *prefix) {
  size_t len = strlen(prefix);
  return uri.len >= len && memcmp(uri.buf, prefix, len) == 0;
}

static size_t parse_id_array(struct mg_str body, long **out) {
  struct mg_str tok = mg_json_get_tok(body, "$.ids");
  char *copy, *p, *end;
  long *ids = NULL;
  size_t len = 0, cap = 0;

  *out = NULL;
  if (tok.len == 0) return 0;
  copy = (char *) calloc(1, tok.len + 1);
  if (copy == NULL) return 0;
  memcpy(copy, tok.buf, tok.len);
  p = copy;
  while (*p) {
    long id;
    while (*p && !isdigit((unsigned char) *p) && *p != '-') p++;
    if (*p == '\0') break;
    id = strtol(p, &end, 10);
    if (end == p) break;
    if (id > 0) {
      if (len == cap) {
        cap = cap == 0 ? 8 : cap * 2;
        ids = (long *) realloc(ids, cap * sizeof(*ids));
        if (ids == NULL) {
          free(copy);
          return 0;
        }
      }
      ids[len++] = id;
    }
    p = end;
  }
  free(copy);
  *out = ids;
  return len;
}

static sqlite3_int64 count_query(const char *sql) {
  sqlite3_stmt *stmt = NULL;
  sqlite3_int64 count = 0;
  if (s_db == NULL || sql == NULL) return 0;
  if (sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

static void handle_proxy_api(struct mg_connection *c, struct mg_http_message *hm) {
  if (mg_strcmp(hm->method, mg_str("GET")) == 0 &&
      mg_match(hm->uri, mg_str("/api/proxies"), NULL)) {
    char *json = proxy_pool_list_json(s_db);
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json ? json : "{\"items\":[]}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/proxies/import"), NULL)) {
    char *text = mg_json_get_str(hm->body, "$.text");
    struct proxy_import_result result;
    if (text == NULL) {
      mg_http_reply(c, 400, JSON_HEADERS, "{\"error\":\"missing text\"}\n");
      return;
    }
    proxy_pool_import_text(s_db, text, &result);
    mg_free(text);
    mg_http_reply(c, 200, JSON_HEADERS,
                  "{%m:%d,%m:%d,%m:%d}\n",
                  MG_ESC("imported"), result.imported,
                  MG_ESC("skipped"), result.skipped,
                  MG_ESC("invalid"), result.invalid);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/proxies/test"), NULL)) {
    long *ids = NULL;
    size_t count = parse_id_array(hm->body, &ids);
    char *json = proxy_pool_test_ids(s_db, ids, count);
    free(ids);
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json ? json : "{\"items\":[]}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/proxies/delete"), NULL)) {
    long *ids = NULL;
    size_t count = parse_id_array(hm->body, &ids);
    int deleted = proxy_pool_delete_ids(s_db, ids, count);
    free(ids);
    mg_http_reply(c, deleted < 0 ? 500 : 200, JSON_HEADERS, "{%m:%d}\n",
                  MG_ESC("deleted"), deleted < 0 ? 0 : deleted);
  } else {
    mg_http_reply(c, 404, JSON_HEADERS, "{\"error\":\"not found\"}\n");
  }
}

static void handle_mail_api(struct mg_connection *c, struct mg_http_message *hm) {
  if (mg_strcmp(hm->method, mg_str("GET")) == 0 &&
      mg_match(hm->uri, mg_str("/api/mail/config"), NULL)) {
    char *json = rapid_inbox_config_json(s_db);
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json ? json : "{}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/mail/config"), NULL)) {
    int api_key_len = 0;
    bool has_api_key_field =
        mg_json_get(hm->body, "$.api_key", &api_key_len) >= 0;
    char *base_url = mg_json_get_str(hm->body, "$.base_url");
    char *api_key = mg_json_get_str(hm->body, "$.api_key");
    int rc = rapid_inbox_save_config(s_db, base_url,
                                     has_api_key_field ? (api_key ? api_key : "") : NULL);
    mg_free(base_url);
    mg_free(api_key);
    mg_http_reply(c, rc == 0 ? 200 : 500, JSON_HEADERS, "{%m:%d}\n",
                  MG_ESC("ok"), rc == 0);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/mail/domains"), NULL)) {
    char *pattern = mg_json_get_str(hm->body, "$.pattern");
    char *json = rapid_inbox_add_domain_json(s_db, pattern);
    mg_free(pattern);
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json ? json : "{\"ok\":0}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/mail/domains/delete"), NULL)) {
    long *ids = NULL;
    size_t count = parse_id_array(hm->body, &ids);
    int deleted = rapid_inbox_delete_domain_ids(s_db, ids, count);
    free(ids);
    mg_http_reply(c, deleted < 0 ? 500 : 200, JSON_HEADERS, "{%m:%d}\n",
                  MG_ESC("deleted"), deleted < 0 ? 0 : deleted);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/mail/fetch"), NULL)) {
    char *mailbox = mg_json_get_str(hm->body, "$.mailbox");
    char *action = mg_json_get_str(hm->body, "$.action");
    char *delivery_id = mg_json_get_str(hm->body, "$.delivery_id");
    long limit = mg_json_get_long(hm->body, "$.limit", 20);
    char *json = rapid_inbox_fetch_json(s_db, mailbox,
                                        action ? action : "codes",
                                        delivery_id, limit);
    mg_free(mailbox);
    mg_free(action);
    mg_free(delivery_id);
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json ? json : "{\"ok\":0}");
    free(json);
  } else {
    mg_http_reply(c, 404, JSON_HEADERS, "{\"error\":\"not found\"}\n");
  }
}

static void handle_redeem_api(struct mg_connection *c,
                              struct mg_http_message *hm) {
  if (mg_strcmp(hm->method, mg_str("GET")) == 0 &&
      mg_match(hm->uri, mg_str("/api/redeem/config"), NULL)) {
    char *json = redeem_config_json(s_db);
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json ? json : "{}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("GET")) == 0 &&
             mg_match(hm->uri, mg_str("/api/redeem"), NULL)) {
    char status[32] = "";
    char cursor_buf[32] = "";
    char limit_buf[32] = "";
    long cursor = 0, limit = 50;
    char *json;
    mg_http_get_var(&hm->query, "status", status, sizeof(status));
    if (mg_http_get_var(&hm->query, "cursor", cursor_buf, sizeof(cursor_buf)) >
        0) {
      cursor = strtol(cursor_buf, NULL, 10);
    }
    if (mg_http_get_var(&hm->query, "limit", limit_buf, sizeof(limit_buf)) > 0) {
      limit = strtol(limit_buf, NULL, 10);
    }
    json = redeem_list_json(s_db, status, cursor, limit);
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json ? json : "{\"items\":[]}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/redeem/config"), NULL)) {
    char *base_url = mg_json_get_str(hm->body, "$.base_url");
    int rc = redeem_save_config(s_db, base_url);
    mg_free(base_url);
    mg_http_reply(c, rc == 0 ? 200 : 500, JSON_HEADERS, "{%m:%d}\n",
                  MG_ESC("ok"), rc == 0);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/redeem/import"), NULL)) {
    char *text = mg_json_get_str(hm->body, "$.text");
    char *json = redeem_import_json(s_db, text ? text : "");
    mg_free(text);
    mg_http_reply(c, 200, JSON_HEADERS, "%s\n",
                  json ? json : "{\"ok\":0,\"error\":\"导入失败\"}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/redeem/delete"), NULL)) {
    long *ids = NULL;
    size_t count = parse_id_array(hm->body, &ids);
    int deleted = redeem_delete_ids(s_db, ids, count);
    free(ids);
    mg_http_reply(c, deleted < 0 ? 500 : 200, JSON_HEADERS, "{%m:%d}\n",
                  MG_ESC("deleted"), deleted < 0 ? 0 : deleted);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/redeem/redeem"), NULL)) {
    long *ids = NULL;
    size_t count = parse_id_array(hm->body, &ids);
    char *json = redeem_redeem_ids_json(s_db, ids, count);
    free(ids);
    mg_http_reply(c, 200, JSON_HEADERS, "%s\n",
                  json ? json : "{\"ok\":0,\"error\":\"兑换失败\"}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/redeem/register"), NULL)) {
    struct registration_start_options options;
    char task_id[REG_TASK_ID_LEN];
    char error[256] = "";
    long *ids = NULL;
    size_t count = parse_id_array(hm->body, &ids);
    char *workflow = mg_json_get_str(hm->body, "$.workflow");
    char *target_workspaces = mg_json_get_str(hm->body, "$.target_workspaces");
    char *pool_type = mg_json_get_str(hm->body, "$.aether_pool_type");
    bool detailed = false;
    bool auto_upload = false;

    if (count == 0) {
      free(ids);
      mg_free(workflow);
      mg_free(target_workspaces);
      mg_free(pool_type);
      mg_http_reply(c, 400, JSON_HEADERS, "{%m:%d,%m:%m}\n", MG_ESC("ok"), 0,
                    MG_ESC("error"), MG_ESC("请先选择要注册的兑换码"));
      return;
    }
    memset(&options, 0, sizeof(options));
    options.workflow = REG_WORKFLOW_REGISTER_ONLY;
    options.target_metric = REG_TARGET_REGISTER_TASK;
    options.register_provider = REG_REGISTER_PROVIDER_REDEEM;
    options.count = (int) count;
    options.concurrency = (int) mg_json_get_long(hm->body, "$.concurrency", 1);
    options.redeem_ids = ids;
    options.redeem_id_count = count;
    if (workflow != NULL && strcmp(workflow, "register_then_oauth") == 0) {
      options.workflow = REG_WORKFLOW_REGISTER_THEN_OAUTH;
    }
    /* 有目标工作区时，必须走 OAuth 才能拿到 workspace-scoped token 用于上车与推送。 */
    if (target_workspaces != NULL && target_workspaces[0] != '\0') {
      options.workflow = REG_WORKFLOW_REGISTER_THEN_OAUTH;
      options.target_workspaces = target_workspaces;
    }
    if (pool_type != NULL && pool_type[0] != '\0') {
      options.aether_pool_type = pool_type;
    }
    if (mg_json_get_bool(hm->body, "$.detailed_logs", &detailed)) {
      options.detailed_logs = detailed;
    }
    if (mg_json_get_bool(hm->body, "$.auto_upload_oauth_success", &auto_upload)) {
      options.auto_upload_oauth_success = auto_upload;
    }
    mg_free(workflow);
    if (registration_tasks_start(&options, task_id, sizeof(task_id), error,
                                 sizeof(error)) != 0) {
      free(ids);
      mg_free(target_workspaces);
      mg_free(pool_type);
      mg_http_reply(c, 400, JSON_HEADERS, "{%m:%d,%m:%m}\n", MG_ESC("ok"), 0,
                    MG_ESC("error"), MG_ESC(error));
      return;
    }
    free(ids);
    mg_free(target_workspaces);
    mg_free(pool_type);
    mg_http_reply(c, 200, JSON_HEADERS, "{%m:%d,%m:%m,%m:%d}\n", MG_ESC("ok"), 1,
                  MG_ESC("task_id"), MG_ESC(task_id), MG_ESC("affected"),
                  (int) count);
  } else {
    mg_http_reply(c, 404, JSON_HEADERS, "{\"error\":\"not found\"}\n");
  }
}

static void handle_registration_api(struct mg_connection *c,
                                    struct mg_http_message *hm) {
  if (mg_strcmp(hm->method, mg_str("GET")) == 0 &&
      mg_match(hm->uri, mg_str("/api/registration/status"), NULL)) {
    sqlite3_int64 domains =
        count_query("SELECT count(*) FROM mail_domain_rules WHERE is_active=1");
    sqlite3_int64 proxies =
        count_query("SELECT count(*) FROM proxy_nodes WHERE status='active'");
    sqlite3_int64 temp_accounts =
        count_query("SELECT count(*) FROM accounts WHERE status='temp'");
    int active_tasks = 0, active_flows = 0, queued_flows = 0;
    int provider_ready = 0;
    char impersonate_bin[512];
    registration_tasks_counts(&active_tasks, &active_flows, &queued_flows);
    provider_ready = flow_impersonate_available(impersonate_bin,
                                                sizeof(impersonate_bin)) == 0;
    mg_http_reply(c, 200, JSON_HEADERS,
                  "{%m:%d,%m:%m,%m:%d,%m:%d,%m:%d,%m:%d,%m:%lld,%m:%lld,%m:%lld}\n",
                  MG_ESC("ok"), 1, MG_ESC("engine"), MG_ESC("curl_impersonate"),
                  MG_ESC("provider_ready"), provider_ready, MG_ESC("active_tasks"),
                  active_tasks, MG_ESC("active_flows"), active_flows,
                  MG_ESC("queued_flows"), queued_flows,
                  MG_ESC("active_domains"), domains,
                  MG_ESC("active_proxies"), proxies, MG_ESC("temp_accounts"),
                  temp_accounts);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/registration/start"), NULL)) {
    struct registration_start_options options;
    char task_id[REG_TASK_ID_LEN];
    char error[256] = "";
    char *workflow = mg_json_get_str(hm->body, "$.workflow");
    char *mode = mg_json_get_str(hm->body, "$.mode");
    char *scheduler_mode = mg_json_get_str(hm->body, "$.scheduler_mode");
    const char *workflow_value;
    char *register_provider = mg_json_get_str(hm->body, "$.register_provider");
    char *target_metric = mg_json_get_str(hm->body, "$.target_metric");
    bool detailed = false;
    bool infinite = false;
    bool auto_upload_oauth_success = false;

    memset(&options, 0, sizeof(options));
    options.workflow = REG_WORKFLOW_REGISTER_ONLY;
    options.target_metric = REG_TARGET_REGISTER_TASK;
    options.count = (int) mg_json_get_long(
        hm->body, "$.target_count", mg_json_get_long(hm->body, "$.count", 1));
    options.concurrency = (int) mg_json_get_long(hm->body, "$.concurrency", 1);
    options.max_inflight =
        (int) mg_json_get_long(hm->body, "$.max_inflight", 0);
    options.oauth_delay_seconds =
        (int) mg_json_get_long(hm->body, "$.oauth_delay_seconds", 0);
    if (mg_json_get_bool(hm->body, "$.detailed_logs", &detailed)) {
      options.detailed_logs = detailed;
    }
    if (mg_json_get_bool(hm->body, "$.infinite", &infinite)) {
      options.infinite = infinite;
    }
    if (mg_json_get_bool(hm->body, "$.auto_upload_oauth_success",
                         &auto_upload_oauth_success)) {
      options.auto_upload_oauth_success = auto_upload_oauth_success;
    }
    workflow_value = workflow != NULL ? workflow : mode;
    if (workflow_value != NULL &&
        strcmp(workflow_value, "register_then_oauth") == 0) {
      options.workflow = REG_WORKFLOW_REGISTER_THEN_OAUTH;
    } else if (workflow_value != NULL &&
               strcmp(workflow_value, "oauth_only") == 0) {
      options.workflow = REG_WORKFLOW_OAUTH_ONLY;
    } else if (workflow_value != NULL &&
               strcmp(workflow_value, "fastlane") == 0) {
      options.scheduler_mode = REG_SCHEDULER_FASTLANE;
    }
    if ((scheduler_mode != NULL && strcmp(scheduler_mode, "fastlane") == 0) ||
        (mode != NULL && strcmp(mode, "fastlane") == 0)) {
      options.scheduler_mode = REG_SCHEDULER_FASTLANE;
    }
    if (target_metric != NULL && strcmp(target_metric, "oauth_success") == 0) {
      options.target_metric = REG_TARGET_OAUTH_SUCCESS;
    }
    if (register_provider != NULL &&
        (strcmp(register_provider, "temporary") == 0 ||
         strcmp(register_provider, "temp") == 0 ||
         strcmp(register_provider, "web") == 0 ||
         strcmp(register_provider, "web_register") == 0)) {
      options.register_provider = REG_REGISTER_PROVIDER_TEMPORARY;
    }
    mg_free(workflow);
    mg_free(mode);
    mg_free(scheduler_mode);
    mg_free(register_provider);
    mg_free(target_metric);

    if (registration_tasks_start(&options, task_id, sizeof(task_id), error,
                                 sizeof(error)) != 0) {
      mg_http_reply(c, 400, JSON_HEADERS,
                    "{%m:%d,%m:%m}\n", MG_ESC("ok"), 0,
                    MG_ESC("error"), MG_ESC(error));
      return;
    }
    mg_http_reply(c, 200, JSON_HEADERS,
                  "{%m:%d,%m:%m}\n", MG_ESC("ok"), 1,
                  MG_ESC("task_id"), MG_ESC(task_id));
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/registration/stop"), NULL)) {
    char *id = mg_json_get_str(hm->body, "$.task_id");
    char error[256] = "";
    int rc = registration_tasks_stop(id, error, sizeof(error));
    mg_free(id);
    if (rc != 0) {
      mg_http_reply(c, 400, JSON_HEADERS, "{%m:%d,%m:%m}\n",
                    MG_ESC("ok"), 0, MG_ESC("error"), MG_ESC(error));
    } else {
      mg_http_reply(c, 200, JSON_HEADERS, "{%m:%d}\n", MG_ESC("ok"), 1);
    }
  } else if (mg_strcmp(hm->method, mg_str("GET")) == 0 &&
             mg_match(hm->uri, mg_str("/api/registration/tasks"), NULL)) {
    char *json = registration_tasks_list_json();
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json ? json : "{\"items\":[]}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("GET")) == 0 &&
             mg_match(hm->uri, mg_str("/api/registration/task"), NULL)) {
    char id[REG_TASK_ID_LEN] = "";
    char *json;
    mg_http_get_var(&hm->query, "id", id, sizeof(id));
    json = registration_task_detail_json(id, true);
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json ? json : "{\"ok\":0}");
    free(json);
  } else {
    mg_http_reply(c, 404, JSON_HEADERS, "{\"error\":\"not found\"}\n");
  }
}

static void handle_accounts_api(struct mg_connection *c, struct mg_http_message *hm) {
  if (mg_strcmp(hm->method, mg_str("GET")) == 0 &&
      mg_match(hm->uri, mg_str("/api/accounts/summary"), NULL)) {
    char *json = account_summary_json(s_db);
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json ? json : "{}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("GET")) == 0 &&
             mg_match(hm->uri, mg_str("/api/accounts/detail"), NULL)) {
    char id_buf[32] = "";
    long id = 0;
    char *json;
    if (mg_http_get_var(&hm->query, "id", id_buf, sizeof(id_buf)) > 0) {
      id = strtol(id_buf, NULL, 10);
    }
    json = account_detail_json(s_db, id);
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json ? json : "{\"ok\":0}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("GET")) == 0 &&
             mg_match(hm->uri, mg_str("/api/accounts"), NULL)) {
    char q[256] = "";
    char status[32] = "";
    char upload_state[32] = "";
    char cursor_buf[32] = "";
    char limit_buf[32] = "";
    long cursor = 0, limit = 50;
    char *json;

    mg_http_get_var(&hm->query, "q", q, sizeof(q));
    mg_http_get_var(&hm->query, "status", status, sizeof(status));
    mg_http_get_var(&hm->query, "upload_state", upload_state,
                    sizeof(upload_state));
    if (mg_http_get_var(&hm->query, "cursor", cursor_buf,
                        sizeof(cursor_buf)) > 0) {
      cursor = strtol(cursor_buf, NULL, 10);
    }
    if (mg_http_get_var(&hm->query, "limit", limit_buf,
                        sizeof(limit_buf)) > 0) {
      limit = strtol(limit_buf, NULL, 10);
    }
    json = account_list_json(s_db, q, status, upload_state, cursor, limit);
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json ? json : "{\"items\":[]}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/accounts/action"), NULL)) {
    long *ids = NULL;
    size_t count = parse_id_array(hm->body, &ids);
    char *action = mg_json_get_str(hm->body, "$.action");
    int changed = -1;

    if (action != NULL && strcmp(action, "refresh-token") == 0) {
      char *json = account_refresh_tokens_json(s_db, ids, count);
      mg_free(action);
      free(ids);
      mg_http_reply(c, 200, JSON_HEADERS, "%s\n",
                    json ? json : "{\"ok\":0,\"error\":\"Token 刷新失败\"}");
      free(json);
      return;
    } else if (action != NULL && strcmp(action, "validate-token") == 0) {
      char *json = account_validate_tokens_json(s_db, ids, count);
      mg_free(action);
      free(ids);
      mg_http_reply(c, 200, JSON_HEADERS, "%s\n",
                    json ? json : "{\"ok\":0,\"error\":\"Token 验证失败\"}");
      free(json);
      return;
    } else if (action != NULL && strcmp(action, "reupload") == 0) {
      char *pool_type = mg_json_get_str(hm->body, "$.pool_type");
      char *json = aether_upload_accounts_json(s_db, ids, count, pool_type);
      mg_free(pool_type);
      mg_free(action);
      free(ids);
      mg_http_reply(c, 200, JSON_HEADERS, "%s\n",
                    json ? json : "{\"ok\":0,\"error\":\"Aether 上传失败\"}");
      free(json);
      return;
    } else if (action != NULL && strcmp(action, "workspace-join") == 0) {
      char *json = workspace_join_json(s_db, ids, count);
      mg_free(action);
      free(ids);
      mg_http_reply(c, 200, JSON_HEADERS, "%s\n",
                    json ? json : "{\"ok\":0,\"error\":\"加入工作区失败\"}");
      free(json);
      return;
    } else if (action != NULL && strcmp(action, "workspace-leave") == 0) {
      char *json = workspace_leave_json(s_db, ids, count);
      mg_free(action);
      free(ids);
      mg_http_reply(c, 200, JSON_HEADERS, "%s\n",
                    json ? json : "{\"ok\":0,\"error\":\"退出工作区失败\"}");
      free(json);
      return;
    } else if (action != NULL && strcmp(action, "export-credentials") == 0) {
      char *format = mg_json_get_str(hm->body, "$.format");
      char *json = workspace_export_json(s_db, ids, count,
                                         format ? format : "codex");
      mg_free(format);
      mg_free(action);
      free(ids);
      mg_http_reply(c, 200, JSON_HEADERS, "%s\n",
                    json ? json : "{\"ok\":0,\"error\":\"导出凭证失败\"}");
      free(json);
      return;
    } else if (action != NULL && strcmp(action, "delete") == 0) {
      changed = account_delete_ids(s_db, ids, count);
    } else if (action != NULL && strcmp(action, "oauth") == 0) {
      struct registration_start_options options;
      char task_id[REG_TASK_ID_LEN];
      char error[256] = "";
      bool detailed = false;
      memset(&options, 0, sizeof(options));
      options.workflow = REG_WORKFLOW_OAUTH_ONLY;
      options.target_metric = REG_TARGET_OAUTH_SUCCESS;
      options.count = (int) count;
      options.concurrency = (int) mg_json_get_long(hm->body, "$.concurrency", 10);
      if (mg_json_get_bool(hm->body, "$.detailed_logs", &detailed)) {
        options.detailed_logs = detailed;
      }
      options.account_ids = ids;
      options.account_id_count = count;
      if (registration_tasks_start(&options, task_id, sizeof(task_id), error,
                                   sizeof(error)) == 0) {
        mg_free(action);
        free(ids);
        mg_http_reply(c, 200, JSON_HEADERS,
                      "{%m:%d,%m:%m,%m:%d}\n", MG_ESC("ok"), 1,
                      MG_ESC("task_id"), MG_ESC(task_id), MG_ESC("affected"),
                      (int) count);
        return;
      }
      mg_free(action);
      free(ids);
      mg_http_reply(c, 400, JSON_HEADERS, "{%m:%d,%m:%m}\n",
                    MG_ESC("ok"), 0, MG_ESC("error"), MG_ESC(error));
      return;
    }

    mg_free(action);
    free(ids);
    if (changed < 0) {
      mg_http_reply(c, 400, JSON_HEADERS, "{%m:%d,%m:%m}\n",
                    MG_ESC("ok"), 0, MG_ESC("error"),
                    MG_ESC("账号动作无效或执行失败"));
    } else {
      mg_http_reply(c, 200, JSON_HEADERS, "{%m:%d,%m:%d}\n",
                    MG_ESC("ok"), 1, MG_ESC("affected"), changed);
    }
  } else {
    mg_http_reply(c, 404, JSON_HEADERS, "{\"error\":\"not found\"}\n");
  }
}

static void handle_upload_api(struct mg_connection *c,
                              struct mg_http_message *hm) {
  if (mg_strcmp(hm->method, mg_str("GET")) == 0 &&
      mg_match(hm->uri, mg_str("/api/upload/aether"), NULL)) {
    char *json = aether_config_json(s_db);
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json ? json : "{}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/upload/aether/service"), NULL)) {
    char *json = aether_service_save_json(s_db, hm->body);
    mg_http_reply(c, 200, JSON_HEADERS, "%s\n",
                  json ? json : "{\"ok\":0,\"error\":\"保存失败\"}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/upload/aether/options"), NULL)) {
    char *json = aether_options_json(s_db, hm->body);
    mg_http_reply(c, 200, JSON_HEADERS, "%s\n",
                  json ? json : "{\"ok\":0,\"error\":\"读取选项失败\"}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/upload/aether/service/delete"),
                      NULL)) {
    char *json = aether_service_delete_json(s_db, hm->body);
    mg_http_reply(c, 200, JSON_HEADERS, "%s\n",
                  json ? json : "{\"ok\":0,\"error\":\"删除失败\"}");
    free(json);
  } else if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
             mg_match(hm->uri, mg_str("/api/upload/aether/test"), NULL)) {
    char *json = aether_service_test_json(s_db, hm->body);
    mg_http_reply(c, 200, JSON_HEADERS, "%s\n",
                  json ? json : "{\"ok\":0,\"error\":\"测试失败\"}");
    free(json);
  } else {
    mg_http_reply(c, 404, JSON_HEADERS, "{\"error\":\"not found\"}\n");
  }
}

static void handle_http(struct mg_connection *c, struct mg_http_message *hm) {
  if (uri_has_prefix(hm->uri, "/api/auth")) {
    app_auth_handle_api(s_db, c, hm);
    return;
  }
  if (app_auth_enabled() && !app_auth_is_public_route(hm) &&
      !app_auth_is_authenticated(s_db, hm)) {
    app_auth_reply_unauthorized(c, hm);
    return;
  }

  if (mg_match(hm->uri, mg_str("/api/status"), NULL)) {
    uint64_t uptime = mg_millis() - s_started_ms;
    char *json = system_monitor_status_json(s_db, uptime,
                                            count_connections(c->mgr));
    mg_http_reply(c, 200, JSON_HEADERS, "%s\n", json ? json : "{}");
    free(json);
  } else if (mg_match(hm->uri, mg_str("/api/browser-profile"), NULL)) {
    char region[16] = "";
    char device[16] = "";
    char json[2048];
    struct browser_profile profile;

    mg_http_get_var(&hm->query, "region", region, sizeof(region));
    mg_http_get_var(&hm->query, "device", device, sizeof(device));
    browser_profile_generate(&profile, region, device);
    browser_profile_to_json(&profile, json, sizeof(json));
    mg_http_reply(c, 200, JSON_HEADERS, "%s", json);
  } else if (uri_has_prefix(hm->uri, "/api/proxies")) {
    handle_proxy_api(c, hm);
  } else if (uri_has_prefix(hm->uri, "/api/mail")) {
    handle_mail_api(c, hm);
  } else if (uri_has_prefix(hm->uri, "/api/redeem")) {
    handle_redeem_api(c, hm);
  } else if (uri_has_prefix(hm->uri, "/api/upload")) {
    handle_upload_api(c, hm);
  } else if (uri_has_prefix(hm->uri, "/api/accounts")) {
    handle_accounts_api(c, hm);
  } else if (uri_has_prefix(hm->uri, "/api/registration")) {
    handle_registration_api(c, hm);
  } else if (mg_match(hm->uri, mg_str("/api/echo"), NULL)) {
    mg_http_reply(c, 200, JSON_HEADERS,
                  "{%m:%m,%m:%m,%m:%m}\n",
                  MG_ESC("status"), MG_ESC("ok"),
                  MG_ESC("method"), mg_print_esc, (int) hm->method.len,
                  hm->method.buf, MG_ESC("body"), mg_print_esc,
                  (int) hm->body.len, hm->body.buf);
  } else if (uri_has_prefix(hm->uri, "/api/")) {
    mg_http_reply(c, 404, JSON_HEADERS, "{\"error\":\"not found\"}\n");
  } else if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
    mg_ws_upgrade(c, hm, NULL);
  } else if (mg_match(hm->uri, mg_str("/"), NULL) ||
             mg_match(hm->uri, mg_str("/index.html"), NULL)) {
    mg_http_reply(c, 302, "Location: /console\r\nCache-Control: no-store\r\n", "");
  } else {
    struct mg_http_serve_opts opts = {
        .root_dir = "/web",
        .page404 = "/web/index.html",
        .fs = &mg_fs_packed,
        .extra_headers = "Cache-Control: no-cache\r\n",
    };
    mg_http_serve_dir(c, hm, &opts);
  }
}

static void event_handler(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_HTTP_MSG) {
    handle_http(c, (struct mg_http_message *) ev_data);
  } else if (ev == MG_EV_POLL) {
    registration_ws_poll(c->mgr, s_db, s_started_ms);
  } else if (ev == MG_EV_WS_OPEN) {
    registration_ws_open(c);
    mg_ws_printf(c, WEBSOCKET_OP_TEXT, "{%m:%m,%m:%llu}",
                 MG_ESC("type"), MG_ESC("hello"),
                 MG_ESC("time_ms"), (unsigned long long) mg_millis());
  } else if (ev == MG_EV_WS_MSG) {
    struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
    if (!registration_ws_handle_message(c, wm->data, s_db, s_started_ms)) {
      mg_ws_printf(c, WEBSOCKET_OP_TEXT, "{%m:%m,%m:%m,%m:%m}",
                   MG_ESC("type"), MG_ESC("echo"),
                   MG_ESC("status"), MG_ESC("ok"),
                   MG_ESC("message"), mg_print_esc, (int) wm->data.len,
                   wm->data.buf);
    }
  } else if (ev == MG_EV_CLOSE) {
    if (c->fn_data != NULL) registration_ws_close(c);
  }
}

int main(int argc, char **argv) {
  const char *listen_url = argc > 1 ? argv[1] : "http://0.0.0.0:8000";
  struct mg_mgr mgr;

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  configure_mongoose_log();
  mg_mgr_init(&mgr);
  if (http_client_global_init() != 0) {
    fprintf(stderr, "Cannot initialize libcurl\n");
    mg_mgr_free(&mgr);
    return 1;
  }
  if (app_db_open("data/app.db", &s_db) != 0) {
    fprintf(stderr, "Cannot open data/app.db\n");
    http_client_global_cleanup();
    mg_mgr_free(&mgr);
    return 1;
  }
  if (app_auth_init(s_db) != 0) {
    app_db_close(s_db);
    http_client_global_cleanup();
    mg_mgr_free(&mgr);
    return 1;
  }
  browser_profile_seed((uint64_t) time(NULL) ^ (uint64_t) mg_millis());
  mg_mem_files = mg_packed_files;
  s_started_ms = mg_millis();

  if (mg_http_listen(&mgr, listen_url, event_handler, NULL) == NULL) {
    fprintf(stderr, "Cannot listen on %s\n", listen_url);
    app_db_close(s_db);
    http_client_global_cleanup();
    mg_mgr_free(&mgr);
    return 1;
  }

  printf("Mongoose app listening on %s\n", listen_url);
  printf("Open http://127.0.0.1:8000\n");

  while (s_running) mg_mgr_poll(&mgr, 50);
  app_db_close(s_db);
  http_client_global_cleanup();
  mg_mgr_free(&mgr);
  return 0;
}
