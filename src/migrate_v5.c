#include "cmny_internal.h"

#include <stdio.h>

static void v5_error(sqlite3 *handle, char *err, size_t err_size,
                     const char *context, const char *detail) {
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s: %s", context,
                       detail != NULL ? detail : sqlite3_errmsg(handle));
    }
}

static bool v5_exec(sqlite3 *handle, const char *sql, char *err, size_t err_size) {
    char *detail = NULL;
    int rc = sqlite3_exec(handle, sql, NULL, NULL, &detail);
    if (rc == SQLITE_OK) return true;
    v5_error(handle, err, err_size, "v5 migration failed", detail);
    sqlite3_free(detail);
    return false;
}

static bool v5_count(sqlite3 *handle, const char *sql, sqlite3_int64 *value,
                     char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_type(stmt, 0) == SQLITE_INTEGER;
    if (ok) *value = sqlite3_column_int64(stmt, 0);
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) v5_error(handle, err, err_size, "cannot verify v5 migration", NULL);
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_migrate_v5(sqlite3 *handle, char *err, size_t err_size) {
    if (handle == NULL) {
        v5_error(handle, err, err_size, "cannot migrate imports", "no database handle");
        return false;
    }
    static const char *steps[] = {
        "BEGIN IMMEDIATE;"
        "CREATE TABLE import_profiles("
        " id INTEGER PRIMARY KEY,"
        " name TEXT NOT NULL COLLATE NOCASE UNIQUE"
        "  CHECK(length(name) BETWEEN 1 AND 64 AND name NOT GLOB '*[^ -~]*'),"
        " delimiter INTEGER NOT NULL CHECK(delimiter IN(0,9,44,59)),"
        " date_column INTEGER NOT NULL CHECK(date_column BETWEEN 0 AND 63),"
        " amount_column INTEGER NOT NULL CHECK(amount_column BETWEEN -1 AND 63),"
        " debit_column INTEGER NOT NULL CHECK(debit_column BETWEEN -1 AND 63),"
        " credit_column INTEGER NOT NULL CHECK(credit_column BETWEEN -1 AND 63),"
        " payee_column INTEGER NOT NULL CHECK(payee_column BETWEEN -1 AND 63),"
        " note_column INTEGER NOT NULL CHECK(note_column BETWEEN -1 AND 63),"
        " external_id_column INTEGER NOT NULL CHECK(external_id_column BETWEEN -1 AND 63),"
        " date_format INTEGER NOT NULL CHECK(date_format BETWEEN 0 AND 2),"
        " sign_convention INTEGER NOT NULL CHECK(sign_convention BETWEEN 0 AND 3),"
        " decimal_separator INTEGER NOT NULL CHECK(decimal_separator IN(44,46)),"
        " thousands_separator INTEGER NOT NULL CHECK(thousands_separator IN(0,32,39,44,46)),"
        " revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),"
        " created_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " updated_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " CHECK(decimal_separator<>thousands_separator),"
        " CHECK((amount_column>=0 AND debit_column=-1 AND credit_column=-1)"
        "  OR (amount_column=-1 AND debit_column>=0 AND credit_column>=0))"
        ") STRICT;",
        "CREATE TABLE categorization_rules("
        " id INTEGER PRIMARY KEY,"
        " name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 64"
        "  AND name NOT GLOB '*[^ -~]*'),"
        " enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN(0,1)),"
        " sort_order INTEGER NOT NULL CHECK(sort_order BETWEEN 0 AND 2147483647),"
        " account_id INTEGER REFERENCES accounts(id) ON DELETE RESTRICT,"
        " payee_mode INTEGER NOT NULL CHECK(payee_mode BETWEEN 0 AND 2),"
        " payee_pattern TEXT NOT NULL DEFAULT '' CHECK(length(payee_pattern)<=256"
        "  AND payee_pattern NOT GLOB '*[^ -~]*'),"
        " note_mode INTEGER NOT NULL CHECK(note_mode BETWEEN 0 AND 2),"
        " note_pattern TEXT NOT NULL DEFAULT '' CHECK(length(note_pattern)<=512"
        "  AND note_pattern NOT GLOB '*[^ -~]*'),"
        " minimum_amount_minor INTEGER CHECK(minimum_amount_minor BETWEEN"
        "  -9000000000000000 AND 9000000000000000),"
        " maximum_amount_minor INTEGER CHECK(maximum_amount_minor BETWEEN"
        "  -9000000000000000 AND 9000000000000000),"
        " category_id INTEGER NOT NULL REFERENCES categories(id) ON DELETE RESTRICT,"
        " tag_id INTEGER REFERENCES tags(id) ON DELETE RESTRICT,"
        " revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),"
        " created_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " updated_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " CHECK((payee_mode=0 AND payee_pattern='') OR (payee_mode<>0 AND payee_pattern<>'')),"
        " CHECK((note_mode=0 AND note_pattern='') OR (note_mode<>0 AND note_pattern<>'')),"
        " CHECK(minimum_amount_minor IS NULL OR maximum_amount_minor IS NULL"
        "  OR minimum_amount_minor<=maximum_amount_minor)"
        ") STRICT;"
        "CREATE INDEX categorization_rules_ordered"
        " ON categorization_rules(enabled,sort_order,id);",
        "CREATE TABLE import_batches("
        " id INTEGER PRIMARY KEY,"
        " account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,"
        " profile_id INTEGER REFERENCES import_profiles(id) ON DELETE SET NULL,"
        " source_name TEXT NOT NULL CHECK(length(source_name) BETWEEN 1 AND 255"
        "  AND source_name NOT GLOB '*[^ -~]*'),"
        " delimiter INTEGER NOT NULL CHECK(delimiter IN(0,9,44,59)),"
        " heuristic_policy INTEGER NOT NULL CHECK(heuristic_policy IN(1,2)),"
        " status INTEGER NOT NULL DEFAULT 1 CHECK(status IN(1,2)),"
        " input_rows INTEGER NOT NULL DEFAULT 0 CHECK(input_rows>=0),"
        " normalized_rows INTEGER NOT NULL DEFAULT 0 CHECK(normalized_rows>=0),"
        " imported_rows INTEGER NOT NULL DEFAULT 0 CHECK(imported_rows>=0),"
        " hard_duplicates INTEGER NOT NULL DEFAULT 0 CHECK(hard_duplicates>=0),"
        " heuristic_duplicates INTEGER NOT NULL DEFAULT 0 CHECK(heuristic_duplicates>=0),"
        " heuristic_skipped INTEGER NOT NULL DEFAULT 0 CHECK(heuristic_skipped>=0),"
        " revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),"
        " created_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " updated_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " rolled_back_at INTEGER,"
        " CHECK(normalized_rows=imported_rows+hard_duplicates+heuristic_skipped),"
        " CHECK(input_rows=normalized_rows),"
        " CHECK(heuristic_skipped<=heuristic_duplicates),"
        " CHECK(heuristic_duplicates<=imported_rows+heuristic_skipped),"
        " CHECK((status=1 AND rolled_back_at IS NULL) OR"
        "  (status=2 AND rolled_back_at IS NOT NULL))"
        ") STRICT;"
        "CREATE INDEX import_batches_account_recent ON import_batches(account_id,id DESC);",
        "CREATE TABLE import_records("
        " id INTEGER PRIMARY KEY,"
        " batch_id INTEGER NOT NULL REFERENCES import_batches(id) ON DELETE CASCADE,"
        " account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,"
        " entry_id INTEGER REFERENCES entries(id) ON DELETE RESTRICT,"
        " duplicate_of_record_id INTEGER REFERENCES import_records(id) ON DELETE RESTRICT,"
        " rule_id INTEGER REFERENCES categorization_rules(id) ON DELETE SET NULL,"
        " category_id INTEGER REFERENCES categories(id) ON DELETE RESTRICT,"
        " tag_id INTEGER REFERENCES tags(id) ON DELETE RESTRICT,"
        " decision INTEGER NOT NULL CHECK(decision BETWEEN 1 AND 3),"
        " dedupe_active INTEGER NOT NULL CHECK(dedupe_active IN(0,1)),"
        " heuristic_duplicate INTEGER NOT NULL DEFAULT 0 CHECK(heuristic_duplicate IN(0,1)),"
        " occurred_on TEXT NOT NULL CHECK(occurred_on GLOB"
        "  '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'"
        "  AND date(occurred_on,'+0 days')=occurred_on),"
        " amount_minor INTEGER NOT NULL CHECK(amount_minor<>0 AND amount_minor BETWEEN"
        "  -9000000000000000 AND 9000000000000000),"
        " payee TEXT NOT NULL CHECK(length(payee)<=256),"
        " note TEXT NOT NULL CHECK(length(note)<=512),"
        " external_id TEXT NOT NULL CHECK(length(external_id)<=128),"
        " identity TEXT NOT NULL CHECK(length(identity) BETWEEN 1 AND 1099),"
        " physical_line INTEGER NOT NULL CHECK(physical_line>0),"
        " record_number INTEGER NOT NULL CHECK(record_number>0),"
        " created_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " CHECK((decision=1 AND entry_id IS NOT NULL AND category_id IS NOT NULL)"
        "  OR (decision IN(2,3) AND entry_id IS NULL AND category_id IS NULL AND tag_id IS NULL)),"
        " CHECK((decision=2 AND external_id<>'' AND duplicate_of_record_id IS NOT NULL)"
        "  OR decision<>2),"
        " CHECK((heuristic_duplicate=1 AND duplicate_of_record_id IS NOT NULL)"
        "  OR heuristic_duplicate=0),"
        " CHECK((decision=3 AND heuristic_duplicate=1) OR decision<>3)"
        " ,CHECK(decision=1 OR dedupe_active=0)"
        ") STRICT;",
        "CREATE UNIQUE INDEX import_records_entry ON import_records(entry_id)"
        " WHERE entry_id IS NOT NULL;"
        "CREATE UNIQUE INDEX import_records_external_exact"
        " ON import_records(account_id,external_id)"
        " WHERE decision=1 AND dedupe_active=1 AND external_id<>'';"
        "CREATE INDEX import_records_identity"
        " ON import_records(account_id,identity,id);"
        "CREATE INDEX import_records_batch ON import_records(batch_id,id);"
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        if (!v5_exec(handle, steps[i], err, err_size)) {
            (void)sqlite3_exec(handle, "ROLLBACK", NULL, NULL, NULL);
            return false;
        }
    }

    sqlite3_int64 table_count = 0;
    sqlite3_int64 foreign_key_failures = 0;
    bool verified = v5_count(handle,
        "SELECT COUNT(*) FROM sqlite_schema WHERE type='table' AND name IN("
        "'import_profiles','categorization_rules','import_batches','import_records')",
        &table_count, err, err_size) &&
        v5_count(handle, "SELECT COUNT(*) FROM pragma_foreign_key_check",
                 &foreign_key_failures, err, err_size);
    if (!verified || table_count != 4 || foreign_key_failures != 0) {
        if (verified) v5_error(handle, err, err_size, "v5 migration verification failed",
                               "schema or foreign keys are incomplete");
        (void)sqlite3_exec(handle, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    if (!v5_exec(handle, "PRAGMA user_version=5;COMMIT;", err, err_size)) {
        (void)sqlite3_exec(handle, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    return true;
}
