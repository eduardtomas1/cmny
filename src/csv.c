#include "cmny.h"
#include "cmny_csv_parser.h"

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

#define CSV_FIELDS 5U
#define CSV_IMPORT_LIMIT 1000000U
#define CSV_IMPORT_MAX_BYTES (512U * 1024U * 1024U)

typedef struct {
    FILE *file;
    int error_number;
} CsvFileReader;

typedef struct {
    CmnyDb *db;
    bool apply;
    bool saw_header;
    bool failed;
    CmnyImportPreview result;
    char *err;
    size_t err_size;
} CsvImportContext;

static void csv_error(char *err, size_t err_size, const char *message) {
    if (err != NULL && err_size > 0) (void)snprintf(err, err_size, "%s", message);
}

static void csv_errno_error(char *err, size_t err_size, const char *prefix,
                            int error_number) {
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s: %s", prefix, strerror(error_number));
    }
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
    if (file == NULL) {
        int saved_errno = errno;
        (void)cmny_close(descriptor);
        (void)remove(path);
        errno = saved_errno;
    }
    return file;
}

static bool write_field(FILE *file, const char *value) {
    bool quote = strchr(value, ',') != NULL || strchr(value, '"') != NULL ||
                 strchr(value, '\r') != NULL || strchr(value, '\n') != NULL;
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
        csv_errno_error(err, err_size, "cannot create export (choose a new path)", errno);
        return false;
    }
    bool ok = fputs("kind,amount,category,note,date\n", file) >= 0;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT kind,amount_cents,category,note,occurred_on FROM transactions "
        "ORDER BY occurred_on,id", -1, &stmt, NULL);
    size_t rows = 0;
    if (rc != SQLITE_OK) {
        if (err != NULL && err_size > 0) {
            (void)snprintf(err, err_size, "cannot read ledger for export: %s",
                           sqlite3_errmsg(db->handle));
        }
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
        if (err != NULL && err_size > 0) {
            (void)snprintf(err, err_size, "cannot read ledger for export: %s",
                           sqlite3_errmsg(db->handle));
        }
        ok = false;
    }
    int finalize_rc = sqlite3_finalize(stmt);
    if (ok && finalize_rc != SQLITE_OK) {
        csv_error(err, err_size, "cannot finish reading ledger for export");
        ok = false;
    }
    if (ok && fflush(file) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        if (err != NULL && err_size > 0 && err[0] == '\0') {
            csv_error(err, err_size, "cannot write export");
        }
        (void)remove(path);
        return false;
    }
    *count = rows;
    return true;
}

static CmnyCsvReadStatus file_read(void *context, unsigned char *buffer,
                                   size_t capacity, size_t *length) {
    CsvFileReader *reader = context;
    errno = 0;
    size_t amount = fread(buffer, 1, capacity, reader->file);
    if (amount > 0) {
        *length = amount;
        return CMNY_CSV_READ_DATA;
    }
    *length = 0;
    if (ferror(reader->file)) {
        reader->error_number = errno != 0 ? errno : EIO;
        return CMNY_CSV_READ_ERROR;
    }
    if (feof(reader->file)) return CMNY_CSV_READ_END;
    reader->error_number = EIO;
    return CMNY_CSV_READ_ERROR;
}

static void record_error(CsvImportContext *context, const CmnyCsvRecord *record,
                         const char *message) {
    context->failed = true;
    if (context->err != NULL && context->err_size > 0) {
        (void)snprintf(context->err, context->err_size,
                       "invalid CSV row %zu (record %zu, physical line %zu): %s",
                       record->record_number, record->record_number,
                       record->physical_line_start, message);
    }
}

static bool header_valid(const CmnyCsvRecord *record) {
    static const char *expected[CSV_FIELDS] = {
        "kind", "amount", "category", "note", "date"
    };
    if (record->field_count != CSV_FIELDS) return false;
    for (size_t field = 0; field < CSV_FIELDS; field++) {
        if (strcmp(record->fields[field], expected[field]) != 0) return false;
    }
    return true;
}

static bool transaction_from_record(const CmnyCsvRecord *record, CmnyTransaction *tx) {
    if (record->field_count != CSV_FIELDS) return false;
    if (strcmp(record->fields[0], "expense") == 0) tx->kind = CMNY_EXPENSE;
    else if (strcmp(record->fields[0], "income") == 0) tx->kind = CMNY_INCOME;
    else return false;
    if (!cmny_money_parse(record->fields[1], &tx->amount_cents)) return false;
    if (strlen(record->fields[2]) > CMNY_CATEGORY_MAX ||
        strlen(record->fields[3]) > CMNY_NOTE_MAX || strlen(record->fields[4]) != 10) {
        return false;
    }
    (void)snprintf(tx->category, sizeof(tx->category), "%s", record->fields[2]);
    (void)snprintf(tx->note, sizeof(tx->note), "%s", record->fields[3]);
    (void)snprintf(tx->occurred_on, sizeof(tx->occurred_on), "%s", record->fields[4]);
    return cmny_transaction_valid(tx, false);
}

static CmnyCsvVisitResult import_record(const CmnyCsvRecord *record, void *opaque) {
    CsvImportContext *context = opaque;
    if (!context->saw_header) {
        context->saw_header = true;
        if (!header_valid(record)) {
            context->failed = true;
            if (context->err != NULL && context->err_size > 0) {
                (void)snprintf(context->err, context->err_size,
                               "CSV header must be kind,amount,category,note,date "
                               "(record %zu, physical line %zu)",
                               record->record_number, record->physical_line_start);
            }
            return CMNY_CSV_VISIT_ERROR;
        }
        return CMNY_CSV_VISIT_CONTINUE;
    }

    if (context->result.transaction_count >= CSV_IMPORT_LIMIT) {
        record_error(context, record,
                     "CSV exceeds the 1,000,000 transaction import limit");
        return CMNY_CSV_VISIT_ERROR;
    }
    CmnyTransaction tx = {0};
    if (!transaction_from_record(record, &tx)) {
        record_error(context, record, "invalid transaction data");
        return CMNY_CSV_VISIT_ERROR;
    }
    int64_t *total = tx.kind == CMNY_INCOME ? &context->result.income_cents
                                             : &context->result.expense_cents;
    if (*total > INT64_MAX - tx.amount_cents) {
        record_error(context, record, "transaction total exceeds the supported range");
        return CMNY_CSV_VISIT_ERROR;
    }
    if (context->apply) {
        char db_error[256] = {0};
        if (!cmny_db_add(context->db, &tx, NULL, db_error, sizeof(db_error))) {
            record_error(context, record,
                         db_error[0] != '\0' ? db_error : "cannot save transaction");
            return CMNY_CSV_VISIT_ERROR;
        }
    }
    context->result.transaction_count++;
    *total += tx.amount_cents;
    return CMNY_CSV_VISIT_CONTINUE;
}

static void parser_error(char *err, size_t err_size,
                         const CmnyCsvParseError *parse_error,
                         const CsvFileReader *reader) {
    if (err == NULL || err_size == 0) return;
    if (reader->error_number != 0) {
        (void)snprintf(err, err_size,
                       "cannot read import file at record %zu, physical line %zu: %s",
                       parse_error->record_number, parse_error->physical_line,
                       strerror(reader->error_number));
        return;
    }
    (void)snprintf(err, err_size,
                   "invalid CSV syntax at record %zu, physical line %zu: %s",
                   parse_error->record_number, parse_error->physical_line,
                   parse_error->message != NULL ? parse_error->message : "parse failed");
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
        csv_errno_error(err, err_size, "cannot open import", errno);
        return false;
    }

    bool transaction_started = false;
    bool ok = true;
    if (apply) {
        int rc = sqlite3_exec(db->handle, "BEGIN IMMEDIATE", NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            if (err != NULL && err_size > 0) {
                (void)snprintf(err, err_size, "cannot start import: %s",
                               sqlite3_errmsg(db->handle));
            }
            ok = false;
        } else {
            transaction_started = true;
        }
    }

    CsvFileReader reader = {.file = file};
    CsvImportContext context = {
        .db = db,
        .apply = apply,
        .err = err,
        .err_size = err_size,
    };
    CmnyCsvParseError parse_error = {0};
    CmnyCsvLimits limits;
    cmny_csv_limits_default(&limits);
    limits.max_bytes = CSV_IMPORT_MAX_BYTES;
    limits.max_records = CSV_IMPORT_LIMIT + 2U;
    limits.max_fields = CMNY_CSV_MAX_FIELDS;
    limits.max_field_bytes = CMNY_NOTE_MAX;
    if (ok && !cmny_csv_parse_stream(file_read, &reader, ',', &limits,
                                      import_record, &context, &parse_error)) {
        ok = false;
        if (!context.failed) parser_error(err, err_size, &parse_error, &reader);
    }
    if (ok && !context.saw_header) {
        csv_error(err, err_size, "CSV header must be kind,amount,category,note,date "
                                "(record 1, physical line 1)");
        ok = false;
    }
    if (fclose(file) != 0) {
        if (ok) csv_errno_error(err, err_size, "cannot finish reading import file", errno);
        ok = false;
    }

    if (transaction_started) {
        if (ok) {
            int rc = sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
            if (rc != SQLITE_OK) {
                if (err != NULL && err_size > 0) {
                    (void)snprintf(err, err_size, "cannot commit import: %s",
                                   sqlite3_errmsg(db->handle));
                }
                ok = false;
            }
        }
        if (!ok) (void)sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
    }
    if (!ok) return false;
    *preview = context.result;
    return true;
}
