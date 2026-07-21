#include "cmny.h"
#include "test.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

_Static_assert(_Alignof(CmnyAccount) >= _Alignof(int64_t), "account must preserve int64 alignment");
_Static_assert(_Alignof(CmnyLedgerEntry) >= _Alignof(void *), "aggregate must preserve pointer alignment");
_Static_assert(offsetof(CmnyLedgerEntry, postings) % _Alignof(void *) == 0,
               "posting pointer must be naturally aligned");
_Static_assert(offsetof(CmnyLedgerEntry, posting_count) % _Alignof(size_t) == 0,
               "posting count must be naturally aligned");
_Static_assert(offsetof(CmnyNormalEntryDraft, splits) % _Alignof(void *) == 0,
               "borrowed split pointer must be naturally aligned");
_Static_assert(offsetof(CmnyNormalEntryDraft, split_count) % _Alignof(size_t) == 0,
               "split count must be naturally aligned");
_Static_assert(offsetof(CmnyLedgerEntry, type) > offsetof(CmnyLedgerEntry, tag_count),
               "narrow aggregate fields belong after pointer-sized fields");

static bool find_account(CmnyDb *db, int64_t id, CmnyAccount *found,
                         char *err, size_t err_size) {
    CmnyAccount accounts[16];
    size_t count = 0;
    if (!cmny_account_list(db, true, accounts, 16, &count, err, err_size)) return false;
    for (size_t i = 0; i < count; i++) {
        if (accounts[i].id == id) {
            *found = accounts[i];
            return true;
        }
    }
    return false;
}

static int deny_commit(void *context, int action, const char *first,
                       const char *second, const char *database,
                       const char *trigger) {
    (void)context;
    (void)second;
    (void)database;
    (void)trigger;
    return action == SQLITE_TRANSACTION && first != NULL && strcmp(first, "COMMIT") == 0
               ? SQLITE_DENY
               : SQLITE_OK;
}

int main(void) {
    char path[] = "build/cmny-ledger-XXXXXX";
    int descriptor = mkstemp(path);
    ASSERT_TRUE(descriptor >= 0);
    ASSERT_TRUE(close(descriptor) == 0);

    CmnyDb db = {0};
    char currency[4] = {0};
    char err[256] = {0};
    ASSERT_TRUE(cmny_db_open(&db, path, false, "EUR", currency, err, sizeof(err)));

    CmnyAccount accounts[8];
    size_t count = 0;
    ASSERT_TRUE(cmny_account_list(&db, false, accounts, 8, &count, err, sizeof(err)));
    ASSERT_EQ_I64(1, count);
    int64_t cash_id = accounts[0].id;
    ASSERT_TRUE(strcmp(accounts[0].name, "Cash") == 0);
    ASSERT_EQ_I64(0, accounts[0].balance_minor);

    int64_t savings_id = 0;
    ASSERT_TRUE(cmny_account_create_with_opening(&db, "Rainy day", CMNY_ACCOUNT_SAVINGS,
        "Local bank", 50000, "2026-07-01", &savings_id, err, sizeof(err)));
    int64_t balance = 0;
    ASSERT_TRUE(cmny_account_balance(&db, savings_id, &balance, err, sizeof(err)));
    ASSERT_EQ_I64(50000, balance);

    CmnyAccount savings = {0};
    ASSERT_TRUE(find_account(&db, savings_id, &savings, err, sizeof(err)));
    ASSERT_EQ_I64(50000, savings.balance_minor);
    ASSERT_TRUE(strcmp(savings.currency, "EUR") == 0);
    ASSERT_TRUE(cmny_account_update(&db, savings_id, savings.revision, "Rainy day fund",
        CMNY_ACCOUNT_SAVINGS, "Local credit union", err, sizeof(err)));
    ASSERT_TRUE(!cmny_account_update(&db, savings_id, savings.revision, "Stale name",
        CMNY_ACCOUNT_SAVINGS, "", err, sizeof(err)));
    ASSERT_TRUE(find_account(&db, savings_id, &savings, err, sizeof(err)));
    ASSERT_TRUE(strcmp(savings.name, "Rainy day fund") == 0);
    ASSERT_TRUE(!cmny_account_create_with_opening(&db, "Invalid account", CMNY_ACCOUNT_CASH,
        "", 1, "2026-02-30", NULL, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_set_authorizer(db.handle, deny_commit, NULL) == SQLITE_OK);
    ASSERT_TRUE(!cmny_account_create(&db, "Commit must roll back", CMNY_ACCOUNT_CASH,
                                     "", NULL, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_get_autocommit(db.handle) != 0);
    ASSERT_TRUE(sqlite3_set_authorizer(db.handle, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(cmny_account_list(&db, true, accounts, 8, &count, err, sizeof(err)));
    ASSERT_EQ_I64(2, count);

    sqlite3_stmt *stmt = NULL;
    ASSERT_TRUE(sqlite3_prepare_v2(db.handle,
        "SELECT e.id FROM entries e JOIN postings p ON p.entry_id=e.id"
        " WHERE e.entry_type=3 AND p.account_id=?", -1, &stmt, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_bind_int64(stmt, 1, savings_id) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(stmt) == SQLITE_ROW);
    int64_t opening_id = sqlite3_column_int64(stmt, 0);
    ASSERT_TRUE(sqlite3_finalize(stmt) == SQLITE_OK);
    CmnyLedgerEntry opening = {0};
    ASSERT_TRUE(cmny_ledger_entry_get(&db, opening_id, &opening, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_ENTRY_ADJUSTMENT, opening.type);
    ASSERT_EQ_I64(1, opening.posting_count);
    ASSERT_EQ_I64(50000, opening.postings[0].amount_minor);
    ASSERT_EQ_I64(0, opening.allocation_count);
    cmny_ledger_entry_destroy(&opening);

    int64_t food_id = 0;
    int64_t housing_id = 0;
    ASSERT_TRUE(cmny_category_create_styled(&db, "Food", CMNY_CATEGORY_EXPENSE, 0,
        "$", "amber", &food_id, err, sizeof(err)));
    ASSERT_TRUE(cmny_category_create_styled(&db, "Housing", CMNY_CATEGORY_EXPENSE, 0,
        "#", "violet", &housing_id, err, sizeof(err)));
    CmnyCategory food = {0};
    ASSERT_TRUE(cmny_category_find(&db, "Food", &food, err, sizeof(err)));
    ASSERT_EQ_I64(food_id, food.id);
    ASSERT_TRUE(strcmp(food.marker, "$") == 0);
    ASSERT_TRUE(strcmp(food.color, "amber") == 0);
    ASSERT_TRUE(cmny_category_update_styled(&db, food_id, food.revision, "Food",
        CMNY_CATEGORY_EXPENSE, 0, "F", "gold", err, sizeof(err)));
    ASSERT_TRUE(cmny_category_find(&db, "Food", &food, err, sizeof(err)));
    ASSERT_TRUE(strcmp(food.marker, "F") == 0);
    ASSERT_TRUE(strcmp(food.color, "gold") == 0);

    int64_t tag_id = 0;
    ASSERT_TRUE(cmny_tag_create(&db, "essential", &tag_id, err, sizeof(err)));
    CmnySplitDraft splits[2] = {
        {.category_id = food_id, .amount_minor = -7000, .note = "market"},
        {.category_id = housing_id, .amount_minor = -5000, .note = "supplies"}
    };
    int64_t tags[1] = {tag_id};
    CmnyNormalEntryDraft draft = {
        .account_id = cash_id,
        .amount_minor = -12000,
        .split_count = 2,
        .tag_count = 1,
        .occurred_on = "2026-07-10",
        .payee = "Corner shop",
        .note = "Shared purchase",
        .splits = splits,
        .tag_ids = tags
    };
    int64_t entry_id = 0;
    ASSERT_TRUE(cmny_entry_create_normal(&db, &draft, &entry_id, err, sizeof(err)));

    ASSERT_TRUE(sqlite3_exec(db.handle,
        "CREATE TEMP TRIGGER reject_nested_allocation BEFORE INSERT ON allocations "
        "WHEN NEW.note='reject nested' BEGIN SELECT RAISE(ABORT,'late failure'); END;"
        "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CmnySplitDraft rejected_split = {
        .category_id = food_id, .amount_minor = -100, .note = "reject nested"
    };
    CmnyNormalEntryDraft rejected_draft = {
        .account_id = cash_id,
        .amount_minor = -100,
        .split_count = 1,
        .occurred_on = "2026-07-09",
        .payee = "Must not persist",
        .note = "savepoint regression",
        .splits = &rejected_split,
    };
    ASSERT_TRUE(!cmny_entry_create_normal(&db, &rejected_draft, NULL, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_get_autocommit(db.handle) == 0);
    sqlite3_stmt *atomic = NULL;
    ASSERT_TRUE(sqlite3_prepare_v2(db.handle,
        "SELECT COUNT(*) FROM entries WHERE payee='Must not persist'", -1,
        &atomic, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(atomic) == SQLITE_ROW);
    ASSERT_EQ_I64(0, sqlite3_column_int64(atomic, 0));
    ASSERT_TRUE(sqlite3_finalize(atomic) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(db.handle,
        "ROLLBACK;DROP TRIGGER reject_nested_allocation", NULL, NULL, NULL) == SQLITE_OK);

    CmnyLedgerEntry entry = {0};
    ASSERT_TRUE(cmny_ledger_entry_get(&db, entry_id, &entry, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_ENTRY_NORMAL, entry.type);
    ASSERT_EQ_I64(1, entry.posting_count);
    ASSERT_EQ_I64(2, entry.allocation_count);
    ASSERT_EQ_I64(1, entry.tag_count);
    ASSERT_EQ_I64(-12000, entry.postings[0].amount_minor);
    ASSERT_EQ_I64(tag_id, entry.tag_ids[0]);
    int64_t entry_revision = entry.revision;
    cmny_ledger_entry_destroy(&entry);
    ASSERT_TRUE(entry.postings == NULL);

    CmnySplitDraft invalid_splits[2] = {
        {.category_id = food_id, .amount_minor = -6000, .note = ""},
        {.category_id = housing_id, .amount_minor = -5000, .note = ""}
    };
    CmnyNormalEntryDraft invalid_draft = draft;
    invalid_draft.splits = invalid_splits;
    ASSERT_TRUE(!cmny_entry_create_normal(&db, &invalid_draft, NULL, err, sizeof(err)));

    CmnyMonthSummary summary = {0};
    ASSERT_TRUE(cmny_db_month_summary(&db, "2026-07", &summary, err, sizeof(err)));
    ASSERT_EQ_I64(0, summary.income_cents);
    ASSERT_EQ_I64(12000, summary.expense_cents);
    ASSERT_EQ_I64(1, summary.transaction_count);

    ASSERT_TRUE(cmny_db_budget_set(&db, "2026-07", "Food", 10000, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_budget_set(&db, "2026-07", "Housing", 8000, err, sizeof(err)));
    CmnyBudget budgets[4];
    ASSERT_TRUE(cmny_db_budget_list(&db, "2026-07", budgets, 4, &count, err, sizeof(err)));
    ASSERT_EQ_I64(2, count);
    ASSERT_EQ_I64(7000, budgets[0].spent_cents);
    ASSERT_EQ_I64(5000, budgets[1].spent_cents);

    CmnyTransferDraft transfer = {
        .from_account_id = cash_id,
        .to_account_id = savings_id,
        .amount_minor = 10000,
        .tag_count = 1,
        .occurred_on = "2026-07-11",
        .payee = "Internal",
        .note = "Save",
        .tag_ids = tags
    };
    int64_t transfer_id = 0;
    ASSERT_TRUE(cmny_transfer_create(&db, &transfer, &transfer_id, err, sizeof(err)));
    CmnyLedgerEntry transfer_entry = {0};
    ASSERT_TRUE(cmny_ledger_entry_get(&db, transfer_id, &transfer_entry, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_ENTRY_TRANSFER, transfer_entry.type);
    ASSERT_EQ_I64(2, transfer_entry.posting_count);
    ASSERT_EQ_I64(0, transfer_entry.allocation_count);
    ASSERT_EQ_I64(0, transfer_entry.postings[0].amount_minor + transfer_entry.postings[1].amount_minor);
    cmny_ledger_entry_destroy(&transfer_entry);

    ASSERT_TRUE(cmny_account_balance(&db, cash_id, &balance, err, sizeof(err)));
    ASSERT_EQ_I64(-22000, balance);
    ASSERT_TRUE(cmny_account_balance(&db, savings_id, &balance, err, sizeof(err)));
    ASSERT_EQ_I64(60000, balance);
    ASSERT_TRUE(cmny_db_month_summary(&db, "2026-07", &summary, err, sizeof(err)));
    ASSERT_EQ_I64(12000, summary.expense_cents);
    ASSERT_EQ_I64(1, summary.transaction_count);

    ASSERT_TRUE(!cmny_entry_update_normal(&db, entry_id, entry_revision + 1, &draft,
                                           err, sizeof(err)));
    splits[0].amount_minor = -4000;
    splits[1].amount_minor = -5000;
    draft.amount_minor = -9000;
    ASSERT_TRUE(cmny_entry_update_normal(&db, entry_id, entry_revision, &draft, err, sizeof(err)));
    ASSERT_TRUE(cmny_account_balance(&db, cash_id, &balance, err, sizeof(err)));
    ASSERT_EQ_I64(-19000, balance);

    ASSERT_TRUE(cmny_category_merge(&db, food_id, housing_id, err, sizeof(err)));
    ASSERT_TRUE(cmny_category_find(&db, "Food", &food, err, sizeof(err)));
    ASSERT_TRUE(food.archived);
    ASSERT_EQ_I64(housing_id, food.merged_into_id);
    ASSERT_EQ_I64(food_id, food.id);
    ASSERT_TRUE(cmny_db_budget_list(&db, "2026-07", budgets, 4, &count, err, sizeof(err)));
    ASSERT_EQ_I64(1, count);
    ASSERT_TRUE(strcmp(budgets[0].category, "Housing") == 0);
    ASSERT_EQ_I64(18000, budgets[0].limit_cents);
    ASSERT_EQ_I64(9000, budgets[0].spent_cents);

    CmnyTag tag_rows[4];
    ASSERT_TRUE(cmny_tag_list(&db, false, tag_rows, 4, &count, err, sizeof(err)));
    ASSERT_EQ_I64(1, count);
    ASSERT_TRUE(cmny_tag_update(&db, tag_id, tag_rows[0].revision, "core", err, sizeof(err)));
    ASSERT_TRUE(cmny_tag_list(&db, false, tag_rows, 4, &count, err, sizeof(err)));
    ASSERT_TRUE(strcmp(tag_rows[0].name, "core") == 0);
    ASSERT_TRUE(cmny_tag_set_archived(&db, tag_id, tag_rows[0].revision, true, err, sizeof(err)));
    transfer.amount_minor = 1;
    ASSERT_TRUE(!cmny_transfer_create(&db, &transfer, NULL, err, sizeof(err)));
    ASSERT_TRUE(find_account(&db, savings_id, &savings, err, sizeof(err)));
    ASSERT_TRUE(cmny_account_set_archived(&db, savings_id, savings.revision, true, err, sizeof(err)));
    transfer.tag_count = 0;
    transfer.tag_ids = NULL;
    ASSERT_TRUE(!cmny_transfer_create(&db, &transfer, NULL, err, sizeof(err)));

    ASSERT_TRUE(cmny_db_check(&db, err, sizeof(err)));
    cmny_db_close(&db);
    ASSERT_TRUE(unlink(path) == 0);
    char sidecar[4200];
    (void)snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    (void)unlink(sidecar);
    (void)snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    (void)unlink(sidecar);
    (void)printf("ok - ledger tests\n");
    return 0;
}
