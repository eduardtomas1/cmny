#include "cmny_history.h"
#include "test.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

_Static_assert(_Alignof(CmnyHistoryAction) >= _Alignof(int64_t),
               "history actions must preserve int64 alignment");
_Static_assert(offsetof(CmnyHistoryAction, type) > offsetof(CmnyHistoryAction, undone_at),
               "narrow history fields belong after int64 fields");

static int64_t first_account(CmnyDb *db, char *err, size_t err_size) {
    CmnyAccount account[2];
    size_t count = 0;
    ASSERT_TRUE(cmny_account_list(db, false, account, 2, &count, err, err_size));
    ASSERT_TRUE(count > 0);
    return account[0].id;
}

static int64_t add_normal(CmnyDb *db, int64_t account_id, int64_t category_id,
                          int64_t amount, const char *note, char *err, size_t err_size) {
    CmnySplitDraft split = {.category_id = category_id, .amount_minor = amount, .note = "part"};
    CmnyNormalEntryDraft draft = {
        .account_id = account_id,
        .amount_minor = amount,
        .split_count = 1,
        .occurred_on = "2026-09-10",
        .payee = "History shop",
        .note = note,
        .splits = &split
    };
    int64_t id = 0;
    ASSERT_TRUE(cmny_entry_create_normal(db, &draft, &id, err, err_size));
    return id;
}

int main(void) {
    char path[] = "build/cmny-history-XXXXXX";
    int descriptor = mkstemp(path);
    ASSERT_TRUE(descriptor >= 0);
    ASSERT_TRUE(close(descriptor) == 0);
    CmnyDb db = {0};
    char currency[4] = {0};
    char err[256] = {0};
    ASSERT_TRUE(cmny_db_open(&db, path, false, "EUR", currency, err, sizeof(err)));
    int64_t account_id = first_account(&db, err, sizeof(err));
    int64_t category_id = 0;
    int64_t tag_id = 0;
    ASSERT_TRUE(cmny_category_create(&db, "History", CMNY_CATEGORY_EXPENSE, 0,
                                     &category_id, err, sizeof(err)));
    ASSERT_TRUE(cmny_tag_create(&db, "remember", &tag_id, err, sizeof(err)));

    int64_t entry_id = add_normal(&db, account_id, category_id, -1200, "original",
                                  err, sizeof(err));
    CmnyHistoryAction actions[16];
    size_t count = 0;
    ASSERT_TRUE(cmny_history_list(&db, false, 0, actions, 16, &count, err, sizeof(err)));
    ASSERT_EQ_I64(1, count);
    ASSERT_EQ_I64(CMNY_HISTORY_CREATE, actions[0].type);
    ASSERT_EQ_I64(entry_id, actions[0].entry_id);
    ASSERT_EQ_I64(0, actions[0].before_revision);
    ASSERT_EQ_I64(1, actions[0].after_revision);
    int64_t create_action = actions[0].id;
    cmny_db_close(&db);

    ASSERT_TRUE(cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    int64_t new_revision = 0;
    ASSERT_TRUE(cmny_history_undo(&db, create_action, 1, &new_revision, err, sizeof(err)));
    ASSERT_EQ_I64(2, new_revision);
    CmnyLedgerEntry aggregate = {0};
    ASSERT_TRUE(!cmny_ledger_entry_get(&db, entry_id, &aggregate, err, sizeof(err)));
    ASSERT_TRUE(!cmny_history_undo(&db, create_action, 1, NULL, err, sizeof(err)));
    ASSERT_TRUE(cmny_history_list(&db, true, 0, actions, 16, &count, err, sizeof(err)));
    ASSERT_TRUE(actions[0].undone);

    entry_id = add_normal(&db, account_id, category_id, -2500, "before update",
                          err, sizeof(err));
    ASSERT_TRUE(cmny_ledger_entry_get(&db, entry_id, &aggregate, err, sizeof(err)));
    int64_t initial_revision = aggregate.revision;
    cmny_ledger_entry_destroy(&aggregate);
    CmnySplitDraft updated_splits[2] = {
        {.category_id = category_id, .amount_minor = -1000, .note = "one"},
        {.category_id = category_id, .amount_minor = -2000, .note = "two"}
    };
    int64_t tag_ids[1] = {tag_id};
    CmnyNormalEntryDraft updated = {
        .account_id = account_id,
        .amount_minor = -3000,
        .split_count = 2,
        .tag_count = 1,
        .occurred_on = "2026-09-11",
        .payee = "Updated payee",
        .note = "after update",
        .splits = updated_splits,
        .tag_ids = tag_ids
    };
    ASSERT_TRUE(cmny_entry_update_normal(&db, entry_id, initial_revision, &updated,
                                         err, sizeof(err)));
    ASSERT_TRUE(cmny_history_list(&db, false, 0, actions, 16, &count, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_HISTORY_UPDATE, actions[0].type);
    int64_t update_action = actions[0].id;
    int64_t update_revision = actions[0].after_revision;
    cmny_db_close(&db);
    ASSERT_TRUE(cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(cmny_history_undo(&db, update_action, update_revision, &new_revision,
                                  err, sizeof(err)));
    ASSERT_TRUE(cmny_ledger_entry_get(&db, entry_id, &aggregate, err, sizeof(err)));
    ASSERT_TRUE(strcmp(aggregate.note, "before update") == 0);
    ASSERT_TRUE(strcmp(aggregate.payee, "History shop") == 0);
    ASSERT_EQ_I64(-2500, aggregate.postings[0].amount_minor);
    ASSERT_EQ_I64(1, aggregate.allocation_count);
    ASSERT_EQ_I64(0, aggregate.tag_count);
    ASSERT_EQ_I64(new_revision, aggregate.revision);
    int64_t restored_revision = aggregate.revision;
    cmny_ledger_entry_destroy(&aggregate);

    ASSERT_TRUE(cmny_entry_delete(&db, entry_id, restored_revision, err, sizeof(err)));
    ASSERT_TRUE(cmny_history_list(&db, false, 0, actions, 16, &count, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_HISTORY_DELETE, actions[0].type);
    int64_t delete_action = actions[0].id;
    int64_t delete_revision = actions[0].after_revision;
    cmny_db_close(&db);
    ASSERT_TRUE(cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(!cmny_ledger_entry_get(&db, entry_id, &aggregate, err, sizeof(err)));
    ASSERT_TRUE(cmny_history_undo(&db, delete_action, delete_revision, &new_revision,
                                  err, sizeof(err)));
    ASSERT_TRUE(cmny_ledger_entry_get(&db, entry_id, &aggregate, err, sizeof(err)));
    ASSERT_EQ_I64(-2500, aggregate.postings[0].amount_minor);
    cmny_ledger_entry_destroy(&aggregate);

    int64_t second_account = 0;
    ASSERT_TRUE(cmny_account_create(&db, "History savings", CMNY_ACCOUNT_SAVINGS, "",
                                    &second_account, err, sizeof(err)));
    CmnyTransferDraft transfer = {
        .from_account_id = account_id,
        .to_account_id = second_account,
        .amount_minor = 400,
        .occurred_on = "2026-09-12",
        .payee = "Internal",
        .note = "history transfer"
    };
    int64_t transfer_id = 0;
    ASSERT_TRUE(cmny_transfer_create(&db, &transfer, &transfer_id, err, sizeof(err)));
    ASSERT_TRUE(cmny_history_list(&db, false, 0, actions, 16, &count, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_HISTORY_TRANSFER, actions[0].type);
    int64_t transfer_action = actions[0].id;
    int64_t transfer_revision = actions[0].after_revision;
    cmny_db_close(&db);
    ASSERT_TRUE(cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(cmny_history_undo(&db, transfer_action, transfer_revision, NULL,
                                  err, sizeof(err)));
    ASSERT_TRUE(!cmny_ledger_entry_get(&db, transfer_id, &aggregate, err, sizeof(err)));

    CmnyTransaction legacy = {0};
    legacy.kind = CMNY_EXPENSE;
    legacy.amount_cents = 725;
    (void)snprintf(legacy.category, sizeof(legacy.category), "History");
    (void)snprintf(legacy.note, sizeof(legacy.note), "legacy");
    (void)snprintf(legacy.occurred_on, sizeof(legacy.occurred_on), "2026-09-13");
    ASSERT_TRUE(cmny_db_add(&db, &legacy, &legacy.id, err, sizeof(err)));
    legacy.amount_cents = 800;
    ASSERT_TRUE(cmny_db_update(&db, &legacy, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_delete(&db, legacy.id, err, sizeof(err)));
    ASSERT_TRUE(cmny_history_list(&db, false, 0, actions, 16, &count, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_HISTORY_DELETE, actions[0].type);
    ASSERT_EQ_I64(legacy.id, actions[0].entry_id);
    ASSERT_TRUE(cmny_history_undo_latest(&db, NULL, NULL, err, sizeof(err)));
    CmnyTransaction rows[4];
    ASSERT_TRUE(cmny_db_list(&db, "2026-09", "legacy", 0, 0, rows, 4, &count,
                             err, sizeof(err)));
    ASSERT_EQ_I64(1, count);

    ASSERT_TRUE(!cmny_history_list(&db, true, 0, actions,
                                    CMNY_HISTORY_LIST_LIMIT + 1U, &count, err, sizeof(err)));
    size_t removed = 0;
    ASSERT_TRUE(cmny_history_prune(&db, 2, &removed, err, sizeof(err)));
    ASSERT_TRUE(removed > 0);
    ASSERT_TRUE(cmny_history_list(&db, true, 0, actions, 16, &count, err, sizeof(err)));
    ASSERT_EQ_I64(2, count);
    ASSERT_TRUE(cmny_db_check(&db, err, sizeof(err)));
    cmny_db_close(&db);

    sqlite3 *raw = NULL;
    ASSERT_TRUE(sqlite3_open(path, &raw) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(raw,
        "DROP TABLE import_records;DROP TABLE import_batches;"
        "DROP TABLE categorization_rules;DROP TABLE import_profiles;"
        "DROP TABLE reconciliation_items;DROP TABLE reconciliation_sessions;"
        "DROP TABLE history_tag_snapshots;DROP TABLE history_allocation_snapshots;"
        "DROP TABLE history_posting_snapshots;DROP TABLE history_entry_snapshots;"
        "DROP TABLE history_actions;PRAGMA user_version=3;"
        "CREATE TABLE reconciliation_sessions(clash INTEGER) STRICT", NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(raw) == SQLITE_OK);
    ASSERT_TRUE(!cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_open(path, &raw) == SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    ASSERT_TRUE(sqlite3_prepare_v2(raw,
        "SELECT (SELECT user_version FROM pragma_user_version),"
        "(SELECT COUNT(*) FROM sqlite_schema WHERE type='table' AND name='history_entry_snapshots')",
        -1, &stmt, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(stmt) == SQLITE_ROW);
    ASSERT_EQ_I64(3, sqlite3_column_int(stmt, 0));
    ASSERT_EQ_I64(0, sqlite3_column_int(stmt, 1));
    ASSERT_TRUE(sqlite3_finalize(stmt) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_exec(raw, "DROP TABLE reconciliation_sessions", NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_close(raw) == SQLITE_OK);
    ASSERT_TRUE(cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(cmny_db_check(&db, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_prepare_v2(db.handle, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(stmt) == SQLITE_ROW);
    ASSERT_EQ_I64(5, sqlite3_column_int(stmt, 0));
    ASSERT_TRUE(sqlite3_finalize(stmt) == SQLITE_OK);
    cmny_db_close(&db);

    ASSERT_TRUE(unlink(path) == 0);
    char sidecar[4200];
    (void)snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    (void)unlink(sidecar);
    (void)snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    (void)unlink(sidecar);
    (void)printf("ok - history tests\n");
    return 0;
}
