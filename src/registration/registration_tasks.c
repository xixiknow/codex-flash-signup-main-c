#include "registration/registration_tasks.h"

#include "account/account_store.h"
#include "flow/flow_impersonate.h"
#include "http_client/browser_profile.h"
#include "identity/identity_generator.h"
#include "oauth/oauth_provider.h"
#include "proxy/proxy_pool.h"
#include "registration/platform_register_provider.h"
#include "registration/redeem_register_provider.h"
#include "registration/web_register_provider.h"
#include "redeem/redeem_client.h"
#include "redeem/redeem_store.h"
#include "storage/app_db.h"
#include "system/system_monitor.h"
#include "upload/aether_upload.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define REG_TASK_STATUS_LEN 24
#define REG_TASK_ERROR_LEN 256
#define REG_TASK_LOG_MSG_LEN 768
#define REG_TASK_MAX_LOGS 4000
#define REG_TASK_IDLE_RETENTION_MS (10ULL * 60ULL * 1000ULL)
#define OAUTH_RACE_BRANCHES 2
#define ENVIRONMENT_RETRY_LIMIT 2
#define FASTLANE_DEFAULT_MAX_INFLIGHT 20
#define FASTLANE_MAX_INFLIGHT_LIMIT 1000

enum registration_fastlane_stage {
  REG_FLOW_STAGE_NONE = 0,
  REG_FLOW_STAGE_PRE_EMAIL_ACTIVE,
  REG_FLOW_STAGE_WAITING_EMAIL,
  REG_FLOW_STAGE_POST_EMAIL_ACTIVE,
  REG_FLOW_STAGE_TERMINAL
};

struct registration_log_entry {
  uint64_t seq;
  uint64_t ts_ms;
  char level[12];
  char flow_id[FLOW_ID_LEN];
  char message[REG_TASK_LOG_MSG_LEN];
};

struct registration_task {
  char id[REG_TASK_ID_LEN];
  enum registration_workflow workflow;
  enum registration_scheduler_mode scheduler_mode;
  enum registration_target_metric target_metric;
  enum registration_register_provider register_provider;
  int target_count;
  int concurrency;
  int max_inflight;
  int oauth_delay_seconds;
  bool detailed_logs;
  bool infinite;
  bool auto_upload_oauth_success;
  bool stop_requested;
  int started;
  int active;
  int success;
  int failed;
  int register_success;
  int register_failed;
  int oauth_success;
  int oauth_failed;
  int expired_written;
  int temp_written;
  int upload_success;
  int upload_failed;
  int upload_skipped;
  int fastlane_pre_email_active;
  int fastlane_waiting_email;
  int fastlane_post_email_active;
  char status[REG_TASK_STATUS_LEN];
  char error[REG_TASK_ERROR_LEN];
  uint64_t created_ms;
  uint64_t started_ms;
  uint64_t updated_ms;
  uint64_t finished_ms;
  long *account_ids;
  size_t account_id_count;
  size_t next_account_index;
  long *redeem_ids;
  size_t redeem_id_count;
  size_t next_redeem_index;
  struct registration_log_entry *logs;
  size_t log_len;
  size_t log_cap;
  pthread_t thread;
  struct registration_task *next;
};

struct registration_ws_state {
  char task_id[REG_TASK_ID_LEN];
  uint64_t last_seq;
  uint64_t last_status_ms;
  bool system_subscribed;
  uint64_t system_interval_ms;
  uint64_t last_system_status_ms;
};

struct registration_flow_job {
  struct registration_task *task;
  enum registration_workflow workflow;
  enum registration_register_provider register_provider;
  struct identity_result identity;
  struct browser_profile profile;
  char proxy_url[FLOW_PROXY_LEN];
  char workspace_id[FLOW_WORKSPACE_ID_LEN];
  char redeem_code[FLOW_REDEEM_CODE_LEN];
  long account_id;
  long redeem_id;
  long deadline_ms;
  int oauth_delay_seconds;
  bool auto_upload_oauth_success;
  enum registration_fastlane_stage scheduler_stage;
};

struct oauth_race_state {
  pthread_mutex_t mu;
  int winner_index;
  bool cancel[OAUTH_RACE_BRANCHES];
};

struct oauth_race_runner {
  struct oauth_race_state *race;
  int index;
  bool launched;
  int rc;
  pthread_t thread;
  struct registration_flow_job job;
  struct flow_context flow;
};

static int reassign_job_environment(sqlite3 *db, struct registration_flow_job *job,
                                    const char *phase, int attempt,
                                    int limit);
static void fastlane_set_job_stage(struct registration_flow_job *job,
                                   enum registration_fastlane_stage next_stage,
                                   const char *flow_id,
                                   const char *reason);
static bool task_stop_requested(struct registration_task *task);

static pthread_mutex_t s_tasks_mu = PTHREAD_MUTEX_INITIALIZER;
static struct registration_task *s_tasks;
static uint64_t s_log_seq;

static uint64_t now_ms(void) {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0) {
    return (uint64_t) tv.tv_sec * 1000ULL + (uint64_t) tv.tv_usec / 1000ULL;
  }
  return mg_millis();
}

static void set_error(char *error, size_t error_len, const char *message) {
  if (error_len == 0) return;
  mg_snprintf(error, error_len, "%s", message ? message : "注册任务失败");
}

static const char *workflow_name(enum registration_workflow workflow) {
  switch (workflow) {
    case REG_WORKFLOW_REGISTER_THEN_OAUTH: return "register_then_oauth";
    case REG_WORKFLOW_OAUTH_ONLY: return "oauth_only";
    case REG_WORKFLOW_REGISTER_ONLY:
    default: return "register_only";
  }
}

static const char *scheduler_mode_name(enum registration_scheduler_mode mode) {
  return mode == REG_SCHEDULER_FASTLANE ? "fastlane" : "normal";
}

static const char *fastlane_stage_name(enum registration_fastlane_stage stage) {
  switch (stage) {
    case REG_FLOW_STAGE_PRE_EMAIL_ACTIVE: return "pre_email_active";
    case REG_FLOW_STAGE_WAITING_EMAIL: return "waiting_email";
    case REG_FLOW_STAGE_POST_EMAIL_ACTIVE: return "post_email_active";
    case REG_FLOW_STAGE_TERMINAL: return "terminal";
    case REG_FLOW_STAGE_NONE:
    default: return "none";
  }
}

static const char *fastlane_stage_label(enum registration_fastlane_stage stage) {
  switch (stage) {
    case REG_FLOW_STAGE_PRE_EMAIL_ACTIVE: return "注册前置阶段";
    case REG_FLOW_STAGE_WAITING_EMAIL: return "等待邮箱验证码";
    case REG_FLOW_STAGE_POST_EMAIL_ACTIVE: return "验证码后置阶段";
    case REG_FLOW_STAGE_TERMINAL: return "已结束";
    case REG_FLOW_STAGE_NONE:
    default: return "-";
  }
}

static const char *target_metric_name(enum registration_target_metric metric) {
  return metric == REG_TARGET_OAUTH_SUCCESS ? "oauth_success"
                                            : "register_task";
}

static const char *register_provider_name(
    enum registration_register_provider provider) {
  if (provider == REG_REGISTER_PROVIDER_TEMPORARY) return "temporary";
  if (provider == REG_REGISTER_PROVIDER_REDEEM) return "redeem";
  return "platform";
}

static const char *register_provider_label(
    enum registration_register_provider provider) {
  if (provider == REG_REGISTER_PROVIDER_TEMPORARY) return "临时账号注册";
  if (provider == REG_REGISTER_PROVIDER_REDEEM) return "兑换码注册";
  return "过期账号注册";
}

static const char *task_register_label(const struct registration_task *task) {
  if (task == NULL) return "-";
  if (task->workflow == REG_WORKFLOW_OAUTH_ONLY) return "账号池 OAuth";
  return register_provider_label(task->register_provider);
}

static bool task_uses_fastlane_unlocked(const struct registration_task *task) {
  return task != NULL && task->scheduler_mode == REG_SCHEDULER_FASTLANE &&
         task->workflow != REG_WORKFLOW_OAUTH_ONLY;
}

static int task_fastlane_alive_unlocked(const struct registration_task *task) {
  if (task == NULL) return 0;
  return task->fastlane_pre_email_active + task->fastlane_waiting_email +
         task->fastlane_post_email_active;
}

static void adjust_fastlane_stage_count_unlocked(
    struct registration_task *task, enum registration_fastlane_stage stage,
    int delta) {
  int *slot = NULL;
  if (task == NULL || delta == 0) return;
  switch (stage) {
    case REG_FLOW_STAGE_PRE_EMAIL_ACTIVE:
      slot = &task->fastlane_pre_email_active;
      break;
    case REG_FLOW_STAGE_WAITING_EMAIL:
      slot = &task->fastlane_waiting_email;
      break;
    case REG_FLOW_STAGE_POST_EMAIL_ACTIVE:
      slot = &task->fastlane_post_email_active;
      break;
    default:
      return;
  }
  *slot += delta;
  if (*slot < 0) *slot = 0;
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

static void generate_task_id(char *out, size_t out_len) {
  uint64_t seed = 0;
  if (out_len == 0) return;
  if (!mg_random(&seed, sizeof(seed))) seed = now_ms();
  mg_snprintf(out, out_len, "rt-%llx", (unsigned long long) seed);
}

static struct registration_task *find_task_locked(const char *task_id) {
  for (struct registration_task *task = s_tasks; task != NULL; task = task->next) {
    if (strcmp(task->id, task_id) == 0) return task;
  }
  return NULL;
}

static bool task_is_operating_unlocked(const struct registration_task *task) {
  if (task == NULL) return false;
  return task->active > 0 || strcmp(task->status, "queued") == 0 ||
         strcmp(task->status, "running") == 0 ||
         strcmp(task->status, "stopping") == 0;
}

static bool task_is_expired_unlocked(const struct registration_task *task,
                                     uint64_t now) {
  uint64_t idle_since;
  if (task == NULL || task_is_operating_unlocked(task)) return false;
  idle_since = task->finished_ms > 0 ? task->finished_ms : task->updated_ms;
  return idle_since > 0 && now >= idle_since &&
         now - idle_since >= REG_TASK_IDLE_RETENTION_MS;
}

static void free_task(struct registration_task *task) {
  if (task == NULL) return;
  free(task->account_ids);
  free(task->redeem_ids);
  free(task->logs);
  free(task);
}

static void purge_expired_tasks_locked(void) {
  struct registration_task **link = &s_tasks;
  uint64_t now = now_ms();
  while (*link != NULL) {
    struct registration_task *task = *link;
    if (task_is_expired_unlocked(task, now)) {
      *link = task->next;
      free_task(task);
    } else {
      link = &task->next;
    }
  }
}

static void append_log_locked(struct registration_task *task,
                              const char *flow_id, const char *level,
                              const char *message) {
  struct registration_log_entry *entry;

  if (task == NULL || message == NULL) return;
  if (task->log_len == task->log_cap) {
    size_t cap = task->log_cap == 0 ? 128 : task->log_cap * 2;
    struct registration_log_entry *next;
    if (cap > REG_TASK_MAX_LOGS) cap = REG_TASK_MAX_LOGS;
    if (task->log_len == REG_TASK_MAX_LOGS) {
      memmove(task->logs, task->logs + 1,
              (REG_TASK_MAX_LOGS - 1) * sizeof(*task->logs));
      task->log_len--;
    } else {
      next = (struct registration_log_entry *) realloc(
          task->logs, cap * sizeof(*next));
      if (next == NULL) return;
      task->logs = next;
      task->log_cap = cap;
    }
  }
  entry = &task->logs[task->log_len++];
  memset(entry, 0, sizeof(*entry));
  entry->seq = ++s_log_seq;
  entry->ts_ms = now_ms();
  mg_snprintf(entry->level, sizeof(entry->level), "%s",
              level ? level : "info");
  mg_snprintf(entry->flow_id, sizeof(entry->flow_id), "%s",
              flow_id ? flow_id : "");
  mg_snprintf(entry->message, sizeof(entry->message), "%s", message);
  task->updated_ms = entry->ts_ms;
}

static void append_logf_locked(struct registration_task *task,
                               const char *flow_id, const char *level,
                               const char *fmt, ...) {
  char message[REG_TASK_LOG_MSG_LEN];
  va_list ap;
  if (fmt == NULL) return;
  va_start(ap, fmt);
  vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);
  append_log_locked(task, flow_id, level, message);
}

static void task_log(struct registration_task *task, const char *flow_id,
                     const char *level, const char *fmt, ...) {
  char message[REG_TASK_LOG_MSG_LEN];
  va_list ap;

  if (task == NULL || fmt == NULL) return;
  va_start(ap, fmt);
  vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);

  pthread_mutex_lock(&s_tasks_mu);
  append_log_locked(task, flow_id, level, message);
  pthread_mutex_unlock(&s_tasks_mu);
}

static void task_log_account_message(struct registration_task *task,
                                     const char *flow_id, const char *level,
                                     long account_id, const char *message) {
  char wrapped[REG_TASK_LOG_MSG_LEN];

  if (task == NULL || message == NULL) return;
  if (account_id > 0 && strstr(message, "账号 ID=") == NULL &&
      strstr(message, "账号ID=") == NULL) {
    mg_snprintf(wrapped, sizeof(wrapped), "账号 ID=%ld %s", account_id,
                message);
    task_log(task, flow_id, level, "%s", wrapped);
    return;
  }
  task_log(task, flow_id, level, "%s", message);
}

static void flow_log_callback(struct flow_context *flow, const char *level,
                              const char *message, void *userdata) {
  struct registration_task *task = (struct registration_task *) userdata;
  if (task == NULL || flow == NULL || message == NULL) return;
  if (!task->detailed_logs && level != NULL && strcmp(level, "debug") == 0) {
    return;
  }
  task_log_account_message(task, flow->id, level, flow->account_id, message);
}

static void flow_job_log_callback(struct flow_context *flow, const char *level,
                                  const char *message, void *userdata) {
  struct registration_flow_job *job = (struct registration_flow_job *) userdata;
  struct registration_task *task = job ? job->task : NULL;
  if (task == NULL || flow == NULL || message == NULL) return;
  if (!task->detailed_logs && level != NULL && strcmp(level, "debug") == 0) {
    return;
  }
  task_log_account_message(task, flow->id, level, flow->account_id, message);
}

static void flow_job_event_callback(struct flow_context *flow,
                                    const char *event, void *userdata) {
  struct registration_flow_job *job = (struct registration_flow_job *) userdata;
  if (job == NULL || event == NULL) return;
  if (strcmp(event, FLOW_EVENT_EMAIL_OTP_WAITING) == 0) {
    fastlane_set_job_stage(job, REG_FLOW_STAGE_WAITING_EMAIL,
                           flow ? flow->id : "",
                           "已进入邮箱验证码等待，释放前置启动槽");
  } else if (strcmp(event, FLOW_EVENT_EMAIL_OTP_VALIDATED) == 0) {
    fastlane_set_job_stage(job, REG_FLOW_STAGE_POST_EMAIL_ACTIVE,
                           flow ? flow->id : "",
                           "邮箱验证码已通过，进入后置阶段");
  }
}

static bool flow_job_cancel_callback(struct flow_context *flow,
                                     void *userdata) {
  struct registration_flow_job *job = (struct registration_flow_job *) userdata;
  (void) flow;
  return job != NULL && task_stop_requested(job->task);
}

static int task_target_progress_unlocked(const struct registration_task *task) {
  if (task == NULL) return 0;
  return task->target_metric == REG_TARGET_OAUTH_SUCCESS ? task->oauth_success
                                                         : task->started;
}

static bool task_goal_met_unlocked(const struct registration_task *task) {
  if (task == NULL || task->infinite) return false;
  return task->target_count > 0 &&
         task_target_progress_unlocked(task) >= task->target_count;
}

static void task_refresh_success_unlocked(struct registration_task *task) {
  if (task != NULL) task->success = task_target_progress_unlocked(task);
}

static bool task_should_launch_unlocked(const struct registration_task *task) {
  if (task == NULL || strcmp(task->status, "running") != 0 ||
      task->stop_requested) {
    return false;
  }
  if (task_uses_fastlane_unlocked(task)) {
    if (task->fastlane_pre_email_active >= task->concurrency ||
        task->active >= task->max_inflight) {
      return false;
    }
  } else if (task->active >= task->concurrency) {
    return false;
  }
  if (task->workflow == REG_WORKFLOW_OAUTH_ONLY) {
    return task->next_account_index < task->account_id_count;
  }
  if (task->register_provider == REG_REGISTER_PROVIDER_REDEEM) {
    return task->next_redeem_index < task->redeem_id_count;
  }
  return task->infinite || !task_goal_met_unlocked(task);
}

static bool task_is_done_unlocked(const struct registration_task *task) {
  if (task == NULL || task->active > 0) return false;
  if (task->stop_requested) return true;
  if (task->workflow == REG_WORKFLOW_OAUTH_ONLY) {
    return task->next_account_index >= task->account_id_count;
  }
  if (task->register_provider == REG_REGISTER_PROVIDER_REDEEM) {
    return task->next_redeem_index >= task->redeem_id_count;
  }
  return !task->infinite && task_goal_met_unlocked(task);
}

static bool task_should_continue(struct registration_task *task) {
  bool keep_running;
  pthread_mutex_lock(&s_tasks_mu);
  keep_running = strcmp(task->status, "running") == 0 ||
                 strcmp(task->status, "stopping") == 0;
  pthread_mutex_unlock(&s_tasks_mu);
  return keep_running;
}

static bool can_launch_more(struct registration_task *task) {
  bool ok;
  pthread_mutex_lock(&s_tasks_mu);
  ok = task_should_launch_unlocked(task);
  pthread_mutex_unlock(&s_tasks_mu);
  return ok;
}

static bool task_is_done(struct registration_task *task) {
  bool done;
  pthread_mutex_lock(&s_tasks_mu);
  done = task_is_done_unlocked(task);
  pthread_mutex_unlock(&s_tasks_mu);
  return done;
}

static bool task_stop_requested(struct registration_task *task) {
  bool stop;
  pthread_mutex_lock(&s_tasks_mu);
  stop = task != NULL &&
         (task->stop_requested || strcmp(task->status, "stopping") == 0 ||
          strcmp(task->status, "stopped") == 0);
  pthread_mutex_unlock(&s_tasks_mu);
  return stop;
}

static void mark_task_running(struct registration_task *task) {
  pthread_mutex_lock(&s_tasks_mu);
  mg_snprintf(task->status, sizeof(task->status),
              task->stop_requested ? "stopping" : "running");
  task->started_ms = now_ms();
  task->updated_ms = task->started_ms;
  append_logf_locked(task, "", "info",
                     "任务编排已启动: workflow=%s scheduler=%s register=%s target=%s%s 并发=%d%s",
                     workflow_name(task->workflow),
                     scheduler_mode_name(task->scheduler_mode),
                     task_register_label(task),
                     target_metric_name(task->target_metric),
                     task->infinite ? " infinite" : "",
                     task->concurrency,
                     task_uses_fastlane_unlocked(task) ? " 高速模式" : "");
  if (task_uses_fastlane_unlocked(task)) {
    append_logf_locked(task, "", "info",
                       "高速模式调度: 前置并发=%d 最大存活=%d，进入邮箱等待后释放启动槽",
                       task->concurrency, task->max_inflight);
  }
  if (!task->infinite) {
    append_logf_locked(task, "", "info", "目标数量: %d", task->target_count);
  }
  if (task->workflow == REG_WORKFLOW_REGISTER_THEN_OAUTH &&
      task->oauth_delay_seconds > 0) {
    append_logf_locked(task, "", "info", "注册完成后延迟 %d 秒再执行 OAuth",
                       task->oauth_delay_seconds);
  }
  if (task->auto_upload_oauth_success) {
    append_log_locked(task, "", "info",
                      "OAuth 成功后将自动上传到上传配置中的默认 Aether 服务");
  }
  pthread_mutex_unlock(&s_tasks_mu);
}

static void mark_task_failed(struct registration_task *task,
                             const char *message) {
  pthread_mutex_lock(&s_tasks_mu);
  mg_snprintf(task->status, sizeof(task->status), "failed");
  mg_snprintf(task->error, sizeof(task->error), "%s",
              message ? message : "注册任务失败");
  task_refresh_success_unlocked(task);
  task->active = 0;
  task->finished_ms = now_ms();
  task->updated_ms = task->finished_ms;
  append_log_locked(task, "", "error", task->error);
  pthread_mutex_unlock(&s_tasks_mu);
}

static void mark_task_finished(struct registration_task *task) {
  bool goal_met;
  pthread_mutex_lock(&s_tasks_mu);
  task_refresh_success_unlocked(task);
  goal_met = task_goal_met_unlocked(task);
  if (task->stop_requested) {
    mg_snprintf(task->status, sizeof(task->status), "stopped");
  } else if (goal_met || (task->workflow == REG_WORKFLOW_OAUTH_ONLY &&
                          task->oauth_success == (int) task->account_id_count)) {
    mg_snprintf(task->status, sizeof(task->status), "success");
  } else if (task->success > 0) {
    mg_snprintf(task->status, sizeof(task->status), "partial");
  } else {
    mg_snprintf(task->status, sizeof(task->status), "failed");
  }
  if (strcmp(task->status, "failed") == 0 && task->error[0] == '\0') {
    mg_snprintf(task->error, sizeof(task->error), "任务未达到目标，请查看详细日志");
  }
  task->finished_ms = now_ms();
  task->updated_ms = task->finished_ms;
  append_logf_locked(task, "", strcmp(task->status, "success") == 0 ? "info" : "warn",
                     "任务结束: 注册成功 %d / OAuth 成功 %d / 失败 %d",
                     task->register_success, task->oauth_success, task->failed);
  if (task->auto_upload_oauth_success) {
    append_logf_locked(task, "", "info",
                       "自动上传统计: 成功 %d / 失败 %d / 跳过 %d",
                       task->upload_success, task->upload_failed,
                       task->upload_skipped);
  }
  pthread_mutex_unlock(&s_tasks_mu);
}

static void fastlane_set_job_stage(struct registration_flow_job *job,
                                   enum registration_fastlane_stage next_stage,
                                   const char *flow_id,
                                   const char *reason) {
  struct registration_task *task = job ? job->task : NULL;
  enum registration_fastlane_stage old_stage;

  if (task == NULL || next_stage == REG_FLOW_STAGE_NONE) return;
  pthread_mutex_lock(&s_tasks_mu);
  if (!task_uses_fastlane_unlocked(task) ||
      job->scheduler_stage == REG_FLOW_STAGE_TERMINAL) {
    pthread_mutex_unlock(&s_tasks_mu);
    return;
  }
  old_stage = job->scheduler_stage;
  if (old_stage == next_stage) {
    pthread_mutex_unlock(&s_tasks_mu);
    return;
  }
  adjust_fastlane_stage_count_unlocked(task, old_stage, -1);
  adjust_fastlane_stage_count_unlocked(task, next_stage, 1);
  job->scheduler_stage = next_stage;
  task->updated_ms = now_ms();
  if (reason != NULL && reason[0] != '\0') {
    append_logf_locked(task, flow_id ? flow_id : "", "info",
                       "高速模式阶段切换: %s -> %s，%s",
                       fastlane_stage_label(old_stage),
                       fastlane_stage_label(next_stage), reason);
  }
  pthread_mutex_unlock(&s_tasks_mu);
}

static enum registration_fastlane_stage mark_flow_launch(
    struct registration_task *task) {
  enum registration_fastlane_stage stage = REG_FLOW_STAGE_NONE;
  pthread_mutex_lock(&s_tasks_mu);
  task->started++;
  task->active++;
  if (task_uses_fastlane_unlocked(task)) {
    stage = REG_FLOW_STAGE_PRE_EMAIL_ACTIVE;
    adjust_fastlane_stage_count_unlocked(task, stage, 1);
  }
  task_refresh_success_unlocked(task);
  task->updated_ms = now_ms();
  pthread_mutex_unlock(&s_tasks_mu);
  return stage;
}

static void mark_job_done(struct registration_flow_job *job) {
  struct registration_task *task = job ? job->task : NULL;
  pthread_mutex_lock(&s_tasks_mu);
  if (task != NULL) {
    if (task_uses_fastlane_unlocked(task) &&
        job->scheduler_stage != REG_FLOW_STAGE_TERMINAL) {
      adjust_fastlane_stage_count_unlocked(task, job->scheduler_stage, -1);
      job->scheduler_stage = REG_FLOW_STAGE_TERMINAL;
    }
    if (task->active > 0) task->active--;
    task_refresh_success_unlocked(task);
    task->updated_ms = now_ms();
  }
  pthread_mutex_unlock(&s_tasks_mu);
}

static void mark_flow_launch_failed(struct registration_task *task,
                                    const char *message) {
  pthread_mutex_lock(&s_tasks_mu);
  if (task->active > 0) task->active--;
  if (task_uses_fastlane_unlocked(task)) {
    adjust_fastlane_stage_count_unlocked(task, REG_FLOW_STAGE_PRE_EMAIL_ACTIVE,
                                         -1);
  }
  task->failed++;
  if (task->workflow == REG_WORKFLOW_OAUTH_ONLY) task->oauth_failed++;
  else task->register_failed++;
  task_refresh_success_unlocked(task);
  if (task->error[0] == '\0') {
    mg_snprintf(task->error, sizeof(task->error), "%s",
                message ? message : "流程启动失败");
  }
  append_log_locked(task, "", "error", message ? message : "流程启动失败");
  pthread_mutex_unlock(&s_tasks_mu);
}

static void mark_account_load_failed(struct registration_task *task, long id) {
  pthread_mutex_lock(&s_tasks_mu);
  task->started++;
  task->failed++;
  task->oauth_failed++;
  task_refresh_success_unlocked(task);
  task->updated_ms = now_ms();
  append_logf_locked(task, "", "error", "账号 ID=%ld 不存在或无法读取，跳过 OAuth", id);
  pthread_mutex_unlock(&s_tasks_mu);
}

static void record_register_success(struct registration_task *task,
                                    const struct flow_context *flow) {
  const char *status;
  pthread_mutex_lock(&s_tasks_mu);
  task->register_success++;
  status = flow && flow->success_account_status[0]
               ? flow->success_account_status
               : "temp";
  if (strcmp(status, "expired") == 0) {
    task->expired_written++;
  } else {
    task->temp_written++;
  }
  task_refresh_success_unlocked(task);
  task->updated_ms = now_ms();
  append_logf_locked(task, flow ? flow->id : "", "info",
                     "注册成功，已写入%s账号 ID=%ld",
                     strcmp(status, "expired") == 0 ? "过期" : "临时",
                     flow ? flow->persisted_account_id : 0);
  pthread_mutex_unlock(&s_tasks_mu);
}

static void record_register_failure(struct registration_task *task,
                                    const struct flow_context *flow,
                                    const char *message) {
  pthread_mutex_lock(&s_tasks_mu);
  task->register_failed++;
  task->failed++;
  task_refresh_success_unlocked(task);
  if (task->error[0] == '\0') {
    mg_snprintf(task->error, sizeof(task->error), "%s",
                message ? message : "注册失败");
  }
  task->updated_ms = now_ms();
  append_logf_locked(task, flow ? flow->id : "", "error", "注册失败: %s",
                     message ? message : "-");
  pthread_mutex_unlock(&s_tasks_mu);
}

static void record_oauth_success(struct registration_task *task,
                                 const struct flow_context *flow,
                                 long account_id) {
  pthread_mutex_lock(&s_tasks_mu);
  task->oauth_success++;
  task_refresh_success_unlocked(task);
  task->updated_ms = now_ms();
  append_logf_locked(task, flow ? flow->id : "", "info",
                     "OAuth 成功，账号 ID=%ld 已更新为活跃账号", account_id);
  pthread_mutex_unlock(&s_tasks_mu);
}

static void record_oauth_failure(struct registration_task *task,
                                 const struct flow_context *flow,
                                 long account_id, const char *message) {
  pthread_mutex_lock(&s_tasks_mu);
  task->oauth_failed++;
  task->failed++;
  task_refresh_success_unlocked(task);
  if (task->error[0] == '\0') {
    mg_snprintf(task->error, sizeof(task->error), "%s",
                message ? message : "OAuth 失败");
  }
  task->updated_ms = now_ms();
  append_logf_locked(task, flow ? flow->id : "", "error",
                     "OAuth 失败，账号 ID=%ld 状态保持不变: %s", account_id,
                     message ? message : "-");
  pthread_mutex_unlock(&s_tasks_mu);
}

static void record_auto_upload_result(struct registration_task *task,
                                      const char *flow_id, long account_id,
                                      bool ok, long success_count,
                                      long failed_count, long skipped_count,
                                      const char *error) {
  pthread_mutex_lock(&s_tasks_mu);
  if (success_count > 0) task->upload_success += (int) success_count;
  if (failed_count > 0) task->upload_failed += (int) failed_count;
  if (skipped_count > 0) task->upload_skipped += (int) skipped_count;
  if (!ok && success_count == 0 && failed_count == 0 && skipped_count == 0) {
    task->upload_failed++;
  }
  task->updated_ms = now_ms();
  if (ok && success_count > 0) {
    append_logf_locked(task, flow_id ? flow_id : "", "info",
                       "账号 ID=%ld 自动上传完成: 成功 %ld / 失败 %ld / 跳过 %ld",
                       account_id, success_count, failed_count,
                       skipped_count);
  } else {
    append_logf_locked(task, flow_id ? flow_id : "", "warn",
                       "账号 ID=%ld 自动上传未成功: 成功 %ld / 失败 %ld / 跳过 %ld%s%s",
                       account_id, success_count, failed_count,
                       skipped_count,
                       error != NULL && error[0] != '\0' ? "，原因: " : "",
                       error != NULL && error[0] != '\0' ? error : "");
  }
  pthread_mutex_unlock(&s_tasks_mu);
}

static long take_next_account_id(struct registration_task *task) {
  long id = 0;
  pthread_mutex_lock(&s_tasks_mu);
  if (task->next_account_index < task->account_id_count) {
    id = task->account_ids[task->next_account_index++];
  }
  pthread_mutex_unlock(&s_tasks_mu);
  return id;
}

static long take_next_redeem_id(struct registration_task *task) {
  long id = 0;
  pthread_mutex_lock(&s_tasks_mu);
  if (task->next_redeem_index < task->redeem_id_count) {
    id = task->redeem_ids[task->next_redeem_index++];
  }
  pthread_mutex_unlock(&s_tasks_mu);
  return id;
}

/* 兑换码注册：确保兑换码已兑换出邮箱，并把邮箱写入 identity。
 * 返回 0 成功，-1 失败（error 填写原因）。 */
static int prepare_redeem_identity(sqlite3 *db, long redeem_id,
                                   struct identity_result *identity,
                                   char *redeem_code, size_t redeem_code_len,
                                   char *error, size_t error_len) {
  char email[REDEEM_EMAIL_LEN] = "";
  if (redeem_code != NULL && redeem_code_len > 0) redeem_code[0] = '\0';
  if (redeem_load_code(db, redeem_id, redeem_code, redeem_code_len, email,
                       sizeof(email)) != 0) {
    mg_snprintf(error, error_len, "兑换码 ID=%ld 不存在", redeem_id);
    return -1;
  }
  if (email[0] == '\0') {
    struct redeem_result result;
    char redeem_err[256] = "";
    if (redeem_apply(db, redeem_code, &result, redeem_err,
                     sizeof(redeem_err)) != 0) {
      redeem_mark_failed(db, redeem_id, redeem_err);
      mg_snprintf(error, error_len, "%s", redeem_err[0] ? redeem_err : "兑换失败");
      return -1;
    }
    redeem_mark_redeemed(db, redeem_id, &result);
    mg_snprintf(email, sizeof(email), "%s", result.email);
  }
  /* 姓名/生日仍由身份生成器提供，仅覆盖邮箱为兑换码返回的邮箱。
   * 使用 profile_only 变体，避免依赖邮件域名规则（兑换流程不需要域名）。 */
  if (identity_generate_profile_only(identity, error, error_len) != 0) {
    return -1;
  }
  mg_snprintf(identity->email, sizeof(identity->email), "%s", email);
  return 0;
}

static int run_oauth_flow_impl(
    sqlite3 *db, struct registration_flow_job *job,
    struct flow_context *oauth_flow,
    void (*log_fn)(struct flow_context *flow, const char *level,
                   const char *message, void *userdata),
    bool (*cancel_fn)(struct flow_context *flow, void *userdata),
    void (*event_fn)(struct flow_context *flow, const char *event,
                     void *userdata),
    void *userdata) {
  struct flow_start_options options;
  memset(&options, 0, sizeof(options));
  memset(oauth_flow, 0, sizeof(*oauth_flow));
  options.mode = FLOW_MODE_REGISTER_THEN_OAUTH;
  options.proxy_url = job->proxy_url;
  options.profile = &job->profile;
  options.identity = &job->identity;
  options.workspace_id = job->workspace_id;
  options.db = db;
  options.persist_on_success = false;
  options.account_id = job->account_id;
  options.deadline_ms = (long) mg_millis() + 180000;
  options.log_fn = log_fn ? log_fn : flow_log_callback;
  options.cancel_fn = cancel_fn;
  options.event_fn = event_fn;
  options.callback_data = userdata ? userdata : job->task;
  return flow_impersonate_run(oauth_code_provider(), &options, oauth_flow);
}

static void oauth_race_log_callback(struct flow_context *flow, const char *level,
                                    const char *message, void *userdata) {
  struct oauth_race_runner *runner = (struct oauth_race_runner *) userdata;
  struct registration_task *task = runner ? runner->job.task : NULL;
  if (task == NULL || flow == NULL || message == NULL) return;
  if (!task->detailed_logs && level != NULL && strcmp(level, "debug") == 0) {
    return;
  }
  task_log_account_message(task, flow->id, level, flow->account_id, message);
}

static bool oauth_race_cancel_callback(struct flow_context *flow,
                                      void *userdata) {
  struct oauth_race_runner *runner = (struct oauth_race_runner *) userdata;
  bool cancel = false;
  if (runner == NULL || runner->race == NULL) return false;
  pthread_mutex_lock(&runner->race->mu);
  cancel = runner->race->cancel[runner->index] ||
           (runner->race->winner_index >= 0 &&
            runner->race->winner_index != runner->index);
  pthread_mutex_unlock(&runner->race->mu);
  if (!cancel && runner->job.task != NULL) {
    cancel = task_stop_requested(runner->job.task);
  }
  (void) flow;
  return cancel;
}

static void oauth_race_event_callback(struct flow_context *flow,
                                      const char *event, void *userdata) {
  struct oauth_race_runner *runner = (struct oauth_race_runner *) userdata;
  bool became_winner = false;
  bool became_loser = false;

  if (runner == NULL || runner->race == NULL || event == NULL || flow == NULL) {
    return;
  }
  if (strcmp(event, FLOW_EVENT_OAUTH_OTP_VALIDATED) != 0) return;
  pthread_mutex_lock(&runner->race->mu);
  if (runner->race->winner_index < 0) {
    runner->race->winner_index = runner->index;
    for (int i = 0; i < OAUTH_RACE_BRANCHES; i++) {
      if (i != runner->index) runner->race->cancel[i] = true;
    }
    became_winner = true;
  } else if (runner->race->winner_index != runner->index) {
    runner->race->cancel[runner->index] = true;
    became_loser = true;
  }
  pthread_mutex_unlock(&runner->race->mu);

  if (became_winner && runner->job.task != NULL) {
    task_log(runner->job.task, flow->id, "info",
             "账号 ID=%ld OAuth 抢码分支 %d 已通过验证码校验，取消另一路 OAuth",
             runner->job.account_id, runner->index + 1);
  } else if (became_loser && runner->job.task != NULL) {
    task_log(runner->job.task, flow->id, "warn",
             "账号 ID=%ld OAuth 抢码分支 %d 已取消，验证码校验已由另一路通过",
             runner->job.account_id, runner->index + 1);
  }
}

static void oauth_race_prepare_branch(sqlite3 *db,
                                      struct registration_flow_job *job,
                                      int branch_index) {
  char proxy_url[FLOW_PROXY_LEN] = "";
  char old_proxy[FLOW_PROXY_LEN];
  int proxy_rc = 0;

  if (job == NULL) return;
  if (branch_index <= 0) return;
  mg_snprintf(old_proxy, sizeof(old_proxy), "%s", job->proxy_url);
  browser_profile_generate(&job->profile, NULL, NULL);
  if (db != NULL) {
    for (int i = 0; i < 5; i++) {
      proxy_rc = proxy_pool_pick_active_url(db, proxy_url, sizeof(proxy_url));
      if (proxy_rc <= 0 || old_proxy[0] == '\0' ||
          strcmp(old_proxy, proxy_url) != 0) {
        break;
      }
    }
    if (proxy_rc > 0) {
      mg_snprintf(job->proxy_url, sizeof(job->proxy_url), "%s", proxy_url);
    }
  }
  task_log(job->task, "", "info",
           "账号 ID=%ld OAuth 备用分支已准备独立环境: proxy=%s profile=%s %s",
           job->account_id, job->proxy_url[0] ? job->proxy_url : "direct",
           job->profile.browser[0] ? job->profile.browser : "browser",
           job->profile.browser_version[0] ? job->profile.browser_version : "-");
}

static void *oauth_race_runner_thread(void *arg) {
  struct oauth_race_runner *runner = (struct oauth_race_runner *) arg;
  sqlite3 *db = NULL;

  if (runner == NULL) return NULL;
  memset(&runner->flow, 0, sizeof(runner->flow));
  runner->rc = -1;
  if (app_db_open("data/app.db", &db) != 0) {
    flow_context_fail(&runner->flow, "流程无法打开 SQLite 数据库");
    goto finish;
  }

  runner->rc = run_oauth_flow_impl(db, &runner->job, &runner->flow,
                                   oauth_race_log_callback,
                                   oauth_race_cancel_callback,
                                   oauth_race_event_callback, runner);

finish:
  if (db != NULL) app_db_close(db);
  runner->flow.log_fn = NULL;
  runner->flow.finish_fn = NULL;
  runner->flow.cancel_fn = NULL;
  runner->flow.event_fn = NULL;
  return NULL;
}

static int run_oauth_flow_race_with_environment_retry(
    sqlite3 *db, struct registration_flow_job *job,
    struct flow_context *oauth_flow) {
  int environment_retry_count = 0;

  for (;;) {
    struct oauth_race_state race;
    struct oauth_race_runner runners[OAUTH_RACE_BRANCHES];
    int winner_index = -1;
    int rc = -1;

    memset(&race, 0, sizeof(race));
    pthread_mutex_init(&race.mu, NULL);
    race.winner_index = -1;
    for (int i = 0; i < OAUTH_RACE_BRANCHES; i++) {
      memset(&runners[i], 0, sizeof(runners[i]));
      runners[i].race = &race;
      runners[i].index = i;
      runners[i].job = *job;
      oauth_race_prepare_branch(db, &runners[i].job, i);
    }

    for (int i = 0; i < OAUTH_RACE_BRANCHES; i++) {
      if (pthread_create(&runners[i].thread, NULL, oauth_race_runner_thread,
                         &runners[i]) != 0) {
        runners[i].launched = false;
        runners[i].rc = -1;
        flow_context_fail(&runners[i].flow, "OAuth 线程创建失败");
      } else {
        runners[i].launched = true;
      }
    }

    for (int i = 0; i < OAUTH_RACE_BRANCHES; i++) {
      if (runners[i].launched) pthread_join(runners[i].thread, NULL);
    }

    winner_index = race.winner_index;
    if (winner_index < 0) {
      for (int i = 0; i < OAUTH_RACE_BRANCHES; i++) {
        if (!runners[i].launched) continue;
        if (runners[i].flow.status == FLOW_STATUS_SUCCESS) {
          winner_index = i;
          break;
        }
      }
    }
    if (winner_index < 0) {
      for (int i = 0; i < OAUTH_RACE_BRANCHES; i++) {
        if (!runners[i].launched) continue;
        if (runners[i].flow.status != FLOW_STATUS_CANCELLED) {
          winner_index = i;
          break;
        }
      }
    }
    if (winner_index >= 0) {
      *oauth_flow = runners[winner_index].flow;
      rc = runners[winner_index].rc;
      if (oauth_flow->status == FLOW_STATUS_SUCCESS && rc == 0) {
        oauth_flow->log_fn = NULL;
        oauth_flow->finish_fn = NULL;
        oauth_flow->cancel_fn = NULL;
        oauth_flow->event_fn = NULL;
        oauth_flow->callback_data = NULL;
        pthread_mutex_destroy(&race.mu);
        return 0;
      }
    }
    if (winner_index < 0) {
      *oauth_flow = runners[0].flow;
      rc = runners[0].rc;
    }
    oauth_flow->log_fn = NULL;
    oauth_flow->finish_fn = NULL;
    oauth_flow->cancel_fn = NULL;
    oauth_flow->event_fn = NULL;
    oauth_flow->callback_data = NULL;

    if (oauth_flow->environment_retryable &&
        environment_retry_count < ENVIRONMENT_RETRY_LIMIT &&
        !task_stop_requested(job->task)) {
      environment_retry_count++;
      if (reassign_job_environment(db, job, "OAuth 流程", environment_retry_count,
                                   ENVIRONMENT_RETRY_LIMIT) != 0) {
        flow_context_fail(oauth_flow, "重新分配 OAuth 环境失败");
        pthread_mutex_destroy(&race.mu);
        return -1;
      }
      memset(oauth_flow, 0, sizeof(*oauth_flow));
      pthread_mutex_destroy(&race.mu);
      usleep(250000);
      continue;
    }
    pthread_mutex_destroy(&race.mu);
    return rc;
  }
  return -1;
}

static int apply_oauth_success(sqlite3 *db, struct registration_flow_job *job,
                               const struct flow_context *oauth_flow) {
  struct account_success_record record;
  memset(&record, 0, sizeof(record));
  record.email = job->identity.email;
  record.password = job->identity.password;
  record.status = "active";
  record.upload_state = "not_uploaded";
  record.access_token = oauth_flow->access_token;
  record.refresh_token = oauth_flow->refresh_token;
  record.external_account_id = oauth_flow->external_account_id;
  record.workspace_id = oauth_flow->workspace_id[0] ? oauth_flow->workspace_id
                                                    : job->workspace_id;
  return account_apply_oauth_success(db, job->account_id, &record);
}

static void maybe_auto_upload_oauth_success(sqlite3 *db,
                                            struct registration_flow_job *job,
                                            const struct flow_context *flow) {
  char *json;
  struct mg_str body;
  bool ok = false;
  long success_count = 0;
  long failed_count = 0;
  long skipped_count = 0;
  char *error = NULL;

  if (db == NULL || job == NULL || !job->auto_upload_oauth_success ||
      job->account_id <= 0) {
    return;
  }
  if (task_stop_requested(job->task)) {
    record_auto_upload_result(job->task, flow ? flow->id : "", job->account_id,
                              false, 0, 0, 1, "任务已请求停止，跳过自动上传");
    return;
  }

  task_log(job->task, flow ? flow->id : "", "info",
           "账号 ID=%ld OAuth 成功，开始自动上传到上传配置", job->account_id);
  json = aether_upload_accounts_json(db, &job->account_id, 1, "oauth");
  if (json == NULL) {
    record_auto_upload_result(job->task, flow ? flow->id : "", job->account_id,
                              false, 0, 1, 0, "Aether 上传失败");
    return;
  }

  body = mg_str(json);
  ok = mg_json_get_long(body, "$.ok", 0) == 1;
  success_count = mg_json_get_long(body, "$.success_count", 0);
  failed_count = mg_json_get_long(body, "$.failed_count", 0);
  skipped_count = mg_json_get_long(body, "$.skipped_count", 0);
  error = mg_json_get_str(body, "$.error");
  record_auto_upload_result(job->task, flow ? flow->id : "", job->account_id,
                            ok, success_count, failed_count, skipped_count,
                            error);
  mg_free(error);
  free(json);
}

static int reassign_job_environment(sqlite3 *db, struct registration_flow_job *job,
                                    const char *phase, int attempt,
                                    int limit) {
  char old_proxy[FLOW_PROXY_LEN];
  char proxy_url[FLOW_PROXY_LEN] = "";
  char scheme[32];
  int proxy_rc = 0;

  if (job == NULL) return -1;
  mg_snprintf(old_proxy, sizeof(old_proxy), "%s", job->proxy_url);
  browser_profile_generate(&job->profile, NULL, NULL);
  if (db != NULL) {
    for (int i = 0; i < 5; i++) {
      proxy_rc = proxy_pool_pick_active_url(db, proxy_url, sizeof(proxy_url));
      if (proxy_rc <= 0 || old_proxy[0] == '\0' ||
          strcmp(old_proxy, proxy_url) != 0) {
        break;
      }
    }
    if (proxy_rc < 0) return -1;
  }
  job->proxy_url[0] = '\0';
  if (proxy_rc > 0) {
    mg_snprintf(job->proxy_url, sizeof(job->proxy_url), "%s", proxy_url);
  }
  proxy_scheme_label(job->proxy_url, scheme, sizeof(scheme));
  if (job->account_id > 0) {
    task_log(job->task, "", "warn",
             "账号 ID=%ld %s 触发边缘风控，重新分配环境后重试 %d/%d: proxy=%s profile=%s %s",
             job->account_id, phase ? phase : "流程", attempt, limit,
             scheme[0] ? scheme : "direct",
             job->profile.browser[0] ? job->profile.browser : "browser",
             job->profile.browser_version[0] ? job->profile.browser_version : "-");
  } else {
    task_log(job->task, "", "warn",
             "%s 触发边缘风控，重新分配环境后重试 %d/%d: proxy=%s profile=%s %s",
             phase ? phase : "流程", attempt, limit,
             scheme[0] ? scheme : "direct",
             job->profile.browser[0] ? job->profile.browser : "browser",
             job->profile.browser_version[0] ? job->profile.browser_version : "-");
  }
  return 0;
}

static int run_registration_flow_with_environment_retry(
    sqlite3 *db, struct registration_flow_job *job, struct flow_context *reg_flow) {
  const struct flow_provider *provider;

  if (job == NULL || reg_flow == NULL) return -1;
  if (job->register_provider == REG_REGISTER_PROVIDER_REDEEM) {
    provider = redeem_register_provider();
  } else if (job->register_provider == REG_REGISTER_PROVIDER_TEMPORARY) {
    provider = web_register_provider();
  } else {
    provider = platform_register_provider();
  }

  for (int attempt = 0; attempt <= ENVIRONMENT_RETRY_LIMIT; attempt++) {
    struct flow_start_options options;
    int rc;

    memset(reg_flow, 0, sizeof(*reg_flow));
    memset(&options, 0, sizeof(options));
    options.mode = job->workflow == REG_WORKFLOW_REGISTER_THEN_OAUTH
                       ? FLOW_MODE_REGISTER_THEN_OAUTH
                       : FLOW_MODE_REGISTER_ONLY;
    options.proxy_url = job->proxy_url;
    options.profile = &job->profile;
    options.identity = &job->identity;
    options.redeem_code = job->redeem_code[0] ? job->redeem_code : NULL;
    options.db = db;
    options.persist_on_success = true;
    options.deadline_ms = job->deadline_ms;
    options.log_fn = flow_job_log_callback;
    options.cancel_fn = flow_job_cancel_callback;
    options.event_fn = flow_job_event_callback;
    options.callback_data = job;

    rc = flow_impersonate_run(provider, &options, reg_flow);
    if (rc == 0 && reg_flow->status == FLOW_STATUS_SUCCESS) return 0;
    if (attempt < ENVIRONMENT_RETRY_LIMIT && reg_flow->environment_retryable &&
        !task_stop_requested(job->task)) {
      if (reassign_job_environment(db, job, "注册流程", attempt + 1,
                                   ENVIRONMENT_RETRY_LIMIT) != 0) {
        flow_context_fail(reg_flow, "重新分配注册环境失败");
        return -1;
      }
      fastlane_set_job_stage(job, REG_FLOW_STAGE_PRE_EMAIL_ACTIVE,
                             reg_flow->id,
                             "注册流程重新分配环境，回到前置阶段");
      usleep(250000);
      continue;
    }
    return rc;
  }
  return -1;
}

static bool wait_before_oauth(struct registration_flow_job *job) {
  int remaining_ms;
  if (job == NULL || job->workflow != REG_WORKFLOW_REGISTER_THEN_OAUTH ||
      job->oauth_delay_seconds <= 0) {
    return true;
  }
  task_log(job->task, "", "info", "账号 ID=%ld 注册完成，等待 %d 秒后执行 OAuth",
           job->account_id, job->oauth_delay_seconds);
  remaining_ms = job->oauth_delay_seconds * 1000;
  while (remaining_ms > 0) {
    int chunk_ms = remaining_ms > 250 ? 250 : remaining_ms;
    if (task_stop_requested(job->task)) {
      task_log(job->task, "", "warn",
               "账号 ID=%ld 任务已请求停止，跳过等待中的 OAuth",
               job->account_id);
      return false;
    }
    usleep((useconds_t) chunk_ms * 1000);
    remaining_ms -= chunk_ms;
  }
  if (task_stop_requested(job->task)) {
    task_log(job->task, "", "warn",
             "账号 ID=%ld 任务已请求停止，跳过等待中的 OAuth",
             job->account_id);
    return false;
  }
  task_log(job->task, "", "info", "账号 ID=%ld OAuth 延迟等待结束，继续执行",
           job->account_id);
  return true;
}

static void *flow_worker(void *arg) {
  struct registration_flow_job *job = (struct registration_flow_job *) arg;
  sqlite3 *db = NULL;
  struct flow_context reg_flow;
  struct flow_context oauth_flow;

  if (job == NULL) return NULL;
  memset(&reg_flow, 0, sizeof(reg_flow));
  memset(&oauth_flow, 0, sizeof(oauth_flow));
  if (app_db_open("data/app.db", &db) != 0) {
    if (job->workflow == REG_WORKFLOW_OAUTH_ONLY) {
      record_oauth_failure(job->task, NULL, job->account_id,
                           "流程无法打开 SQLite 数据库");
    } else {
      record_register_failure(job->task, NULL, "流程无法打开 SQLite 数据库");
    }
    mark_job_done(job);
    free(job);
    return NULL;
  }

  if (job->workflow != REG_WORKFLOW_OAUTH_ONLY) {
    if (run_registration_flow_with_environment_retry(db, job, &reg_flow) != 0 ||
        reg_flow.status != FLOW_STATUS_SUCCESS) {
      record_register_failure(job->task, &reg_flow,
                              reg_flow.error[0] ? reg_flow.error
                                                : "注册流程失败");
      if (job->redeem_id > 0) {
        redeem_mark_failed(db, job->redeem_id,
                           reg_flow.error[0] ? reg_flow.error : "注册流程失败");
      }
      mark_job_done(job);
      app_db_close(db);
      free(job);
      return NULL;
    }
    job->account_id = reg_flow.persisted_account_id;
    record_register_success(job->task, &reg_flow);
    if (job->redeem_id > 0) {
      redeem_mark_registered(db, job->redeem_id, job->account_id);
    }
    fastlane_set_job_stage(job, REG_FLOW_STAGE_POST_EMAIL_ACTIVE,
                           reg_flow.id,
                           "注册已通过邮箱阶段");
    if (job->workflow == REG_WORKFLOW_REGISTER_ONLY) {
      mark_job_done(job);
      app_db_close(db);
      free(job);
      return NULL;
    }
    if (!wait_before_oauth(job)) {
      mark_job_done(job);
      app_db_close(db);
      free(job);
      return NULL;
    }
  }

  if (run_oauth_flow_race_with_environment_retry(db, job, &oauth_flow) == 0 &&
      oauth_flow.status == FLOW_STATUS_SUCCESS) {
    if (apply_oauth_success(db, job, &oauth_flow) == 0) {
      record_oauth_success(job->task, &oauth_flow, job->account_id);
      maybe_auto_upload_oauth_success(db, job, &oauth_flow);
    } else {
      record_oauth_failure(job->task, &oauth_flow, job->account_id,
                           "OAuth 结果写入账号库失败");
    }
  } else {
    record_oauth_failure(job->task, &oauth_flow, job->account_id,
                         oauth_flow.error[0] ? oauth_flow.error
                                             : "OAuth 流程失败");
  }

  mark_job_done(job);
  app_db_close(db);
  free(job);
  return NULL;
}

static int launch_flow_job(struct registration_task *task,
                           enum registration_workflow workflow,
                           enum registration_register_provider register_provider,
                           const struct identity_result *identity,
                           const struct browser_profile *profile,
                           const char *proxy_url, long account_id,
                           const char *workspace_id,
                           long redeem_id, const char *redeem_code,
                           enum registration_fastlane_stage initial_stage) {
  struct registration_flow_job *job;
  pthread_t thread;

  job = (struct registration_flow_job *) calloc(1, sizeof(*job));
  if (job == NULL) return -1;
  job->task = task;
  job->workflow = workflow;
  job->register_provider = register_provider;
  job->account_id = account_id;
  job->redeem_id = redeem_id;
  job->scheduler_stage = initial_stage;
  job->auto_upload_oauth_success = task->auto_upload_oauth_success;
  job->oauth_delay_seconds =
      workflow == REG_WORKFLOW_REGISTER_THEN_OAUTH ? task->oauth_delay_seconds : 0;
  if (identity != NULL) job->identity = *identity;
  if (profile != NULL) job->profile = *profile;
  if (proxy_url != NULL) {
    mg_snprintf(job->proxy_url, sizeof(job->proxy_url), "%s", proxy_url);
  }
  if (workspace_id != NULL) {
    mg_snprintf(job->workspace_id, sizeof(job->workspace_id), "%s",
                workspace_id);
  }
  if (redeem_code != NULL) {
    mg_snprintf(job->redeem_code, sizeof(job->redeem_code), "%s", redeem_code);
  }
  job->deadline_ms = (long) mg_millis() + 180000;

  if (pthread_create(&thread, NULL, flow_worker, job) != 0) {
    free(job);
    return -1;
  }
  pthread_detach(thread);
  return 0;
}

static int prepare_oauth_identity(sqlite3 *db, long account_id,
                                  struct identity_result *identity,
                                  char *workspace_id,
                                  size_t workspace_id_len) {
  struct account_oauth_record record;
  if (identity != NULL) memset(identity, 0, sizeof(*identity));
  if (workspace_id != NULL && workspace_id_len > 0) workspace_id[0] = '\0';
  if (account_load_oauth_record(db, account_id, &record) != 0) return -1;
  mg_snprintf(identity->email, sizeof(identity->email), "%s", record.email);
  mg_snprintf(identity->password, sizeof(identity->password), "%s",
              record.password);
  if (workspace_id != NULL && workspace_id_len > 0) {
    mg_snprintf(workspace_id, workspace_id_len, "%s", record.workspace_id);
  }
  return 0;
}

static void *task_worker(void *arg) {
  struct registration_task *task = (struct registration_task *) arg;
  sqlite3 *db = NULL;
  char error[256] = "";
  char impersonate_path[512] = "";
  bool fatal = false;

  if (app_db_open("data/app.db", &db) != 0) {
    mark_task_failed(task, "任务无法打开 SQLite 数据库");
    return NULL;
  }

  if (flow_impersonate_available(impersonate_path, sizeof(impersonate_path)) != 0) {
    app_db_close(db);
    mark_task_failed(task, "未找到 curl-impersonate，请安装 curl_chrome145 或设置 CURL_IMPERSONATE_BIN");
    return NULL;
  }

  mark_task_running(task);
  task_log(task, "", "info", "执行器: curl-impersonate %s", impersonate_path);
  if (task->workflow == REG_WORKFLOW_OAUTH_ONLY) {
    task_log(task, "", "info", "任务类型: 账号池 OAuth");
  } else {
    task_log(task, "", "info", "注册方式: %s",
             register_provider_label(task->register_provider));
    task_log(task, "", "info", "注册成功但未 OAuth 的账号将按注册方式写入对应状态");
  }

  while (task_should_continue(task)) {
    while (can_launch_more(task)) {
      struct identity_result identity;
      struct browser_profile profile;
      char proxy_url[FLOW_PROXY_LEN] = "";
      char workspace_id[FLOW_WORKSPACE_ID_LEN] = "";
      char redeem_code[FLOW_REDEEM_CODE_LEN] = "";
      long account_id = 0;
      long redeem_id = 0;
      int proxy_rc;
      enum registration_fastlane_stage initial_stage;

      if (task->workflow == REG_WORKFLOW_OAUTH_ONLY) {
        account_id = take_next_account_id(task);
        if (account_id <= 0) break;
        if (prepare_oauth_identity(db, account_id, &identity, workspace_id,
                                   sizeof(workspace_id)) != 0) {
          mark_account_load_failed(task, account_id);
          continue;
        }
      } else if (task->register_provider == REG_REGISTER_PROVIDER_REDEEM) {
        redeem_id = take_next_redeem_id(task);
        if (redeem_id <= 0) break;
        if (prepare_redeem_identity(db, redeem_id, &identity, redeem_code,
                                    sizeof(redeem_code), error,
                                    sizeof(error)) != 0) {
          mark_flow_launch_failed(
              task, error[0] ? error : "兑换码准备失败，跳过该兑换码");
          continue;
        }
      } else if (identity_generate(db, &identity, error, sizeof(error)) != 0) {
        mark_task_failed(task, error[0] ? error : "身份信息生成失败");
        fatal = true;
        break;
      }

      browser_profile_generate(&profile, NULL, NULL);
      proxy_rc = proxy_pool_pick_active_url(db, proxy_url, sizeof(proxy_url));
      if (proxy_rc < 0) {
        mark_task_failed(task, "读取代理池失败");
        fatal = true;
        break;
      }

      initial_stage = mark_flow_launch(task);
      if (launch_flow_job(task, task->workflow, task->register_provider,
                          &identity, &profile,
                          proxy_rc > 0 ? proxy_url : "", account_id,
                          workspace_id, redeem_id,
                          redeem_code[0] ? redeem_code : NULL,
                          initial_stage) != 0) {
        mark_flow_launch_failed(task, "任务流程启动 curl-impersonate 线程失败");
      }
    }

    if (fatal || task_is_done(task)) break;
    usleep(50000);
  }

  app_db_close(db);
  if (!fatal) mark_task_finished(task);
  return NULL;
}

int registration_tasks_start(const struct registration_start_options *options,
                             char *task_id, size_t task_id_len,
                             char *error, size_t error_len) {
  struct registration_task *task;
  int target_count, concurrency, max_inflight;
  enum registration_scheduler_mode scheduler_mode;

  if (task_id != NULL && task_id_len > 0) task_id[0] = '\0';
  if (options == NULL) {
    set_error(error, error_len, "任务参数为空");
    return -1;
  }
  if (options->workflow == REG_WORKFLOW_OAUTH_ONLY &&
      (options->account_ids == NULL || options->account_id_count == 0)) {
    set_error(error, error_len, "OAuth 任务缺少账号 ID");
    return -1;
  }
  if (options->register_provider == REG_REGISTER_PROVIDER_REDEEM &&
      options->workflow != REG_WORKFLOW_OAUTH_ONLY &&
      (options->redeem_ids == NULL || options->redeem_id_count == 0)) {
    set_error(error, error_len, "兑换码注册任务缺少兑换码 ID");
    return -1;
  }

  target_count = options->count <= 0 ? 1 : options->count;
  if (options->infinite && options->workflow != REG_WORKFLOW_OAUTH_ONLY) {
    target_count = 0;
  }
  if (options->workflow == REG_WORKFLOW_OAUTH_ONLY) {
    target_count = (int) options->account_id_count;
  } else if (options->register_provider == REG_REGISTER_PROVIDER_REDEEM) {
    target_count = (int) options->redeem_id_count;
  }
  if (target_count > 10000) target_count = 10000;
  concurrency = options->concurrency <= 0 ? 1 : options->concurrency;
  if (!options->infinite && concurrency > target_count) concurrency = target_count;
  if (concurrency > 5000) concurrency = 5000;
  if (concurrency <= 0) concurrency = 1;
  scheduler_mode = options->scheduler_mode == REG_SCHEDULER_FASTLANE &&
                           options->workflow != REG_WORKFLOW_OAUTH_ONLY
                       ? REG_SCHEDULER_FASTLANE
                       : REG_SCHEDULER_NORMAL;
  if (scheduler_mode == REG_SCHEDULER_FASTLANE &&
      concurrency > FASTLANE_MAX_INFLIGHT_LIMIT) {
    concurrency = FASTLANE_MAX_INFLIGHT_LIMIT;
  }
  max_inflight = concurrency;
  if (scheduler_mode == REG_SCHEDULER_FASTLANE) {
    if (options->max_inflight > 0) {
      max_inflight = options->max_inflight;
      if (!options->infinite && target_count > 0 && max_inflight > target_count) {
        max_inflight = target_count;
      }
      if (max_inflight < concurrency) max_inflight = concurrency;
    } else {
      max_inflight = concurrency * 3;
      if (max_inflight < FASTLANE_DEFAULT_MAX_INFLIGHT) {
        max_inflight = FASTLANE_DEFAULT_MAX_INFLIGHT;
      }
      if (!options->infinite && target_count > 0 && max_inflight > target_count) {
        max_inflight = target_count;
      }
      if (max_inflight < concurrency) max_inflight = concurrency;
    }
    if (max_inflight > FASTLANE_MAX_INFLIGHT_LIMIT) {
      max_inflight = FASTLANE_MAX_INFLIGHT_LIMIT;
    }
    if (max_inflight <= 0) max_inflight = concurrency;
  }

  task = (struct registration_task *) calloc(1, sizeof(*task));
  if (task == NULL) {
    set_error(error, error_len, "任务内存分配失败");
    return -1;
  }
  generate_task_id(task->id, sizeof(task->id));
  task->workflow = options->workflow;
  task->scheduler_mode = scheduler_mode;
  task->register_provider =
      options->register_provider == REG_REGISTER_PROVIDER_TEMPORARY
          ? REG_REGISTER_PROVIDER_TEMPORARY
      : options->register_provider == REG_REGISTER_PROVIDER_REDEEM
          ? REG_REGISTER_PROVIDER_REDEEM
          : REG_REGISTER_PROVIDER_PLATFORM;
  task->target_metric = options->workflow == REG_WORKFLOW_OAUTH_ONLY
                            ? REG_TARGET_OAUTH_SUCCESS
                            : options->target_metric;
  if (task->workflow == REG_WORKFLOW_REGISTER_ONLY) {
    task->target_metric = REG_TARGET_REGISTER_TASK;
  }
  task->target_count = target_count;
  task->concurrency = concurrency;
  task->max_inflight = max_inflight;
  task->oauth_delay_seconds = options->oauth_delay_seconds;
  if (task->oauth_delay_seconds < 0) task->oauth_delay_seconds = 0;
  if (task->oauth_delay_seconds > 3600) task->oauth_delay_seconds = 3600;
  task->detailed_logs = options->detailed_logs;
  task->auto_upload_oauth_success =
      options->auto_upload_oauth_success &&
      options->workflow != REG_WORKFLOW_REGISTER_ONLY;
  task->infinite = options->workflow == REG_WORKFLOW_OAUTH_ONLY ||
                           options->register_provider ==
                               REG_REGISTER_PROVIDER_REDEEM
                       ? false
                       : options->infinite;
  if (options->account_id_count > 0) {
    task->account_ids = (long *) calloc(options->account_id_count,
                                        sizeof(*task->account_ids));
    if (task->account_ids == NULL) {
      free(task);
      set_error(error, error_len, "账号 ID 列表内存分配失败");
      return -1;
    }
    memcpy(task->account_ids, options->account_ids,
           options->account_id_count * sizeof(*task->account_ids));
    task->account_id_count = options->account_id_count;
  }
  if (options->redeem_id_count > 0) {
    task->redeem_ids = (long *) calloc(options->redeem_id_count,
                                       sizeof(*task->redeem_ids));
    if (task->redeem_ids == NULL) {
      free(task->account_ids);
      free(task);
      set_error(error, error_len, "兑换码 ID 列表内存分配失败");
      return -1;
    }
    memcpy(task->redeem_ids, options->redeem_ids,
           options->redeem_id_count * sizeof(*task->redeem_ids));
    task->redeem_id_count = options->redeem_id_count;
  }
  mg_snprintf(task->status, sizeof(task->status), "queued");
  task->created_ms = now_ms();
  task->updated_ms = task->created_ms;

  pthread_mutex_lock(&s_tasks_mu);
  task->next = s_tasks;
  s_tasks = task;
  append_logf_locked(task, "", "info",
                     "任务已创建 workflow=%s scheduler=%s register=%s target=%s count=%d infinite=%s 并发=%d max_inflight=%d oauth_delay=%d auto_upload=%s",
                     workflow_name(task->workflow),
                     scheduler_mode_name(task->scheduler_mode),
                     task_register_label(task),
                     target_metric_name(task->target_metric),
                     task->target_count, task->infinite ? "yes" : "no",
                     task->concurrency, task->max_inflight,
                     task->oauth_delay_seconds,
                     task->auto_upload_oauth_success ? "yes" : "no");
  pthread_mutex_unlock(&s_tasks_mu);

  if (pthread_create(&task->thread, NULL, task_worker, task) != 0) {
    mark_task_failed(task, "任务线程创建失败");
    set_error(error, error_len, "任务线程创建失败");
    return -1;
  }
  pthread_detach(task->thread);
  mg_snprintf(task_id, task_id_len, "%s", task->id);
  return 0;
}

int registration_tasks_stop(const char *task_id, char *error, size_t error_len) {
  struct registration_task *task;
  int rc = -1;
  pthread_mutex_lock(&s_tasks_mu);
  task = find_task_locked(task_id ? task_id : "");
  if (task == NULL) {
    set_error(error, error_len, "任务不存在");
  } else if (strcmp(task->status, "running") != 0 &&
             strcmp(task->status, "queued") != 0) {
    set_error(error, error_len, "任务当前状态不可停止");
  } else {
    task->stop_requested = true;
    mg_snprintf(task->status, sizeof(task->status), "stopping");
    task->updated_ms = now_ms();
    append_log_locked(task, "", "warn", "已请求停止任务，将等待运行中的流程结束");
    rc = 0;
  }
  pthread_mutex_unlock(&s_tasks_mu);
  return rc;
}

static void append_task_json(struct mg_iobuf *io,
                             const struct registration_task *task) {
  int alive_total = task_fastlane_alive_unlocked(task);
  mg_xprintf(mg_pfn_iobuf, io, "{");
  mg_xprintf(mg_pfn_iobuf, io,
             "%m:%m,%m:%m,%m:%m,%m:%m,%m:%m,%m:%m,%m:%m",
             MG_ESC("id"), MG_ESC(task->id), MG_ESC("status"),
             MG_ESC(task->status), MG_ESC("mode"),
             MG_ESC(workflow_name(task->workflow)), MG_ESC("workflow"),
             MG_ESC(workflow_name(task->workflow)), MG_ESC("scheduler_mode"),
             MG_ESC(scheduler_mode_name(task->scheduler_mode)),
             MG_ESC("target_metric"),
             MG_ESC(target_metric_name(task->target_metric)),
             MG_ESC("register_provider"),
             MG_ESC(register_provider_name(task->register_provider)));
  mg_xprintf(mg_pfn_iobuf, io,
             ",%m:%d,%m:%d,%m:%d,%m:%d,%m:%d,%m:%d,%m:%d,%m:%d,%m:%d,%m:%d",
             MG_ESC("count"), task->target_count, MG_ESC("target_count"),
             task->target_count, MG_ESC("concurrency"), task->concurrency,
             MG_ESC("max_inflight"), task->max_inflight,
             MG_ESC("oauth_delay_seconds"), task->oauth_delay_seconds,
             MG_ESC("detailed_logs"), task->detailed_logs ? 1 : 0,
             MG_ESC("auto_upload_oauth_success"),
             task->auto_upload_oauth_success ? 1 : 0,
             MG_ESC("infinite"), task->infinite ? 1 : 0,
             MG_ESC("stop_requested"), task->stop_requested ? 1 : 0,
             MG_ESC("started"), task->started);
  mg_xprintf(mg_pfn_iobuf, io,
             ",%m:%d,%m:%d,%m:%d,%m:%d,%m:%d,%m:%d,%m:%d,%m:%d,%m:%d",
             MG_ESC("active"), task->active, MG_ESC("success"),
             task->success, MG_ESC("failed"), task->failed,
             MG_ESC("register_success"), task->register_success,
             MG_ESC("register_failed"), task->register_failed,
             MG_ESC("oauth_success"), task->oauth_success,
             MG_ESC("oauth_failed"), task->oauth_failed,
             MG_ESC("expired_written"), task->expired_written,
             MG_ESC("temp_written"), task->temp_written);
  mg_xprintf(mg_pfn_iobuf, io,
             ",%m:%d,%m:%d,%m:%d",
             MG_ESC("upload_success"), task->upload_success,
             MG_ESC("upload_failed"), task->upload_failed,
             MG_ESC("upload_skipped"), task->upload_skipped);
  mg_xprintf(mg_pfn_iobuf, io,
             ",%m:%d,%m:%d,%m:%d,%m:%d",
             MG_ESC("fastlane_pre_email_active"),
             task->fastlane_pre_email_active,
             MG_ESC("fastlane_waiting_email"),
             task->fastlane_waiting_email,
             MG_ESC("fastlane_post_email_active"),
             task->fastlane_post_email_active,
             MG_ESC("fastlane_alive_total"), alive_total);
  mg_xprintf(mg_pfn_iobuf, io,
             ",%m:%llu,%m:%llu,%m:%llu,%m:%llu,%m:%m}",
             MG_ESC("created_ms"), (unsigned long long) task->created_ms,
             MG_ESC("started_ms"), (unsigned long long) task->started_ms,
             MG_ESC("updated_ms"), (unsigned long long) task->updated_ms,
             MG_ESC("finished_ms"), (unsigned long long) task->finished_ms,
             MG_ESC("error"), MG_ESC(task->error));
}

char *registration_tasks_list_json(void) {
  struct mg_iobuf io = {0, 0, 0, 1024};
  bool first = true;

  pthread_mutex_lock(&s_tasks_mu);
  purge_expired_tasks_locked();
  mg_xprintf(mg_pfn_iobuf, &io, "{%m:[", MG_ESC("items"));
  for (struct registration_task *task = s_tasks; task != NULL; task = task->next) {
    if (!first) mg_xprintf(mg_pfn_iobuf, &io, ",");
    first = false;
    append_task_json(&io, task);
  }
  mg_xprintf(mg_pfn_iobuf, &io, "]}");
  pthread_mutex_unlock(&s_tasks_mu);
  mg_iobuf_add(&io, io.len, "", 1);
  return (char *) io.buf;
}

static void append_logs_json(struct mg_iobuf *io,
                             const struct registration_task *task,
                             uint64_t since_seq, uint64_t *last_seq) {
  bool first = true;
  mg_xprintf(mg_pfn_iobuf, io, "[");
  for (size_t i = 0; i < task->log_len; i++) {
    const struct registration_log_entry *log = &task->logs[i];
    if (log->seq <= since_seq) continue;
    if (!first) mg_xprintf(mg_pfn_iobuf, io, ",");
    first = false;
    if (last_seq != NULL && log->seq > *last_seq) *last_seq = log->seq;
    mg_xprintf(mg_pfn_iobuf, io,
               "{%m:%llu,%m:%llu,%m:%m,%m:%m,%m:%m}",
               MG_ESC("seq"), (unsigned long long) log->seq,
               MG_ESC("ts_ms"), (unsigned long long) log->ts_ms,
               MG_ESC("level"), MG_ESC(log->level), MG_ESC("flow_id"),
               MG_ESC(log->flow_id), MG_ESC("message"), MG_ESC(log->message));
  }
  mg_xprintf(mg_pfn_iobuf, io, "]");
}

char *registration_task_detail_json(const char *task_id, bool include_logs) {
  struct mg_iobuf io = {0, 0, 0, 2048};
  struct registration_task *task;

  pthread_mutex_lock(&s_tasks_mu);
  purge_expired_tasks_locked();
  task = find_task_locked(task_id ? task_id : "");
  if (task == NULL) {
    mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:%m}", MG_ESC("ok"), 0,
               MG_ESC("error"), MG_ESC("任务不存在"));
  } else {
    mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:", MG_ESC("ok"), 1,
               MG_ESC("task"));
    append_task_json(&io, task);
    if (include_logs) {
      mg_xprintf(mg_pfn_iobuf, &io, ",%m:", MG_ESC("logs"));
      append_logs_json(&io, task, 0, NULL);
    }
    mg_xprintf(mg_pfn_iobuf, &io, "}");
  }
  pthread_mutex_unlock(&s_tasks_mu);
  mg_iobuf_add(&io, io.len, "", 1);
  return (char *) io.buf;
}

void registration_tasks_counts(int *active_tasks, int *active_flows,
                               int *queued_flows) {
  int tasks = 0, active = 0, queued = 0;
  pthread_mutex_lock(&s_tasks_mu);
  purge_expired_tasks_locked();
  for (struct registration_task *task = s_tasks; task != NULL; task = task->next) {
    if (strcmp(task->status, "running") == 0 ||
        strcmp(task->status, "queued") == 0 ||
        strcmp(task->status, "stopping") == 0) {
      tasks++;
      active += task->active;
      if (task->workflow == REG_WORKFLOW_OAUTH_ONLY) {
        queued += (int) (task->account_id_count - task->next_account_index);
      } else if (!task->infinite) {
        int remain = task->target_count - task_target_progress_unlocked(task);
        queued += remain > 0 ? remain : 0;
      }
    }
  }
  pthread_mutex_unlock(&s_tasks_mu);
  if (active_tasks != NULL) *active_tasks = tasks;
  if (active_flows != NULL) *active_flows = active;
  if (queued_flows != NULL) *queued_flows = queued < 0 ? 0 : queued;
}

static unsigned ws_count_connections(struct mg_mgr *mgr) {
  unsigned count = 0;
  if (mgr == NULL) return 0;
  for (struct mg_connection *c = mgr->conns; c != NULL; c = c->next) count++;
  return count;
}

static uint64_t clamp_system_interval(long interval_ms) {
  if (interval_ms <= 0) return 3000;
  if (interval_ms < 1000) return 1000;
  if (interval_ms > 60000) return 60000;
  return (uint64_t) interval_ms;
}

static void send_system_status(struct mg_connection *c, sqlite3 *db,
                               uint64_t started_ms) {
  char *json;
  uint64_t current_ms = mg_millis();
  uint64_t uptime = current_ms >= started_ms ? current_ms - started_ms : 0;
  if (c == NULL || !c->is_websocket) return;
  json = system_monitor_status_json(db, uptime, ws_count_connections(c->mgr));
  mg_ws_printf(c, WEBSOCKET_OP_TEXT, "{%m:%m,%m:%s}",
               MG_ESC("type"), MG_ESC("system_status"),
               MG_ESC("payload"), json ? json : "{}");
  free(json);
}

static char *logs_since_json(const char *task_id, uint64_t since_seq,
                             uint64_t *last_seq, bool *has_logs) {
  struct mg_iobuf io = {0, 0, 0, 2048};
  struct registration_task *task;
  uint64_t before = last_seq != NULL ? *last_seq : since_seq;

  if (has_logs != NULL) *has_logs = false;
  pthread_mutex_lock(&s_tasks_mu);
  purge_expired_tasks_locked();
  task = find_task_locked(task_id ? task_id : "");
  if (task == NULL) {
    mg_xprintf(mg_pfn_iobuf, &io, "[]");
  } else {
    if (last_seq != NULL && *last_seq < since_seq) *last_seq = since_seq;
    append_logs_json(&io, task, since_seq, last_seq);
    if (has_logs != NULL && last_seq != NULL && *last_seq > before) {
      *has_logs = true;
    }
  }
  pthread_mutex_unlock(&s_tasks_mu);
  mg_iobuf_add(&io, io.len, "", 1);
  return (char *) io.buf;
}

void registration_ws_open(struct mg_connection *c) {
  struct registration_ws_state *state;
  if (c == NULL) return;
  state = (struct registration_ws_state *) calloc(1, sizeof(*state));
  c->fn_data = state;
}

void registration_ws_close(struct mg_connection *c) {
  if (c == NULL) return;
  free(c->fn_data);
  c->fn_data = NULL;
}

bool registration_ws_handle_message(struct mg_connection *c, struct mg_str data,
                                    sqlite3 *db, uint64_t started_ms) {
  struct registration_ws_state *state;
  char *type;
  char *task_id;

  if (c == NULL || !c->is_websocket) return false;
  state = (struct registration_ws_state *) c->fn_data;
  if (state == NULL) return false;

  type = mg_json_get_str(data, "$.type");
  task_id = mg_json_get_str(data, "$.task_id");
  if (type != NULL && strcmp(type, "system_subscribe") == 0) {
    long interval_ms = mg_json_get_long(data, "$.interval_ms", 3000);
    state->system_subscribed = true;
    state->system_interval_ms = clamp_system_interval(interval_ms);
    state->last_system_status_ms = 0;
    mg_ws_printf(c, WEBSOCKET_OP_TEXT,
                 "{%m:%m,%m:%llu}", MG_ESC("type"),
                 MG_ESC("system_subscribed"), MG_ESC("interval_ms"),
                 (unsigned long long) state->system_interval_ms);
    send_system_status(c, db, started_ms);
    state->last_system_status_ms = now_ms();
  } else if (type != NULL && strcmp(type, "system_unsubscribe") == 0) {
    state->system_subscribed = false;
    mg_ws_printf(c, WEBSOCKET_OP_TEXT,
                 "{%m:%m}", MG_ESC("type"), MG_ESC("system_unsubscribed"));
  } else if (type != NULL &&
      (strcmp(type, "registration_subscribe") == 0 ||
       strcmp(type, "subscribe_registration") == 0 ||
       strcmp(type, "subscribe") == 0) &&
      task_id != NULL && task_id[0] != '\0') {
    char *detail;
    mg_snprintf(state->task_id, sizeof(state->task_id), "%s", task_id);
    state->last_seq = 0;
    state->last_status_ms = 0;
    mg_ws_printf(c, WEBSOCKET_OP_TEXT,
                 "{%m:%m,%m:%m}", MG_ESC("type"),
                 MG_ESC("registration_subscribed"), MG_ESC("task_id"),
                 MG_ESC(state->task_id));
    detail = registration_task_detail_json(state->task_id, false);
    mg_ws_printf(c, WEBSOCKET_OP_TEXT,
                 "{%m:%m,%m:%s}", MG_ESC("type"),
                 MG_ESC("registration_task"), MG_ESC("payload"),
                 detail ? detail : "{}");
    free(detail);
  } else {
    mg_ws_printf(c, WEBSOCKET_OP_TEXT,
                 "{%m:%m,%m:%m}", MG_ESC("type"), MG_ESC("echo"),
                 MG_ESC("status"), MG_ESC("ok"));
  }
  mg_free(type);
  mg_free(task_id);
  return true;
}

void registration_ws_poll(struct mg_mgr *mgr, sqlite3 *db, uint64_t started_ms) {
  static uint64_t s_last_poll_ms;
  uint64_t now = now_ms();
  if (mgr == NULL) return;
  if (now - s_last_poll_ms < 200) return;
  s_last_poll_ms = now;
  for (struct mg_connection *c = mgr->conns; c != NULL; c = c->next) {
    struct registration_ws_state *state =
        (struct registration_ws_state *) c->fn_data;
    if (!c->is_websocket || state == NULL) continue;

    if (state->system_subscribed &&
        now - state->last_system_status_ms >= state->system_interval_ms) {
      send_system_status(c, db, started_ms);
      state->last_system_status_ms = now;
    }

    if (state->task_id[0] == '\0') continue;

    bool has_logs = false;
    uint64_t last_seq = state->last_seq;
    char *logs = logs_since_json(state->task_id, state->last_seq, &last_seq,
                                 &has_logs);
    if (has_logs) {
      mg_ws_printf(c, WEBSOCKET_OP_TEXT,
                   "{%m:%m,%m:%m,%m:%s}", MG_ESC("type"),
                   MG_ESC("registration_logs"), MG_ESC("task_id"),
                   MG_ESC(state->task_id), MG_ESC("logs"), logs ? logs : "[]");
      state->last_seq = last_seq;
    }
    free(logs);

    if (now - state->last_status_ms >= 1000) {
      char *detail = registration_task_detail_json(state->task_id, false);
      mg_ws_printf(c, WEBSOCKET_OP_TEXT,
                   "{%m:%m,%m:%s}", MG_ESC("type"),
                   MG_ESC("registration_task"), MG_ESC("payload"),
                   detail ? detail : "{}");
      free(detail);
      state->last_status_ms = now;
    }
  }
}
