#include "cmny.h"
#include "test.h"

#include <stdio.h>
#include <string.h>
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

int main(void) {
    char source_path[] = "build/cmny-csv-source-XXXXXX";
    char target_path[] = "build/cmny-csv-target-XXXXXX";
    char export_path[] = "build/cmny-export-XXXXXX";
    char invalid_path[] = "build/cmny-invalid-csv-XXXXXX";
    int source_fd = mkstemp(source_path);
    int target_fd = mkstemp(target_path);
    int export_fd = mkstemp(export_path);
    int invalid_fd = mkstemp(invalid_path);
    ASSERT_TRUE(source_fd >= 0 && target_fd >= 0 && export_fd >= 0 && invalid_fd >= 0);
    ASSERT_TRUE(close(source_fd) == 0 && close(target_fd) == 0 && close(export_fd) == 0);
    ASSERT_TRUE(unlink(export_path) == 0);

    char err[256] = {0};
    char currency[4] = {0};
    CmnyDb source = {0};
    CmnyDb target = {0};
    ASSERT_TRUE(cmny_db_open(&source, source_path, false, "EUR", currency, err, sizeof(err)));
    CmnyTransaction expense = transaction(CMNY_EXPENSE, 1234, "Food, Drink",
                                          "Lunch with \"friends\"", "2026-07-10");
    CmnyTransaction income = transaction(CMNY_INCOME, 250000, "Salary", "July", "2026-07-01");
    ASSERT_TRUE(cmny_db_add(&source, &expense, NULL, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_add(&source, &income, NULL, err, sizeof(err)));
    size_t exported = 0;
    ASSERT_TRUE(cmny_csv_export(&source, export_path, &exported, err, sizeof(err)));
    ASSERT_EQ_I64(2, exported);
    ASSERT_TRUE(!cmny_csv_export(&source, export_path, &exported, err, sizeof(err)));
    cmny_db_close(&source);

    FILE *exported_file = fopen(export_path, "rb");
    ASSERT_TRUE(exported_file != NULL);
    char contents[1024] = {0};
    ASSERT_TRUE(fread(contents, 1, sizeof(contents) - 1, exported_file) > 0);
    ASSERT_TRUE(fclose(exported_file) == 0);
    ASSERT_TRUE(strstr(contents, "\"Food, Drink\"") != NULL);
    ASSERT_TRUE(strstr(contents, "\"Lunch with \"\"friends\"\"\"") != NULL);

    ASSERT_TRUE(cmny_db_open(&target, target_path, false, "EUR", currency, err, sizeof(err)));
    CmnyImportPreview preview = {0};
    ASSERT_TRUE(cmny_csv_import(&target, export_path, false, &preview, err, sizeof(err)));
    ASSERT_EQ_I64(2, preview.transaction_count);
    ASSERT_EQ_I64(250000, preview.income_cents);
    ASSERT_EQ_I64(1234, preview.expense_cents);
    CmnyMonthSummary summary = {0};
    ASSERT_TRUE(cmny_db_month_summary(&target, "2026-07", &summary, err, sizeof(err)));
    ASSERT_EQ_I64(0, summary.transaction_count);
    ASSERT_TRUE(cmny_csv_import(&target, export_path, true, &preview, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_month_summary(&target, "2026-07", &summary, err, sizeof(err)));
    ASSERT_EQ_I64(2, summary.transaction_count);

    FILE *invalid_file = fdopen(invalid_fd, "wb");
    ASSERT_TRUE(invalid_file != NULL);
    ASSERT_TRUE(fputs("kind,amount,category,note,date\n"
                      "expense,1.00,Food,Valid,2026-07-02\n"
                      "expense,nope,Food,Invalid,2026-07-03\n", invalid_file) >= 0);
    ASSERT_TRUE(fclose(invalid_file) == 0);
    err[0] = '\0';
    ASSERT_TRUE(!cmny_csv_import(&target, invalid_path, true, &preview, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "row 3") != NULL);
    ASSERT_TRUE(cmny_db_month_summary(&target, "2026-07", &summary, err, sizeof(err)));
    ASSERT_EQ_I64(2, summary.transaction_count);
    cmny_db_close(&target);

    remove_database(source_path);
    remove_database(target_path);
    ASSERT_TRUE(unlink(export_path) == 0);
    ASSERT_TRUE(unlink(invalid_path) == 0);
    (void)printf("ok - CSV tests\n");
    return 0;
}
