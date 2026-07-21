#include "cmny.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define CMNY_APPLICATION_ID 1129139801
#define CMNY_SCHEMA_VERSION 2

static bool copy_column(char *out, size_t out_size, sqlite3_stmt *stmt, int column);

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
    if (version < 1) {
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
    if (version < 2) {
        static const char *migration =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE budgets ("
        " month TEXT NOT NULL CHECK(month GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]'"
        "   AND date(month || '-01', '+0 days') = month || '-01'),"
        " category TEXT NOT NULL CHECK(length(category) BETWEEN 1 AND 32"
        "   AND category NOT GLOB '*[^ -~]*'),"
        " limit_cents INTEGER NOT NULL CHECK(limit_cents > 0 AND limit_cents <= 9000000000000000),"
        " PRIMARY KEY(month, category)"
        ") STRICT;"
        "CREATE TABLE recurring ("
        " id INTEGER PRIMARY KEY,"
        " kind INTEGER NOT NULL CHECK(kind IN (1, 2)),"
        " amount_cents INTEGER NOT NULL CHECK(amount_cents > 0 AND amount_cents <= 9000000000000000),"
        " category TEXT NOT NULL CHECK(length(category) BETWEEN 1 AND 32"
        "   AND category NOT GLOB '*[^ -~]*'),"
        " note TEXT NOT NULL DEFAULT '' CHECK(length(note) <= 120"
        "   AND note NOT GLOB '*[^ -~]*'),"
        " day_of_month INTEGER NOT NULL CHECK(day_of_month BETWEEN 1 AND 31),"
        " created_at INTEGER NOT NULL DEFAULT(CAST(strftime('%s','now') AS INTEGER)),"
        " UNIQUE(kind, amount_cents, category, note, day_of_month)"
        ") STRICT;"
        "PRAGMA user_version = 2;"
        "COMMIT;";
        if (!exec_sql(db, migration, err, err_size)) {
            (void)sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
            return false;
        }
    }

    sqlite3_stmt *verify = NULL;
    rc = sqlite3_prepare_v2(db->handle,
        "SELECT t.id, t.kind, t.amount_cents, t.category, t.note, t.occurred_on, s.key, s.value, "
        "b.month, b.category, b.limit_cents, r.id, r.day_of_month "
        "FROM transactions AS t LEFT JOIN settings AS s ON 0 LEFT JOIN budgets AS b ON 0 "
        "LEFT JOIN recurring AS r ON 0 LIMIT 0", -1, &verify, NULL);
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
        int stored_size = sqlite3_column_bytes(stmt, 0);
        bool valid = stored != NULL && stored_size == 3 &&
                     memchr(stored, '\0', (size_t)stored_size) == NULL &&
                     cmny_currency_supported((const char *)stored, normalized) &&
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

bool cmny_db_setting_get(CmnyDb *db, const char *key, char *out, size_t out_size) {
    if (db == NULL || db->handle == NULL || !cmny_text_valid(key, 32, false) ||
        out == NULL || out_size == 0) return false;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, "SELECT value FROM settings WHERE key=?", -1,
                                &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_text(stmt, 0) != NULL;
    if (ok) {
        const char *value = (const char *)sqlite3_column_text(stmt, 0);
        int value_size = sqlite3_column_bytes(stmt, 0);
        ok = value_size >= 0 && (size_t)value_size < out_size &&
             memchr(value, '\0', (size_t)value_size) == NULL &&
             strlen(value) == (size_t)value_size && cmny_text_valid(value, 120, true);
        if (ok) (void)snprintf(out, out_size, "%s", value);
    }
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_db_setting_set(CmnyDb *db, const char *key, const char *value,
                         char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || !cmny_text_valid(key, 32, false) ||
        !cmny_text_valid(value, 120, true)) {
        set_error(err, err_size, "invalid preference");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO settings(key,value) VALUES(?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (!ok) set_db_error(db, err, err_size, "cannot save preference");
    (void)sqlite3_finalize(stmt);
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

static bool copy_database(sqlite3 *destination, sqlite3 *source, char *err, size_t err_size) {
    sqlite3_backup *backup = sqlite3_backup_init(destination, "main", source, "main");
    if (backup == NULL) {
        if (err != NULL && err_size > 0) {
            (void)snprintf(err, err_size, "cannot start database copy: %s", sqlite3_errmsg(destination));
        }
        return false;
    }
    int rc = sqlite3_backup_step(backup, -1);
    int finish_rc = sqlite3_backup_finish(backup);
    if (rc != SQLITE_DONE || finish_rc != SQLITE_OK) {
        if (err != NULL && err_size > 0) {
            (void)snprintf(err, err_size, "cannot complete database copy: %s",
                           sqlite3_errmsg(destination));
        }
        return false;
    }
    return true;
}

static bool create_private_file(const char *path, char *err, size_t err_size) {
#ifdef _WIN32
    int descriptor = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                           _S_IREAD | _S_IWRITE);
    if (descriptor >= 0) (void)_close(descriptor);
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int descriptor = open(path, flags, 0600);
    if (descriptor >= 0) (void)close(descriptor);
#endif
    if (descriptor >= 0) return true;
    if (errno == EEXIST) {
        set_error(err, err_size, "backup path already exists");
    } else if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "cannot create backup: %s", strerror(errno));
    }
    return false;
}

static void remove_backup_sidecars(const char *path) {
    char sidecar[4200];
    static const char *suffixes[] = {"-wal", "-shm"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        int written = snprintf(sidecar, sizeof(sidecar), "%s%s", path, suffixes[i]);
        if (written >= 0 && (size_t)written < sizeof(sidecar)) (void)remove(sidecar);
    }
}

bool cmny_db_backup(CmnyDb *db, const char *path, char *err, size_t err_size) {
    if (err != NULL && err_size > 0) err[0] = '\0';
    if (db == NULL || db->handle == NULL || path == NULL || *path == '\0') {
        set_error(err, err_size, "invalid backup path");
        return false;
    }
    if (!create_private_file(path, err, err_size)) return false;
    int flags = SQLITE_OPEN_READWRITE;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    sqlite3 *destination = NULL;
    int rc = sqlite3_open_v2(path, &destination, flags, NULL);
    bool ok = rc == SQLITE_OK && copy_database(destination, db->handle, err, err_size);
    CmnyDb destination_db = {.handle = destination};
    if (ok) {
        ok = exec_sql(&destination_db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;",
                      err, err_size);
    }
    if (rc != SQLITE_OK && err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "cannot create backup: %s",
                       destination != NULL ? sqlite3_errmsg(destination) : "open failed");
    }
    if (destination != NULL) (void)sqlite3_close(destination);
    if (ok) remove_backup_sidecars(path);
#ifndef _WIN32
    if (ok && chmod(path, 0600) != 0) {
        (void)snprintf(err, err_size, "cannot protect backup file: %s", strerror(errno));
        ok = false;
    }
#endif
    if (!ok) (void)remove(path);
    return ok;
}

bool cmny_db_restore(CmnyDb *db, const char *path, char currency_out[4],
                     char *err, size_t err_size) {
    if (err != NULL && err_size > 0) err[0] = '\0';
    if (db == NULL || db->handle == NULL || path == NULL || *path == '\0' ||
        currency_out == NULL) {
        set_error(err, err_size, "invalid restore path");
        return false;
    }
    int flags = SQLITE_OPEN_READONLY;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    sqlite3 *source_handle = NULL;
    int rc = sqlite3_open_v2(path, &source_handle, flags, NULL);
    if (rc != SQLITE_OK) {
        (void)snprintf(err, err_size, "cannot open restore file: %s",
                       source_handle != NULL ? sqlite3_errmsg(source_handle) : "open failed");
        if (source_handle != NULL) (void)sqlite3_close(source_handle);
        return false;
    }
    CmnyDb source = {.handle = source_handle};
    (void)sqlite3_busy_timeout(source_handle, 3000);
    if (!exec_sql(&source, "PRAGMA trusted_schema=OFF;", err, err_size)) {
        (void)sqlite3_close(source_handle);
        return false;
    }
#ifdef SQLITE_DBCONFIG_DEFENSIVE
    if (sqlite3_db_config(source_handle, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL) != SQLITE_OK) {
        set_db_error(&source, err, err_size, "cannot inspect restore file safely");
        (void)sqlite3_close(source_handle);
        return false;
    }
#endif
    int application_id = 0;
    int version = 0;
    bool ok = read_pragma_int(&source, "PRAGMA application_id", &application_id, err, err_size) &&
              read_pragma_int(&source, "PRAGMA user_version", &version, err, err_size) &&
              application_id == CMNY_APPLICATION_ID && version >= 1 && version <= CMNY_SCHEMA_VERSION &&
              cmny_db_check(&source, err, err_size);
    if (!ok && err != NULL && err_size > 0 && err[0] == '\0') {
        set_error(err, err_size, "restore file is not a supported CMNY ledger");
    }
    if (ok) ok = copy_database(db->handle, source_handle, err, err_size);
    (void)sqlite3_close(source_handle);
    if (ok) ok = migrate(db, err, err_size);
    if (ok) ok = ensure_currency(db, NULL, currency_out, err, err_size);
    if (ok) ok = cmny_db_check(db, err, err_size);
    if (ok) ok = exec_sql(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;", err, err_size);
    return ok;
}

bool cmny_db_check(CmnyDb *db, char *err, size_t err_size) {
    if (err != NULL && err_size > 0) err[0] = '\0';
    if (db == NULL || db->handle == NULL) {
        set_error(err, err_size, "database is not open");
        return false;
    }
    int application_id = 0;
    int version = 0;
    if (!read_pragma_int(db, "PRAGMA application_id", &application_id, err, err_size) ||
        !read_pragma_int(db, "PRAGMA user_version", &version, err, err_size) ||
        application_id != CMNY_APPLICATION_ID || version < 1 || version > CMNY_SCHEMA_VERSION) {
        if (err != NULL && err_size > 0 && err[0] == '\0') {
            set_error(err, err_size, "ledger metadata is invalid");
        }
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, "PRAGMA quick_check", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_text(stmt, 0) != NULL &&
              strcmp((const char *)sqlite3_column_text(stmt, 0), "ok") == 0 &&
              sqlite3_step(stmt) == SQLITE_DONE;
    (void)sqlite3_finalize(stmt);
    if (!ok) {
        set_error(err, err_size, "SQLite integrity check failed");
        return false;
    }

    rc = sqlite3_prepare_v2(db->handle,
        "SELECT id,kind,amount_cents,category,note,occurred_on FROM transactions", -1,
        &stmt, NULL);
    if (rc != SQLITE_OK) {
        (void)sqlite3_finalize(stmt);
        set_db_error(db, err, err_size, "cannot inspect ledger transactions");
        return false;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        CmnyTransaction tx = {0};
        tx.id = sqlite3_column_int64(stmt, 0);
        tx.kind = (CmnyKind)sqlite3_column_int(stmt, 1);
        tx.amount_cents = sqlite3_column_int64(stmt, 2);
        bool text_ok = copy_column(tx.category, sizeof(tx.category), stmt, 3) &&
                       copy_column(tx.note, sizeof(tx.note), stmt, 4) &&
                       copy_column(tx.occurred_on, sizeof(tx.occurred_on), stmt, 5);
        if (!text_ok || !cmny_transaction_valid(&tx, true)) {
            rc = SQLITE_CORRUPT;
            break;
        }
    }
    ok = rc == SQLITE_DONE;
    (void)sqlite3_finalize(stmt);
    if (!ok) {
        set_error(err, err_size, "ledger contains invalid transaction data");
        return false;
    }

    rc = sqlite3_prepare_v2(db->handle, "SELECT key,value FROM settings", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        set_db_error(db, err, err_size, "cannot inspect ledger preferences");
        (void)sqlite3_finalize(stmt);
        return false;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        char key[33], value[121];
        bool text_ok = copy_column(key, sizeof(key), stmt, 0) &&
                       copy_column(value, sizeof(value), stmt, 1);
        if (!text_ok || !cmny_text_valid(key, 32, false) ||
            !cmny_text_valid(value, 120, true)) {
            rc = SQLITE_CORRUPT;
            break;
        }
    }
    ok = rc == SQLITE_DONE;
    (void)sqlite3_finalize(stmt);
    if (!ok) {
        set_error(err, err_size, "ledger contains invalid preference data");
        return false;
    }
    if (version < 2) return true;

    rc = sqlite3_prepare_v2(db->handle,
        "SELECT month,category,limit_cents FROM budgets", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        set_db_error(db, err, err_size, "cannot inspect ledger budgets");
        (void)sqlite3_finalize(stmt);
        return false;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        char month[8], category[CMNY_CATEGORY_MAX + 1];
        bool text_ok = copy_column(month, sizeof(month), stmt, 0) &&
                       copy_column(category, sizeof(category), stmt, 1);
        int64_t limit = sqlite3_column_int64(stmt, 2);
        if (!text_ok || !cmny_month_valid(month) ||
            !cmny_text_valid(category, CMNY_CATEGORY_MAX, false) ||
            limit <= 0 || limit > 9000000000000000LL) {
            rc = SQLITE_CORRUPT;
            break;
        }
    }
    ok = rc == SQLITE_DONE;
    (void)sqlite3_finalize(stmt);
    if (!ok) {
        set_error(err, err_size, "ledger contains invalid budget data");
        return false;
    }

    rc = sqlite3_prepare_v2(db->handle,
        "SELECT id,kind,amount_cents,category,note,day_of_month FROM recurring", -1,
        &stmt, NULL);
    if (rc != SQLITE_OK) {
        set_db_error(db, err, err_size, "cannot inspect recurring templates");
        (void)sqlite3_finalize(stmt);
        return false;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        CmnyTransaction tx = {0};
        tx.id = sqlite3_column_int64(stmt, 0);
        tx.kind = (CmnyKind)sqlite3_column_int(stmt, 1);
        tx.amount_cents = sqlite3_column_int64(stmt, 2);
        bool text_ok = copy_column(tx.category, sizeof(tx.category), stmt, 3) &&
                       copy_column(tx.note, sizeof(tx.note), stmt, 4);
        int day = sqlite3_column_int(stmt, 5);
        (void)snprintf(tx.occurred_on, sizeof(tx.occurred_on), "2000-01-%02d", day);
        if (!text_ok || day < 1 || day > 31 || !cmny_transaction_valid(&tx, true)) {
            rc = SQLITE_CORRUPT;
            break;
        }
    }
    ok = rc == SQLITE_DONE;
    (void)sqlite3_finalize(stmt);
    if (!ok) set_error(err, err_size, "ledger contains invalid recurring data");
    return ok;
}

static bool valid_transaction(const CmnyTransaction *tx, bool require_id, char *err, size_t err_size) {
    if (!cmny_transaction_valid(tx, require_id)) {
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

static bool copy_column(char *out, size_t out_size, sqlite3_stmt *stmt, int column) {
    if (out == NULL || out_size == 0) return false;
    const unsigned char *text = sqlite3_column_text(stmt, column);
    int bytes = sqlite3_column_bytes(stmt, column);
    if (text == NULL || bytes < 0 || (size_t)bytes >= out_size ||
        memchr(text, '\0', (size_t)bytes) != NULL) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, text, (size_t)bytes);
    out[bytes] = '\0';
    return true;
}

static bool make_like_pattern(const char *search, char *out, size_t out_size) {
    const char *input = search != NULL ? search : "";
    size_t used = 0;
    if (out_size < 3) return false;
    out[used++] = '%';
    while (*input != '\0') {
        if ((*input == '%' || *input == '_' || *input == '\\') && used + 1 >= out_size) return false;
        if (*input == '%' || *input == '_' || *input == '\\') out[used++] = '\\';
        if (used + 1 >= out_size) return false;
        out[used++] = *input++;
    }
    out[used++] = '%';
    out[used] = '\0';
    return true;
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
    char pattern[(CMNY_NOTE_MAX + 1) * 2 + 3];
    bool all_months = search != NULL && *search != '\0';
    (void)snprintf(start, sizeof(start), "%s-01", month);
    (void)snprintf(end, sizeof(end), "%s-01", next_month);
    if (!make_like_pattern(search, pattern, sizeof(pattern))) {
        set_error(err, err_size, "search text is too long");
        return false;
    }

    static const char *sql =
        "SELECT id, kind, amount_cents, category, note, occurred_on FROM transactions "
        "WHERE (?=1 OR (occurred_on>=? AND occurred_on<?)) AND (?=0 OR kind=?) "
        "AND (category LIKE ? ESCAPE '\\' OR note LIKE ? ESCAPE '\\') "
        "ORDER BY occurred_on DESC, id DESC LIMIT ? OFFSET ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, all_months ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, start, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, end, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 4, kind_filter);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 5, kind_filter);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 6, pattern, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 7, pattern, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 8, (sqlite3_int64)cap);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 9, (sqlite3_int64)offset);
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
        bool text_ok = copy_column(out[used].category, sizeof(out[used].category), stmt, 3) &&
                       copy_column(out[used].note, sizeof(out[used].note), stmt, 4) &&
                       copy_column(out[used].occurred_on, sizeof(out[used].occurred_on), stmt, 5);
        if (!text_ok || !valid_transaction(&out[used], true, err, err_size)) {
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
    char pattern[(CMNY_NOTE_MAX + 1) * 2 + 3];
    bool all_months = search != NULL && *search != '\0';
    if (!cmny_month_shift(month, 1, next_month)) {
        set_error(err, err_size, "invalid month range");
        return false;
    }
    (void)snprintf(start, sizeof(start), "%s-01", month);
    (void)snprintf(end, sizeof(end), "%s-01", next_month);
    if (!make_like_pattern(search, pattern, sizeof(pattern))) {
        set_error(err, err_size, "search text is too long");
        return false;
    }
    static const char *sql =
        "SELECT COUNT(*) FROM transactions WHERE (?=1 OR (occurred_on>=? AND occurred_on<?)) "
        "AND (?=0 OR kind=?) AND (category LIKE ? ESCAPE '\\' OR note LIKE ? ESCAPE '\\')";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, all_months ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, start, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, end, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 4, kind_filter);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 5, kind_filter);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 6, pattern, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 7, pattern, -1, SQLITE_TRANSIENT);
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
        bool text_ok = copy_column(out[used].category, sizeof(out[used].category), stmt, 0);
        out[used].amount_cents = sqlite3_column_int64(stmt, 1);
        if (!text_ok || !cmny_text_valid(out[used].category, CMNY_CATEGORY_MAX, false) ||
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

bool cmny_db_budget_set(CmnyDb *db, const char *month, const char *category,
                        int64_t limit_cents, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || !cmny_month_valid(month) ||
        !cmny_text_valid(category, CMNY_CATEGORY_MAX, false) || limit_cents < 0 ||
        limit_cents > 9000000000000000LL) {
        set_error(err, err_size, "invalid budget");
        return false;
    }
    const char *sql = limit_cents == 0
        ? "DELETE FROM budgets WHERE month=? AND category=?"
        : "INSERT INTO budgets(month,category,limit_cents) VALUES(?,?,?) "
          "ON CONFLICT(month,category) DO UPDATE SET limit_cents=excluded.limit_cents";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, month, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, category, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK && limit_cents > 0) rc = sqlite3_bind_int64(stmt, 3, limit_cents);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (!ok) set_db_error(db, err, err_size, "cannot save budget");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_db_budget_list(CmnyDb *db, const char *month, CmnyBudget *out, size_t cap,
                         size_t *count, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || !cmny_month_valid(month) || out == NULL ||
        count == NULL) {
        set_error(err, err_size, "invalid budget query");
        return false;
    }
    char next_month[8], start[11], end[11];
    if (!cmny_month_shift(month, 1, next_month)) {
        set_error(err, err_size, "invalid budget month");
        return false;
    }
    (void)snprintf(start, sizeof(start), "%s-01", month);
    (void)snprintf(end, sizeof(end), "%s-01", next_month);
    static const char *sql =
        "SELECT b.category,b.limit_cents,COALESCE(SUM(t.amount_cents),0) "
        "FROM budgets AS b LEFT JOIN transactions AS t ON t.kind=1 "
        "AND t.category=b.category AND t.occurred_on>=? AND t.occurred_on<? "
        "WHERE b.month=? GROUP BY b.category,b.limit_cents ORDER BY b.category LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, start, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, end, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, month, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 4, (sqlite3_int64)cap);
    if (rc != SQLITE_OK) {
        set_db_error(db, err, err_size, "cannot prepare budget query");
        (void)sqlite3_finalize(stmt);
        return false;
    }
    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap) {
        bool text_ok = copy_column(out[used].category, sizeof(out[used].category), stmt, 0);
        out[used].limit_cents = sqlite3_column_int64(stmt, 1);
        out[used].spent_cents = sqlite3_column_int64(stmt, 2);
        if (!text_ok || !cmny_text_valid(out[used].category, CMNY_CATEGORY_MAX, false) ||
            out[used].limit_cents <= 0 || out[used].spent_cents < 0) {
            set_error(err, err_size, "database contains invalid budget data");
            (void)sqlite3_finalize(stmt);
            *count = used;
            return false;
        }
        used++;
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) set_db_error(db, err, err_size, "cannot read budgets");
    *count = used;
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_db_recurring_add(CmnyDb *db, const CmnyTransaction *tx,
                           char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || !cmny_transaction_valid(tx, false)) {
        set_error(err, err_size, "invalid recurring template");
        return false;
    }
    int day = (tx->occurred_on[8] - '0') * 10 + (tx->occurred_on[9] - '0');
    static const char *sql =
        "INSERT OR IGNORE INTO recurring(kind,amount_cents,category,note,day_of_month) "
        "VALUES(?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, (int)tx->kind);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, tx->amount_cents);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, tx->category, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, tx->note, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 5, day);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (!ok) set_db_error(db, err, err_size, "cannot save recurring template");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_db_recurring_list(CmnyDb *db, CmnyRecurring *out, size_t cap,
                            size_t *count, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || out == NULL || count == NULL) {
        set_error(err, err_size, "invalid recurring query");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT id,kind,amount_cents,category,note,day_of_month FROM recurring "
        "ORDER BY kind,category,id LIMIT ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)cap);
    if (rc != SQLITE_OK) {
        set_db_error(db, err, err_size, "cannot prepare recurring query");
        (void)sqlite3_finalize(stmt);
        return false;
    }
    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap) {
        out[used].id = sqlite3_column_int64(stmt, 0);
        out[used].kind = (CmnyKind)sqlite3_column_int(stmt, 1);
        out[used].amount_cents = sqlite3_column_int64(stmt, 2);
        bool text_ok = copy_column(out[used].category, sizeof(out[used].category), stmt, 3) &&
                       copy_column(out[used].note, sizeof(out[used].note), stmt, 4);
        out[used].day_of_month = sqlite3_column_int(stmt, 5);
        CmnyTransaction tx = {0};
        tx.kind = out[used].kind;
        tx.amount_cents = out[used].amount_cents;
        (void)snprintf(tx.category, sizeof(tx.category), "%s", out[used].category);
        (void)snprintf(tx.note, sizeof(tx.note), "%s", out[used].note);
        (void)snprintf(tx.occurred_on, sizeof(tx.occurred_on), "2000-01-%02d",
                       out[used].day_of_month);
        if (!text_ok || out[used].id <= 0 || !cmny_transaction_valid(&tx, false)) {
            set_error(err, err_size, "database contains invalid recurring data");
            (void)sqlite3_finalize(stmt);
            *count = used;
            return false;
        }
        used++;
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) set_db_error(db, err, err_size, "cannot read recurring templates");
    *count = used;
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_db_recurring_delete(CmnyDb *db, int64_t id, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || id <= 0) {
        set_error(err, err_size, "invalid recurring template id");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, "DELETE FROM recurring WHERE id=?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) set_db_error(db, err, err_size, "cannot delete recurring template");
    (void)sqlite3_finalize(stmt);
    return ok;
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
    if (!cmny_db_budget_set(db, "2026-07", "Housing", 120000, err, err_size) ||
        !cmny_db_budget_set(db, "2026-07", "Food", 35000, err, err_size) ||
        !cmny_db_budget_set(db, "2026-07", "Fun", 10000, err, err_size)) {
        return false;
    }
    CmnyTransaction recurring = {0};
    recurring.kind = CMNY_INCOME;
    recurring.amount_cents = 320000;
    (void)snprintf(recurring.category, sizeof(recurring.category), "Salary");
    (void)snprintf(recurring.note, sizeof(recurring.note), "Monthly salary");
    (void)snprintf(recurring.occurred_on, sizeof(recurring.occurred_on), "2026-07-01");
    if (!cmny_db_recurring_add(db, &recurring, err, err_size)) return false;
    recurring.kind = CMNY_EXPENSE;
    recurring.amount_cents = 110000;
    (void)snprintf(recurring.category, sizeof(recurring.category), "Housing");
    (void)snprintf(recurring.note, sizeof(recurring.note), "Rent");
    (void)snprintf(recurring.occurred_on, sizeof(recurring.occurred_on), "2026-07-02");
    if (!cmny_db_recurring_add(db, &recurring, err, err_size)) return false;
    return true;
}
