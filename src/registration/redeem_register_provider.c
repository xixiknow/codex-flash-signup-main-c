#include "registration/redeem_register_provider.h"

#include "redeem/redeem_client.h"
#include "mongoose.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_BASE_URL "https://chatgpt.com"
#define AUTH_BASE_URL "https://auth.openai.com"
#define OAUTH_SCOPE_CALLBACK "openid+email+profile+offline_access+model.request+model.read+organization.read+organization.write"

enum register_step {
  STEP_PROVIDERS = 0,
  STEP_CSRF,
  STEP_SIGNIN,
  STEP_AUTHORIZE,
  STEP_EMAIL_VERIFICATION,
  STEP_OTP_RESEND,
  STEP_OTP_VALIDATE,
  STEP_SESSION_DUMP,
  STEP_CREATE_ACCOUNT,
  STEP_CALLBACK,
  STEP_ME,
  STEP_ACCOUNT_CHECK,
  STEP_DONE
};

struct redeem_register_state {
  char device_id[48];
  char logging_id[48];
  char csrf[160];
  char authorize_url[FLOW_URL_LEN];
  char email_verification_url[FLOW_URL_LEN];
  char callback_url[FLOW_URL_LEN];
  char state[256];
  char otp_code[64];
  char body[1024];
  char url[FLOW_URL_LEN];
  int otp_attempts;
};

static const struct flow_http_header s_ofoco_api_headers[] = {
    {"Accept", "*/*"},
    {"Content-Type", "application/json"},
    {"Referer", APP_BASE_URL "/"},
    {"Sec-Fetch-Dest", "empty"},
    {"Sec-Fetch-Mode", "cors"},
    {"Sec-Fetch-Site", "same-origin"},
};

static const struct flow_http_header s_signin_headers[] = {
    {"Accept", "*/*"},
    {"Content-Type", "application/x-www-form-urlencoded"},
    {"Origin", APP_BASE_URL},
    {"Referer", APP_BASE_URL "/"},
    {"Sec-Fetch-Dest", "empty"},
    {"Sec-Fetch-Mode", "cors"},
    {"Sec-Fetch-Site", "same-origin"},
};

static const struct flow_http_header s_auth_nav_headers[] = {
    {"Accept",
     "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"
     "image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7"},
    {"Referer", APP_BASE_URL "/"},
    {"Sec-Fetch-Dest", "document"},
    {"Sec-Fetch-Mode", "navigate"},
    {"Sec-Fetch-Site", "cross-site"},
    {"Sec-Fetch-User", "?1"},
    {"Upgrade-Insecure-Requests", "1"},
};

static const struct flow_http_header s_auth_json_headers[] = {
    {"Accept", "application/json"},
    {"Content-Type", "application/json"},
    {"Origin", AUTH_BASE_URL},
    {"Referer", AUTH_BASE_URL "/email-verification"},
    {"Sec-Fetch-Dest", "empty"},
    {"Sec-Fetch-Mode", "cors"},
    {"Sec-Fetch-Site", "same-origin"},
};

static const struct flow_http_header s_auth_resend_headers[] = {
    {"Accept", "*/*"},
    {"Origin", AUTH_BASE_URL},
    {"Referer", AUTH_BASE_URL "/email-verification"},
    {"Sec-Fetch-Dest", "empty"},
    {"Sec-Fetch-Mode", "cors"},
    {"Sec-Fetch-Site", "same-origin"},
};

static const struct flow_http_header s_create_account_headers[] = {
    {"Accept", "application/json"},
    {"Content-Type", "application/json"},
    {"Origin", AUTH_BASE_URL},
    {"Referer", AUTH_BASE_URL "/about-you"},
    {"Sec-Fetch-Dest", "empty"},
    {"Sec-Fetch-Mode", "cors"},
    {"Sec-Fetch-Site", "same-origin"},
};

static const struct flow_http_header s_callback_headers[] = {
    {"Accept",
     "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"
     "image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7"},
    {"Referer", AUTH_BASE_URL "/"},
    {"Sec-Fetch-Dest", "document"},
    {"Sec-Fetch-Mode", "navigate"},
    {"Sec-Fetch-Site", "cross-site"},
    {"Sec-Fetch-User", "?1"},
    {"Upgrade-Insecure-Requests", "1"},
};

static const struct flow_http_header s_me_headers[] = {
    {"Accept", "*/*"},
    {"Referer", APP_BASE_URL "/"},
    {"Sec-Fetch-Dest", "empty"},
    {"Sec-Fetch-Mode", "cors"},
    {"Sec-Fetch-Site", "same-origin"},
};

static void random_uuid_like(char *out, size_t out_len) {
  uint8_t bytes[16];
  if (out_len == 0) return;
  if (!mg_random(bytes, sizeof(bytes))) {
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t) rand();
  }
  bytes[6] = (uint8_t) ((bytes[6] & 0x0f) | 0x40);
  bytes[8] = (uint8_t) ((bytes[8] & 0x3f) | 0x80);
  mg_snprintf(out, out_len,
              "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
              "%02x%02x%02x%02x%02x%02x",
              bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
              bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
              bytes[12], bytes[13], bytes[14], bytes[15]);
}

static bool copy_json_path(const char *json, const char *path, char *out,
                           size_t out_len) {
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

static void proxy_scheme_label(const char *proxy_url, char *out, size_t out_len) {
  const char *p;
  size_t len;
  if (out_len == 0) return;
  out[0] = '\0';
  if (proxy_url == NULL || proxy_url[0] == '\0') return;
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

static bool copy_json_string_key(const char *json, const char *key, char *out,
                                 size_t out_len) {
  const char *p, *value, *end;
  char needle[128];

  if (json == NULL || key == NULL || out == NULL || out_len == 0) return false;
  mg_snprintf(needle, sizeof(needle), "\"%s\"", key);
  p = strstr(json, needle);
  if (p == NULL) return false;
  p += strlen(needle);
  while (*p && isspace((unsigned char) *p)) p++;
  if (*p != ':') return false;
  p++;
  while (*p && isspace((unsigned char) *p)) p++;
  if (*p != '"') return false;
  value = ++p;
  while (*p && (*p != '"' || p[-1] == '\\')) p++;
  end = p;
  if (end <= value) return false;
  mg_snprintf(out, out_len, "%.*s", (int) (end - value), value);
  return true;
}

static bool copy_first_url(const char *body, char *out, size_t out_len,
                           const char *must_contain) {
  const char *p = body;
  if (body == NULL || out == NULL || out_len == 0) return false;
  while ((p = strstr(p, "https://")) != NULL) {
    const char *end = p;
    size_t len;
    while (*end && *end != '"' && *end != '\'' && !isspace((unsigned char) *end) &&
           *end != '<' && *end != '\\') {
      end++;
    }
    len = (size_t) (end - p);
    if (must_contain == NULL || must_contain[0] == '\0' ||
        (len > 0 && strstr(p, must_contain) != NULL)) {
      if (len >= out_len) len = out_len - 1;
      memcpy(out, p, len);
      out[len] = '\0';
      return true;
    }
    p = end;
  }
  return false;
}

static bool copy_any_redirect_url(const char *body, char *out, size_t out_len,
                                  const char *must_contain) {
  return copy_json_path(body, "$.url", out, out_len) ||
         copy_json_path(body, "$.redirectUrl", out, out_len) ||
         copy_json_path(body, "$.redirect_url", out, out_len) ||
         copy_json_path(body, "$.callbackUrl", out, out_len) ||
         copy_json_path(body, "$.authorizationUrl", out, out_len) ||
         copy_json_path(body, "$.authorization_url", out, out_len) ||
         copy_first_url(body, out, out_len, must_contain);
}

static void query_value_from_url(const char *url, const char *name, char *out,
                                 size_t out_len) {
  const char *q;
  char buf[512];
  if (out_len == 0) return;
  out[0] = '\0';
  if (url == NULL || name == NULL) return;
  q = strchr(url, '?');
  if (q == NULL) return;
  struct mg_str query = mg_str_n((char *) q + 1, strlen(q + 1));
  if (mg_http_get_var(&query, name, buf, sizeof(buf)) > 0) {
    mg_snprintf(out, out_len, "%s", buf);
  }
}

static void request_get(struct flow_http_request *request, const char *url,
                        const struct flow_http_header *headers,
                        size_t num_headers) {
  request->method = "GET";
  request->url = url;
  request->timeout_ms = 30000;
  request->follow_location = false;
  request->headers = headers;
  request->num_headers = num_headers;
}

static void request_post(struct flow_http_request *request, const char *url,
                         const char *body,
                         const struct flow_http_header *headers,
                         size_t num_headers) {
  request->method = "POST";
  request->url = url;
  request->body = body;
  request->body_len = body == NULL ? 0 : strlen(body);
  request->timeout_ms = 30000;
  request->follow_location = false;
  request->headers = headers;
  request->num_headers = num_headers;
}

static void request_post_no_body(struct flow_http_request *request, const char *url,
                                 const struct flow_http_header *headers,
                                 size_t num_headers) {
  request->method = "POST";
  request->url = url;
  request->body = NULL;
  request->body_len = 0;
  request->timeout_ms = 30000;
  request->follow_location = false;
  request->headers = headers;
  request->num_headers = num_headers;
}

static const char *step_label(enum register_step step) {
  switch (step) {
    case STEP_PROVIDERS: return "auth providers";
    case STEP_CSRF: return "csrf";
    case STEP_SIGNIN: return "signin";
    case STEP_AUTHORIZE: return "authorize";
    case STEP_EMAIL_VERIFICATION: return "email verification";
    case STEP_OTP_RESEND: return "email otp resend";
    case STEP_OTP_VALIDATE: return "email otp";
    case STEP_SESSION_DUMP: return "session dump";
    case STEP_CREATE_ACCOUNT: return "create account";
    case STEP_CALLBACK: return "callback";
    case STEP_ME: return "backend me";
    case STEP_ACCOUNT_CHECK: return "account check";
    case STEP_DONE: return "done";
    default: return "unknown";
  }
}

static void response_summary(const struct flow_http_response *response,
                             char *out, size_t out_len) {
  if (out_len == 0) return;
  out[0] = '\0';
  if (response == NULL) return;
  mg_snprintf(out, out_len,
              "HTTP %ld url=%s body=%luB ip=%s ct=%s server=%s location=%s",
              response->status_code,
              response->effective_url[0] ? response->effective_url : "-",
              (unsigned long) response->body_len,
              response->primary_ip[0] ? response->primary_ip : "-",
              response->content_type[0] ? response->content_type : "-",
              response->server[0] ? response->server : "-",
              response->location[0] ? response->location : "-");
}

static void body_preview(const struct flow_http_response *response, char *out,
                         size_t out_len) {
  size_t i, n = 0;
  if (out_len == 0) return;
  out[0] = '\0';
  if (response == NULL || response->body == NULL || response->body_len == 0) return;
  for (i = 0; i < response->body_len && n + 1 < out_len; i++) {
    unsigned char ch = (unsigned char) response->body[i];
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      if (n > 0 && out[n - 1] != ' ') out[n++] = ' ';
    } else if (isprint(ch)) {
      out[n++] = (char) ch;
    } else if (n > 0 && out[n - 1] != ' ') {
      out[n++] = ' ';
    }
    if (n >= 160) break;
  }
  while (n > 0 && out[n - 1] == ' ') n--;
  out[n] = '\0';
}

static int provider_start(struct flow_context *flow) {
  struct redeem_register_state *state;

  if (flow->redeem_code[0] == '\0') {
    flow_context_fail(flow, "缺少兑换码，无法执行兑换注册");
    return -1;
  }
  state = (struct redeem_register_state *) calloc(1, sizeof(*state));
  if (state == NULL) return -1;
  random_uuid_like(state->device_id, sizeof(state->device_id));
  random_uuid_like(state->logging_id, sizeof(state->logging_id));
  flow->provider_data = state;
  flow->step = STEP_PROVIDERS;
  if (flow->deadline_ms <= 0) flow->deadline_ms = (long) mg_millis() + 180000;
  flow_context_log(flow, "info", "准备兑换码注册邮箱 %s", flow->identity.email);
  if (flow->proxy_url[0] != '\0') {
    char scheme[24];
    proxy_scheme_label(flow->proxy_url, scheme, sizeof(scheme));
    flow_context_log(flow, "info", "流程代理已锚定: %s",
                     scheme[0] ? scheme : "proxy");
  } else {
    flow_context_log(flow, "info", "未分配代理，当前流程使用直连");
  }
  return 0;
}

static enum flow_provider_action next_otp_request(struct flow_context *flow,
                                                  struct flow_http_request *request) {
  struct redeem_register_state *state =
      (struct redeem_register_state *) flow->provider_data;
  char error[256] = "";
  int rc;

  if ((long) mg_millis() > flow->deadline_ms) {
    flow_context_fail(flow, "等待邮箱验证码超时");
    return FLOW_PROVIDER_FAILED;
  }
  if ((long) mg_millis() < flow->next_retry_ms) return FLOW_PROVIDER_WAIT;

  state->otp_attempts++;
  rc = redeem_lookup_code(flow->db, flow->redeem_code, state->otp_code,
                          sizeof(state->otp_code), error, sizeof(error));
  if (rc == 0) {
    flow_context_log(flow, "debug", "等待兑换验证码，第 %d 次轮询",
                     state->otp_attempts);
    flow->next_retry_ms = (long) mg_millis() + 2500;
    return FLOW_PROVIDER_WAIT;
  }
  if (rc < 0) {
    flow_context_log(flow, "warn", "读取兑换验证码失败: %s", error);
    flow->next_retry_ms = (long) mg_millis() + 3500;
    return FLOW_PROVIDER_WAIT;
  }

  mg_snprintf(state->body, sizeof(state->body), "{\"code\":\"%s\"}",
              state->otp_code);
  request_post(request, AUTH_BASE_URL "/api/accounts/email-otp/validate",
               state->body, s_auth_json_headers,
               sizeof(s_auth_json_headers) / sizeof(s_auth_json_headers[0]));
  flow_context_log(flow, "info", "兑换验证码已获取，提交邮箱验证");
  return FLOW_PROVIDER_REQUEST;
}

static enum flow_provider_action provider_next(struct flow_context *flow,
                                               struct flow_http_request *request) {
  struct redeem_register_state *state =
      (struct redeem_register_state *) flow->provider_data;
  char email_enc[512];
  char csrf_enc[256];

  if (state == NULL || request == NULL) {
    flow_context_fail(flow, "注册 provider 状态丢失");
    return FLOW_PROVIDER_FAILED;
  }

  switch (flow->step) {
    case STEP_PROVIDERS:
      request_get(request, APP_BASE_URL "/api/auth/providers",
                  s_ofoco_api_headers,
                  sizeof(s_ofoco_api_headers) / sizeof(s_ofoco_api_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 请求 %s", step_label(flow->step),
                       request->url);
      return FLOW_PROVIDER_REQUEST;
    case STEP_CSRF:
      request_get(request, APP_BASE_URL "/api/auth/csrf", s_ofoco_api_headers,
                  sizeof(s_ofoco_api_headers) / sizeof(s_ofoco_api_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 请求 %s", step_label(flow->step),
                       request->url);
      return FLOW_PROVIDER_REQUEST;
    case STEP_SIGNIN:
      if (mg_url_encode(flow->identity.email, strlen(flow->identity.email),
                        email_enc, sizeof(email_enc)) >= sizeof(email_enc) ||
          mg_url_encode(state->csrf, strlen(state->csrf), csrf_enc,
                        sizeof(csrf_enc)) >= sizeof(csrf_enc)) {
        flow_context_fail(flow, "注册邮箱或 CSRF 编码失败");
        return FLOW_PROVIDER_FAILED;
      }
      mg_snprintf(state->url, sizeof(state->url),
                  APP_BASE_URL
                  "/api/auth/signin/openai?prompt=login&ext-oai-did=%s&"
                  "auth_session_logging_id=%s&"
                  "ext-passkey-client-capabilities=1111&"
                  "screen_hint=login_or_signup&login_hint=%s",
                  state->device_id, state->logging_id, email_enc);
      mg_snprintf(state->body, sizeof(state->body),
                  "callbackUrl=https%%3A%%2F%%2Fchatgpt.com%%2F&csrfToken=%s&"
                  "json=true",
                  csrf_enc);
      request_post(request, state->url, state->body, s_signin_headers,
                   sizeof(s_signin_headers) / sizeof(s_signin_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 提交注册入口 %s", step_label(flow->step),
                       flow->identity.email);
      return FLOW_PROVIDER_REQUEST;
    case STEP_AUTHORIZE:
      request_get(request, state->authorize_url, s_auth_nav_headers,
                  sizeof(s_auth_nav_headers) / sizeof(s_auth_nav_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 打开授权入口 %s",
                       step_label(flow->step), request->url);
      return FLOW_PROVIDER_REQUEST;
    case STEP_EMAIL_VERIFICATION:
      if (state->email_verification_url[0] == '\0') {
        mg_snprintf(state->email_verification_url,
                    sizeof(state->email_verification_url), "%s",
                    AUTH_BASE_URL "/email-verification");
      }
      request_get(request, state->email_verification_url, s_auth_nav_headers,
                  sizeof(s_auth_nav_headers) / sizeof(s_auth_nav_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 打开邮箱验证页面 %s",
                       step_label(flow->step), request->url);
      return FLOW_PROVIDER_REQUEST;
    case STEP_OTP_RESEND:
      request_post_no_body(
          request, AUTH_BASE_URL "/api/accounts/email-otp/resend",
          s_auth_resend_headers,
          sizeof(s_auth_resend_headers) / sizeof(s_auth_resend_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 触发验证码邮件重发",
                       step_label(flow->step));
      return FLOW_PROVIDER_REQUEST;
    case STEP_OTP_VALIDATE:
      return next_otp_request(flow, request);
    case STEP_SESSION_DUMP:
      request_get(request, AUTH_BASE_URL "/api/accounts/client_auth_session_dump",
                  s_auth_json_headers,
                  sizeof(s_auth_json_headers) / sizeof(s_auth_json_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 读取 client auth session",
                       step_label(flow->step));
      return FLOW_PROVIDER_REQUEST;
    case STEP_CREATE_ACCOUNT:
      mg_snprintf(state->body, sizeof(state->body),
                  "{\"name\":\"%s\",\"birthdate\":\"%s\"}",
                  flow->identity.full_name, flow->identity.birthdate);
      request_post(request, AUTH_BASE_URL "/api/accounts/create_account",
                   state->body, s_create_account_headers,
                   sizeof(s_create_account_headers) /
                       sizeof(s_create_account_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 创建账号资料 %s / %s",
                       step_label(flow->step),
                       flow->identity.full_name, flow->identity.birthdate);
      return FLOW_PROVIDER_REQUEST;
    case STEP_CALLBACK:
      request_get(request, state->callback_url, s_callback_headers,
                  sizeof(s_callback_headers) / sizeof(s_callback_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 回调站点 %s",
                       step_label(flow->step), request->url);
      return FLOW_PROVIDER_REQUEST;
    case STEP_ME:
      request_get(request, APP_BASE_URL "/backend-api/me", s_me_headers,
                  sizeof(s_me_headers) / sizeof(s_me_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 读取用户信息", step_label(flow->step));
      return FLOW_PROVIDER_REQUEST;
    case STEP_ACCOUNT_CHECK:
      request_get(request,
                  APP_BASE_URL
                  "/backend-api/accounts/check/v4-2023-04-27?"
                  "timezone_offset_min=-480",
                  s_me_headers, sizeof(s_me_headers) / sizeof(s_me_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 读取账号检查信息",
                       step_label(flow->step));
      return FLOW_PROVIDER_REQUEST;
    case STEP_DONE:
      return FLOW_PROVIDER_DONE;
    default:
      flow_context_fail(flow, "未知注册步骤");
      return FLOW_PROVIDER_FAILED;
  }
}

static bool expect_success(struct flow_context *flow,
                           const struct flow_http_response *response,
                           const char *label) {
  if (response->status_code >= 200 && response->status_code < 400) return true;
  if (flow_response_is_edge_block(response)) {
    flow_context_mark_environment_retry(flow,
                                        "边缘风控拦截，需要重新分配环境重试");
  } else {
    flow_context_fail(flow, "注册请求返回非成功状态");
  }
  {
    char summary[FLOW_LOG_LEN];
    char preview[FLOW_LOG_LEN];
    response_summary(response, summary, sizeof(summary));
    body_preview(response, preview, sizeof(preview));
    flow_context_log(flow, "error", "%s 失败: %s", label, summary);
    if (flow->environment_retryable) {
      flow_context_log(flow, "warn", "%s 触发边缘风控，当前环境将被丢弃并重试",
                       label);
    }
    if (preview[0] != '\0') {
      flow_context_log(flow, "warn", "%s 响应片段: %s", label, preview);
    }
  }
  return false;
}

static int provider_response(struct flow_context *flow,
                             const struct flow_http_response *response) {
  struct redeem_register_state *state =
      (struct redeem_register_state *) flow->provider_data;

  if (state == NULL || response == NULL) {
    flow_context_fail(flow, "注册响应状态丢失");
    return -1;
  }

  switch (flow->step) {
    case STEP_PROVIDERS:
      if (!expect_success(flow, response, "auth providers")) return -1;
      flow_context_log(flow, "info", "步骤 %s 完成", step_label(flow->step));
      flow->step = STEP_CSRF;
      return 0;
    case STEP_CSRF:
      if (!expect_success(flow, response, "csrf")) return -1;
      if (!copy_json_path(response->body, "$.csrfToken", state->csrf,
                          sizeof(state->csrf))) {
        flow_context_fail(flow, "CSRF token 解析失败");
        return -1;
      }
      flow_context_log(flow, "info", "步骤 %s 完成，csrf=%s",
                       step_label(flow->step), state->csrf);
      flow->step = STEP_SIGNIN;
      return 0;
    case STEP_SIGNIN:
      if (!expect_success(flow, response, "signin")) return -1;
      if (response->location[0] != '\0') {
        mg_snprintf(state->authorize_url, sizeof(state->authorize_url), "%s",
                    response->location);
      } else if (!copy_any_redirect_url(response->body, state->authorize_url,
                                        sizeof(state->authorize_url),
                                        "/api/accounts/authorize")) {
        flow_context_fail(flow, "signin 响应中未找到 authorize URL");
        return -1;
      }
      query_value_from_url(state->authorize_url, "state", state->state,
                           sizeof(state->state));
      flow_context_log(flow, "info", "步骤 %s 完成，authorize=%s state=%s",
                       step_label(flow->step), state->authorize_url,
                       state->state[0] ? state->state : "-");
      flow->step = STEP_AUTHORIZE;
      return 0;
    case STEP_AUTHORIZE:
      if (!expect_success(flow, response, "authorize")) return -1;
      if (response->location[0] != '\0') {
        mg_snprintf(state->email_verification_url,
                    sizeof(state->email_verification_url), "%s",
                    response->location);
      } else {
        mg_snprintf(state->email_verification_url,
                    sizeof(state->email_verification_url), "%s",
                    AUTH_BASE_URL "/email-verification");
      }
      flow_context_log(flow, "info", "步骤 %s 完成，email_verification=%s",
                       step_label(flow->step), state->email_verification_url);
      flow->step = STEP_EMAIL_VERIFICATION;
      return 0;
    case STEP_EMAIL_VERIFICATION:
      if (!expect_success(flow, response, "email verification")) return -1;
      flow_context_log(flow, "info", "步骤 %s 完成，已触发邮箱验证页面",
                       step_label(flow->step));
      flow->step = STEP_OTP_RESEND;
      return 0;
    case STEP_OTP_RESEND:
      if (!expect_success(flow, response, "email otp resend")) return -1;
      flow_context_log(flow, "info", "步骤 %s 完成，已请求验证码重发",
                       step_label(flow->step));
      flow->step = STEP_OTP_VALIDATE;
      flow->next_retry_ms = 0;
      flow_context_emit_event(flow, FLOW_EVENT_EMAIL_OTP_WAITING);
      return 0;
    case STEP_OTP_VALIDATE:
      if (!expect_success(flow, response, "email otp")) return -1;
      flow_context_emit_event(flow, FLOW_EVENT_EMAIL_OTP_VALIDATED);
      flow_context_log(flow, "info", "步骤 %s 完成，验证码已验证", step_label(flow->step));
      flow->step = STEP_SESSION_DUMP;
      return 0;
    case STEP_SESSION_DUMP:
      if (!expect_success(flow, response, "session dump")) return -1;
      flow_context_log(flow, "info", "步骤 %s 完成", step_label(flow->step));
      flow->step = STEP_CREATE_ACCOUNT;
      return 0;
    case STEP_CREATE_ACCOUNT:
      if (!expect_success(flow, response, "create account")) return -1;
      if (response->location[0] != '\0') {
        mg_snprintf(state->callback_url, sizeof(state->callback_url), "%s",
                    response->location);
      } else if (!copy_any_redirect_url(response->body, state->callback_url,
                                        sizeof(state->callback_url),
                                        "/api/auth/callback/openai")) {
        char code[256] = "";
        if (copy_json_path(response->body, "$.code", code, sizeof(code)) &&
            state->state[0] != '\0') {
          mg_snprintf(state->callback_url, sizeof(state->callback_url),
                      APP_BASE_URL
                      "/api/auth/callback/openai?code=%s&scope=%s&state=%s",
                      code, OAUTH_SCOPE_CALLBACK, state->state);
        }
      }
      if (state->callback_url[0] == '\0') {
        flow_context_fail(flow, "create_account 响应中未找到 callback URL");
        return -1;
      }
      flow_context_log(flow, "info", "步骤 %s 完成，callback=%s",
                       step_label(flow->step), state->callback_url);
      flow->step = STEP_CALLBACK;
      return 0;
    case STEP_CALLBACK:
      if (!expect_success(flow, response, "callback")) return -1;
      flow_context_log(flow, "info", "步骤 %s 完成", step_label(flow->step));
      flow->step = STEP_ME;
      return 0;
    case STEP_ME:
      if (!expect_success(flow, response, "me")) return -1;
      copy_json_path(response->body, "$.orgs.data[0].id", flow->workspace_id,
                     sizeof(flow->workspace_id));
      if (flow->workspace_id[0] == '\0') {
        copy_json_string_key(response->body, "organization_id",
                             flow->workspace_id, sizeof(flow->workspace_id));
      }
      flow_context_log(flow, "info", "步骤 %s 完成，workspace=%s",
                       step_label(flow->step),
                       flow->workspace_id[0] ? flow->workspace_id : "-");
      flow->step = STEP_ACCOUNT_CHECK;
      return 0;
    case STEP_ACCOUNT_CHECK:
      if (!expect_success(flow, response, "account check")) return -1;
      copy_json_string_key(response->body, "account_id",
                           flow->external_account_id,
                           sizeof(flow->external_account_id));
      if (flow->workspace_id[0] == '\0') {
        copy_json_string_key(response->body, "organization_id",
                             flow->workspace_id, sizeof(flow->workspace_id));
      }
      flow_context_log(flow, "info", "兑换注册成功，Account ID=%s Workspace ID=%s",
                       flow->external_account_id[0] ? flow->external_account_id
                                                    : "-",
                       flow->workspace_id[0] ? flow->workspace_id : "-");
      flow->step = STEP_DONE;
      return 0;
    default:
      flow_context_fail(flow, "收到未知步骤响应");
      return -1;
  }
}

static void provider_cleanup(struct flow_context *flow) {
  if (flow == NULL) return;
  free(flow->provider_data);
  flow->provider_data = NULL;
}

static const struct flow_provider s_provider = {
    "redeem-register-har",
    provider_start,
    provider_next,
    provider_response,
    provider_cleanup,
};

const struct flow_provider *redeem_register_provider(void) {
  return &s_provider;
}
