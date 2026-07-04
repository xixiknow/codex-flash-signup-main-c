#include "flow/flow_engine.h"

#include "account/account_store.h"
#include "mongoose.h"

#include <curl/curl.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct response_buffer {
  char *data;
  size_t len;
  size_t cap;
};

struct flow_slot {
  struct flow_context flow;
  const struct flow_provider *provider;
  CURL *easy;
  struct curl_slist *headers;
  struct response_buffer response;
  char location[FLOW_URL_LEN];
  char content_type[160];
  char server[80];
  char cf_mitigated[80];
  char cf_ray[160];
  char device_id[80];
  char auth_session_cookie[FLOW_COOKIE_LEN];
  char auth_info_cookie[FLOW_COOKIE_LEN];
  char request_url[FLOW_URL_LEN];
  char request_method[16];
  bool in_multi;
  bool finished;
};

struct flow_engine {
  CURLM *multi;
  sqlite3 *db;
  size_t max_concurrency;
  struct flow_slot **slots;
  size_t len;
  size_t cap;
};

static void copy_set_cookie_value(const char *line, size_t len,
                                  const char *name, char *out,
                                  size_t out_len);

static bool proxy_url_has_scheme(const char *url, const char *scheme) {
  size_t len;
  if (url == NULL || scheme == NULL) return false;
  len = strlen(scheme);
  return strncasecmp(url, scheme, len) == 0 && strncmp(url + len, "://", 3) == 0;
}

static size_t write_body(char *ptr, size_t size, size_t nmemb, void *userdata) {
  struct response_buffer *buffer = (struct response_buffer *) userdata;
  size_t len = size * nmemb;
  char *next;

  if (len == 0) return 0;
  if (buffer->len + len + 1 > buffer->cap) {
    size_t cap = buffer->cap == 0 ? 4096 : buffer->cap;
    while (cap < buffer->len + len + 1) cap *= 2;
    next = (char *) realloc(buffer->data, cap);
    if (next == NULL) return 0;
    buffer->data = next;
    buffer->cap = cap;
  }
  memcpy(buffer->data + buffer->len, ptr, len);
  buffer->len += len;
  buffer->data[buffer->len] = '\0';
  return len;
}

static size_t write_header(char *ptr, size_t size, size_t nmemb,
                           void *userdata) {
  struct flow_slot *slot = (struct flow_slot *) userdata;
  size_t len = size * nmemb;
  if (slot == NULL || len == 0) return len;
  if (len > 10 && strncasecmp(ptr, "Location:", 9) == 0) {
    const char *value = ptr + 9;
    size_t vlen = len - 9;
    while (vlen > 0 && (*value == ' ' || *value == '\t')) {
      value++;
      vlen--;
    }
    while (vlen > 0 && (value[vlen - 1] == '\r' || value[vlen - 1] == '\n')) {
      vlen--;
    }
    mg_snprintf(slot->location, sizeof(slot->location), "%.*s",
                (int) vlen, value);
  } else if (len > 14 && strncasecmp(ptr, "Content-Type:", 13) == 0) {
    const char *value = ptr + 13;
    size_t vlen = len - 13;
    while (vlen > 0 && (*value == ' ' || *value == '\t')) {
      value++;
      vlen--;
    }
    while (vlen > 0 && (value[vlen - 1] == '\r' || value[vlen - 1] == '\n')) {
      vlen--;
    }
    mg_snprintf(slot->content_type, sizeof(slot->content_type), "%.*s",
                (int) vlen, value);
  } else if (len > 8 && strncasecmp(ptr, "Server:", 7) == 0) {
    const char *value = ptr + 7;
    size_t vlen = len - 7;
    while (vlen > 0 && (*value == ' ' || *value == '\t')) {
      value++;
      vlen--;
    }
    while (vlen > 0 && (value[vlen - 1] == '\r' || value[vlen - 1] == '\n')) {
      vlen--;
    }
    mg_snprintf(slot->server, sizeof(slot->server), "%.*s", (int) vlen,
                value);
  } else if (len > 14 && strncasecmp(ptr, "CF-Mitigated:", 13) == 0) {
    const char *value = ptr + 13;
    size_t vlen = len - 13;
    while (vlen > 0 && (*value == ' ' || *value == '\t')) {
      value++;
      vlen--;
    }
    while (vlen > 0 && (value[vlen - 1] == '\r' || value[vlen - 1] == '\n')) {
      vlen--;
    }
    mg_snprintf(slot->cf_mitigated, sizeof(slot->cf_mitigated), "%.*s",
                (int) vlen, value);
  } else if (len > 8 && strncasecmp(ptr, "CF-Ray:", 7) == 0) {
    const char *value = ptr + 7;
    size_t vlen = len - 7;
    while (vlen > 0 && (*value == ' ' || *value == '\t')) {
      value++;
      vlen--;
    }
    while (vlen > 0 && (value[vlen - 1] == '\r' || value[vlen - 1] == '\n')) {
      vlen--;
    }
    mg_snprintf(slot->cf_ray, sizeof(slot->cf_ray), "%.*s", (int) vlen,
                value);
  } else if (len > 11 && strncasecmp(ptr, "Set-Cookie:", 11) == 0) {
    copy_set_cookie_value(ptr, len, "oai-did", slot->device_id,
                          sizeof(slot->device_id));
    copy_set_cookie_value(ptr, len, "oai-client-auth-session",
                          slot->auth_session_cookie,
                          sizeof(slot->auth_session_cookie));
    copy_set_cookie_value(ptr, len, "oai_client_auth_session",
                          slot->auth_session_cookie,
                          sizeof(slot->auth_session_cookie));
    copy_set_cookie_value(ptr, len, "oai-client-auth-info",
                          slot->auth_info_cookie,
                          sizeof(slot->auth_info_cookie));
    copy_set_cookie_value(ptr, len, "oai_client_auth_info",
                          slot->auth_info_cookie,
                          sizeof(slot->auth_info_cookie));
  }
  return size * nmemb;
}

static void response_buffer_reset(struct response_buffer *buffer) {
  if (buffer == NULL) return;
  buffer->len = 0;
  if (buffer->data != NULL) buffer->data[0] = '\0';
}

static void response_buffer_clear_location(struct flow_slot *slot) {
  if (slot == NULL) return;
  slot->location[0] = '\0';
  slot->content_type[0] = '\0';
  slot->server[0] = '\0';
  slot->cf_mitigated[0] = '\0';
  slot->cf_ray[0] = '\0';
  slot->device_id[0] = '\0';
  slot->auth_session_cookie[0] = '\0';
  slot->auth_info_cookie[0] = '\0';
}

static void response_buffer_free(struct response_buffer *buffer) {
  if (buffer == NULL) return;
  free(buffer->data);
  memset(buffer, 0, sizeof(*buffer));
}

static struct curl_slist *append_header(struct curl_slist *headers,
                                        const char *name,
                                        const char *value) {
  char line[768];
  if (name == NULL || value == NULL || value[0] == '\0') return headers;
  snprintf(line, sizeof(line), "%s: %s", name, value);
  return curl_slist_append(headers, line);
}

static struct curl_slist *append_profile_headers(
    struct curl_slist *headers, const struct browser_profile *profile) {
  if (profile == NULL) return headers;
  headers = append_header(headers, "Accept", profile->accept);
  headers = append_header(headers, "Accept-Language", profile->accept_language);
  headers = append_header(headers, "Sec-CH-UA", profile->sec_ch_ua);
  headers = append_header(headers, "Sec-CH-UA-Platform",
                          profile->sec_ch_ua_platform);
  headers = append_header(headers, "Sec-CH-UA-Mobile",
                          profile->sec_ch_ua_mobile);
  return headers;
}

static void copy_set_cookie_value(const char *line, size_t len,
                                  const char *name, char *out,
                                  size_t out_len) {
  const char *value;
  const char *end;
  size_t name_len;
  if (line == NULL || name == NULL || out == NULL || out_len == 0) return;
  if (len <= 11 || strncasecmp(line, "Set-Cookie:", 11) != 0) return;
  value = line + 11;
  while ((size_t) (value - line) < len && (*value == ' ' || *value == '\t')) {
    value++;
  }
  name_len = strlen(name);
  if ((size_t) (value - line) + name_len + 1 > len ||
      strncasecmp(value, name, name_len) != 0 || value[name_len] != '=') {
    return;
  }
  value += name_len + 1;
  end = memchr(value, ';', len - (size_t) (value - line));
  if (end == NULL) end = line + len;
  while (end > value && (end[-1] == '\r' || end[-1] == '\n')) end--;
  if ((size_t) (end - value) >= out_len) end = value + out_len - 1;
  mg_snprintf(out, out_len, "%.*s", (int) (end - value), value);
}

static void sanitized_url(const char *url, char *out, size_t out_len) {
  const char *q;
  size_t len;
  if (out_len == 0) return;
  out[0] = '\0';
  if (url == NULL || url[0] == '\0') return;
  q = strchr(url, '?');
  len = q == NULL ? strlen(url) : (size_t) (q - url);
  if (len >= out_len) len = out_len - 1;
  memcpy(out, url, len);
  out[len] = '\0';
  if (q != NULL && len + 4 < out_len) strncat(out, "?...", out_len - strlen(out) - 1);
}

static void proxy_scheme_label(const char *proxy_url, char *out, size_t out_len) {
  const char *p;
  size_t len;
  if (out_len == 0) return;
  out[0] = '\0';
  if (proxy_url == NULL || proxy_url[0] == '\0') {
    mg_snprintf(out, out_len, "direct");
    return;
  }
  p = strstr(proxy_url, "://");
  if (p == NULL) {
    mg_snprintf(out, out_len, "proxy");
    return;
  }
  len = (size_t) (p - proxy_url);
  if (len >= out_len) len = out_len - 1;
  memcpy(out, proxy_url, len);
  out[len] = '\0';
}

static void free_slot(struct flow_slot *slot) {
  if (slot == NULL) return;
  if (slot->provider != NULL && slot->provider->cleanup != NULL) {
    slot->provider->cleanup(&slot->flow);
  }
  if (slot->headers != NULL) curl_slist_free_all(slot->headers);
  if (slot->easy != NULL) curl_easy_cleanup(slot->easy);
  response_buffer_free(&slot->response);
  free(slot);
}

static void generate_flow_id(char *out, size_t out_len) {
  uint64_t seed = 0;
  if (out_len == 0) return;
  if (!mg_random(&seed, sizeof(seed))) seed = (uint64_t) rand();
  mg_snprintf(out, out_len, "flow-%llx",
              (unsigned long long) seed);
}

const char *flow_status_name(enum flow_status status) {
  switch (status) {
    case FLOW_STATUS_PENDING:
      return "pending";
    case FLOW_STATUS_RUNNING:
      return "running";
    case FLOW_STATUS_SUCCESS:
      return "success";
    case FLOW_STATUS_FAILED:
      return "failed";
    case FLOW_STATUS_CANCELLED:
      return "cancelled";
    default:
      return "unknown";
  }
}

void flow_context_fail(struct flow_context *flow, const char *message) {
  if (flow == NULL) return;
  flow->status = FLOW_STATUS_FAILED;
  mg_snprintf(flow->error, sizeof(flow->error), "%s",
              message ? message : "flow failed");
}

void flow_context_cancel(struct flow_context *flow, const char *message) {
  if (flow == NULL) return;
  flow->status = FLOW_STATUS_CANCELLED;
  mg_snprintf(flow->error, sizeof(flow->error), "%s",
              message ? message : "流程已取消");
}

bool flow_context_cancel_requested(struct flow_context *flow) {
  if (flow == NULL) return false;
  if (flow->status == FLOW_STATUS_CANCELLED) return true;
  if (flow->cancel_fn == NULL) return false;
  return flow->cancel_fn(flow, flow->callback_data);
}

void flow_context_mark_environment_retry(struct flow_context *flow,
                                         const char *message) {
  if (flow == NULL) return;
  flow->environment_retryable = true;
  flow_context_fail(flow, message ? message : "需要重新分配环境重试");
}

static bool contains_case(const char *s, const char *needle) {
  size_t nlen;
  if (s == NULL || needle == NULL || needle[0] == '\0') return false;
  nlen = strlen(needle);
  for (; *s != '\0'; s++) {
    if (strncasecmp(s, needle, nlen) == 0) return true;
  }
  return false;
}

bool flow_response_is_edge_block(const struct flow_http_response *response) {
  bool edge_signal, html_signal;
  if (response == NULL) return false;
  if (response->status_code != 403 && response->status_code != 429) return false;
  edge_signal = contains_case(response->server, "cloudflare") ||
                response->cf_ray[0] != '\0' ||
                response->cf_mitigated[0] != '\0' ||
                contains_case(response->body, "cloudflare");
  if (!edge_signal) return false;
  html_signal = contains_case(response->content_type, "text/html") ||
                contains_case(response->body, "<html") ||
                contains_case(response->body, "<!doctype html") ||
                contains_case(response->body, "challenge") ||
                contains_case(response->body, "blocked");
  return html_signal;
}

void flow_context_log(struct flow_context *flow, const char *level,
                      const char *fmt, ...) {
  char message[FLOW_LOG_LEN];
  va_list ap;

  if (flow == NULL || flow->log_fn == NULL || fmt == NULL) return;
  va_start(ap, fmt);
  vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);
  flow->log_fn(flow, level != NULL && level[0] != '\0' ? level : "info",
               message, flow->callback_data);
}

void flow_context_emit_event(struct flow_context *flow, const char *event) {
  if (flow == NULL || flow->event_fn == NULL || event == NULL) return;
  flow->event_fn(flow, event, flow->callback_data);
}

int flow_engine_create(const struct flow_engine_options *options,
                       struct flow_engine **out) {
  struct flow_engine *engine;
  if (out == NULL) return -1;
  *out = NULL;
  engine = (struct flow_engine *) calloc(1, sizeof(*engine));
  if (engine == NULL) return -1;
  engine->multi = curl_multi_init();
  if (engine->multi == NULL) {
    free(engine);
    return -1;
  }
  engine->db = options ? options->db : NULL;
  engine->max_concurrency =
      options != NULL && options->max_concurrency > 0 ? options->max_concurrency
                                                      : 512;
  *out = engine;
  return 0;
}

void flow_engine_destroy(struct flow_engine *engine) {
  if (engine == NULL) return;
  for (size_t i = 0; i < engine->len; i++) free_slot(engine->slots[i]);
  free(engine->slots);
  if (engine->multi != NULL) curl_multi_cleanup(engine->multi);
  free(engine);
}

static int append_slot(struct flow_engine *engine, struct flow_slot *slot) {
  struct flow_slot **next;
  if (engine->len == engine->cap) {
    size_t cap = engine->cap == 0 ? 16 : engine->cap * 2;
    next = (struct flow_slot **) realloc(engine->slots, cap * sizeof(*next));
    if (next == NULL) return -1;
    engine->slots = next;
    engine->cap = cap;
  }
  engine->slots[engine->len++] = slot;
  return 0;
}

static size_t live_slot_count(const struct flow_engine *engine) {
  size_t live = 0;
  if (engine == NULL) return 0;
  for (size_t i = 0; i < engine->len; i++) {
    if (engine->slots[i] != NULL && !engine->slots[i]->finished) live++;
  }
  return live;
}

static int persist_success(struct flow_engine *engine, struct flow_context *flow) {
  struct account_success_record record;
  if (engine == NULL || engine->db == NULL || flow == NULL ||
      !flow->persist_on_success) {
    return 0;
  }
  memset(&record, 0, sizeof(record));
  record.email = flow->identity.email;
  record.password = flow->identity.password;
  record.status = flow->success_account_status[0] ? flow->success_account_status : "temp";
  record.upload_state = "not_uploaded";
  record.access_token = flow->access_token;
  record.refresh_token = flow->refresh_token;
  record.external_account_id = flow->external_account_id;
  record.workspace_id = flow->workspace_id;
  if (account_insert_success(engine->db, &record,
                             &flow->persisted_account_id) != 0) {
    flow_context_fail(flow, "成功结果写入账号库失败");
    return -1;
  }
  return 0;
}

static int configure_request(struct flow_slot *slot,
                             const struct flow_http_request *request) {
  CURL *easy = slot->easy;
  const char *method;

  if (easy == NULL || request == NULL || request->url == NULL ||
      request->url[0] == '\0') {
    flow_context_fail(&slot->flow, "请求未配置 URL");
    return -1;
  }
  response_buffer_reset(&slot->response);
  response_buffer_clear_location(slot);
  if (slot->headers != NULL) {
    curl_slist_free_all(slot->headers);
    slot->headers = NULL;
  }
  curl_easy_reset(easy);
  method = request->method != NULL && request->method[0] != '\0'
               ? request->method
               : "GET";
  mg_snprintf(slot->request_method, sizeof(slot->request_method), "%s", method);
  sanitized_url(request->url, slot->request_url, sizeof(slot->request_url));

  curl_easy_setopt(easy, CURLOPT_URL, request->url);
  curl_easy_setopt(easy, CURLOPT_PRIVATE, slot);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION,
                   request->follow_location ? 1L : 0L);
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_body);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &slot->response);
  curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_header);
  curl_easy_setopt(easy, CURLOPT_HEADERDATA, slot);
  curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, slot->flow.error);
  curl_easy_setopt(easy, CURLOPT_COOKIEFILE, "");
  curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
  if (request->timeout_ms > 0) {
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, request->timeout_ms);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, request->timeout_ms);
  }
  if (slot->flow.proxy_url[0] != '\0') {
    curl_easy_setopt(easy, CURLOPT_PROXY, slot->flow.proxy_url);
    if (proxy_url_has_scheme(slot->flow.proxy_url, "socks5")) {
      curl_easy_setopt(easy, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
#if LIBCURL_VERSION_NUM >= 0x073E00
      curl_easy_setopt(easy, CURLOPT_DOH_URL, "https://1.1.1.1/dns-query");
#endif
    }
  }
  if (slot->flow.profile.user_agent[0] != '\0') {
    curl_easy_setopt(easy, CURLOPT_USERAGENT, slot->flow.profile.user_agent);
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING,
                     slot->flow.profile.accept_encoding);
    slot->headers = append_profile_headers(slot->headers, &slot->flow.profile);
  } else {
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
  }
  for (size_t i = 0; i < request->num_headers; i++) {
    slot->headers = append_header(slot->headers, request->headers[i].name,
                                  request->headers[i].value);
  }
  if (slot->headers != NULL) curl_easy_setopt(easy, CURLOPT_HTTPHEADER,
                                              slot->headers);

  if (strcmp(method, "POST") == 0) {
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS,
                     request->body != NULL ? request->body : "");
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long) request->body_len);
  } else if (strcmp(method, "GET") != 0) {
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method);
    if (request->body != NULL) {
      curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request->body);
      curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long) request->body_len);
    }
  }
  {
    char scheme[24];
    proxy_scheme_label(slot->flow.proxy_url, scheme, sizeof(scheme));
    flow_context_log(&slot->flow, "debug",
                     "HTTP 请求: %s %s timeout=%ldms proxy=%s body=%zuB",
                     method, slot->request_url, request->timeout_ms,
                     scheme, request->body_len);
  }
  return 0;
}

static int schedule_next(struct flow_engine *engine, struct flow_slot *slot) {
  struct flow_http_request request;
  enum flow_provider_action action;

  if (slot == NULL || slot->provider == NULL ||
      slot->provider->next_request == NULL || slot->finished) {
    return 0;
  }
  if (flow_context_cancel_requested(&slot->flow)) {
    flow_context_cancel(&slot->flow, "流程已取消");
    slot->finished = true;
    if (slot->flow.finish_fn != NULL) {
      slot->flow.finish_fn(&slot->flow, slot->flow.callback_data);
    }
    return -1;
  }
  if (slot->flow.status == FLOW_STATUS_PENDING) {
    slot->flow.status = FLOW_STATUS_RUNNING;
  }
  memset(&request, 0, sizeof(request));
  action = slot->provider->next_request(&slot->flow, &request);
  if (action == FLOW_PROVIDER_DONE) {
    slot->flow.status = FLOW_STATUS_SUCCESS;
    if (persist_success(engine, &slot->flow) != 0) {
      slot->finished = true;
      if (slot->flow.finish_fn != NULL) {
        slot->flow.finish_fn(&slot->flow, slot->flow.callback_data);
      }
      return -1;
    }
    slot->finished = true;
    if (slot->flow.finish_fn != NULL) {
      slot->flow.finish_fn(&slot->flow, slot->flow.callback_data);
    }
    return 0;
  }
  if (action == FLOW_PROVIDER_WAIT) return 0;
  if (action == FLOW_PROVIDER_FAILED) {
    if (slot->flow.error[0] == '\0') {
      flow_context_fail(&slot->flow, "provider failed");
    }
    slot->finished = true;
    if (slot->flow.finish_fn != NULL) {
      slot->flow.finish_fn(&slot->flow, slot->flow.callback_data);
    }
    return -1;
  }
  if (configure_request(slot, &request) != 0) {
    slot->finished = true;
    if (slot->flow.finish_fn != NULL) {
      slot->flow.finish_fn(&slot->flow, slot->flow.callback_data);
    }
    return -1;
  }
  if (curl_multi_add_handle(engine->multi, slot->easy) != CURLM_OK) {
    flow_context_fail(&slot->flow, "curl_multi_add_handle failed");
    slot->finished = true;
    if (slot->flow.finish_fn != NULL) {
      slot->flow.finish_fn(&slot->flow, slot->flow.callback_data);
    }
    return -1;
  }
  slot->in_multi = true;
  slot->flow.status = FLOW_STATUS_RUNNING;
  return 0;
}

int flow_engine_add(struct flow_engine *engine, const struct flow_provider *provider,
                    const struct flow_start_options *options,
                    struct flow_context **out_flow) {
  struct flow_slot *slot;
  if (out_flow != NULL) *out_flow = NULL;
  if (engine == NULL || provider == NULL) return -1;
  if (live_slot_count(engine) >= engine->max_concurrency) return -1;

  slot = (struct flow_slot *) calloc(1, sizeof(*slot));
  if (slot == NULL) return -1;
  slot->easy = curl_easy_init();
  if (slot->easy == NULL) {
    free(slot);
    return -1;
  }
  slot->provider = provider;
  slot->flow.status = FLOW_STATUS_PENDING;
  slot->flow.mode = options ? options->mode : FLOW_MODE_REGISTER_ONLY;
  slot->flow.persist_on_success = options ? options->persist_on_success : false;
  slot->flow.account_id = options ? options->account_id : 0;
  slot->flow.deadline_ms = options ? options->deadline_ms : 0;
  slot->flow.db = options ? options->db : NULL;
  slot->flow.log_fn = options ? options->log_fn : NULL;
  slot->flow.finish_fn = options ? options->finish_fn : NULL;
  slot->flow.cancel_fn = options ? options->cancel_fn : NULL;
  slot->flow.event_fn = options ? options->event_fn : NULL;
  slot->flow.callback_data = options ? options->callback_data : NULL;
  generate_flow_id(slot->flow.id, sizeof(slot->flow.id));
  if (options != NULL && options->proxy_url != NULL) {
    mg_snprintf(slot->flow.proxy_url, sizeof(slot->flow.proxy_url), "%s",
                options->proxy_url);
  }
  if (options != NULL && options->profile != NULL) {
    slot->flow.profile = *options->profile;
  }
  if (options != NULL && options->identity != NULL) {
    slot->flow.identity = *options->identity;
  }
  if (options != NULL && options->workspace_id != NULL) {
    mg_snprintf(slot->flow.workspace_id, sizeof(slot->flow.workspace_id), "%s",
                options->workspace_id);
  }
  if (provider->start != NULL && provider->start(&slot->flow) != 0) {
    free_slot(slot);
    return -1;
  }
  if (append_slot(engine, slot) != 0) {
    free_slot(slot);
    return -1;
  }
  if (out_flow != NULL) *out_flow = &slot->flow;
  return schedule_next(engine, slot);
}

static void schedule_waiting(struct flow_engine *engine) {
  for (size_t i = 0; i < engine->len; i++) {
    if (engine->slots[i] == NULL) continue;
    if (!engine->slots[i]->finished && !engine->slots[i]->in_multi) {
      (void) schedule_next(engine, engine->slots[i]);
    }
  }
}

static void complete_easy(struct flow_engine *engine, CURLMsg *msg) {
  struct flow_slot *slot = NULL;
  struct flow_http_response response;
  CURLcode result = msg->data.result;

  curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &slot);
  if (slot == NULL) return;
  curl_multi_remove_handle(engine->multi, msg->easy_handle);
  slot->in_multi = false;
  memset(&response, 0, sizeof(response));
  curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE,
                    &response.status_code);
  {
    char *effective_url = NULL;
    char *primary_ip = NULL;
    curl_easy_getinfo(msg->easy_handle, CURLINFO_EFFECTIVE_URL, &effective_url);
    curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIMARY_IP, &primary_ip);
    sanitized_url(effective_url ? effective_url : slot->request_url,
                  response.effective_url, sizeof(response.effective_url));
    mg_snprintf(response.primary_ip, sizeof(response.primary_ip), "%s",
                primary_ip ? primary_ip : "");
  }
  if (slot->location[0] != '\0') {
    mg_snprintf(response.location, sizeof(response.location), "%s",
                slot->location);
  }
  mg_snprintf(response.content_type, sizeof(response.content_type), "%s",
              slot->content_type);
  mg_snprintf(response.server, sizeof(response.server), "%s", slot->server);
  mg_snprintf(response.cf_mitigated, sizeof(response.cf_mitigated), "%s",
              slot->cf_mitigated);
  mg_snprintf(response.cf_ray, sizeof(response.cf_ray), "%s", slot->cf_ray);
  mg_snprintf(response.device_id, sizeof(response.device_id), "%s",
              slot->device_id);
  mg_snprintf(response.auth_session_cookie,
              sizeof(response.auth_session_cookie), "%s",
              slot->auth_session_cookie);
  mg_snprintf(response.auth_info_cookie, sizeof(response.auth_info_cookie),
              "%s", slot->auth_info_cookie);
  response.body = slot->response.data;
  response.body_len = slot->response.len;
  if (result != CURLE_OK) {
    mg_snprintf(response.error, sizeof(response.error), "%s",
                slot->flow.error[0] != '\0' ? slot->flow.error
                                            : curl_easy_strerror(result));
    flow_context_fail(&slot->flow, response.error);
    {
      char scheme[24];
      proxy_scheme_label(slot->flow.proxy_url, scheme, sizeof(scheme));
      flow_context_log(&slot->flow, "error",
                       "HTTP 失败: %s %s error=%s proxy=%s",
                       slot->request_method, slot->request_url, response.error,
                       scheme);
    }
    slot->finished = true;
    if (slot->flow.finish_fn != NULL) {
      slot->flow.finish_fn(&slot->flow, slot->flow.callback_data);
    }
    return;
  }
  flow_context_log(&slot->flow, "debug",
                   "HTTP 响应: %s %s -> %ld body=%zuB ip=%s server=%s cf=%s ray=%s location=%s",
                   slot->request_method, response.effective_url,
                   response.status_code, response.body_len,
                   response.primary_ip[0] ? response.primary_ip : "-",
                   response.server[0] ? response.server : "-",
                   response.cf_mitigated[0] ? response.cf_mitigated : "-",
                   response.cf_ray[0] ? response.cf_ray : "-",
                   response.location[0] ? response.location : "-");
  if (slot->provider != NULL && slot->provider->on_response != NULL &&
      slot->provider->on_response(&slot->flow, &response) != 0) {
    if (slot->flow.error[0] == '\0') {
      flow_context_fail(&slot->flow, "provider response handling failed");
    }
    slot->finished = true;
    if (slot->flow.finish_fn != NULL) {
      slot->flow.finish_fn(&slot->flow, slot->flow.callback_data);
    }
    return;
  }
  schedule_next(engine, slot);
}

int flow_engine_run_once(struct flow_engine *engine, long timeout_ms) {
  int running = 0;
  int msgs_left = 0;
  CURLMsg *msg;
  if (engine == NULL || engine->multi == NULL) return -1;

  schedule_waiting(engine);
  curl_multi_perform(engine->multi, &running);
  curl_multi_wait(engine->multi, NULL, 0, timeout_ms > 0 ? timeout_ms : 10,
                  NULL);
  schedule_waiting(engine);
  curl_multi_perform(engine->multi, &running);
  while ((msg = curl_multi_info_read(engine->multi, &msgs_left)) != NULL) {
    if (msg->msg == CURLMSG_DONE) complete_easy(engine, msg);
  }
  schedule_waiting(engine);
  return 0;
}

int flow_engine_run_until_idle(struct flow_engine *engine, long timeout_ms) {
  while (flow_engine_active_count(engine) > 0) {
    if (flow_engine_run_once(engine, timeout_ms) != 0) return -1;
  }
  return 0;
}

size_t flow_engine_active_count(const struct flow_engine *engine) {
  size_t active = 0;
  if (engine == NULL) return 0;
  for (size_t i = 0; i < engine->len; i++) {
    if (engine->slots[i] != NULL && !engine->slots[i]->finished) active++;
  }
  return active;
}

size_t flow_engine_total_count(const struct flow_engine *engine) {
  return engine == NULL ? 0 : engine->len;
}
