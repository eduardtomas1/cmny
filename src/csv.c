#include "cmny.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define cmny_close _close
#define cmny_fdopen _fdopen
#else
#include <fcntl.h>
#include <unistd.h>
#define cmny_close close
#define cmny_fdopen fdopen
#endif

#define CSV_FIELDS 5
#define CSV_FIELD_SIZE (CMNY_NOTE_MAX + 1)
#define CSV_IMPORT_LIMIT 1000000U

static void csv_error(char *err, size_t err_size, const char *message) {
    if (err != NULL && err_size > 0) (void)snprintf(err, err_size, "%s", message);
}

static FILE *create_exclusive(const char *path) {
#ifdef _WIN32
    int descriptor = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                           _S_IREAD | _S_IWRITE);
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int descriptor = open(path, flags, 0600);
#endif
    if (descriptor < 0) return NULL;
    FILE *file = cmny_fdopen(descriptor, "wb");
    if (file == NULL) (void)cmny_close(descriptor);
    return file;
}

static bool write_field(FILE *file, const char *value) {
    bool quote = strchr(value, ',') != NULL || strchr(value, '"') != NULL;
    if (quote && fputc('"', file) == EOF) return false;
    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (*cursor == '"' && fputc('"', file) == EOF) return false;
        if (fputc(*cursor, file) == EOF) return false;
    }
    return !quote || fputc('"', file) != EOF;
}

static bool read_text_column(sqlite3_stmt *stmt, int column, char *out, size_t out_size) {
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

bool cmny_csv_export(CmnyDb *db, const char *path, size_t *count,
                     char *err, size_t err_size) {
    if (err != NULL && err_size > 0) err[0] = '\0';
    if (db == NULL || db->handle == NULL || path == NULL || *path == '\0' || count == NULL) {
        csv_error(err, err_size, "invalid export path");
        return false;
    }
    FILE *file = create_exclusive(path);
    if (file == NULL) {
        (void)snprintf(err, err_size, "cannot create export (choose a new path): %s", strerror(errno));
        return false;
    }
    bool ok = fputs("kind,amount,category,note,date\n", file) >= 0;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT kind,amount_cents,category,note,occurred_on FROM transactions "
        "ORDER BY occurred_on,id", -1, &stmt, NULL);
    size_t rows = 0;
    if (rc != SQLITE_OK) {
        (void)snprintf(err, err_size, "cannot read ledger for export: %s",
                       sqlite3_errmsg(db->handle));
        ok = false;
    }
    while (ok && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        CmnyTransaction tx = {0};
        tx.kind = (CmnyKind)sqlite3_column_int(stmt, 0);
        tx.amount_cents = sqlite3_column_int64(stmt, 1);
        bool text_ok = read_text_column(stmt, 2, tx.category, sizeof(tx.category)) &&
                       read_text_column(stmt, 3, tx.note, sizeof(tx.note)) &&
                       read_text_column(stmt, 4, tx.occurred_on, sizeof(tx.occurred_on));
        if (!text_ok || !cmny_transaction_valid(&tx, false)) {
            csv_error(err, err_size, "ledger contains invalid transaction data");
            ok = false;
            break;
        }
        char amount[64];
        cmny_money_format_plain(tx.amount_cents, amount, sizeof(amount));
        const char *kind = tx.kind == CMNY_INCOME ? "income" : "expense";
        ok = write_field(file, kind) && fputc(',', file) != EOF &&
             write_field(file, amount) && fputc(',', file) != EOF &&
             write_field(file, tx.category) && fputc(',', file) != EOF &&
             write_field(file, tx.note) && fputc(',', file) != EOF &&
             write_field(file, tx.occurred_on) && fputc('\n', file) != EOF;
        rows++;
    }
    if (rc != SQLITE_DONE && ok) {
        (void)snprintf(err, err_size, "cannot read ledger for export: %s", sqlite3_errmsg(db->handle));
        ok = false;
    }
    (void)sqlite3_finalize(stmt);
    if (ok && fflush(file) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        if (err != NULL && err_size > 0 && err[0] == '\0') csv_error(err, err_size, "cannot write export");
        (void)remove(path);
        return false;
    }
    *count = rows;
    return true;
}

static bool parse_csv_line(char *line, char fields[CSV_FIELDS][CSV_FIELD_SIZE]) {
    size_t length = strlen(line);
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[--length] = '\0';
    }
    const char *cursor = line;
    for (size_t field = 0; field < CSV_FIELDS; field++) {
        size_t used = 0;
        bool quoted = *cursor == '"';
        if (quoted) cursor++;
        for (;;) {
            if (*cursor == '\0') {
                if (quoted || field + 1 != CSV_FIELDS) return false;
                break;
            }
            if (quoted && *cursor == '"') {
                if (cursor[1] == '"') {
                    if (used + 1 >= CSV_FIELD_SIZE) return false;
                    fields[field][used++] = '"';
                    cursor += 2;
                    continue;
                }
                cursor++;
                if (field + 1 == CSV_FIELDS) {
                    if (*cursor != '\0') return false;
                } else if (*cursor++ != ',') {
                    return false;
                }
                break;
            }
            if (!quoted && *cursor == ',') {
                if (field + 1 == CSV_FIELDS) return false;
                cursor++;
                break;
            }
            if (!quoted && *cursor == '"') return false;
            if (used + 1 >= CSV_FIELD_SIZE) return false;
            fields[field][used++] = *cursor++;
        }
        fields[field][used] = '\0';
    }
    return *cursor == '\0';
}

static bool read_transaction(char *line, CmnyTransaction *tx) {
    char fields[CSV_FIELDS][CSV_FIELD_SIZE] = {{0}};
    if (!parse_csv_line(line, fields)) return false;
    if (strcmp(fields[0], "expense") == 0) tx->kind = CMNY_EXPENSE;
    else if (strcmp(fields[0], "income") == 0) tx->kind = CMNY_INCOME;
    else return false;
    if (!cmny_money_parse(fields[1], &tx->amount_cents)) return false;
    if (strlen(fields[2]) > CMNY_CATEGORY_MAX || strlen(fields[3]) > CMNY_NOTE_MAX ||
        strlen(fields[4]) != 10) return false;
    (void)snprintf(tx->category, sizeof(tx->category), "%s", fields[2]);
    (void)snprintf(tx->note, sizeof(tx->note), "%s", fields[3]);
    (void)snprintf(tx->occurred_on, sizeof(tx->occurred_on), "%s", fields[4]);
    return cmny_transaction_valid(tx, false);
}

bool cmny_csv_import(CmnyDb *db, const char *path, bool apply,
                     CmnyImportPreview *preview, char *err, size_t err_size) {
    if (err != NULL && err_size > 0) err[0] = '\0';
    if (db == NULL || db->handle == NULL || path == NULL || *path == '\0' || preview == NULL) {
        csv_error(err, err_size, "invalid import path");
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        (void)snprintf(err, err_size, "cannot open import: %s", strerror(errno));
        return false;
    }
    char line[1024];
    char header[CSV_FIELDS][CSV_FIELD_SIZE] = {{0}};
    bool ok = fgets(line, sizeof(line), file) != NULL && strchr(line, '\n') != NULL &&
              parse_csv_line(line, header) && strcmp(header[0], "kind") == 0 &&
              strcmp(header[1], "amount") == 0 && strcmp(header[2], "category") == 0 &&
              strcmp(header[3], "note") == 0 && strcmp(header[4], "date") == 0;
    if (!ok) csv_error(err, err_size, "CSV header must be kind,amount,category,note,date");
    CmnyImportPreview result = {0};
    if (ok && apply && sqlite3_exec(db->handle, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        (void)snprintf(err, err_size, "cannot start import: %s", sqlite3_errmsg(db->handle));
        ok = false;
    }
    size_t line_number = 1;
    while (ok && fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            ok = false;
        } else {
            CmnyTransaction tx = {0};
            ok = read_transaction(line, &tx);
            if (ok && result.transaction_count >= CSV_IMPORT_LIMIT) {
                csv_error(err, err_size, "CSV exceeds the 1,000,000 transaction import limit");
                ok = false;
            }
            int64_t *total = tx.kind == CMNY_INCOME ? &result.income_cents : &result.expense_cents;
            if (ok && *total > INT64_MAX - tx.amount_cents) ok = false;
            if (ok) {
                result.transaction_count++;
                *total += tx.amount_cents;
                if (apply) ok = cmny_db_add(db, &tx, NULL, err, err_size);
            }
        }
        if (!ok && err != NULL && err_size > 0 && err[0] == '\0') {
            (void)snprintf(err, err_size, "invalid CSV row %zu", line_number);
        }
    }
    if (ferror(file)) {
        csv_error(err, err_size, "cannot read import file");
        ok = false;
    }
    if (fclose(file) != 0 && ok) {
        csv_error(err, err_size, "cannot finish reading import file");
        ok = false;
    }
    if (apply) {
        if (ok && sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            (void)snprintf(err, err_size, "cannot commit import: %s", sqlite3_errmsg(db->handle));
            ok = false;
        }
        if (!ok) (void)sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
    }
    if (!ok) return false;
    *preview = result;
    return true;
}
