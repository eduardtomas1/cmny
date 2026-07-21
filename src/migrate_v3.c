#include "cmny_internal.h"

#include <stdio.h>

static void migration_error(sqlite3 *handle, char *err, size_t err_size,
                            const char *context, const char *detail) {
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s: %s", context,
                       detail != NULL ? detail : sqlite3_errmsg(handle));
    }
}

static bool migration_exec(sqlite3 *handle, const char *sql, char *err, size_t err_size,
                           const char *context) {
    char *detail = NULL;
    int rc = sqlite3_exec(handle, sql, NULL, NULL, &detail);
    if (rc == SQLITE_OK) return true;
    migration_error(handle, err, err_size, context, detail);
    sqlite3_free(detail);
    return false;
}

static bool query_int64(sqlite3 *handle, const char *sql, sqlite3_int64 *value,
                        char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_type(stmt, 0) == SQLITE_INTEGER;
    if (ok) *value = sqlite3_column_int64(stmt, 0);
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) migration_error(handle, err, err_size, "cannot verify v3 migration", NULL);
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_migrate_v3(sqlite3 *handle, char *err, size_t err_size) {
    if (handle == NULL) {
        migration_error(handle, err, err_size, "cannot migrate ledger", "no database handle");
        return false;
    }

    static const char *create_sql[] = {
        "BEGIN IMMEDIATE;"
        "CREATE TABLE accounts ("
        " id INTEGER PRIMARY KEY,"
        " name TEXT NOT NULL UNIQUE CHECK(length(name) BETWEEN 1 AND 64 AND name NOT GLOB '*[^ -~]*'),"
        " account_type INTEGER NOT NULL CHECK(account_type BETWEEN 1 AND 7),"
        " currency_code TEXT NOT NULL CHECK(length(currency_code)=3 AND currency_code GLOB '[A-Z][A-Z][A-Z]'),"
        " institution TEXT NOT NULL DEFAULT '' CHECK(length(institution)<=64 AND institution NOT GLOB '*[^ -~]*'),"
        " archived INTEGER NOT NULL DEFAULT 0 CHECK(archived IN (0,1)),"
        " sort_order INTEGER NOT NULL DEFAULT 0,"
        " created_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " updated_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0)"
        ") STRICT;"
        "CREATE TABLE categories ("
        " id INTEGER PRIMARY KEY,"
        " parent_id INTEGER REFERENCES categories(id) ON DELETE RESTRICT,"
        " merged_into_id INTEGER REFERENCES categories(id) ON DELETE RESTRICT,"
        " name TEXT NOT NULL UNIQUE CHECK(length(name) BETWEEN 1 AND 32 AND name NOT GLOB '*[^ -~]*'),"
        " marker TEXT NOT NULL DEFAULT '' CHECK(length(marker)<=8 AND marker NOT GLOB '*[^ -~]*'),"
        " color TEXT NOT NULL DEFAULT '' CHECK(length(color)<=16 AND color NOT GLOB '*[^ -~]*'),"
        " kind_mask INTEGER NOT NULL CHECK(kind_mask BETWEEN 1 AND 3),"
        " archived INTEGER NOT NULL DEFAULT 0 CHECK(archived IN (0,1)),"
        " created_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " updated_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),"
        " CHECK(parent_id IS NULL OR parent_id<>id),"
        " CHECK(merged_into_id IS NULL OR (merged_into_id<>id AND archived=1))"
        ") STRICT;"
        "CREATE TABLE tags ("
        " id INTEGER PRIMARY KEY,"
        " name TEXT NOT NULL UNIQUE CHECK(length(name) BETWEEN 1 AND 32 AND name NOT GLOB '*[^ -~]*'),"
        " archived INTEGER NOT NULL DEFAULT 0 CHECK(archived IN (0,1)),"
        " created_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " updated_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0)"
        ") STRICT;",
        "CREATE TABLE entries ("
        " id INTEGER PRIMARY KEY,"
        " entry_type INTEGER NOT NULL CHECK(entry_type BETWEEN 1 AND 3),"
        " occurred_on TEXT NOT NULL CHECK(occurred_on GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'"
        "   AND date(occurred_on,'+0 days') IS NOT NULL AND date(occurred_on,'+0 days')=occurred_on),"
        " payee TEXT NOT NULL DEFAULT '' CHECK(length(payee)<=96 AND payee NOT GLOB '*[^ -~]*'),"
        " note TEXT NOT NULL DEFAULT '' CHECK(length(note)<=240 AND note NOT GLOB '*[^ -~]*'),"
        " origin INTEGER NOT NULL DEFAULT 1 CHECK(origin BETWEEN 1 AND 4),"
        " voided_at INTEGER,"
        " created_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " updated_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0)"
        ") STRICT;"
        "CREATE TABLE postings ("
        " id INTEGER PRIMARY KEY,"
        " entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,"
        " account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,"
        " amount_minor INTEGER NOT NULL CHECK(amount_minor<>0 AND amount_minor BETWEEN -9000000000000000 AND 9000000000000000),"
        " clear_state INTEGER NOT NULL DEFAULT 0 CHECK(clear_state BETWEEN 0 AND 2),"
        " memo TEXT NOT NULL DEFAULT '' CHECK(length(memo)<=120 AND memo NOT GLOB '*[^ -~]*'),"
        " sort_order INTEGER NOT NULL CHECK(sort_order>=0),"
        " UNIQUE(entry_id,sort_order)"
        ") STRICT;"
        "CREATE TABLE allocations ("
        " id INTEGER PRIMARY KEY,"
        " posting_id INTEGER NOT NULL REFERENCES postings(id) ON DELETE CASCADE,"
        " category_id INTEGER NOT NULL REFERENCES categories(id) ON DELETE RESTRICT,"
        " amount_minor INTEGER NOT NULL CHECK(amount_minor<>0 AND amount_minor BETWEEN -9000000000000000 AND 9000000000000000),"
        " note TEXT NOT NULL DEFAULT '' CHECK(length(note)<=120 AND note NOT GLOB '*[^ -~]*'),"
        " sort_order INTEGER NOT NULL CHECK(sort_order>=0),"
        " UNIQUE(posting_id,sort_order)"
        ") STRICT;"
        "CREATE TABLE entry_tags ("
        " entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,"
        " tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE RESTRICT,"
        " PRIMARY KEY(entry_id,tag_id)"
        ") STRICT;"
        "CREATE INDEX entries_by_date ON entries(occurred_on DESC,id DESC);"
        "CREATE INDEX postings_by_account ON postings(account_id,entry_id);"
        "CREATE INDEX allocations_by_category ON allocations(category_id,posting_id);",
        "INSERT INTO accounts(name,account_type,currency_code,sort_order)"
        " VALUES('Cash',1,COALESCE((SELECT value FROM settings WHERE key='currency'),'EUR'),0);"
        "INSERT INTO categories(name,kind_mask)"
        " SELECT category,MAX(mask) FROM ("
        "   SELECT category,CASE kind WHEN 1 THEN 1 ELSE 2 END AS mask FROM transactions"
        "   UNION ALL SELECT category,1 FROM budgets"
        "   UNION ALL SELECT category,CASE kind WHEN 1 THEN 1 ELSE 2 END FROM recurring"
        " ) GROUP BY category;"
        "UPDATE categories SET kind_mask=("
        " SELECT (MAX(CASE WHEN x.mask=1 THEN 1 ELSE 0 END)"
        "       +2*MAX(CASE WHEN x.mask=2 THEN 1 ELSE 0 END)) FROM ("
        "   SELECT category,CASE kind WHEN 1 THEN 1 ELSE 2 END AS mask FROM transactions"
        "   UNION ALL SELECT category,1 FROM budgets"
        "   UNION ALL SELECT category,CASE kind WHEN 1 THEN 1 ELSE 2 END FROM recurring"
        " ) AS x WHERE x.category=categories.name);"
        "ALTER TABLE transactions RENAME TO legacy_transactions_v2;"
        "INSERT INTO entries(id,entry_type,occurred_on,note,origin,created_at,updated_at)"
        " SELECT id,1,occurred_on,note,4,created_at,updated_at FROM legacy_transactions_v2;"
        "INSERT INTO postings(id,entry_id,account_id,amount_minor,sort_order)"
        " SELECT id,id,1,CASE kind WHEN 1 THEN -amount_cents ELSE amount_cents END,0"
        " FROM legacy_transactions_v2;"
        "INSERT INTO allocations(id,posting_id,category_id,amount_minor,sort_order)"
        " SELECT t.id,t.id,c.id,CASE t.kind WHEN 1 THEN -t.amount_cents ELSE t.amount_cents END,0"
        " FROM legacy_transactions_v2 AS t JOIN categories AS c ON c.name=t.category;"
    };

    for (size_t i = 0; i < sizeof(create_sql) / sizeof(create_sql[0]); i++) {
        if (!migration_exec(handle, create_sql[i], err, err_size, "v3 migration failed")) {
            (void)sqlite3_exec(handle, "ROLLBACK", NULL, NULL, NULL);
            return false;
        }
    }

    sqlite3_int64 legacy_count = 0;
    sqlite3_int64 migrated_count = 0;
    sqlite3_int64 mismatch_count = 0;
    sqlite3_int64 invalid_count = 0;
    sqlite3_int64 foreign_key_failures = 0;
    bool verified =
        query_int64(handle, "SELECT COUNT(*) FROM legacy_transactions_v2", &legacy_count,
                    err, err_size) &&
        query_int64(handle, "SELECT COUNT(*) FROM entries WHERE entry_type=1 AND origin=4",
                    &migrated_count, err, err_size) &&
        query_int64(handle,
            "SELECT COUNT(*) FROM legacy_transactions_v2 t"
            " JOIN entries e ON e.id=t.id JOIN postings p ON p.entry_id=e.id"
            " JOIN allocations a ON a.posting_id=p.id JOIN categories c ON c.id=a.category_id"
            " WHERE e.entry_type<>1 OR e.occurred_on<>t.occurred_on OR e.note<>t.note"
            " OR p.account_id<>1 OR p.amount_minor<>CASE t.kind WHEN 1 THEN -t.amount_cents ELSE t.amount_cents END"
            " OR a.amount_minor<>p.amount_minor OR c.name<>t.category",
            &mismatch_count, err, err_size) &&
        query_int64(handle,
            "SELECT COUNT(*) FROM entries AS e WHERE"
            " (e.entry_type=1 AND ((SELECT COUNT(*) FROM postings p WHERE p.entry_id=e.id)<>1"
            " OR (SELECT COUNT(*) FROM allocations a JOIN postings p ON p.id=a.posting_id"
            "     WHERE p.entry_id=e.id)=0"
            " OR (SELECT total(p.amount_minor) FROM postings p WHERE p.entry_id=e.id)<>"
            "    (SELECT total(a.amount_minor) FROM allocations a JOIN postings p ON p.id=a.posting_id"
            "     WHERE p.entry_id=e.id)))"
            " OR (e.entry_type=2 AND ((SELECT COUNT(*) FROM postings p WHERE p.entry_id=e.id)<>2"
            " OR (SELECT total(p.amount_minor) FROM postings p WHERE p.entry_id=e.id)<>0.0"
            " OR EXISTS(SELECT 1 FROM allocations a JOIN postings p ON p.id=a.posting_id"
            "           WHERE p.entry_id=e.id)))",
            &invalid_count, err, err_size) &&
        query_int64(handle, "SELECT COUNT(*) FROM pragma_foreign_key_check",
                    &foreign_key_failures, err, err_size);

    if (!verified || legacy_count != migrated_count || mismatch_count != 0 || invalid_count != 0 ||
        foreign_key_failures != 0) {
        if (verified) migration_error(handle, err, err_size, "v3 migration verification failed",
                                      "row counts or ledger invariants do not match");
        (void)sqlite3_exec(handle, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }

    static const char *finish_sql =
        "DROP TABLE legacy_transactions_v2;"
        "CREATE VIEW transactions AS"
        " SELECT e.id,CASE WHEN p.amount_minor<0 THEN 1 ELSE 2 END AS kind,"
        " CASE WHEN p.amount_minor<0 THEN -p.amount_minor ELSE p.amount_minor END AS amount_cents,"
        " CASE WHEN (SELECT COUNT(*) FROM allocations ac WHERE ac.posting_id=p.id)=1"
        "      THEN (SELECT c.name FROM allocations ac JOIN categories c ON c.id=ac.category_id"
        "            WHERE ac.posting_id=p.id LIMIT 1) ELSE 'Split' END AS category,"
        " e.note,e.occurred_on,e.created_at,e.updated_at"
        " FROM entries e JOIN postings p ON p.entry_id=e.id"
        " WHERE e.entry_type=1 AND e.voided_at IS NULL;"
        "PRAGMA user_version=3;"
        "COMMIT;";
    if (!migration_exec(handle, finish_sql, err, err_size, "cannot finish v3 migration")) {
        (void)sqlite3_exec(handle, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    return true;
}
