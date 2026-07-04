#ifndef APP_REGISTRATION_TASKS_H
#define APP_REGISTRATION_TASKS_H

#include "flow/flow_engine.h"
#include "mongoose.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>

#define REG_TASK_ID_LEN 32

enum registration_workflow {
  REG_WORKFLOW_REGISTER_ONLY = 0,
  REG_WORKFLOW_REGISTER_THEN_OAUTH,
  REG_WORKFLOW_OAUTH_ONLY
};

enum registration_scheduler_mode {
  REG_SCHEDULER_NORMAL = 0,
  REG_SCHEDULER_FASTLANE
};

enum registration_target_metric {
  REG_TARGET_REGISTER_TASK = 0,
  REG_TARGET_OAUTH_SUCCESS
};

enum registration_register_provider {
  REG_REGISTER_PROVIDER_PLATFORM = 0,
  REG_REGISTER_PROVIDER_TEMPORARY,
  REG_REGISTER_PROVIDER_REDEEM
};

struct registration_start_options {
  enum registration_workflow workflow;
  enum registration_scheduler_mode scheduler_mode;
  enum registration_target_metric target_metric;
  enum registration_register_provider register_provider;
  int count;
  int concurrency;
  int max_inflight;
  int oauth_delay_seconds;
  bool detailed_logs;
  bool infinite;
  bool auto_upload_oauth_success;
  const long *account_ids;
  size_t account_id_count;
  const long *redeem_ids;
  size_t redeem_id_count;
  /* 兑换码路径：注册+OAuth 完成后，对以下每个外部目标工作区执行
   * 上车 + 推送 aether（换行/逗号/空格分隔的工作区 ID 列表）。 */
  const char *target_workspaces;
  const char *aether_pool_type;
};

int registration_tasks_start(const struct registration_start_options *options,
                             char *task_id, size_t task_id_len,
                             char *error, size_t error_len);
char *registration_tasks_list_json(void);
char *registration_task_detail_json(const char *task_id, bool include_logs);
void registration_tasks_counts(int *active_tasks, int *active_flows,
                               int *queued_flows);
int registration_tasks_stop(const char *task_id, char *error, size_t error_len);

void registration_ws_open(struct mg_connection *c);
void registration_ws_close(struct mg_connection *c);
bool registration_ws_handle_message(struct mg_connection *c,
                                    struct mg_str data, sqlite3 *db,
                                    uint64_t started_ms);
void registration_ws_poll(struct mg_mgr *mgr, sqlite3 *db, uint64_t started_ms);

#endif
