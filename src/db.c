#include "cmny.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CMNY_APPLICATION_ID 1129139801
#define CMNY_SCHEMA_VERSION 1

static void set_error(char *err, size_t err_size, const char *message) {
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s", message != NULL ? message : "unknown database error");
    }
}

static void set_db_error(CmnyDb *db, char *err, size_t err_size, const char *context) {
    if (err == NULL || err_size == 0) {
        return;
    }
    const char *detail = db != NULL && db->handle != NULL ? sqlite3_errmsg(db->handle) : "no database handle";
    (void)snprintf(err, err_size, "%s: %s", context, detail);
}

static bool exec_sql(CmnyDb *db, const char *sql, char *err, size_t err_size) {
    char *sqlite_error = NULL;
    int rc = sqlite3_exec(db->handle, sql, NULL, NULL, &sqlite_error);
    if (rc == SQLITE_OK) {
        return true;
    }
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s", sqlite_error != NULL ? sqlite_error : sqlite3_errmsg(db->handle));
    }
    sqlite3_free(sqlite_error);
    return false;
}

static bool read_pragma_int(CmnyDb *db, const char *sql, int *value, char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        set_db_error(db, err, err_size, "cannot read database metadata");
        return false;
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *value = sqlite3_column_int(stmt, 0);
    }
    bool ok = rc == SQLITE_ROW;
    if (!ok) {
        set_db_error(db, err, err_size, "cannot read database metadata");
    }
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool migrate(CmnyDb *db, char *err, size_t err_size) {
    int application_id = 0;
    int version = 0;
    if (!read_pragma_int(db, "PRAGMA application_id", &application_id, err, err_size) ||
        !read_pragma_int(db, "PRAGMA user_version", &version, err, err_size)) {
        return false;
    }
    if (application_id != 0 && application_id != CMNY_APPLICATION_ID) {
        set_error(err, err_size, "the selected SQLite file belongs to another application");
        return false;
    }
    sqlite3_stmt *objects = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT COUNT(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'", -1, &objects, NULL);
    if (rc != SQLITE_OK || sqlite3_step(objects) != SQLITE_ROW) {
        set_db_error(db, err, err_size, "cannot inspect database schema");
        (void)sqlite3_finalize(objects);
        return false;
    }
    int object_count = sqlite3_column_int(objects, 0);
    (void)sqlite3_finalize(objects);
    if (application_id == 0 && (version != 0 || object_count != 0)) {
        set_error(err, err_size, "the selected SQLite file is nonempty and is not a CMNY database");
        return false;
    }
    if (version > CMNY_SCHEMA_VERSION) {
        set_error(err, err_size, "database schema is newer than this CMNY build");
        return false;
    }
    if (version < CMNY_SCHEMA_VERSION) {
        static const char *migration =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE transactions ("
        " id INTEGER PRIMARY KEY,"
        " kind INTEGER NOT NULL CHECK(kind IN (1, 2)),"
        " amount_cents INTEGER NOT NULL CHECK(amount_cents > 0 AND amount_cents <= 9000000000000000),"
        " category TEXT NOT NULL CHECK(length(category) BETWEEN 1 AND 32"
        "   AND category NOT GLOB '*[^ -~]*'),"
        " note TEXT NOT NULL DEFAULT '' CHECK(length(note) <= 120"
        "   AND note NOT GLOB '*[^ -~]*'),"
        " occurred_on TEXT NOT NULL CHECK("
        "   occurred_on GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'"
        "   AND date(occurred_on, '+0 days') IS NOT NULL"
        "   AND date(occurred_on, '+0 days') = occurred_on),"
        " created_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " updated_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER))"
        ") STRICT;"
        "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL) STRICT;"
        "CREATE INDEX transactions_by_date ON transactions(occurred_on DESC, id DESC);"
        "CREATE INDEX transactions_by_kind_date ON transactions(kind, occurred_on DESC);"
        "PRAGMA application_id = 1129139801;"
        "PRAGMA user_version = 1;"
        "COMMIT;";

        if (!exec_sql(db, migration, err, err_size)) {
            (void)sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
            return false;
        }
    }

    sqlite3_stmt *verify = NULL;
    rc = sqlite3_prepare_v2(db->handle,
        "SELECT t.id, t.kind, t.amount_cents, t.category, t.note, t.occurred_on, s.key, s.value "
        "FROM transactions AS t LEFT JOIN settings AS s ON 0 LIMIT 0", -1, &verify, NULL);
    if (rc != SQLITE_OK) {
        set_db_error(db, err, err_size, "CMNY database schema is incomplete");
        (void)sqlite3_finalize(verify);
        return false;
    }
    (void)sqlite3_finalize(verify);
    return true;
}

static bool ensure_currency(CmnyDb *db, const char *requested, char currency_out[4],
                            char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, "SELECT value FROM settings WHERE key='currency'", -1,
                                &stmt, NULL);
    if (rc != SQLITE_OK) {
        set_db_error(db, err, err_size, "cannot read ledger currency");
        return false;
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *stored = sqlite3_column_text(stmt, 0);
        char normalized[4] = {0};
        bool valid = stored != NULL && cmny_currency_supported((const char *)stored, normalized) &&
                     strcmp((const char *)stored, normalized) == 0;
        bool matches = valid && (requested == NULL || strcmp((const char *)stored, requested) == 0);
        if (valid) (void)snprintf(currency_out, 4, "%s", (const char *)stored);
        (void)sqlite3_finalize(stmt);
        if (!matches) {
            if (err != NULL && err_size > 0) {
                if (valid && requested != NULL) {
                    (void)snprintf(err, err_size, "ledger uses %s, not requested %s",
                                   currency_out, requested);
                } else {
                    (void)snprintf(err, err_size, "ledger currency setting is invalid");
                }
            }
            return false;
        }
        return true;
    }
    (void)sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_db_error(db, err, err_size, "cannot read ledger currency");
        return false;
    }

    const char *currency = requested != NULL ? requested : "EUR";
    rc = sqlite3_prepare_v2(db->handle, "INSERT INTO settings(key, value) VALUES('currency', ?)",
                            -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, currency, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (!ok) set_db_error(db, err, err_size, "cannot store ledger currency");
    (void)sqlite3_finalize(stmt);
    if (ok) (void)snprintf(currency_out, 4, "%s", currency);
    return ok;
}

bool cmny_db_open(CmnyDb *db, const char *path, bool demo, const char *requested_currency,
                  char currency_out[4], char *err, size_t err_size) {
    if (db == NULL || currency_out == NULL || (!demo && (path == NULL || *path == '\0'))) {
        set_error(err, err_size, "invalid database configuration");
        return false;
    }
    char normalized_currency[4] = {0};
    const char *currency = NULL;
    if (requested_currency != NULL) {
        if (!cmny_currency_supported(requested_currency, normalized_currency)) {
            set_error(err, err_size, "unsupported two-decimal currency");
            return false;
        }
        currency = normalized_currency;
    }
    if (sqlite3_libversion_number() < 3037000) {
        set_error(err, err_size, "CMNY requires SQLite 3.37 or newer");
        return false;
    }
    db->handle = NULL;

    int open_flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
#ifdef SQLITE_OPEN_NOFOLLOW
    open_flags |= SQLITE_OPEN_NOFOLLOW;
#endif
#ifndef _WIN32
    mode_t previous_mask = umask(0077);
#endif
    int rc = sqlite3_open_v2(demo ? ":memory:" : path, &db->handle, open_flags, NULL);
#ifndef _WIN32
    (void)umask(previous_mask);
#endif
    if (rc != SQLITE_OK) {
        set_db_error(db, err, err_size, "cannot open database");
        cmny_db_close(db);
        return false;
    }
    (void)sqlite3_extended_result_codes(db->handle, 1);
    (void)sqlite3_busy_timeout(db->handle, 3000);
    if (!exec_sql(db, "PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF;", err, err_size)) {
        cmny_db_close(db);
        return false;
    }
#ifdef SQLITE_DBCONFIG_DEFENSIVE
    if (sqlite3_db_config(db->handle, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL) != SQLITE_OK) {
        set_db_error(db, err, err_size, "cannot enable SQLite defensive mode");
        cmny_db_close(db);
        return false;
    }
#endif
    if (!migrate(db, err, err_size)) {
        cmny_db_close(db);
        return false;
    }
    if (!ensure_currency(db, currency, currency_out, err, err_size)) {
        cmny_db_close(db);
        return false;
    }
#ifndef _WIN32
    if (!demo && chmod(path, 0600) != 0) {
        (void)snprintf(err, err_size, "cannot protect database file: %s", strerror(errno));
        cmny_db_close(db);
        return false;
    }
#endif
    if (!demo && !exec_sql(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;", err, err_size)) {
        cmny_db_close(db);
        return false;
    }
    return true;
}

void cmny_db_close(CmnyDb *db) {
    if (db != NULL && db->handle != NULL) {
        (void)sqlite3_close(db->handle);
        db->handle = NULL;
    }
}

static bool safe_text(const char *text, size_t maximum, bool allow_empty) {
    if (text == NULL) {
        return false;
    }
    size_t len = strlen(text);
    if (len > maximum || (!allow_empty && len == 0)) {
        return false;
    }
    bool has_non_space = allow_empty;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 32 || ch > 126) {
            return false;
        }
        if (!isspace(ch)) {
            has_non_space = true;
        }
    }
    return has_non_space;
}

static bool valid_transaction(const CmnyTransaction *tx, bool require_id, char *err, size_t err_size) {
    if (tx == NULL || (require_id && tx->id <= 0) ||
        (tx->kind != CMNY_EXPENSE && tx->kind != CMNY_INCOME) || tx->amount_cents <= 0 ||
        tx->amount_cents > 9000000000000000LL ||
        !safe_text(tx->category, CMNY_CATEGORY_MAX, false) ||
        !safe_text(tx->note, CMNY_NOTE_MAX, true) || !cmny_date_valid(tx->occurred_on)) {
        set_error(err, err_size, "invalid transaction data");
        return false;
    }
    return true;
}

bool cmny_db_add(CmnyDb *db, const CmnyTransaction *tx, int64_t *new_id,
                 char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || !valid_transaction(tx, false, err, err_size)) {
        return false;
    }
    static const char *sql =
        "INSERT INTO transactions(kind, amount_cents, category, note, occurred_on) VALUES(?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, (int)tx->kind);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, tx->amount_cents);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, tx->category, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, tx->note, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, tx->occurred_on, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (!ok) {
        set_db_error(db, err, err_size, "cannot save transaction");
    } else if (new_id != NULL) {
        *new_id = sqlite3_last_insert_rowid(db->handle);
    }
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_db_update(CmnyDb *db, const CmnyTransaction *tx, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || !valid_transaction(tx, true, err, err_size)) {
        return false;
    }
    static const char *sql =
        "UPDATE transactions SET kind=?, amount_cents=?, category=?, note=?, occurred_on=?, "
        "updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, (int)tx->kind);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, tx->amount_cents);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, tx->category, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, tx->note, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, tx->occurred_on, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 6, tx->id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) {
        set_db_error(db, err, err_size, "cannot update transaction");
    }
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_db_delete(CmnyDb *db, int64_t id, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || id <= 0) {
        set_error(err, err_size, "invalid transaction id");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, "DELETE FROM transactions WHERE id=?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) {
        set_db_error(db, err, err_size, "cannot delete transaction");
    }
    (void)sqlite3_finalize(stmt);
    return ok;
}

static void copy_column(char *out, size_t out_size, sqlite3_stmt *stmt, int column) {
    const unsigned char *text = sqlite3_column_text(stmt, column);
    (void)snprintf(out, out_size, "%s", text != NULL ? (const char *)text : "");
}

bool cmny_db_list(CmnyDb *db, const char *month, const char *search, int kind_filter,
                  size_t offset, CmnyTransaction *out, size_t cap, size_t *count,
                  char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || !cmny_month_valid(month) || out == NULL || count == NULL) {
        set_error(err, err_size, "invalid transaction query");
        return false;
    }
    char next_month[8];
    if (!cmny_month_shift(month, 1, next_month)) {
        set_error(err, err_size, "invalid month range");
        return false;
    }
    char start[11];
    char end[11];
    char pattern[CMNY_NOTE_MAX + CMNY_CATEGORY_MAX + 8];
    (void)snprintf(start, sizeof(start), "%s-01", month);
    (void)snprintf(end, sizeof(end), "%s-01", next_month);
    (void)snprintf(pattern, sizeof(pattern), "%%%s%%", search != NULL ? search : "");

    static const char *sql =
        "SELECT id, kind, amount_cents, category, note, occurred_on FROM transactions "
        "WHERE occurred_on>=? AND occurred_on<? AND (?=0 OR kind=?) "
        "AND (category LIKE ? ESCAPE '\\' OR note LIKE ? ESCAPE '\\') "
        "ORDER BY occurred_on DESC, id DESC LIMIT ? OFFSET ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, start, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, end, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 3, kind_filter);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 4, kind_filter);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, pattern, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 6, pattern, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 7, (sqlite3_int64)cap);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 8, (sqlite3_int64)offset);
    if (rc != SQLITE_OK) {
        set_db_error(db, err, err_size, "cannot prepare transaction query");
        (void)sqlite3_finalize(stmt);
        return false;
    }

    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap) {
        out[used].id = sqlite3_column_int64(stmt, 0);
        out[used].kind = (CmnyKind)sqlite3_column_int(stmt, 1);
        out[used].amount_cents = sqlite3_column_int64(stmt, 2);
        copy_column(out[used].category, sizeof(out[used].category), stmt, 3);
        copy_column(out[used].note, sizeof(out[used].note), stmt, 4);
        copy_column(out[used].occurred_on, sizeof(out[used].occurred_on), stmt, 5);
        if (!valid_transaction(&out[used], true, err, err_size)) {
            set_error(err, err_size, "database contains invalid transaction data");
            (void)sqlite3_finalize(stmt);
            *count = used;
            return false;
        }
        used++;
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) {
        set_db_error(db, err, err_size, "cannot read transactions");
    }
    *count = used;
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_db_count(CmnyDb *db, const char *month, const char *search, int kind_filter,
                   size_t *count, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || !cmny_month_valid(month) || count == NULL) {
        set_error(err, err_size, "invalid transaction count query");
        return false;
    }
    char next_month[8], start[11], end[11];
    char pattern[CMNY_NOTE_MAX + CMNY_CATEGORY_MAX + 8];
    if (!cmny_month_shift(month, 1, next_month)) {
        set_error(err, err_size, "invalid month range");
        return false;
    }
    (void)snprintf(start, sizeof(start), "%s-01", month);
    (void)snprintf(end, sizeof(end), "%s-01", next_month);
    (void)snprintf(pattern, sizeof(pattern), "%%%s%%", search != NULL ? search : "");
    static const char *sql =
        "SELECT COUNT(*) FROM transactions WHERE occurred_on>=? AND occurred_on<? "
        "AND (?=0 OR kind=?) AND (category LIKE ? ESCAPE '\\' OR note LIKE ? ESCAPE '\\')";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, start, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, end, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 3, kind_filter);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 4, kind_filter);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, pattern, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 6, pattern, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW;
    if (ok) {
        sqlite3_int64 total = sqlite3_column_int64(stmt, 0);
        *count = total >= 0 ? (size_t)total : 0;
    } else {
        set_db_error(db, err, err_size, "cannot count transactions");
    }
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_db_month_summary(CmnyDb *db, const char *month, CmnyMonthSummary *summary,
                           char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || !cmny_month_valid(month) || summary == NULL) {
        set_error(err, err_size, "invalid summary query");
        return false;
    }
    char next_month[8];
    char start[11];
    char end[11];
    if (!cmny_month_shift(month, 1, next_month)) {
        set_error(err, err_size, "invalid month range");
        return false;
    }
    (void)snprintf(start, sizeof(start), "%s-01", month);
    (void)snprintf(end, sizeof(end), "%s-01", next_month);
    static const char *sql =
        "SELECT COALESCE(SUM(CASE WHEN kind=2 THEN amount_cents ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN kind=1 THEN amount_cents ELSE 0 END),0), COUNT(*) "
        "FROM transactions WHERE occurred_on>=? AND occurred_on<?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, start, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, end, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW;
    if (ok) {
        sqlite3_int64 income = sqlite3_column_int64(stmt, 0);
        sqlite3_int64 expense = sqlite3_column_int64(stmt, 1);
        sqlite3_int64 total = sqlite3_column_int64(stmt, 2);
        ok = income >= 0 && expense >= 0 && total >= 0 && total <= INT_MAX;
        if (ok) {
            summary->income_cents = income;
            summary->expense_cents = expense;
            summary->transaction_count = (int)total;
        } else {
            set_error(err, err_size, "database contains invalid summary values");
        }
    } else {
        set_db_error(db, err, err_size, "cannot calculate monthly summary");
    }
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_db_category_totals(CmnyDb *db, const char *month, CmnyCategoryTotal *out,
                             size_t cap, size_t *count, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || !cmny_month_valid(month) || out == NULL || count == NULL) {
        set_error(err, err_size, "invalid category query");
        return false;
    }
    char next_month[8];
    char start[11];
    char end[11];
    if (!cmny_month_shift(month, 1, next_month)) {
        set_error(err, err_size, "invalid month range");
        return false;
    }
    (void)snprintf(start, sizeof(start), "%s-01", month);
    (void)snprintf(end, sizeof(end), "%s-01", next_month);
    static const char *sql =
        "SELECT category, SUM(amount_cents) AS total FROM transactions "
        "WHERE kind=1 AND occurred_on>=? AND occurred_on<? "
        "GROUP BY category ORDER BY total DESC, category LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, start, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, end, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, (sqlite3_int64)cap);
    if (rc != SQLITE_OK) {
        set_db_error(db, err, err_size, "cannot prepare category query");
        (void)sqlite3_finalize(stmt);
        return false;
    }
    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap) {
        copy_column(out[used].category, sizeof(out[used].category), stmt, 0);
        out[used].amount_cents = sqlite3_column_int64(stmt, 1);
        if (!safe_text(out[used].category, CMNY_CATEGORY_MAX, false) ||
            out[used].amount_cents <= 0) {
            set_error(err, err_size, "database contains invalid category totals");
            (void)sqlite3_finalize(stmt);
            *count = used;
            return false;
        }
        used++;
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) set_db_error(db, err, err_size, "cannot calculate category totals");
    *count = used;
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_db_trend(CmnyDb *db, const char *end_month, CmnyMonthTrend *out,
                   size_t count, char *err, size_t err_size) {
    if (out == NULL || count == 0 || !cmny_month_valid(end_month)) {
        set_error(err, err_size, "invalid trend query");
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        int delta = (int)i - (int)count + 1;
        if (!cmny_month_shift(end_month, delta, out[i].month)) {
            set_error(err, err_size, "invalid trend month");
            return false;
        }
        CmnyMonthSummary summary = {0};
        if (!cmny_db_month_summary(db, out[i].month, &summary, err, err_size)) {
            return false;
        }
        out[i].income_cents = summary.income_cents;
        out[i].expense_cents = summary.expense_cents;
    }
    return true;
}

bool cmny_db_seed_demo(CmnyDb *db, char *err, size_t err_size) {
    static const struct {
        CmnyKind kind;
        int64_t cents;
        const char *category;
        const char *note;
        const char *date;
    } rows[] = {
        {CMNY_INCOME, 320000, "Salary", "Monthly salary", "2026-07-01"},
        {CMNY_INCOME, 45000, "Freelance", "Landing page project", "2026-07-08"},
        {CMNY_EXPENSE, 110000, "Housing", "Rent", "2026-07-02"},
        {CMNY_EXPENSE, 18432, "Food", "Groceries", "2026-07-05"},
        {CMNY_EXPENSE, 8900, "Transport", "Metro pass", "2026-07-06"},
        {CMNY_EXPENSE, 4250, "Food", "Dinner with friends", "2026-07-10"},
        {CMNY_EXPENSE, 7999, "Bills", "Phone and internet", "2026-07-12"},
        {CMNY_EXPENSE, 3590, "Health", "Pharmacy", "2026-07-15"},
        {CMNY_EXPENSE, 6750, "Fun", "Concert ticket", "2026-07-18"},
        {CMNY_EXPENSE, 2380, "Food", "Coffee and lunch", "2026-07-20"},
        {CMNY_INCOME, 320000, "Salary", "Monthly salary", "2026-06-01"},
        {CMNY_EXPENSE, 110000, "Housing", "Rent", "2026-06-02"},
        {CMNY_EXPENSE, 28740, "Food", "Groceries and meals", "2026-06-21"},
        {CMNY_EXPENSE, 8900, "Transport", "Metro pass", "2026-06-06"},
        {CMNY_EXPENSE, 12900, "Shopping", "Summer essentials", "2026-06-14"},
        {CMNY_INCOME, 320000, "Salary", "Monthly salary", "2026-05-01"},
        {CMNY_EXPENSE, 110000, "Housing", "Rent", "2026-05-02"},
        {CMNY_EXPENSE, 39200, "Travel", "Weekend away", "2026-05-23"},
        {CMNY_INCOME, 320000, "Salary", "Monthly salary", "2026-04-01"},
        {CMNY_EXPENSE, 110000, "Housing", "Rent", "2026-04-02"},
        {CMNY_EXPENSE, 31250, "Food", "Food and groceries", "2026-04-25"},
        {CMNY_INCOME, 320000, "Salary", "Monthly salary", "2026-03-01"},
        {CMNY_EXPENSE, 110000, "Housing", "Rent", "2026-03-02"},
        {CMNY_EXPENSE, 64000, "Health", "Dental work", "2026-03-17"},
        {CMNY_INCOME, 320000, "Salary", "Monthly salary", "2026-02-01"},
        {CMNY_EXPENSE, 110000, "Housing", "Rent", "2026-02-02"},
        {CMNY_EXPENSE, 26500, "Food", "Food and groceries", "2026-02-24"}
    };
    if (!exec_sql(db, "BEGIN IMMEDIATE", err, err_size)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        CmnyTransaction tx = {0};
        tx.kind = rows[i].kind;
        tx.amount_cents = rows[i].cents;
        (void)snprintf(tx.category, sizeof(tx.category), "%s", rows[i].category);
        (void)snprintf(tx.note, sizeof(tx.note), "%s", rows[i].note);
        (void)snprintf(tx.occurred_on, sizeof(tx.occurred_on), "%s", rows[i].date);
        if (!cmny_db_add(db, &tx, NULL, err, err_size)) {
            (void)sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
            return false;
        }
    }
    if (!exec_sql(db, "COMMIT", err, err_size)) {
        (void)sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    return true;
}
