#include "cmny_internal.h"

#include <stdio.h>

static void v4_error(sqlite3 *handle, char *err, size_t err_size,
                     const char *context, const char *detail) {
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s: %s", context,
                       detail != NULL ? detail : sqlite3_errmsg(handle));
    }
}

static bool v4_exec(sqlite3 *handle, const char *sql, char *err, size_t err_size) {
    char *detail = NULL;
    int rc = sqlite3_exec(handle, sql, NULL, NULL, &detail);
    if (rc == SQLITE_OK) return true;
    v4_error(handle, err, err_size, "v4 migration failed", detail);
    sqlite3_free(detail);
    return false;
}

static bool v4_count(sqlite3 *handle, const char *sql, sqlite3_int64 *value,
                     char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_type(stmt, 0) == SQLITE_INTEGER;
    if (ok) *value = sqlite3_column_int64(stmt, 0);
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) v4_error(handle, err, err_size, "cannot verify v4 migration", NULL);
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_migrate_v4(sqlite3 *handle, char *err, size_t err_size) {
    if (handle == NULL) {
        v4_error(handle, err, err_size, "cannot migrate ledger", "no database handle");
        return false;
    }
    static const char *steps[] = {
        "BEGIN IMMEDIATE;"
        "CREATE TABLE history_actions("
        " id INTEGER PRIMARY KEY,"
        " action_type INTEGER NOT NULL CHECK(action_type BETWEEN 1 AND 4),"
        " entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE RESTRICT,"
        " entry_type INTEGER NOT NULL CHECK(entry_type BETWEEN 1 AND 3),"
        " before_revision INTEGER CHECK(before_revision IS NULL OR before_revision>0),"
        " after_revision INTEGER CHECK(after_revision IS NULL OR after_revision>0),"
        " created_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " undone_at INTEGER"
        ") STRICT;"
        "CREATE TABLE history_entry_snapshots("
        " action_id INTEGER NOT NULL REFERENCES history_actions(id) ON DELETE CASCADE,"
        " snapshot_side INTEGER NOT NULL CHECK(snapshot_side IN(1,2)),"
        " entry_id INTEGER NOT NULL,entry_type INTEGER NOT NULL CHECK(entry_type BETWEEN 1 AND 3),"
        " occurred_on TEXT NOT NULL,payee TEXT NOT NULL,note TEXT NOT NULL,"
        " origin INTEGER NOT NULL,voided_at INTEGER,created_at INTEGER NOT NULL,"
        " updated_at INTEGER NOT NULL,revision INTEGER NOT NULL CHECK(revision>0),"
        " PRIMARY KEY(action_id,snapshot_side)"
        ") STRICT;"
        "CREATE TABLE history_posting_snapshots("
        " action_id INTEGER NOT NULL REFERENCES history_actions(id) ON DELETE CASCADE,"
        " snapshot_side INTEGER NOT NULL CHECK(snapshot_side IN(1,2)),"
        " posting_ordinal INTEGER NOT NULL CHECK(posting_ordinal BETWEEN 0 AND 255),"
        " original_posting_id INTEGER NOT NULL,account_id INTEGER NOT NULL,"
        " amount_minor INTEGER NOT NULL,clear_state INTEGER NOT NULL,memo TEXT NOT NULL,"
        " sort_order INTEGER NOT NULL,PRIMARY KEY(action_id,snapshot_side,posting_ordinal)"
        ") STRICT;",
        "CREATE TABLE history_allocation_snapshots("
        " action_id INTEGER NOT NULL REFERENCES history_actions(id) ON DELETE CASCADE,"
        " snapshot_side INTEGER NOT NULL CHECK(snapshot_side IN(1,2)),"
        " posting_ordinal INTEGER NOT NULL CHECK(posting_ordinal BETWEEN 0 AND 255),"
        " allocation_ordinal INTEGER NOT NULL CHECK(allocation_ordinal BETWEEN 0 AND 255),"
        " original_allocation_id INTEGER NOT NULL,category_id INTEGER NOT NULL,"
        " amount_minor INTEGER NOT NULL,note TEXT NOT NULL,sort_order INTEGER NOT NULL,"
        " PRIMARY KEY(action_id,snapshot_side,posting_ordinal,allocation_ordinal)"
        ") STRICT;"
        "CREATE TABLE history_tag_snapshots("
        " action_id INTEGER NOT NULL REFERENCES history_actions(id) ON DELETE CASCADE,"
        " snapshot_side INTEGER NOT NULL CHECK(snapshot_side IN(1,2)),"
        " tag_id INTEGER NOT NULL,PRIMARY KEY(action_id,snapshot_side,tag_id)"
        ") STRICT;"
        "CREATE INDEX history_actions_recent ON history_actions(id DESC);"
        "CREATE INDEX history_actions_by_entry ON history_actions(entry_id,id DESC);"
        "CREATE TABLE reconciliation_sessions("
        " id INTEGER PRIMARY KEY,account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,"
        " statement_on TEXT NOT NULL CHECK(statement_on GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'"
        " AND date(statement_on,'+0 days') IS NOT NULL AND date(statement_on,'+0 days')=statement_on),"
        " statement_balance_minor INTEGER NOT NULL,"
        " status INTEGER NOT NULL DEFAULT 1 CHECK(status BETWEEN 1 AND 3),"
        " revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),"
        " created_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " updated_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " finalized_at INTEGER,"
        " final_account_balance_minor INTEGER,"
        " final_cleared_balance_minor INTEGER,"
        " final_discrepancy_minor INTEGER,"
        " CHECK((status=2 AND finalized_at IS NOT NULL"
        " AND final_account_balance_minor IS NOT NULL"
        " AND final_cleared_balance_minor IS NOT NULL"
        " AND final_discrepancy_minor IS NOT NULL"
        " AND final_discrepancy_minor=statement_balance_minor-final_cleared_balance_minor)"
        " OR (status<>2 AND finalized_at IS NULL"
        " AND final_account_balance_minor IS NULL"
        " AND final_cleared_balance_minor IS NULL"
        " AND final_discrepancy_minor IS NULL))"
        ") STRICT;"
        "CREATE UNIQUE INDEX reconciliation_one_open_account"
        " ON reconciliation_sessions(account_id) WHERE status=1;",
        "CREATE TABLE reconciliation_items("
        " session_id INTEGER NOT NULL REFERENCES reconciliation_sessions(id) ON DELETE CASCADE,"
        " posting_id INTEGER NOT NULL REFERENCES postings(id) ON DELETE RESTRICT,"
        " prior_clear_state INTEGER NOT NULL CHECK(prior_clear_state BETWEEN 0 AND 2),"
        " PRIMARY KEY(session_id,posting_id)"
        ") STRICT;"
        "CREATE INDEX reconciliation_items_posting ON reconciliation_items(posting_id);"
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        if (!v4_exec(handle, steps[i], err, err_size)) {
            (void)sqlite3_exec(handle, "ROLLBACK", NULL, NULL, NULL);
            return false;
        }
    }

    sqlite3_int64 table_count = 0;
    sqlite3_int64 foreign_key_failures = 0;
    bool verified = v4_count(handle,
        "SELECT COUNT(*) FROM sqlite_schema WHERE type='table' AND name IN("
        "'history_actions','history_entry_snapshots','history_posting_snapshots',"
        "'history_allocation_snapshots','history_tag_snapshots',"
        "'reconciliation_sessions','reconciliation_items')", &table_count, err, err_size) &&
        v4_count(handle, "SELECT COUNT(*) FROM pragma_foreign_key_check",
                 &foreign_key_failures, err, err_size);
    if (!verified || table_count != 7 || foreign_key_failures != 0) {
        if (verified) v4_error(handle, err, err_size, "v4 migration verification failed",
                               "schema or foreign keys are incomplete");
        (void)sqlite3_exec(handle, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    if (!v4_exec(handle, "PRAGMA user_version=4;COMMIT;", err, err_size)) {
        (void)sqlite3_exec(handle, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    return true;
}
