#include "storage/app_db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

static int exec_sql(sqlite3 *db, const char *sql) {
  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "sqlite error: %s\n", err ? err : sqlite3_errmsg(db));
    sqlite3_free(err);
    return -1;
  }
  return 0;
}

static int ensure_data_dir(void) {
  if (mkdir("data", 0755) == 0) return 0;
  return 0;
}

static int migrate(sqlite3 *db) {
  const char *sql =
      "CREATE TABLE IF NOT EXISTS proxy_nodes ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "scheme TEXT NOT NULL,"
      "host TEXT NOT NULL,"
      "port INTEGER NOT NULL,"
      "username TEXT,"
      "password TEXT,"
      "proxy_url TEXT NOT NULL UNIQUE,"
      "label TEXT,"
      "status TEXT NOT NULL DEFAULT 'new',"
      "last_test_ok INTEGER,"
      "last_http_status INTEGER,"
      "exit_ip TEXT,"
      "exit_loc TEXT,"
      "exit_colo TEXT,"
      "trace_http TEXT,"
      "trace_tls TEXT,"
      "last_error TEXT,"
      "last_tested_at INTEGER,"
      "created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_proxy_nodes_status "
      "ON proxy_nodes(status);"
      "CREATE INDEX IF NOT EXISTS idx_proxy_nodes_exit_loc "
      "ON proxy_nodes(exit_loc);"
      "CREATE TABLE IF NOT EXISTS mail_settings ("
      "key TEXT PRIMARY KEY,"
      "value TEXT NOT NULL,"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
      ");"
      "CREATE TABLE IF NOT EXISTS mail_domain_rules ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "pattern TEXT NOT NULL UNIQUE,"
      "base_domain TEXT NOT NULL,"
      "wildcard_depth INTEGER NOT NULL DEFAULT 0,"
      "is_active INTEGER NOT NULL DEFAULT 1,"
      "created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_mail_domain_rules_active "
      "ON mail_domain_rules(is_active);"
      "INSERT OR IGNORE INTO mail_settings(key,value) "
      "VALUES('rapid_inbox_base_url','http://127.0.0.1:8000');"
      "CREATE TABLE IF NOT EXISTS auth_users ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "username TEXT NOT NULL COLLATE NOCASE UNIQUE,"
      "password_hash TEXT NOT NULL,"
      "password_salt TEXT NOT NULL,"
      "iterations INTEGER NOT NULL,"
      "created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch()),"
      "last_login_at INTEGER"
      ");"
      "CREATE TABLE IF NOT EXISTS auth_sessions ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "user_id INTEGER NOT NULL,"
      "token_hash TEXT NOT NULL UNIQUE,"
      "ip_address TEXT,"
      "user_agent TEXT,"
      "expires_at INTEGER NOT NULL,"
      "created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
      "last_seen_at INTEGER,"
      "FOREIGN KEY(user_id) REFERENCES auth_users(id) ON DELETE CASCADE"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_auth_sessions_hash_expires "
      "ON auth_sessions(token_hash,expires_at);"
      "CREATE INDEX IF NOT EXISTS idx_auth_sessions_user_expires "
      "ON auth_sessions(user_id,expires_at);"
      "CREATE TABLE IF NOT EXISTS auth_login_attempts ("
      "ip_address TEXT PRIMARY KEY,"
      "failed_count INTEGER NOT NULL DEFAULT 0,"
      "locked_until INTEGER NOT NULL DEFAULT 0,"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
      ");"
      "CREATE TABLE IF NOT EXISTS accounts ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "email TEXT NOT NULL COLLATE NOCASE UNIQUE,"
      "status TEXT NOT NULL DEFAULT 'active' "
      "CHECK(status IN ('active','expired','temp','failed')),"
      "upload_state TEXT NOT NULL DEFAULT 'not_uploaded' "
      "CHECK(upload_state IN ('uploaded','not_uploaded')),"
      "created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch()),"
      "last_refreshed_at INTEGER"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_accounts_status_upload_id "
      "ON accounts(status,upload_state,id DESC);"
      "CREATE INDEX IF NOT EXISTS idx_accounts_upload_id "
      "ON accounts(upload_state,id DESC);"
      "CREATE INDEX IF NOT EXISTS idx_accounts_created_id "
      "ON accounts(created_at DESC,id DESC);"
      "CREATE INDEX IF NOT EXISTS idx_accounts_last_refreshed "
      "ON accounts(last_refreshed_at DESC);"
      "CREATE TABLE IF NOT EXISTS account_secrets ("
      "account_id INTEGER PRIMARY KEY,"
      "password TEXT,"
      "access_token TEXT,"
      "refresh_token TEXT,"
      "external_account_id TEXT,"
      "workspace_id TEXT,"
      "created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch()),"
      "FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_account_secrets_external_account "
      "ON account_secrets(external_account_id);"
      "CREATE INDEX IF NOT EXISTS idx_account_secrets_workspace "
      "ON account_secrets(workspace_id);"
      "CREATE TABLE IF NOT EXISTS account_stats ("
      "id INTEGER PRIMARY KEY CHECK(id=1),"
      "total INTEGER NOT NULL DEFAULT 0,"
      "active_count INTEGER NOT NULL DEFAULT 0,"
      "expired_count INTEGER NOT NULL DEFAULT 0,"
      "temp_count INTEGER NOT NULL DEFAULT 0,"
      "failed_count INTEGER NOT NULL DEFAULT 0,"
      "uploaded_count INTEGER NOT NULL DEFAULT 0,"
      "not_uploaded_count INTEGER NOT NULL DEFAULT 0,"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
      ");"
      "INSERT OR IGNORE INTO account_stats("
      "id,total,active_count,expired_count,temp_count,failed_count,"
      "uploaded_count,not_uploaded_count) VALUES(1,0,0,0,0,0,0,0);"
      "CREATE TRIGGER IF NOT EXISTS trg_accounts_stats_insert "
      "AFTER INSERT ON accounts BEGIN "
      "UPDATE account_stats SET "
      "total=total+1,"
      "active_count=active_count+CASE WHEN NEW.status='active' THEN 1 ELSE 0 END,"
      "expired_count=expired_count+CASE WHEN NEW.status='expired' THEN 1 ELSE 0 END,"
      "temp_count=temp_count+CASE WHEN NEW.status='temp' THEN 1 ELSE 0 END,"
      "failed_count=failed_count+CASE WHEN NEW.status='failed' THEN 1 ELSE 0 END,"
      "uploaded_count=uploaded_count+CASE WHEN NEW.upload_state='uploaded' THEN 1 ELSE 0 END,"
      "not_uploaded_count=not_uploaded_count+CASE WHEN NEW.upload_state='not_uploaded' THEN 1 ELSE 0 END,"
      "updated_at=unixepoch() WHERE id=1;"
      "END;"
      "CREATE TRIGGER IF NOT EXISTS trg_accounts_stats_delete "
      "AFTER DELETE ON accounts BEGIN "
      "UPDATE account_stats SET "
      "total=total-1,"
      "active_count=active_count-CASE WHEN OLD.status='active' THEN 1 ELSE 0 END,"
      "expired_count=expired_count-CASE WHEN OLD.status='expired' THEN 1 ELSE 0 END,"
      "temp_count=temp_count-CASE WHEN OLD.status='temp' THEN 1 ELSE 0 END,"
      "failed_count=failed_count-CASE WHEN OLD.status='failed' THEN 1 ELSE 0 END,"
      "uploaded_count=uploaded_count-CASE WHEN OLD.upload_state='uploaded' THEN 1 ELSE 0 END,"
      "not_uploaded_count=not_uploaded_count-CASE WHEN OLD.upload_state='not_uploaded' THEN 1 ELSE 0 END,"
      "updated_at=unixepoch() WHERE id=1;"
      "END;"
      "CREATE TRIGGER IF NOT EXISTS trg_accounts_stats_update "
      "AFTER UPDATE OF status,upload_state ON accounts BEGIN "
      "UPDATE account_stats SET "
      "active_count=active_count"
      "+CASE WHEN NEW.status='active' THEN 1 ELSE 0 END"
      "-CASE WHEN OLD.status='active' THEN 1 ELSE 0 END,"
      "expired_count=expired_count"
      "+CASE WHEN NEW.status='expired' THEN 1 ELSE 0 END"
      "-CASE WHEN OLD.status='expired' THEN 1 ELSE 0 END,"
      "temp_count=temp_count"
      "+CASE WHEN NEW.status='temp' THEN 1 ELSE 0 END"
      "-CASE WHEN OLD.status='temp' THEN 1 ELSE 0 END,"
      "failed_count=failed_count"
      "+CASE WHEN NEW.status='failed' THEN 1 ELSE 0 END"
      "-CASE WHEN OLD.status='failed' THEN 1 ELSE 0 END,"
      "uploaded_count=uploaded_count"
      "+CASE WHEN NEW.upload_state='uploaded' THEN 1 ELSE 0 END"
      "-CASE WHEN OLD.upload_state='uploaded' THEN 1 ELSE 0 END,"
      "not_uploaded_count=not_uploaded_count"
      "+CASE WHEN NEW.upload_state='not_uploaded' THEN 1 ELSE 0 END"
      "-CASE WHEN OLD.upload_state='not_uploaded' THEN 1 ELSE 0 END,"
      "updated_at=unixepoch() WHERE id=1;"
      "END;"
      "CREATE TABLE IF NOT EXISTS aether_services ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "name TEXT NOT NULL,"
      "api_url TEXT NOT NULL,"
      "management_token TEXT NOT NULL,"
      "provider_id TEXT NOT NULL,"
      "provider_name TEXT,"
      "chatgpt_web_provider_id TEXT,"
      "chatgpt_web_provider_name TEXT,"
      "proxy_node_id TEXT,"
      "proxy_node_name TEXT,"
      "enabled INTEGER NOT NULL DEFAULT 1,"
      "priority INTEGER NOT NULL DEFAULT 0,"
      "created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_aether_services_enabled_priority "
      "ON aether_services(enabled,priority,id);"
      "CREATE TABLE IF NOT EXISTS aether_upload_stats ("
      "id INTEGER PRIMARY KEY CHECK(id=1),"
      "total_attempted INTEGER NOT NULL DEFAULT 0,"
      "success_count INTEGER NOT NULL DEFAULT 0,"
      "failed_count INTEGER NOT NULL DEFAULT 0,"
      "skipped_count INTEGER NOT NULL DEFAULT 0,"
      "last_success_at INTEGER,"
      "last_failed_at INTEGER,"
      "last_message TEXT,"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
      ");"
      "INSERT OR IGNORE INTO aether_upload_stats("
      "id,total_attempted,success_count,failed_count,skipped_count) "
      "VALUES(1,0,0,0,0);"
      "CREATE TABLE IF NOT EXISTS redeem_codes ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "code TEXT NOT NULL UNIQUE,"
      "status TEXT NOT NULL DEFAULT 'new' "
      "CHECK(status IN ('new','redeemed','registered','failed')),"
      "email TEXT,"
      "product_name TEXT,"
      "card_id INTEGER,"
      "session_id INTEGER,"
      "end_time TEXT,"
      "account_id INTEGER,"
      "last_error TEXT,"
      "created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_redeem_codes_status "
      "ON redeem_codes(status,id DESC);"
      "INSERT OR IGNORE INTO mail_settings(key,value) "
      "VALUES('redeem_base_url','https://sms.paymesh.cn');";
  return exec_sql(db, sql);
}

int app_db_open(const char *path, sqlite3 **db) {
  int rc;
  if (db == NULL) return -1;
  *db = NULL;
  ensure_data_dir();
  rc = sqlite3_open(path, db);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "cannot open sqlite db %s: %s\n", path, sqlite3_errmsg(*db));
    app_db_close(*db);
    *db = NULL;
    return -1;
  }
  sqlite3_busy_timeout(*db, 5000);
  if (exec_sql(*db, "PRAGMA journal_mode=WAL;") != 0 ||
      exec_sql(*db, "PRAGMA synchronous=NORMAL;") != 0 ||
      exec_sql(*db, "PRAGMA foreign_keys=ON;") != 0 ||
      migrate(*db) != 0) {
    app_db_close(*db);
    *db = NULL;
    return -1;
  }
  return 0;
}

void app_db_close(sqlite3 *db) {
  if (db != NULL) sqlite3_close(db);
}
