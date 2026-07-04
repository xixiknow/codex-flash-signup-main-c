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

#endif
