#include "cmny.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void remove_database(const char *path) {
    char sidecar[4200];
    (void)unlink(path);
    (void)snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    (void)unlink(sidecar);
    (void)snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    (void)unlink(sidecar);
}

static CmnyTransaction transaction(CmnyKind kind, int64_t cents, const char *category,
                                   const char *note, const char *date) {
    CmnyTransaction tx = {0};
    tx.kind = kind;
    tx.amount_cents = cents;
    (void)snprintf(tx.category, sizeof(tx.category), "%s", category);
    (void)snprintf(tx.note, sizeof(tx.note), "%s", note);
    (void)snprintf(tx.occurred_on, sizeof(tx.occurred_on), "%s", date);
    return tx;
}

static void open_test_db(CmnyDb *db, char path[], char *err, size_t err_size) {
    int descriptor = mkstemp(path);
    ASSERT_TRUE(descriptor >= 0);
    ASSERT_TRUE(close(descriptor) == 0);
    char currency[4] = {0};
    ASSERT_TRUE(cmny_db_open(db, path, false, "EUR", currency, err, err_size));
}

static void write_bytes(int descriptor, const void *bytes, size_t length) {
    FILE *file = fdopen(descriptor, "wb");
    ASSERT_TRUE(file != NULL);
    ASSERT_TRUE(fwrite(bytes, 1, length, file) == length);
    ASSERT_TRUE(fclose(file) == 0);
}

static void create_csv(char path[], const void *bytes, size_t length) {
    int descriptor = mkstemp(path);
    ASSERT_TRUE(descriptor >= 0);
    write_bytes(descriptor, bytes, length);
}

static size_t month_count(CmnyDb *db, const char *month, char *err, size_t err_size) {
    CmnyMonthSummary summary = {0};
    ASSERT_TRUE(cmny_db_month_summary(db, month, &summary, err, err_size));
    ASSERT_TRUE(summary.transaction_count >= 0);
    return (size_t)summary.transaction_count;
}

static int deny_export_select(void *context, int action, const char *first,
                              const char *second, const char *database,
                              const char *trigger) {
    (void)context;
    (void)first;
    (void)second;
    (void)database;
    (void)trigger;
    return action == SQLITE_SELECT || action == SQLITE_READ ? SQLITE_DENY : SQLITE_OK;
}

static void test_export_security_and_round_trip(void) {
    char source_path[] = "build/cmny-csv-source-XXXXXX";
    char target_path[] = "build/cmny-csv-target-XXXXXX";
    char export_path[] = "build/cmny-export-XXXXXX";
    char failed_export_path[] = "build/cmny-failed-export-XXXXXX";
    char symlink_path[] = "build/cmny-export-link-XXXXXX";
    char err[256] = {0};
    CmnyDb source = {0};
    CmnyDb target = {0};
    open_test_db(&source, source_path, err, sizeof(err));
    open_test_db(&target, target_path, err, sizeof(err));

    int export_fd = mkstemp(export_path);
    int failed_fd = mkstemp(failed_export_path);
    int symlink_fd = mkstemp(symlink_path);
    ASSERT_TRUE(export_fd >= 0 && failed_fd >= 0 && symlink_fd >= 0);
    ASSERT_TRUE(close(export_fd) == 0 && close(failed_fd) == 0 && close(symlink_fd) == 0);
    ASSERT_TRUE(unlink(export_path) == 0);
    ASSERT_TRUE(unlink(failed_export_path) == 0);
    ASSERT_TRUE(unlink(symlink_path) == 0);

    char category[CMNY_CATEGORY_MAX + 1];
    memset(category, 'C', CMNY_CATEGORY_MAX);
    category[4] = ',';
    category[9] = '"';
    category[CMNY_CATEGORY_MAX] = '\0';
    char note[CMNY_NOTE_MAX + 1];
    memset(note, 'n', CMNY_NOTE_MAX);
    note[3] = ',';
    note[7] = '"';
    note[CMNY_NOTE_MAX] = '\0';
    CmnyTransaction expense = transaction(CMNY_EXPENSE, 1234, category, note,
                                          "2026-07-10");
    CmnyTransaction income = transaction(CMNY_INCOME, 250000, "Salary", "July",
                                         "2026-07-01");
    ASSERT_TRUE(cmny_db_add(&source, &expense, NULL, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_add(&source, &income, NULL, err, sizeof(err)));

    size_t exported = 999;
    ASSERT_TRUE(cmny_csv_export(&source, export_path, &exported, err, sizeof(err)));
    ASSERT_EQ_I64(2, exported);
    struct stat info;
    ASSERT_TRUE(stat(export_path, &info) == 0);
    ASSERT_EQ_I64(0600, info.st_mode & 0777);
    exported = 777;
    ASSERT_TRUE(!cmny_csv_export(&source, export_path, &exported, err, sizeof(err)));
    ASSERT_EQ_I64(777, exported);

    FILE *exported_file = fopen(export_path, "rb");
    ASSERT_TRUE(exported_file != NULL);
    char contents[4096] = {0};
    ASSERT_TRUE(fread(contents, 1, sizeof(contents) - 1, exported_file) > 0);
    ASSERT_TRUE(fclose(exported_file) == 0);
    ASSERT_TRUE(strstr(contents, "\"CCCC,CCCC\"\"CCCC") != NULL);
    ASSERT_TRUE(strstr(contents, "nnn,nnn\"\"nnn") != NULL);

    CmnyImportPreview dry_run = {0};
    CmnyImportPreview applied = {0};
    ASSERT_TRUE(cmny_csv_import(&target, export_path, false, &dry_run,
                                err, sizeof(err)));
    ASSERT_TRUE(cmny_csv_import(&target, export_path, true, &applied,
                                err, sizeof(err)));
    ASSERT_TRUE(memcmp(&dry_run, &applied, sizeof(dry_run)) == 0);
    ASSERT_EQ_I64(2, applied.transaction_count);
    ASSERT_EQ_I64(250000, applied.income_cents);
    ASSERT_EQ_I64(1234, applied.expense_cents);
    CmnyTransaction imported[4] = {{0}};
    size_t imported_count = 0;
    ASSERT_TRUE(cmny_db_list(&target, "2026-07", "", 0, 0, imported, 4,
                             &imported_count, err, sizeof(err)));
    ASSERT_EQ_I64(2, imported_count);
    bool found_expense = false;
    for (size_t index = 0; index < imported_count; index++) {
        if (imported[index].kind == CMNY_EXPENSE) {
            found_expense = true;
            ASSERT_TRUE(strcmp(imported[index].category, category) == 0);
            ASSERT_TRUE(strcmp(imported[index].note, note) == 0);
        }
    }
    ASSERT_TRUE(found_expense);

    /* Legacy duplicate behavior is intentionally retained. */
    ASSERT_TRUE(cmny_csv_import(&target, export_path, true, &applied,
                                err, sizeof(err)));
    ASSERT_EQ_I64(4, month_count(&target, "2026-07", err, sizeof(err)));

    ASSERT_TRUE(symlink(export_path, symlink_path) == 0);
    ASSERT_TRUE(!cmny_csv_export(&source, symlink_path, &exported, err, sizeof(err)));
    ASSERT_TRUE(lstat(symlink_path, &info) == 0 && S_ISLNK(info.st_mode));

    ASSERT_TRUE(sqlite3_set_authorizer(source.handle, deny_export_select, NULL) == SQLITE_OK);
    exported = 555;
    ASSERT_TRUE(!cmny_csv_export(&source, failed_export_path, &exported,
                                 err, sizeof(err)));
    ASSERT_EQ_I64(555, exported);
    ASSERT_TRUE(access(failed_export_path, F_OK) != 0);
    ASSERT_TRUE(sqlite3_set_authorizer(source.handle, NULL, NULL) == SQLITE_OK);

    cmny_db_close(&target);
    cmny_db_close(&source);
    remove_database(source_path);
    remove_database(target_path);
    ASSERT_TRUE(unlink(export_path) == 0);
    ASSERT_TRUE(unlink(symlink_path) == 0);
}

static void append_text(char *document, size_t capacity, size_t *used,
                        const char *text) {
    size_t length = strlen(text);
    ASSERT_TRUE(length < capacity - *used);
    memcpy(document + *used, text, length);
    *used += length;
    document[*used] = '\0';
}

static void test_bom_crlf_and_chunk_boundaries(void) {
    char db_path[] = "build/cmny-csv-boundary-db-XXXXXX";
    char csv_path[] = "build/cmny-csv-boundary-XXXXXX";
    char err[256] = {0};
    CmnyDb db = {0};
    open_test_db(&db, db_path, err, sizeof(err));

    char *document = calloc(10000, 1);
    ASSERT_TRUE(document != NULL);
    size_t used = 0;
    append_text(document, 10000, &used,
                "\xEF\xBB\xBF" "kind,amount,category,note,date\r\n");
    const char *plain = "income,1.00,C,,2026-01-01\r\n";
    const char *quote_prefix = "expense,2.00,C,\"";
    while (4095U - (used + strlen(quote_prefix)) > CMNY_NOTE_MAX - 2U) {
        append_text(document, 10000, &used, plain);
    }
    size_t quote_padding = 4095U - used - strlen(quote_prefix);
    ASSERT_TRUE(quote_padding <= CMNY_NOTE_MAX - 2U);
    append_text(document, 10000, &used, quote_prefix);
    memset(document + used, 'q', quote_padding);
    used += quote_padding;
    document[used] = '\0';
    ASSERT_EQ_I64(4095, used);
    append_text(document, 10000, &used, "\"\"x\",2026-01-02\r\n");

    const char *cr_prefix = "income,3.00,C,";
    const char *cr_suffix = ",2026-01-03";
    while (8191U - (used + strlen(cr_prefix) + strlen(cr_suffix)) > CMNY_NOTE_MAX) {
        append_text(document, 10000, &used, plain);
    }
    size_t cr_padding = 8191U - used - strlen(cr_prefix) - strlen(cr_suffix);
    ASSERT_TRUE(cr_padding <= CMNY_NOTE_MAX);
    append_text(document, 10000, &used, cr_prefix);
    memset(document + used, 'r', cr_padding);
    used += cr_padding;
    document[used] = '\0';
    append_text(document, 10000, &used, cr_suffix);
    ASSERT_EQ_I64(8191, used);
    append_text(document, 10000, &used, "\r\n");

    create_csv(csv_path, document, used);
    CmnyImportPreview preview = {0};
    ASSERT_TRUE(cmny_csv_import(&db, csv_path, false, &preview, err, sizeof(err)));
    ASSERT_TRUE(preview.transaction_count > 200);
    ASSERT_EQ_I64(200, preview.expense_cents);
    ASSERT_EQ_I64((int64_t)(preview.transaction_count - 1U) * 100 + 200,
                  preview.income_cents);
    CmnyImportPreview applied = {0};
    ASSERT_TRUE(cmny_csv_import(&db, csv_path, true, &applied, err, sizeof(err)));
    ASSERT_TRUE(memcmp(&preview, &applied, sizeof(preview)) == 0);
    free(document);
    cmny_db_close(&db);
    remove_database(db_path);
    ASSERT_TRUE(unlink(csv_path) == 0);
}

static void assert_failed_import_unchanged(CmnyDb *db, const char *path,
                                           const char *error_fragment,
                                           size_t expected_count,
                                           char *err, size_t err_size) {
    CmnyImportPreview preview = {
        .transaction_count = 71, .income_cents = 72, .expense_cents = 73,
    };
    CmnyImportPreview original = preview;
    err[0] = '\0';
    ASSERT_TRUE(!cmny_csv_import(db, path, true, &preview, err, err_size));
    ASSERT_TRUE(strstr(err, error_fragment) != NULL);
    ASSERT_TRUE(memcmp(&preview, &original, sizeof(preview)) == 0);
    ASSERT_EQ_I64(expected_count, month_count(db, "2026-07", err, err_size));
}

static void test_validation_syntax_and_atomic_rollback(void) {
    char db_path[] = "build/cmny-csv-invalid-db-XXXXXX";
    char multiline_path[] = "build/cmny-csv-multiline-XXXXXX";
    char malformed_path[] = "build/cmny-csv-malformed-XXXXXX";
    char header_path[] = "build/cmny-csv-header-XXXXXX";
    char field_path[] = "build/cmny-csv-field-XXXXXX";
    char row_shape_path[] = "build/cmny-csv-shape-XXXXXX";
    char err[256] = {0};
    CmnyDb db = {0};
    open_test_db(&db, db_path, err, sizeof(err));

    static const char multiline[] =
        "kind,amount,category,note,date\r\n"
        "expense,1.00,Food,valid,2026-07-01\r\n"
        "expense,2.00,Food,\"bad\r\nnote\",2026-07-02\r\n";
    create_csv(multiline_path, multiline, sizeof(multiline) - 1);
    assert_failed_import_unchanged(&db, multiline_path, "row 3", 0,
                                   err, sizeof(err));
    ASSERT_TRUE(strstr(err, "record 3") != NULL);
    ASSERT_TRUE(strstr(err, "physical line 3") != NULL);

    static const char malformed[] =
        "kind,amount,category,note,date\n"
        "expense,1.00,Food,valid,2026-07-01\n"
        "expense,2.00,Food,\"unterminated,2026-07-02\n";
    create_csv(malformed_path, malformed, sizeof(malformed) - 1);
    assert_failed_import_unchanged(&db, malformed_path, "invalid CSV syntax", 0,
                                   err, sizeof(err));
    ASSERT_TRUE(strstr(err, "record 3") != NULL);
    ASSERT_TRUE(strstr(err, "physical line") != NULL);

    static const char wrong_header[] =
        "kind,amount,category,note,date,extra\n";
    create_csv(header_path, wrong_header, sizeof(wrong_header) - 1);
    assert_failed_import_unchanged(&db, header_path, "CSV header must be", 0,
                                   err, sizeof(err));

    char long_field[300] =
        "kind,amount,category,note,date\nexpense,1.00,Food,";
    size_t field_used = strlen(long_field);
    memset(long_field + field_used, 'x', CMNY_NOTE_MAX + 1U);
    field_used += CMNY_NOTE_MAX + 1U;
    (void)snprintf(long_field + field_used, sizeof(long_field) - field_used,
                   ",2026-07-01\n");
    create_csv(field_path, long_field, strlen(long_field));
    assert_failed_import_unchanged(&db, field_path, "field exceeds", 0,
                                   err, sizeof(err));
    ASSERT_TRUE(strstr(err, "record 2") != NULL);

    static const char wrong_shape[] =
        "kind,amount,category,note,date\n"
        "expense,1.00,Food,note,2026-07-01,extra\n";
    create_csv(row_shape_path, wrong_shape, sizeof(wrong_shape) - 1);
    assert_failed_import_unchanged(&db, row_shape_path, "row 2", 0,
                                   err, sizeof(err));

    CmnyImportPreview preview = {0};
    err[0] = '\0';
    ASSERT_TRUE(!cmny_csv_import(&db, "build", false, &preview, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "cannot read import file") != NULL);

    cmny_db_close(&db);
    remove_database(db_path);
    ASSERT_TRUE(unlink(multiline_path) == 0);
    ASSERT_TRUE(unlink(malformed_path) == 0);
    ASSERT_TRUE(unlink(header_path) == 0);
    ASSERT_TRUE(unlink(field_path) == 0);
    ASSERT_TRUE(unlink(row_shape_path) == 0);
}

static void test_database_failure_rolls_back(void) {
    char db_path[] = "build/cmny-csv-db-failure-db-XXXXXX";
    char csv_path[] = "build/cmny-csv-db-failure-XXXXXX";
    char commit_path[] = "build/cmny-csv-commit-failure-XXXXXX";
    char err[256] = {0};
    CmnyDb db = {0};
    open_test_db(&db, db_path, err, sizeof(err));
    ASSERT_TRUE(sqlite3_exec(db.handle,
        "CREATE TRIGGER reject_csv_note BEFORE INSERT ON entries "
        "WHEN NEW.note='stop' BEGIN SELECT RAISE(ABORT,'stop import'); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    static const char document[] =
        "kind,amount,category,note,date\n"
        "expense,1.00,Food,good,2026-07-01\n"
        "expense,2.00,Food,stop,2026-07-02\n";
    create_csv(csv_path, document, sizeof(document) - 1);
    assert_failed_import_unchanged(&db, csv_path, "record 3", 0,
                                   err, sizeof(err));
    ASSERT_TRUE(strstr(err, "physical line 3") != NULL);

    ASSERT_TRUE(sqlite3_exec(db.handle,
        "DROP TRIGGER reject_csv_note;"
        "CREATE TABLE csv_deferred_failure("
        " account_id INTEGER REFERENCES accounts(id) DEFERRABLE INITIALLY DEFERRED);"
        "CREATE TRIGGER defer_csv_failure AFTER INSERT ON entries BEGIN "
        " INSERT INTO csv_deferred_failure(account_id) VALUES(-999999); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    static const char commit_failure[] =
        "kind,amount,category,note,date\n"
        "income,5.00,Salary,commit,2026-07-03\n";
    create_csv(commit_path, commit_failure, sizeof(commit_failure) - 1);
    CmnyImportPreview preview = {0};
    ASSERT_TRUE(!cmny_csv_import(&db, commit_path, true, &preview, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "cannot commit import") != NULL);
    ASSERT_EQ_I64(0, month_count(&db, "2026-07", err, sizeof(err)));

    cmny_db_close(&db);
    remove_database(db_path);
    ASSERT_TRUE(unlink(csv_path) == 0);
    ASSERT_TRUE(unlink(commit_path) == 0);
}

static void test_checked_totals(void) {
    char db_path[] = "build/cmny-csv-total-db-XXXXXX";
    char csv_path[] = "build/cmny-csv-total-XXXXXX";
    char err[256] = {0};
    CmnyDb db = {0};
    open_test_db(&db, db_path, err, sizeof(err));
    int descriptor = mkstemp(csv_path);
    ASSERT_TRUE(descriptor >= 0);
    FILE *file = fdopen(descriptor, "wb");
    ASSERT_TRUE(file != NULL);
    ASSERT_TRUE(fputs("kind,amount,category,note,date\n", file) >= 0);
    for (size_t row = 0; row < 1100; row++) {
        ASSERT_TRUE(fputs("income,90000000000000.00,C,,2026-07-01\n", file) >= 0);
    }
    ASSERT_TRUE(fclose(file) == 0);
    CmnyImportPreview preview = {0};
    ASSERT_TRUE(!cmny_csv_import(&db, csv_path, false, &preview, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "total exceeds") != NULL);
    ASSERT_TRUE(strstr(err, "record") != NULL && strstr(err, "physical line") != NULL);
    cmny_db_close(&db);
    remove_database(db_path);
    ASSERT_TRUE(unlink(csv_path) == 0);
}

static void test_million_row_limit(void) {
    char db_path[] = "build/cmny-csv-limit-db-XXXXXX";
    char csv_path[] = "build/cmny-csv-limit-XXXXXX";
    char err[256] = {0};
    CmnyDb db = {0};
    open_test_db(&db, db_path, err, sizeof(err));
    int descriptor = mkstemp(csv_path);
    ASSERT_TRUE(descriptor >= 0);
    FILE *file = fdopen(descriptor, "wb");
    ASSERT_TRUE(file != NULL);
    ASSERT_TRUE(fputs("kind,amount,category,note,date\n", file) >= 0);
    static const char row[] = "income,1,C,,2026-07-01\n";
    for (size_t index = 0; index <= 1000000U; index++) {
        ASSERT_TRUE(fwrite(row, 1, sizeof(row) - 1, file) == sizeof(row) - 1);
    }
    ASSERT_TRUE(fclose(file) == 0);
    CmnyImportPreview preview = {
        .transaction_count = 41, .income_cents = 42, .expense_cents = 43,
    };
    CmnyImportPreview original = preview;
    ASSERT_TRUE(!cmny_csv_import(&db, csv_path, false, &preview, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "1,000,000 transaction import limit") != NULL);
    ASSERT_TRUE(strstr(err, "record 1000002") != NULL);
    ASSERT_TRUE(memcmp(&preview, &original, sizeof(preview)) == 0);
    cmny_db_close(&db);
    remove_database(db_path);
    ASSERT_TRUE(unlink(csv_path) == 0);
}

int main(void) {
    test_export_security_and_round_trip();
    test_bom_crlf_and_chunk_boundaries();
    test_validation_syntax_and_atomic_rollback();
    test_database_failure_rolls_back();
    test_checked_totals();
    test_million_row_limit();
    (void)printf("ok - CSV tests\n");
    return 0;
}
