#ifndef APP_AETHER_UPLOAD_H
#define APP_AETHER_UPLOAD_H

#include "mongoose.h"

#include <sqlite3.h>
#include <stddef.h>

char *aether_config_json(sqlite3 *db);
char *aether_service_save_json(sqlite3 *db, struct mg_str body);
char *aether_service_delete_json(sqlite3 *db, struct mg_str body);
char *aether_service_test_json(sqlite3 *db, struct mg_str body);
char *aether_options_json(sqlite3 *db, struct mg_str body);
char *aether_upload_accounts_json(sqlite3 *db, const long *ids, size_t count,
                                  const char *pool_type);

/* 显式凭证推送：不依赖账号库中的 workspace_id，调用方可为每个目标工作区
 * 传入独立的 workspace_id，用于兑换码路径「每个目标工作区推送一次」。 */
struct aether_push_credential {
  const char *email;
  const char *access_token;
  const char *refresh_token;
  const char *external_account_id;
  const char *workspace_id;
};

char *aether_upload_credential_json(sqlite3 *db,
                                    const struct aether_push_credential *cred,
                                    const char *pool_type);

#endif
