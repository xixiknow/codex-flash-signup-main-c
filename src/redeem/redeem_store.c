#include "redeem/redeem_store.h"

#include "mongoose.h"

#include <ctype.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *column_text(sqlite3_stmt *stmt, int col) {
  const unsigned char *s = sqlite3_column_text(stmt, col);
  return s == NULL ? "" : (const char *) s;
}

static long clamp_limit(long limit) {
  if (limit <= 0) return 50;
  if (limit > 200) return 200;
  return limit;
}

static void append_row(struct mg_iobuf *io, sqlite3_stmt *stmt) {
  mg_xprintf(
      mg_pfn_iobuf, io,
      "{%m:%lld,%m:%m,%m:%m,%m:%m,%m:%m,%m:%lld,%m:%lld,%m:%m,%m:%lld,%m:%m,"
      "%m:%lld,%m:%lld}",
      MG_ESC("id"), sqlite3_column_int64(stmt, 0), MG_ESC("code"),
      MG_ESC(column_text(stmt, 1)), MG_ESC("status"),
      MG_ESC(column_text(stmt, 2)), MG_ESC("email"),
      MG_ESC(column_text(stmt, 3)), MG_ESC("product_name"),
      MG_ESC(column_text(stmt, 4)), MG_ESC("card_id"),
      sqlite3_column_int64(stmt, 5), MG_ESC("session_id"),
      sqlite3_column_int64(stmt, 6), MG_ESC("end_time"),
      MG_ESC(column_text(stmt, 7)), MG_ESC("account_id"),
      sqlite3_column_int64(stmt, 8), MG_ESC("last_error"),
      MG_ESC(column_text(stmt, 9)), MG_ESC("created_at"),
      sqlite3_column_int64(stmt, 10), MG_ESC("updated_at"),
      sqlite3_column_int64(stmt, 11));
}

char *redeem_list_json(sqlite3 *db, const char *status, long cursor,
                       long limit) {
  sqlite3_stmt *stmt = NULL;
  struct mg_iobuf io = {0, 0, 0, 1024};
  long effective_limit = clamp_limit(limit);
  long bind_limit = effective_limit + 1;
  long next_cursor = 0;
  int returned = 0, row_count = 0;
  bool has_more = false;
  const char *sql =
      "SELECT id,code,status,COALESCE(email,''),COALESCE(product_name,''),"
      "COALESCE(card_id,0),COALESCE(session_id,0),COALESCE(end_time,''),"
      "COALESCE(account_id,0),COALESCE(last_error,''),created_at,updated_at "
      "FROM redeem_codes "
      "WHERE (?1='' OR status=?1) AND (?2=0 OR id < ?2) "
      "ORDER BY id DESC LIMIT ?3";

  mg_xprintf(mg_pfn_iobuf, &io, "{\"items\":[");
  if (db != NULL && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, status ? status : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, cursor > 0 ? cursor : 0);
    sqlite3_bind_int64(stmt, 3, bind_limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      row_count++;
      if (row_count > effective_limit) {
        has_more = true;
        break;
      }
      if (returned > 0) mg_xprintf(mg_pfn_iobuf, &io, ",");
      append_row(&io, stmt);
      next_cursor = (long) sqlite3_column_int64(stmt, 0);
      returned++;
    }
  }
  sqlite3_finalize(stmt);
  if (!has_more) next_cursor = 0;
  mg_xprintf(mg_pfn_iobuf, &io, "],%m:%d,%m:%ld,%m:%ld,%m:%d}",
             MG_ESC("has_more"), has_more, MG_ESC("next_cursor"), next_cursor,
             MG_ESC("limit"), effective_limit, MG_ESC("returned"), returned);
  mg_iobuf_add(&io, io.len, "", 1);
  return (char *) io.buf;
}

/* 兑换码形如 XXXX-XXXX-XXXX-XXXX，逐行提取；保留原始字符（大写字母/数字/连字符）。 */
static char *normalize_code(char *line) {
  char *p = line;
  char *w = line;
  while (*p && isspace((unsigned char) *p)) p++;
  for (; *p; p++) {
    unsigned char ch = (unsigned char) *p;
    if (isalnum(ch) || ch == '-') {
      *w++ = (char) toupper(ch);
    } else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      break;
    }
  }
  *w = '\0';
  return line;
}

char *redeem_import_json(sqlite3 *db, const char *text) {
  sqlite3_stmt *stmt = NULL;
  struct mg_iobuf io = {0, 0, 0, 256};
  int imported = 0, skipped = 0, invalid = 0;
  char *copy, *line, *saveptr = NULL;
  const char *sql =
      "INSERT INTO redeem_codes(code,status,created_at,updated_at) "
      "VALUES(?,'new',unixepoch(),unixepoch()) "
      "ON CONFLICT(code) DO NOTHING";

  if (text == NULL) text = "";
  copy = strdup(text);
  if (copy == NULL) {
    mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:%m}", MG_ESC("ok"), 0,
               MG_ESC("error"), MG_ESC("内存分配失败"));
    mg_iobuf_add(&io, io.len, "", 1);
    return (char *) io.buf;
  }
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    free(copy);
    mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:%m}", MG_ESC("ok"), 0,
               MG_ESC("error"), MG_ESC(sqlite3_errmsg(db)));
    mg_iobuf_add(&io, io.len, "", 1);
    return (char *) io.buf;
  }

  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  for (line = strtok_r(copy, "\n", &saveptr); line != NULL;
       line = strtok_r(NULL, "\n", &saveptr)) {
    char *code = normalize_code(line);
    if (code[0] == '\0') continue;
    if (strlen(code) < 4) {
      invalid++;
      continue;
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, code, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_DONE) {
      if (sqlite3_changes(db) > 0) imported++;
      else skipped++;
    } else {
      invalid++;
    }
  }
  sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  sqlite3_finalize(stmt);
  free(copy);

  mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:%d,%m:%d,%m:%d}", MG_ESC("ok"), 1,
             MG_ESC("imported"), imported, MG_ESC("skipped"), skipped,
             MG_ESC("invalid"), invalid);
  mg_iobuf_add(&io, io.len, "", 1);
  return (char *) io.buf;
}

int redeem_delete_ids(sqlite3 *db, const long *ids, size_t count) {
  sqlite3_stmt *stmt = NULL;
  int deleted = 0;
  if (db == NULL || ids == NULL || count == 0) return 0;
  if (sqlite3_prepare_v2(db, "DELETE FROM redeem_codes WHERE id=?", -1, &stmt,
                         NULL) != SQLITE_OK) {
    return -1;
  }
  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  for (size_t i = 0; i < count; i++) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int64(stmt, 1, ids[i]);
    if (sqlite3_step(stmt) == SQLITE_DONE) deleted += sqlite3_changes(db);
  }
  sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  sqlite3_finalize(stmt);
  return deleted;
}

int redeem_load_code(sqlite3 *db, long id, char *code, size_t code_len,
                     char *email, size_t email_len) {
  sqlite3_stmt *stmt = NULL;
  int rc = -1;
  if (code != NULL && code_len > 0) code[0] = '\0';
  if (email != NULL && email_len > 0) email[0] = '\0';
  if (db == NULL || id <= 0) return -1;
  if (sqlite3_prepare_v2(db,
                         "SELECT code,COALESCE(email,'') FROM redeem_codes "
                         "WHERE id=?",
                         -1, &stmt, NULL) != SQLITE_OK) {
    return -1;
  }
  sqlite3_bind_int64(stmt, 1, id);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    if (code != NULL) mg_snprintf(code, code_len, "%s", column_text(stmt, 0));
    if (email != NULL) mg_snprintf(email, email_len, "%s", column_text(stmt, 1));
    rc = 0;
  }
  sqlite3_finalize(stmt);
  return rc;
}

int redeem_mark_redeemed(sqlite3 *db, long id, const struct redeem_result *r) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  const char *sql =
      "UPDATE redeem_codes SET status='redeemed',email=?,product_name=?,"
      "card_id=?,session_id=?,end_time=?,last_error=NULL,updated_at=unixepoch() "
      "WHERE id=?";
  if (db == NULL || r == NULL) return -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(stmt, 1, r->email, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, r->product_name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, r->card_id);
  sqlite3_bind_int64(stmt, 4, r->session_id);
  sqlite3_bind_text(stmt, 5, r->end_time, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 6, id);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int redeem_mark_registered(sqlite3 *db, long id, long account_id) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  const char *sql =
      "UPDATE redeem_codes SET status='registered',account_id=?,"
      "last_error=NULL,updated_at=unixepoch() WHERE id=?";
  if (db == NULL) return -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int64(stmt, 1, account_id);
  sqlite3_bind_int64(stmt, 2, id);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int redeem_mark_failed(sqlite3 *db, long id, const char *error) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  const char *sql =
      "UPDATE redeem_codes SET status='failed',last_error=?,"
      "updated_at=unixepoch() WHERE id=?";
  if (db == NULL) return -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(stmt, 1, error ? error : "兑换失败", -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, id);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

char *redeem_redeem_ids_json(sqlite3 *db, const long *ids, size_t count) {
  struct mg_iobuf io = {0, 0, 0, 512};
  int success = 0, failed = 0;
  bool first = true;

  mg_xprintf(mg_pfn_iobuf, &io, "{%m:%d,%m:[", MG_ESC("ok"), 1,
             MG_ESC("details"));
  for (size_t i = 0; i < count; i++) {
    char code[128] = "";
    char error[256] = "";
    struct redeem_result result;
    int rc;

    if (!first) mg_xprintf(mg_pfn_iobuf, &io, ",");
    first = false;

    if (redeem_load_code(db, ids[i], code, sizeof(code), NULL, 0) != 0) {
      failed++;
      mg_xprintf(mg_pfn_iobuf, &io, "{%m:%ld,%m:%d,%m:%m}", MG_ESC("id"),
                 ids[i], MG_ESC("ok"), 0, MG_ESC("error"),
                 MG_ESC("兑换码不存在"));
      continue;
    }
    rc = redeem_apply(db, code, &result, error, sizeof(error));
    if (rc == 0) {
      redeem_mark_redeemed(db, ids[i], &result);
      success++;
      mg_xprintf(mg_pfn_iobuf, &io, "{%m:%ld,%m:%d,%m:%m,%m:%m}", MG_ESC("id"),
                 ids[i], MG_ESC("ok"), 1, MG_ESC("email"), MG_ESC(result.email),
                 MG_ESC("product_name"), MG_ESC(result.product_name));
    } else {
      redeem_mark_failed(db, ids[i], error);
      failed++;
      mg_xprintf(mg_pfn_iobuf, &io, "{%m:%ld,%m:%d,%m:%m}", MG_ESC("id"),
                 ids[i], MG_ESC("ok"), 0, MG_ESC("error"), MG_ESC(error));
    }
  }
  mg_xprintf(mg_pfn_iobuf, &io, "],%m:%d,%m:%d}", MG_ESC("success_count"),
             success, MG_ESC("failed_count"), failed);
  mg_iobuf_add(&io, io.len, "", 1);
  return (char *) io.buf;
}
