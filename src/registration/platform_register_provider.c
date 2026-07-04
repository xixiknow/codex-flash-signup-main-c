#include "registration/platform_register_provider.h"

#include "mail/rapid_inbox.h"
#include "mongoose.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PLATFORM_BASE_URL "https://platform.openai.com"
#define AUTH_BASE_URL "https://auth.openai.com"
#define OAUTH_CLIENT_ID "app_2SKx67EdpoN0G6j64rFvigXD"
#define OAUTH_AUDIENCE "https%3A%2F%2Fapi.openai.com%2Fv1"
#define OAUTH_REDIRECT_URI "https%3A%2F%2Fplatform.openai.com%2Fauth%2Fcallback"
#define OAUTH_SCOPE "openid%20profile%20email%20offline_access"
#define OTP_INITIAL_WAIT_BEFORE_RESEND_MS 10000L
#define OTP_RESEND_WAIT_TIMEOUT_MS 30000L

enum platform_step {
  STEP_PLATFORM_LOGIN = 0,
  STEP_AUTHORIZE,
  STEP_PASSWORD_PAGE,
  STEP_USER_REGISTER,
  STEP_EMAIL_OTP_SEND,
  STEP_EMAIL_VERIFICATION,
  STEP_OTP_RESEND,
  STEP_OTP_VALIDATE,
  STEP_SESSION_DUMP,
  STEP_CREATE_ACCOUNT,
  STEP_DONE
};

struct platform_register_state {
  char device_id[48];
  char state[128];
  char nonce[128];
  char code_challenge[96];
  char password_page_url[FLOW_URL_LEN];
  char email_page_url[FLOW_URL_LEN];
  char otp_code[64];
  char body[1024];
  char url[FLOW_URL_LEN];
  int otp_attempts;
  long otp_sent_after_epoch;
  long otp_send_requested_ms;
  long otp_resend_after_epoch;
  long otp_resend_requested_ms;
};

static const struct flow_http_header s_platform_nav_headers[] = {
    {"Accept",
     "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"
     "image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7"},
    {"Sec-Fetch-Dest", "document"},
    {"Sec-Fetch-Mode", "navigate"},
    {"Sec-Fetch-Site", "none"},
    {"Sec-Fetch-User", "?1"},
    {"Upgrade-Insecure-Requests", "1"},
};

static const struct flow_http_header s_authorize_headers[] = {
    {"Accept",
     "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"
     "image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7"},
    {"Referer", PLATFORM_BASE_URL "/"},
    {"Sec-Fetch-Dest", "document"},
    {"Sec-Fetch-Mode", "navigate"},
    {"Sec-Fetch-Site", "same-site"},
    {"Sec-Fetch-User", "?1"},
    {"Upgrade-Insecure-Requests", "1"},
};

static const struct flow_http_header s_password_page_headers[] = {
    {"Accept",
     "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"
     "image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7"},
    {"Referer", PLATFORM_BASE_URL "/"},
    {"Sec-Fetch-Dest", "document"},
    {"Sec-Fetch-Mode", "navigate"},
    {"Sec-Fetch-Site", "same-site"},
    {"Sec-Fetch-User", "?1"},
    {"Upgrade-Insecure-Requests", "1"},
};

static const struct flow_http_header s_register_headers[] = {
    {"Accept", "application/json"},
    {"Content-Type", "application/json"},
    {"Origin", AUTH_BASE_URL},
    {"Referer", AUTH_BASE_URL "/create-account/password"},
    {"Sec-Fetch-Dest", "empty"},
    {"Sec-Fetch-Mode", "cors"},
    {"Sec-Fetch-Site", "same-origin"},
};

static const struct flow_http_header s_send_headers[] = {
    {"Accept",
     "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"
     "image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7"},
    {"Referer", AUTH_BASE_URL "/create-account/password"},
    {"Sec-Fetch-Dest", "document"},
    {"Sec-Fetch-Mode", "navigate"},
    {"Sec-Fetch-Site", "same-origin"},
    {"Sec-Fetch-User", "?1"},
    {"Upgrade-Insecure-Requests", "1"},
};

static const struct flow_http_header s_email_page_headers[] = {
    {"Accept",
     "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"
     "image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7"},
    {"Referer", AUTH_BASE_URL "/create-account/password"},
    {"Sec-Fetch-Dest", "document"},
    {"Sec-Fetch-Mode", "navigate"},
    {"Sec-Fetch-Site", "same-origin"},
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

static const struct flow_http_header s_resend_headers[] = {
    {"Accept", "*/*"},
    {"Origin", AUTH_BASE_URL},
    {"Referer", AUTH_BASE_URL "/email-verification"},
    {"Sec-Fetch-Dest", "empty"},
    {"Sec-Fetch-Mode", "cors"},
    {"Sec-Fetch-Site", "same-origin"},
};

static const struct flow_http_header s_session_headers[] = {
    {"Accept", "application/json"},
    {"Referer", AUTH_BASE_URL "/email-verification"},
    {"Sec-Fetch-Dest", "empty"},
    {"Sec-Fetch-Mode", "cors"},
    {"Sec-Fetch-Site", "same-origin"},
};

static const struct flow_http_header s_create_headers[] = {
    {"Accept", "application/json"},
    {"Content-Type", "application/json"},
    {"Origin", AUTH_BASE_URL},
    {"Referer", AUTH_BASE_URL "/about-you"},
    {"Sec-Fetch-Dest", "empty"},
    {"Sec-Fetch-Mode", "cors"},
    {"Sec-Fetch-Site", "same-origin"},
};

static uint64_t random_u64(void) {
  uint64_t value = 0;
  if (!mg_random(&value, sizeof(value))) {
    value = ((uint64_t) rand() << 32) ^ (uint64_t) rand();
  }
  return value;
}

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

static void random_urlsafe(char *out, size_t out_len, size_t chars) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t n = 0;
  if (out_len == 0) return;
  while (n < chars && n + 1 < out_len) {
    out[n++] = alphabet[random_u64() % (sizeof(alphabet) - 1)];
  }
  out[n] = '\0';
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

static const char *step_label(enum platform_step step) {
  switch (step) {
    case STEP_PLATFORM_LOGIN: return "platform login";
    case STEP_AUTHORIZE: return "platform authorize";
    case STEP_PASSWORD_PAGE: return "password page";
    case STEP_USER_REGISTER: return "user register";
    case STEP_EMAIL_OTP_SEND: return "email otp send";
    case STEP_EMAIL_VERIFICATION: return "email verification";
    case STEP_OTP_RESEND: return "email otp resend";
    case STEP_OTP_VALIDATE: return "email otp validate";
    case STEP_SESSION_DUMP: return "session dump";
    case STEP_CREATE_ACCOUNT: return "create account";
    case STEP_DONE: return "done";
    default: return "unknown";
  }
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
    if (n >= 220) break;
  }
  while (n > 0 && out[n - 1] == ' ') n--;
  out[n] = '\0';
}

static bool expect_success(struct flow_context *flow,
                           const struct flow_http_response *response,
                           const char *label) {
  if (response->status_code >= 200 && response->status_code < 400) return true;
  if (flow_response_is_edge_block(response)) {
    flow_context_mark_environment_retry(flow,
                                        "边缘风控拦截，需要重新分配环境重试");
  } else {
    flow_context_fail(flow, "Platform 注册请求返回非成功状态");
  }
  {
    char preview[FLOW_LOG_LEN];
    body_preview(response, preview, sizeof(preview));
    flow_context_log(flow, "error",
                     "%s 失败: HTTP %ld body=%luB ip=%s server=%s cf=%s ray=%s location=%s",
                     label, response->status_code,
                     (unsigned long) response->body_len,
                     response->primary_ip[0] ? response->primary_ip : "-",
                     response->server[0] ? response->server : "-",
                     response->cf_mitigated[0] ? response->cf_mitigated : "-",
                     response->cf_ray[0] ? response->cf_ray : "-",
                     response->location[0] ? response->location : "-");
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

static int provider_start(struct flow_context *flow) {
  struct platform_register_state *state;

  state = (struct platform_register_state *) calloc(1, sizeof(*state));
  if (state == NULL) return -1;
  random_uuid_like(state->device_id, sizeof(state->device_id));
  random_urlsafe(state->state, sizeof(state->state), 48);
  random_urlsafe(state->nonce, sizeof(state->nonce), 48);
  random_urlsafe(state->code_challenge, sizeof(state->code_challenge), 43);
  flow->provider_data = state;
  flow->step = STEP_PLATFORM_LOGIN;
  if (flow->deadline_ms <= 0) flow->deadline_ms = (long) mg_millis() + 180000;
  mg_snprintf(flow->success_account_status, sizeof(flow->success_account_status),
              "expired");
  flow_context_log(flow, "info", "Platform 注册路径准备邮箱 %s",
                   flow->identity.email);
  flow_context_log(flow, "info", "成功结果将作为过期账号入库，不写入 Access/Refresh Token");
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

static void schedule_otp_poll(struct flow_context *flow,
                              struct platform_register_state *state,
                              long now, long interval_ms) {
  long next = now + interval_ms;
  long cap = 0;
  if (state->otp_resend_requested_ms > 0) {
    cap = state->otp_resend_requested_ms + OTP_RESEND_WAIT_TIMEOUT_MS;
  } else if (state->otp_send_requested_ms > 0) {
    cap = state->otp_send_requested_ms + OTP_INITIAL_WAIT_BEFORE_RESEND_MS;
  }
  if (cap > 0 && next > cap) next = cap;
  flow->next_retry_ms = next;
}

static enum flow_provider_action request_otp_resend(
    struct flow_context *flow, struct flow_http_request *request,
    struct platform_register_state *state, long now) {
  state->otp_resend_requested_ms = now;
  state->otp_resend_after_epoch = (long) time(NULL);
  state->otp_attempts = 0;
  state->otp_code[0] = '\0';
  request_post(request, AUTH_BASE_URL "/api/accounts/email-otp/resend", NULL,
               s_resend_headers,
               sizeof(s_resend_headers) / sizeof(s_resend_headers[0]));
  flow->step = STEP_OTP_RESEND;
  flow_context_log(flow, "warn",
                   "10 秒内未收到邮箱验证码，马上触发验证码重发");
  return FLOW_PROVIDER_REQUEST;
}

static bool resend_wait_timed_out(struct platform_register_state *state,
                                  long now) {
  return state->otp_resend_requested_ms > 0 &&
         now - state->otp_resend_requested_ms >= OTP_RESEND_WAIT_TIMEOUT_MS;
}

static bool should_resend_otp(struct platform_register_state *state, long now) {
  return state->otp_resend_requested_ms <= 0 &&
         state->otp_send_requested_ms > 0 &&
         now - state->otp_send_requested_ms >=
             OTP_INITIAL_WAIT_BEFORE_RESEND_MS;
}

static enum flow_provider_action next_otp_request(struct flow_context *flow,
                                                  struct flow_http_request *request) {
  struct platform_register_state *state =
      (struct platform_register_state *) flow->provider_data;
  char error[256] = "";
  int rc;
  long now = (long) mg_millis();
  long min_received_at;

  if (resend_wait_timed_out(state, now)) {
    flow_context_fail(flow, "重发后 30 秒内未收到邮箱验证码");
    flow_context_log(flow, "warn",
                     "验证码重发后 30 秒内仍未送达，放弃当前过期账号注册流程");
    return FLOW_PROVIDER_FAILED;
  }
  if (now > flow->deadline_ms) {
    flow_context_fail(flow, "等待邮箱验证码超时");
    return FLOW_PROVIDER_FAILED;
  }
  if (now < flow->next_retry_ms) return FLOW_PROVIDER_WAIT;

  state->otp_attempts++;
  min_received_at = state->otp_resend_after_epoch > 0
                        ? state->otp_resend_after_epoch
                        : state->otp_sent_after_epoch;
  if (min_received_at > 0) {
    rc = rapid_inbox_fetch_latest_code_since(
        flow->db, flow->identity.email, min_received_at, state->otp_code,
        sizeof(state->otp_code), error, sizeof(error));
  } else {
    rc = rapid_inbox_fetch_latest_code(flow->db, flow->identity.email,
                                       state->otp_code,
                                       sizeof(state->otp_code), error,
                                       sizeof(error));
  }
  now = (long) mg_millis();
  if (rc == 0) {
    if (resend_wait_timed_out(state, now)) {
      flow_context_fail(flow, "重发后 30 秒内未收到邮箱验证码");
      flow_context_log(flow, "warn",
                       "验证码重发后 30 秒内仍未送达，放弃当前过期账号注册流程");
      return FLOW_PROVIDER_FAILED;
    }
    if (should_resend_otp(state, now)) {
      return request_otp_resend(flow, request, state, now);
    }
    flow_context_log(flow, "debug", "等待%s邮箱验证码，第 %d 次轮询",
                     state->otp_resend_requested_ms > 0 ? "重发后的" : "",
                     state->otp_attempts);
    schedule_otp_poll(flow, state, now, 2500);
    return FLOW_PROVIDER_WAIT;
  }
  if (rc < 0) {
    if (resend_wait_timed_out(state, now)) {
      flow_context_fail(flow, "重发后 30 秒内未收到邮箱验证码");
      flow_context_log(flow, "warn",
                       "验证码读取失败且重发后已超过 30 秒，放弃当前过期账号注册流程");
      return FLOW_PROVIDER_FAILED;
    }
    if (should_resend_otp(state, now)) {
      return request_otp_resend(flow, request, state, now);
    }
    flow_context_log(flow, "warn", "读取验证码失败: %s", error);
    schedule_otp_poll(flow, state, now, 3500);
    return FLOW_PROVIDER_WAIT;
  }

  mg_snprintf(state->body, sizeof(state->body), "{\"code\":\"%s\"}",
              state->otp_code);
  request_post(request, AUTH_BASE_URL "/api/accounts/email-otp/validate",
               state->body, s_auth_json_headers,
               sizeof(s_auth_json_headers) / sizeof(s_auth_json_headers[0]));
  flow_context_log(flow, "info", "验证码已获取，提交邮箱验证");
  return FLOW_PROVIDER_REQUEST;
}

static enum flow_provider_action provider_next(struct flow_context *flow,
                                               struct flow_http_request *request) {
  struct platform_register_state *state =
      (struct platform_register_state *) flow->provider_data;
  char email_enc[512];
  char state_enc[256];
  char nonce_enc[256];
  char challenge_enc[256];

  if (state == NULL || request == NULL) {
    flow_context_fail(flow, "Platform 注册 provider 状态丢失");
    return FLOW_PROVIDER_FAILED;
  }

  switch (flow->step) {
    case STEP_PLATFORM_LOGIN:
      request_get(request, PLATFORM_BASE_URL "/login", s_platform_nav_headers,
                  sizeof(s_platform_nav_headers) / sizeof(s_platform_nav_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 打开 Platform 登录页",
                       step_label(flow->step));
      return FLOW_PROVIDER_REQUEST;
    case STEP_AUTHORIZE:
      if (mg_url_encode(flow->identity.email, strlen(flow->identity.email),
                        email_enc, sizeof(email_enc)) >= sizeof(email_enc) ||
          mg_url_encode(state->state, strlen(state->state), state_enc,
                        sizeof(state_enc)) >= sizeof(state_enc) ||
          mg_url_encode(state->nonce, strlen(state->nonce), nonce_enc,
                        sizeof(nonce_enc)) >= sizeof(nonce_enc) ||
          mg_url_encode(state->code_challenge, strlen(state->code_challenge),
                        challenge_enc, sizeof(challenge_enc)) >=
              sizeof(challenge_enc)) {
        flow_context_fail(flow, "Platform authorize 参数编码失败");
        return FLOW_PROVIDER_FAILED;
      }
      mg_snprintf(state->url, sizeof(state->url),
                  AUTH_BASE_URL
                  "/api/accounts/authorize?issuer=https%%3A%%2F%%2Fauth.openai.com"
                  "&client_id=%s&audience=%s&redirect_uri=%s&device_id=%s"
                  "&screen_hint=login_or_signup&max_age=0&login_hint=%s"
                  "&scope=%s&response_type=code&response_mode=query"
                  "&state=%s&nonce=%s&code_challenge=%s"
                  "&code_challenge_method=S256"
                  "&auth0Client=eyJuYW1lIjoiYXV0aDAtc3BhLWpzIiwidmVyc2lvbiI6IjEuMjEuMCJ9",
                  OAUTH_CLIENT_ID, OAUTH_AUDIENCE, OAUTH_REDIRECT_URI,
                  state->device_id, email_enc, OAUTH_SCOPE, state_enc,
                  nonce_enc, challenge_enc);
      request_get(request, state->url, s_authorize_headers,
                  sizeof(s_authorize_headers) / sizeof(s_authorize_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 打开授权入口",
                       step_label(flow->step));
      return FLOW_PROVIDER_REQUEST;
    case STEP_PASSWORD_PAGE:
      if (state->password_page_url[0] == '\0') {
        mg_snprintf(state->password_page_url, sizeof(state->password_page_url),
                    "%s", AUTH_BASE_URL "/create-account/password");
      }
      request_get(request, state->password_page_url, s_password_page_headers,
                  sizeof(s_password_page_headers) / sizeof(s_password_page_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 打开密码注册页",
                       step_label(flow->step));
      return FLOW_PROVIDER_REQUEST;
    case STEP_USER_REGISTER:
      mg_snprintf(state->body, sizeof(state->body),
                  "{\"password\":\"%s\",\"username\":\"%s\"}",
                  flow->identity.password, flow->identity.email);
      request_post(request, AUTH_BASE_URL "/api/accounts/user/register",
                   state->body, s_register_headers,
                   sizeof(s_register_headers) / sizeof(s_register_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 提交邮箱密码注册",
                       step_label(flow->step));
      return FLOW_PROVIDER_REQUEST;
    case STEP_EMAIL_OTP_SEND:
      state->otp_sent_after_epoch = (long) time(NULL);
      state->otp_send_requested_ms = (long) mg_millis();
      state->otp_resend_after_epoch = 0;
      state->otp_resend_requested_ms = 0;
      state->otp_attempts = 0;
      state->otp_code[0] = '\0';
      request_get(request, AUTH_BASE_URL "/api/accounts/email-otp/send",
                  s_send_headers,
                  sizeof(s_send_headers) / sizeof(s_send_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 触发验证码邮件发送",
                       step_label(flow->step));
      return FLOW_PROVIDER_REQUEST;
    case STEP_EMAIL_VERIFICATION:
      if (state->email_page_url[0] == '\0') {
        mg_snprintf(state->email_page_url, sizeof(state->email_page_url), "%s",
                    AUTH_BASE_URL "/email-verification");
      }
      request_get(request, state->email_page_url, s_email_page_headers,
                  sizeof(s_email_page_headers) / sizeof(s_email_page_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 打开邮箱验证页",
                       step_label(flow->step));
      return FLOW_PROVIDER_REQUEST;
    case STEP_OTP_RESEND:
      return request_otp_resend(flow, request, state, (long) mg_millis());
    case STEP_OTP_VALIDATE:
      return next_otp_request(flow, request);
    case STEP_SESSION_DUMP:
      request_get(request, AUTH_BASE_URL "/api/accounts/client_auth_session_dump",
                  s_session_headers,
                  sizeof(s_session_headers) / sizeof(s_session_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 读取 client auth session",
                       step_label(flow->step));
      return FLOW_PROVIDER_REQUEST;
    case STEP_CREATE_ACCOUNT:
      mg_snprintf(state->body, sizeof(state->body),
                  "{\"name\":\"%s\",\"birthdate\":\"%s\"}",
                  flow->identity.full_name, flow->identity.birthdate);
      request_post(request, AUTH_BASE_URL "/api/accounts/create_account",
                   state->body, s_create_headers,
                   sizeof(s_create_headers) / sizeof(s_create_headers[0]));
      flow_context_log(flow, "info", "步骤 %s: 创建账号资料 %s / %s",
                       step_label(flow->step), flow->identity.full_name,
                       flow->identity.birthdate);
      return FLOW_PROVIDER_REQUEST;
    case STEP_DONE:
      return FLOW_PROVIDER_DONE;
    default:
      flow_context_fail(flow, "未知 Platform 注册步骤");
      return FLOW_PROVIDER_FAILED;
  }
}

static int provider_response(struct flow_context *flow,
                             const struct flow_http_response *response) {
  struct platform_register_state *state =
      (struct platform_register_state *) flow->provider_data;

  if (state == NULL || response == NULL) {
    flow_context_fail(flow, "Platform 注册响应状态丢失");
    return -1;
  }

  switch (flow->step) {
    case STEP_PLATFORM_LOGIN:
      if (!expect_success(flow, response, "platform login")) return -1;
      flow_context_log(flow, "info", "步骤 %s 完成", step_label(flow->step));
      flow->step = STEP_AUTHORIZE;
      return 0;
    case STEP_AUTHORIZE:
      if (!expect_success(flow, response, "platform authorize")) return -1;
      if (response->location[0] != '\0') {
        mg_snprintf(state->password_page_url, sizeof(state->password_page_url),
                    "%s", response->location);
      }
      flow_context_log(flow, "info", "步骤 %s 完成，password_page=%s",
                       step_label(flow->step),
                       state->password_page_url[0] ? state->password_page_url
                                                   : AUTH_BASE_URL "/create-account/password");
      flow->step = STEP_PASSWORD_PAGE;
      return 0;
    case STEP_PASSWORD_PAGE:
      if (!expect_success(flow, response, "password page")) return -1;
      flow_context_log(flow, "info", "步骤 %s 完成", step_label(flow->step));
      flow->step = STEP_USER_REGISTER;
      return 0;
    case STEP_USER_REGISTER:
      if (!expect_success(flow, response, "user register")) return -1;
      {
        char preview[FLOW_LOG_LEN];
        body_preview(response, preview, sizeof(preview));
        if (preview[0] != '\0') {
          flow_context_log(flow, "debug", "步骤 %s 响应片段: %s",
                           step_label(flow->step), preview);
        }
      }
      flow_context_log(flow, "info", "步骤 %s 完成", step_label(flow->step));
      flow->step = STEP_EMAIL_OTP_SEND;
      return 0;
    case STEP_EMAIL_OTP_SEND:
      if (!expect_success(flow, response, "email otp send")) return -1;
      if (response->location[0] != '\0') {
        mg_snprintf(state->email_page_url, sizeof(state->email_page_url), "%s",
                    response->location);
      }
      flow_context_log(flow, "info", "步骤 %s 完成，email_page=%s",
                       step_label(flow->step),
                       state->email_page_url[0] ? state->email_page_url
                                                : AUTH_BASE_URL "/email-verification");
      flow->step = STEP_EMAIL_VERIFICATION;
      return 0;
    case STEP_EMAIL_VERIFICATION:
      if (!expect_success(flow, response, "email verification")) return -1;
      flow_context_log(flow, "info", "步骤 %s 完成", step_label(flow->step));
      flow->step = STEP_OTP_VALIDATE;
      flow->next_retry_ms = 0;
      flow_context_emit_event(flow, FLOW_EVENT_EMAIL_OTP_WAITING);
      return 0;
    case STEP_OTP_RESEND:
      if (!expect_success(flow, response, "email otp resend")) return -1;
      {
        char preview[FLOW_LOG_LEN];
        body_preview(response, preview, sizeof(preview));
        if (preview[0] != '\0') {
          flow_context_log(flow, "debug", "步骤 %s 响应片段: %s",
                           step_label(flow->step), preview);
        }
      }
      flow_context_log(flow, "info",
                       "步骤 %s 完成，重发后最多等待 30 秒",
                       step_label(flow->step));
      flow->step = STEP_OTP_VALIDATE;
      flow->next_retry_ms = 0;
      flow_context_emit_event(flow, FLOW_EVENT_EMAIL_OTP_WAITING);
      return 0;
    case STEP_OTP_VALIDATE:
      if (!expect_success(flow, response, "email otp validate")) return -1;
      flow_context_emit_event(flow, FLOW_EVENT_EMAIL_OTP_VALIDATED);
      flow_context_log(flow, "info", "步骤 %s 完成，验证码已验证",
                       step_label(flow->step));
      flow->step = STEP_SESSION_DUMP;
      return 0;
    case STEP_SESSION_DUMP:
      if (!expect_success(flow, response, "session dump")) return -1;
      copy_json_path(response->body, "$.user_id", flow->external_account_id,
                     sizeof(flow->external_account_id));
      flow_context_log(flow, "info", "步骤 %s 完成，user=%s",
                       step_label(flow->step),
                       flow->external_account_id[0] ? flow->external_account_id
                                                    : "-");
      flow->step = STEP_CREATE_ACCOUNT;
      return 0;
    case STEP_CREATE_ACCOUNT:
      if (!expect_success(flow, response, "create account")) return -1;
      flow->access_token[0] = '\0';
      flow->refresh_token[0] = '\0';
      flow_context_log(flow, "info", "Platform 注册成功，按过期账号入库");
      flow->step = STEP_DONE;
      return 0;
    default:
      flow_context_fail(flow, "收到未知 Platform 注册步骤响应");
      return -1;
  }
}

static void provider_cleanup(struct flow_context *flow) {
  if (flow == NULL) return;
  free(flow->provider_data);
  flow->provider_data = NULL;
}

static const struct flow_provider s_provider = {
    "platform-register-har",
    provider_start,
    provider_next,
    provider_response,
    provider_cleanup,
};

const struct flow_provider *platform_register_provider(void) {
  return &s_provider;
}
