#include "cmny.h"
#include "test.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    char symlink_target[] = "build/cmny-symlink-target-XXXXXX";
    int symlink_descriptor = mkstemp(symlink_target);
    ASSERT_TRUE(symlink_descriptor >= 0);
    ASSERT_TRUE(close(symlink_descriptor) == 0);
    char symlink_path[] = "build/cmny-symlink-path-XXXXXX";
    int placeholder = mkstemp(symlink_path);
    ASSERT_TRUE(placeholder >= 0);
    ASSERT_TRUE(close(placeholder) == 0);
    ASSERT_TRUE(unlink(symlink_path) == 0);
    ASSERT_TRUE(symlink(strrchr(symlink_target, '/') + 1, symlink_path) == 0);

    char foreign_path[] = "build/cmny-foreign-XXXXXX";
    int foreign_descriptor = mkstemp(foreign_path);
    ASSERT_TRUE(foreign_descriptor >= 0);
    ASSERT_TRUE(close(foreign_descriptor) == 0);
    ASSERT_TRUE(chmod(foreign_path, 0640) == 0);
    sqlite3 *foreign = NULL;
    ASSERT_TRUE(sqlite3_open(foreign_path, &foreign) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(foreign, "CREATE TABLE somebody_elses_data(value TEXT)",
                             NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(foreign) == SQLITE_OK);
    struct stat foreign_before;
    ASSERT_TRUE(stat(foreign_path, &foreign_before) == 0);

    char path[] = "build/cmny-test-XXXXXX";
    int descriptor = mkstemp(path);
    ASSERT_TRUE(descriptor >= 0);
    ASSERT_TRUE(close(descriptor) == 0);

    char err[256] = {0};
    char currency[4] = {0};
    CmnyDb db = {0};
    ASSERT_TRUE(!cmny_db_open(&db, symlink_path, false, "EUR", currency, err, sizeof(err)));
    struct stat untouched_target;
    ASSERT_TRUE(stat(symlink_target, &untouched_target) == 0);
    ASSERT_EQ_I64(0, untouched_target.st_size);
    ASSERT_TRUE(unlink(symlink_path) == 0);
    ASSERT_TRUE(unlink(symlink_target) == 0);

    ASSERT_TRUE(!cmny_db_open(&db, foreign_path, false, "EUR", currency, err, sizeof(err)));
    struct stat foreign_after;
    ASSERT_TRUE(stat(foreign_path, &foreign_after) == 0);
    ASSERT_EQ_I64(foreign_before.st_mode & 0777, foreign_after.st_mode & 0777);
    ASSERT_TRUE(sqlite3_open(foreign_path, &foreign) == SQLITE_OK);
    sqlite3_stmt *foreign_check = NULL;
    ASSERT_TRUE(sqlite3_prepare_v2(foreign,
        "SELECT COUNT(*) FROM somebody_elses_data", -1, &foreign_check, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(foreign_check) == SQLITE_ROW);
    ASSERT_TRUE(sqlite3_finalize(foreign_check) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(foreign) == SQLITE_OK);
    ASSERT_TRUE(unlink(foreign_path) == 0);

    ASSERT_TRUE(cmny_db_open(&db, path, false, "EUR", currency, err, sizeof(err)));
    ASSERT_TRUE(strcmp(currency, "EUR") == 0);
    int schema_version = 0;
    ASSERT_TRUE(sqlite3_prepare_v2(db.handle, "PRAGMA user_version", -1, &foreign_check, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(foreign_check) == SQLITE_ROW);
    schema_version = sqlite3_column_int(foreign_check, 0);
    ASSERT_EQ_I64(5, schema_version);
    ASSERT_TRUE(sqlite3_finalize(foreign_check) == SQLITE_OK);
    foreign_check = NULL;

    ASSERT_TRUE(sqlite3_exec(db.handle,
        "INSERT INTO transactions(kind,amount_cents,category,note,occurred_on) "
        "VALUES(1,100,'Food','bad date','2026-13-01')", NULL, NULL, NULL) != SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(db.handle,
        "INSERT INTO transactions(kind,amount_cents,category,note,occurred_on) "
        "VALUES(1,100,'Food','bad day','2026-02-30')", NULL, NULL, NULL) != SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(db.handle,
        "INSERT INTO transactions(kind,amount_cents,category,note,occurred_on) "
        "VALUES(1,100,'Food','bad leap day','2025-02-29')", NULL, NULL, NULL) != SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(db.handle,
        "INSERT INTO transactions(kind,amount_cents,category,note,occurred_on) "
        "VALUES(1,100,'Caf\xC3\xA9','','2026-07-01')", NULL, NULL, NULL) != SQLITE_OK);

    CmnyTransaction salary = transaction(CMNY_INCOME, 300000, "Salary", "July salary", "2026-07-01");
    CmnyTransaction rent = transaction(CMNY_EXPENSE, 100000, "Housing", "Rent", "2026-07-02");
    CmnyTransaction food = transaction(CMNY_EXPENSE, 2530, "Food", "Groceries", "2026-07-12");
    CmnyTransaction old = transaction(CMNY_EXPENSE, 4500, "Food", "June meals", "2026-06-20");
    ASSERT_TRUE(cmny_db_add(&db, &salary, &salary.id, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_add(&db, &rent, &rent.id, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_add(&db, &food, &food.id, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_add(&db, &old, &old.id, err, sizeof(err)));

    CmnyMonthSummary summary = {0};
    ASSERT_TRUE(cmny_db_month_summary(&db, "2026-07", &summary, err, sizeof(err)));
    ASSERT_EQ_I64(300000, summary.income_cents);
    ASSERT_EQ_I64(102530, summary.expense_cents);
    ASSERT_EQ_I64(3, summary.transaction_count);

    CmnyTransaction rows[8];
    size_t count = 0;
    ASSERT_TRUE(cmny_db_list(&db, "2026-07", "", 0, 0, rows, 8, &count, err, sizeof(err)));
    ASSERT_EQ_I64(3, count);
    ASSERT_EQ_I64(food.id, rows[0].id);
    ASSERT_TRUE(cmny_db_list(&db, "2026-07", "Rent", CMNY_EXPENSE, 0,
                             rows, 8, &count, err, sizeof(err)));
    ASSERT_EQ_I64(1, count);
    ASSERT_EQ_I64(rent.id, rows[0].id);
    ASSERT_TRUE(cmny_db_list(&db, "2026-07", "June", CMNY_EXPENSE, 0,
                             rows, 8, &count, err, sizeof(err)));
    ASSERT_EQ_I64(1, count);
    ASSERT_EQ_I64(old.id, rows[0].id);
    ASSERT_TRUE(cmny_db_count(&db, "2026-07", "", 0, &count, err, sizeof(err)));
    ASSERT_EQ_I64(3, count);
    ASSERT_TRUE(cmny_db_list(&db, "2026-07", "", 0, 2, rows, 8, &count, err, sizeof(err)));
    ASSERT_EQ_I64(1, count);
    ASSERT_EQ_I64(salary.id, rows[0].id);

    CmnyCategoryTotal categories[8];
    ASSERT_TRUE(cmny_db_category_totals(&db, "2026-07", categories, 8, &count, err, sizeof(err)));
    ASSERT_EQ_I64(2, count);
    ASSERT_TRUE(strcmp(categories[0].category, "Housing") == 0);
    ASSERT_EQ_I64(100000, categories[0].amount_cents);

    char setting[32] = {0};
    ASSERT_TRUE(cmny_db_setting_set(&db, "theme", "violet", err, sizeof(err)));
    ASSERT_TRUE(cmny_db_setting_get(&db, "theme", setting, sizeof(setting)));
    ASSERT_TRUE(strcmp(setting, "violet") == 0);

    ASSERT_TRUE(cmny_db_budget_set(&db, "2026-07", "Food", 30000, err, sizeof(err)));
    CmnyBudget budgets[4];
    ASSERT_TRUE(cmny_db_budget_list(&db, "2026-07", budgets, 4, &count, err, sizeof(err)));
    ASSERT_EQ_I64(1, count);
    ASSERT_TRUE(strcmp(budgets[0].category, "Food") == 0);
    ASSERT_EQ_I64(30000, budgets[0].limit_cents);
    ASSERT_EQ_I64(2530, budgets[0].spent_cents);

    ASSERT_TRUE(cmny_db_recurring_add(&db, &salary, err, sizeof(err)));
    CmnyRecurring recurring[4];
    ASSERT_TRUE(cmny_db_recurring_list(&db, recurring, 4, &count, err, sizeof(err)));
    ASSERT_EQ_I64(1, count);
    ASSERT_TRUE(strcmp(recurring[0].category, "Salary") == 0);
    ASSERT_EQ_I64(1, recurring[0].day_of_month);

    food.amount_cents = 4000;
    (void)snprintf(food.note, sizeof(food.note), "Groceries and lunch");
    ASSERT_TRUE(cmny_db_update(&db, &food, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_month_summary(&db, "2026-07", &summary, err, sizeof(err)));
    ASSERT_EQ_I64(104000, summary.expense_cents);

    CmnyMonthTrend trend[CMNY_TREND_MONTHS];
    ASSERT_TRUE(cmny_db_trend(&db, "2026-07", trend, CMNY_TREND_MONTHS, err, sizeof(err)));
    ASSERT_TRUE(strcmp(trend[CMNY_TREND_MONTHS - 1].month, "2026-07") == 0);
    ASSERT_EQ_I64(104000, trend[CMNY_TREND_MONTHS - 1].expense_cents);

    ASSERT_TRUE(cmny_db_delete(&db, rent.id, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_month_summary(&db, "2026-07", &summary, err, sizeof(err)));
    ASSERT_EQ_I64(4000, summary.expense_cents);

    CmnyTransaction invalid = transaction(CMNY_EXPENSE, 0, "Food", "bad", "2026-02-30");
    ASSERT_TRUE(!cmny_db_add(&db, &invalid, NULL, err, sizeof(err)));

    char backup_path[] = "build/cmny-backup-XXXXXX";
    int backup_descriptor = mkstemp(backup_path);
    ASSERT_TRUE(backup_descriptor >= 0);
    ASSERT_TRUE(close(backup_descriptor) == 0);
    ASSERT_TRUE(unlink(backup_path) == 0);
    ASSERT_TRUE(cmny_db_check(&db, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_backup(&db, backup_path, err, sizeof(err)));
    CmnyTransaction extra = transaction(CMNY_EXPENSE, 999, "Fun", "Temporary", "2026-07-22");
    ASSERT_TRUE(cmny_db_add(&db, &extra, NULL, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_month_summary(&db, "2026-07", &summary, err, sizeof(err)));
    ASSERT_EQ_I64(3, summary.transaction_count);
    ASSERT_TRUE(cmny_db_restore(&db, backup_path, currency, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_month_summary(&db, "2026-07", &summary, err, sizeof(err)));
    ASSERT_EQ_I64(2, summary.transaction_count);
    ASSERT_TRUE(unlink(backup_path) == 0);

    cmny_db_close(&db);
    ASSERT_TRUE(!cmny_db_open(&db, path, false, "USD", currency, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(strcmp(currency, "EUR") == 0);
    ASSERT_TRUE(cmny_db_month_summary(&db, "2026-07", &summary, err, sizeof(err)));
    ASSERT_EQ_I64(2, summary.transaction_count);
    cmny_db_close(&db);

    sqlite3 *tampered = NULL;
    ASSERT_TRUE(sqlite3_open(path, &tampered) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(tampered,
        "PRAGMA ignore_check_constraints=ON;"
        "UPDATE postings SET amount_minor=0 WHERE entry_id=3", NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(tampered) == SQLITE_OK);
    ASSERT_TRUE(cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(!cmny_db_check(&db, err, sizeof(err)));
    ASSERT_TRUE(!cmny_db_list(&db, "2026-07", "", 0, 0, rows, 8, &count, err, sizeof(err)));
    cmny_db_close(&db);
    ASSERT_TRUE(sqlite3_open(path, &tampered) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(tampered,
        "PRAGMA ignore_check_constraints=ON;"
        "UPDATE postings SET amount_minor=-4000 WHERE entry_id=3;"
        "UPDATE entries SET note=printf('%241c','x') WHERE id=3",
        NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(tampered) == SQLITE_OK);
    ASSERT_TRUE(cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(!cmny_db_check(&db, err, sizeof(err)));
    ASSERT_TRUE(!cmny_db_list(&db, "2026-07", "", 0, 0, rows, 8, &count, err, sizeof(err)));
    cmny_db_close(&db);
    ASSERT_TRUE(sqlite3_open(path, &tampered) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(tampered, "UPDATE entries SET note='Groceries and lunch' WHERE id=3",
                             NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(tampered) == SQLITE_OK);

    ASSERT_TRUE(sqlite3_open(path, &tampered) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(tampered,
        "PRAGMA ignore_check_constraints=ON;"
        "INSERT INTO settings(key,value) VALUES('bad' || char(10) || 'key','value');"
        "INSERT INTO budgets(month,category,limit_cents) VALUES('2026-99','Tampered',1);"
        "INSERT INTO recurring(kind,amount_cents,category,note,day_of_month) "
        "VALUES(1,1,'Tampered','',99)", NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(tampered) == SQLITE_OK);
    ASSERT_TRUE(cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(!cmny_db_check(&db, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_exec(db.handle, "DELETE FROM settings WHERE key LIKE 'bad%key'",
                             NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(!cmny_db_check(&db, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_exec(db.handle, "DELETE FROM budgets WHERE category='Tampered'",
                             NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(!cmny_db_check(&db, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_exec(db.handle, "DELETE FROM recurring WHERE category='Tampered'",
                             NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(cmny_db_check(&db, err, sizeof(err)));
    cmny_db_close(&db);

    char v2_path[] = "build/cmny-v2-XXXXXX";
    int v2_descriptor = mkstemp(v2_path);
    ASSERT_TRUE(v2_descriptor >= 0);
    ASSERT_TRUE(close(v2_descriptor) == 0);
    sqlite3 *v2 = NULL;
    ASSERT_TRUE(sqlite3_open(v2_path, &v2) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(v2,
        "CREATE TABLE transactions(id INTEGER PRIMARY KEY,kind INTEGER NOT NULL,amount_cents INTEGER NOT NULL,"
        "category TEXT NOT NULL,note TEXT NOT NULL,occurred_on TEXT NOT NULL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL) STRICT;"
        "CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT NOT NULL) STRICT;"
        "CREATE TABLE budgets(month TEXT NOT NULL,category TEXT NOT NULL,limit_cents INTEGER NOT NULL,PRIMARY KEY(month,category)) STRICT;"
        "CREATE TABLE recurring(id INTEGER PRIMARY KEY,kind INTEGER NOT NULL,amount_cents INTEGER NOT NULL,category TEXT NOT NULL,"
        "note TEXT NOT NULL,day_of_month INTEGER NOT NULL,created_at INTEGER NOT NULL,UNIQUE(kind,amount_cents,category,note,day_of_month)) STRICT;"
        "INSERT INTO settings VALUES('currency','EUR');"
        "INSERT INTO transactions VALUES(41,1,1250,'Legacy food','Imported','2026-08-04',10,11);"
        "INSERT INTO transactions VALUES(42,2,9900,'Legacy pay','Imported','2026-08-01',12,13);"
        "INSERT INTO budgets VALUES('2026-08','Legacy food',5000);"
        "INSERT INTO recurring VALUES(7,1,1250,'Legacy food','Imported',4,14);"
        "PRAGMA application_id=1129139801;PRAGMA user_version=2", NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(v2) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_open(v2_path, &v2) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(v2, "UPDATE transactions SET amount_cents=0 WHERE id=41",
                             NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(v2) == SQLITE_OK);
    ASSERT_TRUE(!cmny_db_open(&db, v2_path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_open(v2_path, &v2) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_prepare_v2(v2,
        "SELECT (SELECT user_version FROM pragma_user_version),"
        "(SELECT COUNT(*) FROM sqlite_schema WHERE type='table' AND name='transactions'),"
        "(SELECT COUNT(*) FROM sqlite_schema WHERE type='table' AND name='accounts')",
        -1, &foreign_check, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(foreign_check) == SQLITE_ROW);
    ASSERT_EQ_I64(2, sqlite3_column_int(foreign_check, 0));
    ASSERT_EQ_I64(1, sqlite3_column_int(foreign_check, 1));
    ASSERT_EQ_I64(0, sqlite3_column_int(foreign_check, 2));
    ASSERT_TRUE(sqlite3_finalize(foreign_check) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(v2, "UPDATE transactions SET amount_cents=1250 WHERE id=41",
                             NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(v2) == SQLITE_OK);
    ASSERT_TRUE(cmny_db_open(&db, v2_path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_prepare_v2(db.handle, "PRAGMA user_version", -1, &foreign_check, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(foreign_check) == SQLITE_ROW);
    schema_version = sqlite3_column_int(foreign_check, 0);
    ASSERT_EQ_I64(5, schema_version);
    ASSERT_TRUE(sqlite3_finalize(foreign_check) == SQLITE_OK);
    ASSERT_TRUE(cmny_db_month_summary(&db, "2026-08", &summary, err, sizeof(err)));
    ASSERT_EQ_I64(9900, summary.income_cents);
    ASSERT_EQ_I64(1250, summary.expense_cents);
    ASSERT_EQ_I64(2, summary.transaction_count);
    ASSERT_TRUE(cmny_db_check(&db, err, sizeof(err)));
    cmny_db_close(&db);
    ASSERT_TRUE(unlink(v2_path) == 0);

    char v4_path[] = "build/cmny-v4-XXXXXX";
    int v4_descriptor = mkstemp(v4_path);
    ASSERT_TRUE(v4_descriptor >= 0);
    ASSERT_TRUE(close(v4_descriptor) == 0);
    ASSERT_TRUE(cmny_db_open(&db, v4_path, false, "EUR", currency, err, sizeof(err)));
    cmny_db_close(&db);
    sqlite3 *v4 = NULL;
    ASSERT_TRUE(sqlite3_open(v4_path, &v4) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(v4,
        "DROP TABLE import_records;DROP TABLE import_batches;"
        "DROP TABLE categorization_rules;DROP TABLE import_profiles;"
        "PRAGMA user_version=4;CREATE TABLE categorization_rules(id INTEGER)",
        NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(v4) == SQLITE_OK);
    ASSERT_TRUE(!cmny_db_open(&db, v4_path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_open(v4_path, &v4) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_prepare_v2(v4,
        "SELECT (SELECT user_version FROM pragma_user_version),"
        "(SELECT COUNT(*) FROM sqlite_schema WHERE type='table' AND name='import_profiles')",
        -1, &foreign_check, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(foreign_check) == SQLITE_ROW);
    ASSERT_EQ_I64(4, sqlite3_column_int(foreign_check, 0));
    ASSERT_EQ_I64(0, sqlite3_column_int(foreign_check, 1));
    ASSERT_TRUE(sqlite3_finalize(foreign_check) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(v4, "DROP TABLE categorization_rules", NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(v4) == SQLITE_OK);
    ASSERT_TRUE(cmny_db_open(&db, v4_path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_check(&db, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_prepare_v2(db.handle, "PRAGMA user_version", -1,
                                   &foreign_check, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(foreign_check) == SQLITE_ROW);
    ASSERT_EQ_I64(5, sqlite3_column_int(foreign_check, 0));
    ASSERT_TRUE(sqlite3_finalize(foreign_check) == SQLITE_OK);
    cmny_db_close(&db);
    ASSERT_TRUE(unlink(v4_path) == 0);

    sqlite3 *corrupt = NULL;
    ASSERT_TRUE(sqlite3_open(path, &corrupt) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(corrupt, "UPDATE settings SET value='JPY' WHERE key='currency'",
                             NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(corrupt) == SQLITE_OK);
    ASSERT_TRUE(!cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));

    ASSERT_TRUE(unlink(path) == 0);
    char sidecar[4200];
    (void)snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    (void)unlink(sidecar);
    (void)snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    (void)unlink(sidecar);
    (void)printf("ok - database tests\n");
    return 0;
}
