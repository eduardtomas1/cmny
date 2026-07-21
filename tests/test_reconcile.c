#include "cmny_history.h"
#include "cmny_reconcile.h"
#include "test.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

_Static_assert(_Alignof(CmnyReconcileSession) >= _Alignof(int64_t),
               "reconciliation session must preserve int64 alignment");
_Static_assert(offsetof(CmnyReconcileSession, status) >
               offsetof(CmnyReconcileSession, finalized_at),
               "narrow session fields belong after int64 fields");
_Static_assert(offsetof(CmnyReconcilePosting, clear_state) >
               offsetof(CmnyReconcilePosting, amount_minor),
               "narrow posting fields belong after int64 fields");

static int64_t cash_account(CmnyDb *db, char *err, size_t err_size) {
    CmnyAccount rows[4];
    size_t count = 0;
    ASSERT_TRUE(cmny_account_list(db, false, rows, 4, &count, err, err_size));
    ASSERT_TRUE(count > 0);
    return rows[0].id;
}

static int64_t add_entry(CmnyDb *db, int64_t account_id, int64_t category_id,
                         int64_t amount, const char *date, const char *note,
                         char *err, size_t err_size) {
    CmnySplitDraft split = {.category_id = category_id, .amount_minor = amount, .note = ""};
    CmnyNormalEntryDraft draft = {
        .account_id = account_id,
        .amount_minor = amount,
        .split_count = 1,
        .occurred_on = date,
        .payee = "Reconcile test",
        .note = note,
        .splits = &split
    };
    int64_t entry_id = 0;
    ASSERT_TRUE(cmny_entry_create_normal(db, &draft, &entry_id, err, err_size));
    return entry_id;
}

static int64_t entry_posting(CmnyDb *db, int64_t entry_id, int64_t account_id,
                             char *err, size_t err_size) {
    CmnyLedgerEntry entry = {0};
    ASSERT_TRUE(cmny_ledger_entry_get(db, entry_id, &entry, err, err_size));
    int64_t posting_id = 0;
    for (size_t i = 0; i < entry.posting_count; i++)
        if (entry.postings[i].account_id == account_id) posting_id = entry.postings[i].id;
    cmny_ledger_entry_destroy(&entry);
    ASSERT_TRUE(posting_id > 0);
    return posting_id;
}

static int64_t account_balance(CmnyDb *db, int64_t account_id) {
    sqlite3_stmt *stmt = NULL;
    ASSERT_TRUE(sqlite3_prepare_v2(db->handle,
        "SELECT COALESCE(SUM(p.amount_minor),0) FROM postings p"
        " JOIN entries e ON e.id=p.entry_id"
        " WHERE p.account_id=? AND e.voided_at IS NULL", -1, &stmt, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_bind_int64(stmt, 1, account_id) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(stmt) == SQLITE_ROW);
    int64_t balance = sqlite3_column_int64(stmt, 0);
    ASSERT_TRUE(sqlite3_step(stmt) == SQLITE_DONE);
    ASSERT_TRUE(sqlite3_finalize(stmt) == SQLITE_OK);
    return balance;
}

int main(void) {
    char path[] = "build/cmny-reconcile-XXXXXX";
    int descriptor = mkstemp(path);
    ASSERT_TRUE(descriptor >= 0);
    ASSERT_TRUE(close(descriptor) == 0);
    CmnyDb db = {0};
    char currency[4] = {0};
    char err[256] = {0};
    ASSERT_TRUE(cmny_db_open(&db, path, false, "EUR", currency, err, sizeof(err)));
    int64_t cash_id = cash_account(&db, err, sizeof(err));
    int64_t savings_id = 0;
    ASSERT_TRUE(cmny_account_create(&db, "Statement savings", CMNY_ACCOUNT_SAVINGS, "",
                                    &savings_id, err, sizeof(err)));
    int64_t expense_category = 0;
    int64_t income_category = 0;
    ASSERT_TRUE(cmny_category_create(&db, "Recon expense", CMNY_CATEGORY_EXPENSE, 0,
                                     &expense_category, err, sizeof(err)));
    ASSERT_TRUE(cmny_category_create(&db, "Recon income", CMNY_CATEGORY_INCOME, 0,
                                     &income_category, err, sizeof(err)));

    int64_t income_entry = add_entry(&db, cash_id, income_category, 10000,
                                     "2026-09-01", "income", err, sizeof(err));
    int64_t expense_entry = add_entry(&db, cash_id, expense_category, -2500,
                                      "2026-09-02", "expense", err, sizeof(err));
    CmnyTransferDraft transfer = {
        .from_account_id = cash_id,
        .to_account_id = savings_id,
        .amount_minor = 1500,
        .occurred_on = "2026-09-03",
        .payee = "Internal",
        .note = "statement transfer"
    };
    int64_t transfer_entry = 0;
    ASSERT_TRUE(cmny_transfer_create(&db, &transfer, &transfer_entry, err, sizeof(err)));
    int64_t future_entry = add_entry(&db, cash_id, expense_category, -300,
                                     "2026-10-01", "future", err, sizeof(err));
    int64_t income_posting = entry_posting(&db, income_entry, cash_id, err, sizeof(err));
    int64_t expense_posting = entry_posting(&db, expense_entry, cash_id, err, sizeof(err));
    int64_t cash_transfer_posting = entry_posting(&db, transfer_entry, cash_id, err, sizeof(err));
    int64_t savings_transfer_posting = entry_posting(&db, transfer_entry, savings_id,
                                                      err, sizeof(err));
    int64_t future_posting = entry_posting(&db, future_entry, cash_id, err, sizeof(err));

    int64_t session_id = 0;
    ASSERT_TRUE(cmny_reconcile_start(&db, cash_id, "2026-09-30", 6000, &session_id,
                                     err, sizeof(err)));
    int64_t duplicate = 0;
    ASSERT_TRUE(!cmny_reconcile_start(&db, cash_id, "2026-09-30", 6000, &duplicate,
                                      err, sizeof(err)));
    CmnyReconcileSession session = {0};
    ASSERT_TRUE(cmny_reconcile_get(&db, session_id, &session, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_RECONCILE_OPEN, session.status);
    ASSERT_EQ_I64(5700, session.account_balance_minor);
    ASSERT_EQ_I64(0, session.cleared_balance_minor);
    ASSERT_EQ_I64(6000, session.discrepancy_minor);

    CmnyReconcilePosting postings[16];
    size_t count = 0;
    ASSERT_TRUE(cmny_reconcile_postings(&db, session_id, 0, postings, 16, &count,
                                        err, sizeof(err)));
    ASSERT_EQ_I64(3, count);
    for (size_t i = 0; i < count; i++) {
        ASSERT_TRUE(postings[i].posting_id != savings_transfer_posting);
        ASSERT_TRUE(postings[i].posting_id != future_posting);
    }
    ASSERT_TRUE(!cmny_reconcile_postings(&db, session_id, 0, postings,
        CMNY_RECONCILE_LIST_LIMIT + 1U, &count, err, sizeof(err)));

    int64_t revision = session.revision;
    ASSERT_TRUE(!cmny_reconcile_set_cleared(&db, session_id, revision,
        savings_transfer_posting, true, NULL, err, sizeof(err)));
    ASSERT_TRUE(cmny_reconcile_get(&db, session_id, &session, err, sizeof(err)));
    ASSERT_EQ_I64(revision, session.revision);
    ASSERT_TRUE(cmny_reconcile_set_cleared(&db, session_id, revision, income_posting,
                                           true, &revision, err, sizeof(err)));
    ASSERT_TRUE(cmny_reconcile_set_cleared(&db, session_id, revision, income_posting,
                                           true, &revision, err, sizeof(err)));
    sqlite3_stmt *stmt = NULL;
    ASSERT_TRUE(sqlite3_prepare_v2(db.handle,
        "SELECT COUNT(*) FROM reconciliation_items WHERE session_id=? AND posting_id=?",
        -1, &stmt, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_bind_int64(stmt, 1, session_id) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_bind_int64(stmt, 2, income_posting) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(stmt) == SQLITE_ROW);
    ASSERT_EQ_I64(1, sqlite3_column_int(stmt, 0));
    ASSERT_TRUE(sqlite3_finalize(stmt) == SQLITE_OK);
    ASSERT_TRUE(cmny_reconcile_set_cleared(&db, session_id, revision, expense_posting,
                                           true, &revision, err, sizeof(err)));
    ASSERT_TRUE(!cmny_reconcile_finalize(&db, session_id, revision, err, sizeof(err)));
    ASSERT_TRUE(cmny_reconcile_set_cleared(&db, session_id, revision, cash_transfer_posting,
                                           true, &revision, err, sizeof(err)));
    ASSERT_TRUE(cmny_reconcile_get(&db, session_id, &session, err, sizeof(err)));
    ASSERT_EQ_I64(6000, session.cleared_balance_minor);
    ASSERT_EQ_I64(0, session.discrepancy_minor);
    cmny_db_close(&db);

    ASSERT_TRUE(cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    ASSERT_TRUE(cmny_reconcile_get(&db, session_id, &session, err, sizeof(err)));
    ASSERT_EQ_I64(revision, session.revision);
    ASSERT_TRUE(!cmny_reconcile_finalize(&db, session_id, revision - 1, err, sizeof(err)));
    ASSERT_TRUE(cmny_reconcile_finalize(&db, session_id, revision, err, sizeof(err)));
    ASSERT_TRUE(cmny_reconcile_get(&db, session_id, &session, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_RECONCILE_FINALIZED, session.status);
    ASSERT_TRUE(session.finalized_at > 0);
    ASSERT_TRUE(!cmny_reconcile_set_cleared(&db, session_id, session.revision,
        income_posting, false, NULL, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_prepare_v2(db.handle,
        "SELECT clear_state FROM postings WHERE id IN(?,?,?,?) ORDER BY id", -1, &stmt, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_bind_int64(stmt, 1, income_posting) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_bind_int64(stmt, 2, expense_posting) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_bind_int64(stmt, 3, cash_transfer_posting) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_bind_int64(stmt, 4, savings_transfer_posting) == SQLITE_OK);
    int reconciled = 0;
    int uncleared = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_int(stmt, 0) == CMNY_POSTING_RECONCILED) reconciled++;
        if (sqlite3_column_int(stmt, 0) == CMNY_POSTING_UNCLEARED) uncleared++;
    }
    ASSERT_EQ_I64(3, reconciled);
    ASSERT_EQ_I64(1, uncleared);
    ASSERT_TRUE(sqlite3_finalize(stmt) == SQLITE_OK);

    ASSERT_EQ_I64(5700, account_balance(&db, cash_id));
    (void)add_entry(&db, cash_id, income_category, 700,
                    "2026-09-15", "backdated after finalize", err, sizeof(err));
    ASSERT_EQ_I64(6400, account_balance(&db, cash_id));
    cmny_db_close(&db);
    ASSERT_TRUE(cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    ASSERT_EQ_I64(6400, account_balance(&db, cash_id));
    ASSERT_TRUE(cmny_reconcile_get(&db, session_id, &session, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_RECONCILE_FINALIZED, session.status);
    ASSERT_EQ_I64(5700, session.account_balance_minor);
    ASSERT_EQ_I64(6000, session.cleared_balance_minor);
    ASSERT_EQ_I64(0, session.discrepancy_minor);

    int64_t october_session = 0;
    ASSERT_TRUE(cmny_reconcile_start(&db, cash_id, "2026-10-31", 5700,
                                     &october_session, err, sizeof(err)));
    ASSERT_TRUE(cmny_reconcile_get(&db, october_session, &session, err, sizeof(err)));
    ASSERT_EQ_I64(6400, session.account_balance_minor);
    ASSERT_EQ_I64(6000, session.cleared_balance_minor);
    ASSERT_EQ_I64(-300, session.discrepancy_minor);
    revision = session.revision;
    ASSERT_TRUE(cmny_reconcile_set_cleared(&db, october_session, revision, future_posting,
                                           true, &revision, err, sizeof(err)));
    CmnyLedgerEntry future = {0};
    ASSERT_TRUE(cmny_ledger_entry_get(&db, future_entry, &future, err, sizeof(err)));
    CmnySplitDraft changed_split = {
        .category_id = expense_category, .amount_minor = -400, .note = ""
    };
    CmnyNormalEntryDraft changed = {
        .account_id = cash_id, .amount_minor = -400, .split_count = 1,
        .occurred_on = "2026-10-01", .payee = "Changed", .note = "blocked",
        .splits = &changed_split
    };
    ASSERT_TRUE(!cmny_entry_update_normal(&db, future_entry, future.revision, &changed,
                                           err, sizeof(err)));
    cmny_ledger_entry_destroy(&future);
    ASSERT_TRUE(cmny_reconcile_cancel(&db, october_session, revision, err, sizeof(err)));
    ASSERT_TRUE(cmny_reconcile_get(&db, october_session, &session, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_RECONCILE_CANCELLED, session.status);
    ASSERT_EQ_I64(6000, session.cleared_balance_minor);
    ASSERT_TRUE(sqlite3_prepare_v2(db.handle, "SELECT clear_state FROM postings WHERE id=?",
                                   -1, &stmt, NULL) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_bind_int64(stmt, 1, future_posting) == SQLITE_OK);
    ASSERT_TRUE(sqlite3_step(stmt) == SQLITE_ROW);
    ASSERT_EQ_I64(CMNY_POSTING_UNCLEARED, sqlite3_column_int(stmt, 0));
    ASSERT_TRUE(sqlite3_finalize(stmt) == SQLITE_OK);

    int64_t savings_session = 0;
    ASSERT_TRUE(cmny_reconcile_start(&db, savings_id, "2026-09-30", 1500,
                                     &savings_session, err, sizeof(err)));
    ASSERT_TRUE(cmny_reconcile_postings(&db, savings_session, 0, postings, 16, &count,
                                        err, sizeof(err)));
    ASSERT_EQ_I64(1, count);
    ASSERT_EQ_I64(savings_transfer_posting, postings[0].posting_id);
    ASSERT_TRUE(cmny_reconcile_get(&db, savings_session, &session, err, sizeof(err)));
    revision = session.revision;
    ASSERT_TRUE(cmny_reconcile_set_cleared(&db, savings_session, revision,
        savings_transfer_posting, true, &revision, err, sizeof(err)));
    ASSERT_TRUE(cmny_reconcile_finalize(&db, savings_session, revision, err, sizeof(err)));

    CmnyReconcileSession sessions[8];
    ASSERT_TRUE(cmny_reconcile_list(&db, true, 0, sessions, 8, &count, err, sizeof(err)));
    ASSERT_EQ_I64(3, count);
    ASSERT_TRUE(cmny_db_check(&db, err, sizeof(err)));
    cmny_db_close(&db);
    ASSERT_TRUE(unlink(path) == 0);
    char sidecar[4200];
    (void)snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    (void)unlink(sidecar);
    (void)snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    (void)unlink(sidecar);
    (void)printf("ok - reconciliation tests\n");
    return 0;
}
