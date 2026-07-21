#include "cmny_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ledger_error(char *err, size_t err_size, const char *message) {
    if (err != NULL && err_size > 0) (void)snprintf(err, err_size, "%s", message);
}

static void ledger_db_error(CmnyDb *db, char *err, size_t err_size, const char *context) {
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s: %s", context,
                       db != NULL && db->handle != NULL ? sqlite3_errmsg(db->handle) : "database is not open");
    }
}

static bool ledger_exec(CmnyDb *db, const char *sql, char *err, size_t err_size,
                        const char *context) {
    char *detail = NULL;
    int rc = sqlite3_exec(db->handle, sql, NULL, NULL, &detail);
    if (rc == SQLITE_OK) return true;
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s: %s", context,
                       detail != NULL ? detail : sqlite3_errmsg(db->handle));
    }
    sqlite3_free(detail);
    return false;
}

static bool transaction_begin(CmnyDb *db, bool *owner, char *err, size_t err_size) {
    *owner = sqlite3_get_autocommit(db->handle) != 0;
    return ledger_exec(db, *owner ? "BEGIN IMMEDIATE" : "SAVEPOINT cmny_ledger_operation",
                       err, err_size, "cannot begin ledger change");
}

static bool transaction_finish(CmnyDb *db, bool owner, bool ok, char *err, size_t err_size) {
    if (owner) {
        if (ok && ledger_exec(db, "COMMIT", err, err_size,
                              "cannot commit ledger change")) return true;
        (void)sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    if (ok && ledger_exec(db, "RELEASE cmny_ledger_operation", err, err_size,
                          "cannot finish ledger change")) return true;
    (void)sqlite3_exec(db->handle,
                       "ROLLBACK TO cmny_ledger_operation;RELEASE cmny_ledger_operation",
                       NULL, NULL, NULL);
    return false;
}

static bool valid_amount(int64_t amount) {
    return amount != 0 && amount >= -CMNY_AMOUNT_MAX && amount <= CMNY_AMOUNT_MAX;
}

static bool copy_column(sqlite3_stmt *stmt, int column, char *out, size_t out_size) {
    const unsigned char *value = sqlite3_column_text(stmt, column);
    int bytes = sqlite3_column_bytes(stmt, column);
    if (value == NULL || bytes < 0 || (size_t)bytes >= out_size ||
        memchr(value, '\0', (size_t)bytes) != NULL) return false;
    memcpy(out, value, (size_t)bytes);
    out[bytes] = '\0';
    return true;
}

static bool normal_draft_shape(const CmnyNormalEntryDraft *draft, char *err, size_t err_size) {
    const char *payee = draft != NULL && draft->payee != NULL ? draft->payee : "";
    const char *note = draft != NULL && draft->note != NULL ? draft->note : "";
    if (draft == NULL || draft->account_id <= 0 || !valid_amount(draft->amount_minor) ||
        !cmny_date_valid(draft->occurred_on) || !cmny_text_valid(payee, CMNY_PAYEE_MAX, true) ||
        !cmny_text_valid(note, CMNY_LEDGER_NOTE_MAX, true) || draft->splits == NULL ||
        draft->split_count == 0 || draft->split_count > CMNY_ENTRY_CHILD_LIMIT ||
        draft->tag_count > CMNY_ENTRY_CHILD_LIMIT ||
        (draft->tag_count > 0 && draft->tag_ids == NULL)) {
        ledger_error(err, err_size, "invalid normal entry data");
        return false;
    }
    int64_t total = 0;
    for (size_t i = 0; i < draft->split_count; i++) {
        const CmnySplitDraft *split = &draft->splits[i];
        const char *split_note = split->note != NULL ? split->note : "";
        if (split->category_id <= 0 || !valid_amount(split->amount_minor) ||
            (split->amount_minor < 0) != (draft->amount_minor < 0) ||
            !cmny_text_valid(split_note, CMNY_NOTE_MAX, true)) {
            ledger_error(err, err_size, "invalid entry split");
            return false;
        }
        if ((split->amount_minor > 0 && total > CMNY_AMOUNT_MAX - split->amount_minor) ||
            (split->amount_minor < 0 && total < -CMNY_AMOUNT_MAX - split->amount_minor)) {
            ledger_error(err, err_size, "entry split total is too large");
            return false;
        }
        total += split->amount_minor;
    }
    if (total != draft->amount_minor) {
        ledger_error(err, err_size, "entry splits must exactly equal the entry amount");
        return false;
    }
    for (size_t i = 0; i < draft->tag_count; i++) {
        if (draft->tag_ids[i] <= 0) {
            ledger_error(err, err_size, "invalid entry tag");
            return false;
        }
        for (size_t j = i + 1; j < draft->tag_count; j++) {
            if (draft->tag_ids[i] == draft->tag_ids[j]) {
                ledger_error(err, err_size, "entry tags must be unique");
                return false;
            }
        }
    }
    return true;
}

static bool account_available(CmnyDb *db, int64_t id, char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT 1 FROM accounts a JOIN settings s ON s.key='currency'"
        " WHERE a.id=? AND a.archived=0 AND a.currency_code=s.value", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) ledger_error(err, err_size, rc == SQLITE_DONE ? "account is unavailable"
                                                          : sqlite3_errmsg(db->handle));
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool categories_available(CmnyDb *db, const CmnyNormalEntryDraft *draft,
                                 char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int wanted_mask = draft->amount_minor < 0 ? CMNY_CATEGORY_EXPENSE : CMNY_CATEGORY_INCOME;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT 1 FROM categories WHERE id=? AND archived=0 AND merged_into_id IS NULL"
        " AND (kind_mask & ?)<>0", -1, &stmt, NULL);
    for (size_t i = 0; rc == SQLITE_OK && i < draft->split_count; i++) {
        rc = sqlite3_bind_int64(stmt, 1, draft->splits[i].category_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, wanted_mask);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW && sqlite3_step(stmt) == SQLITE_DONE) {
            rc = sqlite3_reset(stmt);
            if (rc == SQLITE_OK) rc = sqlite3_clear_bindings(stmt);
        } else if (rc == SQLITE_DONE) {
            ledger_error(err, err_size, "category is unavailable for this entry kind");
            rc = SQLITE_NOTFOUND;
        }
    }
    bool ok = rc == SQLITE_OK;
    if (!ok && rc != SQLITE_NOTFOUND) ledger_db_error(db, err, err_size, "cannot validate categories");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool tags_available(CmnyDb *db, const int64_t *ids, size_t count,
                           char *err, size_t err_size) {
    if (count == 0) return true;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, "SELECT 1 FROM tags WHERE id=? AND archived=0", -1,
                                &stmt, NULL);
    for (size_t i = 0; rc == SQLITE_OK && i < count; i++) {
        rc = sqlite3_bind_int64(stmt, 1, ids[i]);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW && sqlite3_step(stmt) == SQLITE_DONE) {
            rc = sqlite3_reset(stmt);
            if (rc == SQLITE_OK) rc = sqlite3_clear_bindings(stmt);
        } else if (rc == SQLITE_DONE) {
            ledger_error(err, err_size, "tag is unavailable");
            rc = SQLITE_NOTFOUND;
        }
    }
    bool ok = rc == SQLITE_OK;
    if (!ok && rc != SQLITE_NOTFOUND) ledger_db_error(db, err, err_size, "cannot validate tags");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool entry_not_reconciling(CmnyDb *db, int64_t entry_id,
                                  char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT NOT EXISTS(SELECT 1 FROM reconciliation_items ri"
        " JOIN reconciliation_sessions rs ON rs.id=ri.session_id"
        " JOIN postings p ON p.id=ri.posting_id"
        " WHERE rs.status IN(1,2) AND p.entry_id=?)", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, entry_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1 &&
              sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) ledger_error(err, err_size, rc == SQLITE_ROW || rc == SQLITE_DONE
        ? "entry belongs to an active or finalized reconciliation" : "cannot inspect reconciliation state");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool insert_entry_header(CmnyDb *db, CmnyEntryType type, const char *date,
                                const char *payee, const char *note, int64_t *entry_id,
                                char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO entries(entry_type,occurred_on,payee,note) VALUES(?,?,?,?)", -1,
        &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, (int)type);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, date, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, payee, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, note, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (ok) *entry_id = sqlite3_last_insert_rowid(db->handle);
    if (!ok) ledger_db_error(db, err, err_size, "cannot create ledger entry");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool insert_posting(CmnyDb *db, int64_t entry_id, int64_t account_id, int64_t amount,
                           int order, int64_t *posting_id, char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO postings(entry_id,account_id,amount_minor,sort_order) VALUES(?,?,?,?)", -1,
        &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, entry_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, account_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, amount);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 4, order);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (ok && posting_id != NULL) *posting_id = sqlite3_last_insert_rowid(db->handle);
    if (!ok) ledger_db_error(db, err, err_size, "cannot create posting");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool insert_children(CmnyDb *db, int64_t entry_id, int64_t posting_id,
                            const CmnyNormalEntryDraft *draft, char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO allocations(posting_id,category_id,amount_minor,note,sort_order) VALUES(?,?,?,?,?)",
        -1, &stmt, NULL);
    for (size_t i = 0; rc == SQLITE_OK && i < draft->split_count; i++) {
        rc = sqlite3_bind_int64(stmt, 1, posting_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, draft->splits[i].category_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, draft->splits[i].amount_minor);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4,
            draft->splits[i].note != NULL ? draft->splits[i].note : "", -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 5, (int)i);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            rc = sqlite3_reset(stmt);
            if (rc == SQLITE_OK) rc = sqlite3_clear_bindings(stmt);
        }
    }
    bool ok = rc == SQLITE_OK;
    (void)sqlite3_finalize(stmt);
    stmt = NULL;
    if (ok && draft->tag_count > 0) {
        rc = sqlite3_prepare_v2(db->handle, "INSERT INTO entry_tags(entry_id,tag_id) VALUES(?,?)",
                                -1, &stmt, NULL);
        for (size_t i = 0; rc == SQLITE_OK && i < draft->tag_count; i++) {
            rc = sqlite3_bind_int64(stmt, 1, entry_id);
            if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, draft->tag_ids[i]);
            if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) {
                rc = sqlite3_reset(stmt);
                if (rc == SQLITE_OK) rc = sqlite3_clear_bindings(stmt);
            }
        }
        ok = rc == SQLITE_OK;
        (void)sqlite3_finalize(stmt);
    }
    if (!ok) ledger_db_error(db, err, err_size, "cannot create entry details");
    return ok;
}

bool cmny_entry_create_normal(CmnyDb *db, const CmnyNormalEntryDraft *draft,
                              int64_t *new_id, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || !normal_draft_shape(draft, err, err_size)) return false;
    bool owner = false;
    if (!transaction_begin(db, &owner, err, err_size)) return false;
    bool ok = account_available(db, draft->account_id, err, err_size) &&
              categories_available(db, draft, err, err_size) &&
              tags_available(db, draft->tag_ids, draft->tag_count, err, err_size);
    int64_t entry_id = 0;
    int64_t posting_id = 0;
    const char *payee = draft->payee != NULL ? draft->payee : "";
    const char *note = draft->note != NULL ? draft->note : "";
    if (ok) ok = insert_entry_header(db, CMNY_ENTRY_NORMAL, draft->occurred_on, payee, note,
                                     &entry_id, err, err_size);
    if (ok) ok = insert_posting(db, entry_id, draft->account_id, draft->amount_minor, 0,
                                &posting_id, err, err_size);
    if (ok) ok = insert_children(db, entry_id, posting_id, draft, err, err_size);
    int64_t action_id = 0;
    if (ok) ok = cmny_history_action_begin(db, CMNY_HISTORY_CREATE, entry_id, false,
                                            &action_id, err, err_size);
    if (ok) ok = cmny_history_action_finish(db, action_id, entry_id, err, err_size);
    ok = transaction_finish(db, owner, ok, err, err_size);
    if (ok && new_id != NULL) *new_id = entry_id;
    return ok;
}

bool cmny_entry_update_normal(CmnyDb *db, int64_t id, int64_t expected_revision,
                              const CmnyNormalEntryDraft *draft,
                              char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || id <= 0 || expected_revision <= 0 ||
        !normal_draft_shape(draft, err, err_size)) return false;
    bool owner = false;
    if (!transaction_begin(db, &owner, err, err_size)) return false;
    bool ok = account_available(db, draft->account_id, err, err_size) &&
              categories_available(db, draft, err, err_size) &&
              tags_available(db, draft->tag_ids, draft->tag_count, err, err_size) &&
              entry_not_reconciling(db, id, err, err_size);
    int64_t action_id = 0;
    if (ok) ok = cmny_history_action_begin(db, CMNY_HISTORY_UPDATE, id, true,
                                            &action_id, err, err_size);
    sqlite3_stmt *stmt = NULL;
    int rc = SQLITE_OK;
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "UPDATE entries SET occurred_on=?,payee=?,note=?,revision=revision+1,"
            "updated_at=CAST(strftime('%s','now') AS INTEGER)"
            " WHERE id=? AND entry_type=1 AND revision=? AND voided_at IS NULL", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, draft->occurred_on, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, draft->payee != NULL ? draft->payee : "",
                                                     -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, draft->note != NULL ? draft->note : "",
                                                     -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 4, id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 5, expected_revision);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
        if (!ok) ledger_error(err, err_size, rc == SQLITE_DONE ? "entry changed; reload and retry"
                                                              : sqlite3_errmsg(db->handle));
        (void)sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle, "DELETE FROM postings WHERE entry_id=?", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle, "DELETE FROM entry_tags WHERE entry_id=?", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
    }
    int64_t posting_id = 0;
    if (ok) ok = insert_posting(db, id, draft->account_id, draft->amount_minor, 0,
                                &posting_id, err, err_size);
    if (ok) ok = insert_children(db, id, posting_id, draft, err, err_size);
    if (ok) ok = cmny_history_action_finish(db, action_id, id, err, err_size);
    if (!ok && err != NULL && err_size > 0 && err[0] == '\0')
        ledger_db_error(db, err, err_size, "cannot update entry");
    return transaction_finish(db, owner, ok, err, err_size);
}

bool cmny_entry_delete(CmnyDb *db, int64_t id, int64_t expected_revision,
                       char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || id <= 0 || expected_revision <= 0) {
        ledger_error(err, err_size, "invalid entry revision");
        return false;
    }
    bool owner = false;
    if (!transaction_begin(db, &owner, err, err_size)) return false;
    int64_t action_id = 0;
    bool ok = entry_not_reconciling(db, id, err, err_size) &&
              cmny_history_action_begin(db, CMNY_HISTORY_DELETE, id, true,
                                        &action_id, err, err_size);
    sqlite3_stmt *stmt = NULL;
    int rc = SQLITE_OK;
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "UPDATE entries SET voided_at=CAST(strftime('%s','now') AS INTEGER),"
            "updated_at=CAST(strftime('%s','now') AS INTEGER),revision=revision+1"
            " WHERE id=? AND revision=? AND voided_at IS NULL", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, expected_revision);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
        if (!ok) ledger_error(err, err_size, rc == SQLITE_DONE ? "entry changed; reload and retry"
                                                              : sqlite3_errmsg(db->handle));
    }
    (void)sqlite3_finalize(stmt);
    if (ok) ok = cmny_history_action_finish(db, action_id, id, err, err_size);
    return transaction_finish(db, owner, ok, err, err_size);
}

bool cmny_transfer_create(CmnyDb *db, const CmnyTransferDraft *draft,
                          int64_t *new_id, char *err, size_t err_size) {
    const char *payee = draft != NULL && draft->payee != NULL ? draft->payee : "";
    const char *note = draft != NULL && draft->note != NULL ? draft->note : "";
    if (db == NULL || db->handle == NULL || draft == NULL || draft->from_account_id <= 0 ||
        draft->to_account_id <= 0 || draft->from_account_id == draft->to_account_id ||
        draft->amount_minor <= 0 || draft->amount_minor > CMNY_AMOUNT_MAX ||
        !cmny_date_valid(draft->occurred_on) || !cmny_text_valid(payee, CMNY_PAYEE_MAX, true) ||
        !cmny_text_valid(note, CMNY_LEDGER_NOTE_MAX, true) ||
        draft->tag_count > CMNY_ENTRY_CHILD_LIMIT ||
        (draft->tag_count > 0 && draft->tag_ids == NULL)) {
        ledger_error(err, err_size, "invalid transfer data");
        return false;
    }
    for (size_t i = 0; i < draft->tag_count; i++) {
        if (draft->tag_ids[i] <= 0) { ledger_error(err, err_size, "invalid transfer tag"); return false; }
        for (size_t j = i + 1; j < draft->tag_count; j++) {
            if (draft->tag_ids[i] == draft->tag_ids[j]) {
                ledger_error(err, err_size, "transfer tags must be unique"); return false;
            }
        }
    }
    bool owner = false;
    if (!transaction_begin(db, &owner, err, err_size)) return false;
    bool ok = account_available(db, draft->from_account_id, err, err_size) &&
              account_available(db, draft->to_account_id, err, err_size) &&
              tags_available(db, draft->tag_ids, draft->tag_count, err, err_size);
    int64_t entry_id = 0;
    if (ok) ok = insert_entry_header(db, CMNY_ENTRY_TRANSFER, draft->occurred_on, payee, note,
                                     &entry_id, err, err_size);
    if (ok) ok = insert_posting(db, entry_id, draft->from_account_id, -draft->amount_minor, 0,
                                NULL, err, err_size);
    if (ok) ok = insert_posting(db, entry_id, draft->to_account_id, draft->amount_minor, 1,
                                NULL, err, err_size);
    sqlite3_stmt *stmt = NULL;
    int rc = SQLITE_OK;
    if (ok && draft->tag_count > 0) {
        rc = sqlite3_prepare_v2(db->handle, "INSERT INTO entry_tags(entry_id,tag_id) VALUES(?,?)",
                                -1, &stmt, NULL);
        for (size_t i = 0; rc == SQLITE_OK && i < draft->tag_count; i++) {
            rc = sqlite3_bind_int64(stmt, 1, entry_id);
            if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, draft->tag_ids[i]);
            if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) {
                rc = sqlite3_reset(stmt);
                if (rc == SQLITE_OK) rc = sqlite3_clear_bindings(stmt);
            }
        }
        ok = rc == SQLITE_OK;
        (void)sqlite3_finalize(stmt);
        if (!ok) ledger_db_error(db, err, err_size, "cannot tag transfer");
    }
    int64_t action_id = 0;
    if (ok) ok = cmny_history_action_begin(db, CMNY_HISTORY_TRANSFER, entry_id, false,
                                            &action_id, err, err_size);
    if (ok) ok = cmny_history_action_finish(db, action_id, entry_id, err, err_size);
    ok = transaction_finish(db, owner, ok, err, err_size);
    if (ok && new_id != NULL) *new_id = entry_id;
    return ok;
}

void cmny_ledger_entry_destroy(CmnyLedgerEntry *entry) {
    if (entry == NULL) return;
    free(entry->postings);
    free(entry->allocations);
    free(entry->tag_ids);
    memset(entry, 0, sizeof(*entry));
}

static bool aggregate_count(CmnyDb *db, const char *sql, int64_t id, size_t *count,
                            char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    sqlite3_int64 value = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
    bool ok = rc == SQLITE_ROW && value >= 0 && value <= CMNY_ENTRY_CHILD_LIMIT &&
              sqlite3_step(stmt) == SQLITE_DONE;
    if (ok) *count = (size_t)value;
    if (!ok) ledger_error(err, err_size, value > CMNY_ENTRY_CHILD_LIMIT
        ? "entry has too many child records" : "cannot count entry child records");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_ledger_entry_get(CmnyDb *db, int64_t id, CmnyLedgerEntry *out,
                           char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || id <= 0 || out == NULL) {
        ledger_error(err, err_size, "invalid entry lookup");
        return false;
    }
    CmnyLedgerEntry result = {0};
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT id,entry_type,occurred_on,payee,note,revision FROM entries WHERE id=? AND voided_at IS NULL",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW;
    if (ok) {
        result.id = sqlite3_column_int64(stmt, 0);
        result.type = (CmnyEntryType)sqlite3_column_int(stmt, 1);
        result.revision = sqlite3_column_int64(stmt, 5);
        ok = copy_column(stmt, 2, result.occurred_on, sizeof(result.occurred_on)) &&
             copy_column(stmt, 3, result.payee, sizeof(result.payee)) &&
             copy_column(stmt, 4, result.note, sizeof(result.note));
    }
    (void)sqlite3_finalize(stmt);
    if (!ok) {
        ledger_error(err, err_size, rc == SQLITE_DONE ? "entry does not exist" : "invalid entry data");
        return false;
    }
    ok = aggregate_count(db, "SELECT COUNT(*) FROM postings WHERE entry_id=?", id,
                         &result.posting_count, err, err_size) &&
         aggregate_count(db,
            "SELECT COUNT(*) FROM allocations a JOIN postings p ON p.id=a.posting_id WHERE p.entry_id=?",
            id, &result.allocation_count, err, err_size) &&
         aggregate_count(db, "SELECT COUNT(*) FROM entry_tags WHERE entry_id=?", id,
                         &result.tag_count, err, err_size);
    if (ok && result.posting_count > 0)
        result.postings = calloc(result.posting_count, sizeof(*result.postings));
    if (ok && result.allocation_count > 0)
        result.allocations = calloc(result.allocation_count, sizeof(*result.allocations));
    if (ok && result.tag_count > 0)
        result.tag_ids = calloc(result.tag_count, sizeof(*result.tag_ids));
    if (ok && ((result.posting_count > 0 && result.postings == NULL) ||
               (result.allocation_count > 0 && result.allocations == NULL) ||
               (result.tag_count > 0 && result.tag_ids == NULL))) {
        ledger_error(err, err_size, "out of memory loading entry");
        ok = false;
    }
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "SELECT id,account_id,amount_minor,clear_state,sort_order FROM postings"
            " WHERE entry_id=? ORDER BY sort_order", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
        size_t used = 0;
        if (rc == SQLITE_OK) {
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < result.posting_count) {
                result.postings[used].id = sqlite3_column_int64(stmt, 0);
                result.postings[used].account_id = sqlite3_column_int64(stmt, 1);
                result.postings[used].amount_minor = sqlite3_column_int64(stmt, 2);
                result.postings[used].clear_state = sqlite3_column_int(stmt, 3);
                result.postings[used].sort_order = sqlite3_column_int(stmt, 4);
                used++;
            }
        }
        ok = rc == SQLITE_DONE && used == result.posting_count;
        (void)sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "SELECT a.id,a.posting_id,a.category_id,a.amount_minor,a.sort_order,a.note"
            " FROM allocations a JOIN postings p ON p.id=a.posting_id WHERE p.entry_id=?"
            " ORDER BY p.sort_order,a.sort_order", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
        size_t used = 0;
        if (rc == SQLITE_OK) {
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < result.allocation_count) {
                result.allocations[used].id = sqlite3_column_int64(stmt, 0);
                result.allocations[used].posting_id = sqlite3_column_int64(stmt, 1);
                result.allocations[used].category_id = sqlite3_column_int64(stmt, 2);
                result.allocations[used].amount_minor = sqlite3_column_int64(stmt, 3);
                result.allocations[used].sort_order = sqlite3_column_int(stmt, 4);
                if (!copy_column(stmt, 5, result.allocations[used].note,
                                 sizeof(result.allocations[used].note))) { rc = SQLITE_CORRUPT; break; }
                used++;
            }
        }
        ok = rc == SQLITE_DONE && used == result.allocation_count;
        (void)sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "SELECT tag_id FROM entry_tags WHERE entry_id=? ORDER BY tag_id", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
        size_t used = 0;
        if (rc == SQLITE_OK) {
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < result.tag_count)
                result.tag_ids[used++] = sqlite3_column_int64(stmt, 0);
        }
        ok = rc == SQLITE_DONE && used == result.tag_count;
        (void)sqlite3_finalize(stmt);
    }
    if (!ok) {
        if (err != NULL && err_size > 0 && err[0] == '\0') ledger_db_error(db, err, err_size, "cannot load entry");
        cmny_ledger_entry_destroy(&result);
        return false;
    }
    *out = result;
    return true;
}

static bool default_account(CmnyDb *db, int64_t *account_id, char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT id FROM accounts WHERE archived=0 ORDER BY sort_order,id LIMIT 1", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW;
    if (ok) *account_id = sqlite3_column_int64(stmt, 0);
    if (!ok) ledger_error(err, err_size, "ledger has no active account");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_legacy_transaction_add(CmnyDb *db, const CmnyTransaction *tx, int64_t *new_id,
                                 char *err, size_t err_size) {
    bool owner = false;
    if (!transaction_begin(db, &owner, err, err_size)) return false;
    int64_t category_id = 0;
    int64_t account_id = 0;
    int kind_mask = tx->kind == CMNY_EXPENSE ? CMNY_CATEGORY_EXPENSE : CMNY_CATEGORY_INCOME;
    bool ok = cmny_internal_category_ensure(db, tx->category, kind_mask, &category_id,
                                            err, err_size) &&
              default_account(db, &account_id, err, err_size);
    int64_t amount = tx->kind == CMNY_EXPENSE ? -tx->amount_cents : tx->amount_cents;
    CmnySplitDraft split = {.category_id = category_id, .amount_minor = amount, .note = ""};
    CmnyNormalEntryDraft draft = {
        .account_id = account_id, .amount_minor = amount, .occurred_on = tx->occurred_on,
        .payee = "", .note = tx->note, .splits = &split, .split_count = 1
    };
    int64_t entry_id = 0;
    if (ok) ok = cmny_entry_create_normal(db, &draft, &entry_id, err, err_size);
    ok = transaction_finish(db, owner, ok, err, err_size);
    if (ok && new_id != NULL) *new_id = entry_id;
    return ok;
}

bool cmny_legacy_transaction_update(CmnyDb *db, const CmnyTransaction *tx,
                                    char *err, size_t err_size) {
    CmnyLedgerEntry old = {0};
    if (!cmny_ledger_entry_get(db, tx->id, &old, err, err_size) ||
        old.type != CMNY_ENTRY_NORMAL || old.posting_count != 1) {
        cmny_ledger_entry_destroy(&old);
        ledger_error(err, err_size, "transaction does not exist");
        return false;
    }
    bool owner = false;
    if (!transaction_begin(db, &owner, err, err_size)) { cmny_ledger_entry_destroy(&old); return false; }
    int64_t category_id = 0;
    int kind_mask = tx->kind == CMNY_EXPENSE ? CMNY_CATEGORY_EXPENSE : CMNY_CATEGORY_INCOME;
    bool ok = cmny_internal_category_ensure(db, tx->category, kind_mask, &category_id,
                                            err, err_size);
    int64_t amount = tx->kind == CMNY_EXPENSE ? -tx->amount_cents : tx->amount_cents;
    CmnySplitDraft split = {.category_id = category_id, .amount_minor = amount, .note = ""};
    CmnyNormalEntryDraft draft = {
        .account_id = old.postings[0].account_id, .amount_minor = amount,
        .occurred_on = tx->occurred_on, .payee = old.payee, .note = tx->note,
        .splits = &split, .split_count = 1, .tag_ids = old.tag_ids, .tag_count = old.tag_count
    };
    if (ok) ok = cmny_entry_update_normal(db, tx->id, old.revision, &draft, err, err_size);
    ok = transaction_finish(db, owner, ok, err, err_size);
    cmny_ledger_entry_destroy(&old);
    return ok;
}

bool cmny_legacy_transaction_delete(CmnyDb *db, int64_t id,
                                    char *err, size_t err_size) {
    CmnyLedgerEntry entry = {0};
    if (!cmny_ledger_entry_get(db, id, &entry, err, err_size) || entry.type != CMNY_ENTRY_NORMAL) {
        cmny_ledger_entry_destroy(&entry);
        ledger_error(err, err_size, "transaction does not exist");
        return false;
    }
    bool ok = cmny_entry_delete(db, id, entry.revision, err, err_size);
    cmny_ledger_entry_destroy(&entry);
    return ok;
}

bool cmny_ledger_integrity_check(CmnyDb *db, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL) {
        ledger_error(err, err_size, "database is not open");
        return false;
    }
    static const char *metadata_sql =
        "SELECT COUNT(*)+CASE WHEN NOT EXISTS(SELECT 1 FROM accounts)"
        " OR (SELECT COUNT(*) FROM settings WHERE key='currency')<>1 THEN 1 ELSE 0 END"
        " FROM accounts a WHERE a.id<=0 OR length(a.name) NOT BETWEEN 1 AND 64"
        " OR a.name GLOB '*[^ -~]*' OR a.account_type NOT BETWEEN 1 AND 7"
        " OR length(a.currency_code)<>3 OR a.currency_code NOT GLOB '[A-Z][A-Z][A-Z]'"
        " OR a.currency_code<>(SELECT value FROM settings WHERE key='currency')"
        " OR length(a.institution)>64 OR a.institution GLOB '*[^ -~]*'"
        " OR a.archived NOT IN (0,1) OR a.revision<=0"
        " UNION ALL SELECT COUNT(*) FROM categories c WHERE c.id<=0"
        " OR length(c.name) NOT BETWEEN 1 AND 32 OR c.name GLOB '*[^ -~]*'"
        " OR length(c.marker)>8 OR c.marker GLOB '*[^ -~]*'"
        " OR length(c.color)>16 OR c.color GLOB '*[^ -~]*' OR c.kind_mask NOT BETWEEN 1 AND 3"
        " OR c.archived NOT IN (0,1) OR c.revision<=0 OR c.parent_id=c.id"
        " OR c.merged_into_id=c.id OR (c.merged_into_id IS NOT NULL AND c.archived<>1)"
        " UNION ALL SELECT COUNT(*) FROM tags t WHERE t.id<=0"
        " OR length(t.name) NOT BETWEEN 1 AND 32 OR t.name GLOB '*[^ -~]*'"
        " OR t.archived NOT IN (0,1) OR t.revision<=0"
        " UNION ALL SELECT COUNT(*) FROM entries e WHERE e.id<=0 OR e.entry_type NOT BETWEEN 1 AND 3"
        " OR e.occurred_on NOT GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'"
        " OR date(e.occurred_on,'+0 days') IS NULL OR date(e.occurred_on,'+0 days')<>e.occurred_on"
        " OR length(e.payee)>96 OR e.payee GLOB '*[^ -~]*'"
        " OR length(e.note)>240 OR e.note GLOB '*[^ -~]*'"
        " OR e.origin NOT BETWEEN 1 AND 4 OR e.revision<=0"
        " UNION ALL SELECT COUNT(*) FROM postings p WHERE p.id<=0 OR p.entry_id<=0 OR p.account_id<=0"
        " OR p.amount_minor=0 OR p.amount_minor NOT BETWEEN -9000000000000000 AND 9000000000000000"
        " OR p.clear_state NOT BETWEEN 0 AND 2 OR length(p.memo)>120 OR p.memo GLOB '*[^ -~]*'"
        " OR p.sort_order<0"
        " UNION ALL SELECT COUNT(*) FROM allocations a WHERE a.id<=0 OR a.posting_id<=0 OR a.category_id<=0"
        " OR a.amount_minor=0 OR a.amount_minor NOT BETWEEN -9000000000000000 AND 9000000000000000"
        " OR length(a.note)>120 OR a.note GLOB '*[^ -~]*' OR a.sort_order<0";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, metadata_sql, -1, &stmt, NULL);
    size_t metadata_rows = 0;
    bool metadata_ok = rc == SQLITE_OK;
    while (metadata_ok && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        metadata_ok = sqlite3_column_int64(stmt, 0) == 0;
        metadata_rows++;
    }
    metadata_ok = metadata_ok && rc == SQLITE_DONE && metadata_rows == 6;
    (void)sqlite3_finalize(stmt);
    if (!metadata_ok) {
        ledger_error(err, err_size, "normalized ledger metadata is invalid");
        return false;
    }

    static const char *cycle_sql =
        "WITH RECURSIVE paths(root,node) AS ("
        " SELECT id,parent_id FROM categories WHERE parent_id IS NOT NULL"
        " UNION SELECT paths.root,c.parent_id FROM paths JOIN categories c ON c.id=paths.node"
        " WHERE c.parent_id IS NOT NULL),merges(root,node) AS ("
        " SELECT id,merged_into_id FROM categories WHERE merged_into_id IS NOT NULL"
        " UNION SELECT merges.root,c.merged_into_id FROM merges JOIN categories c ON c.id=merges.node"
        " WHERE c.merged_into_id IS NOT NULL)"
        " SELECT COUNT(*) FROM paths WHERE root=node"
        " UNION ALL SELECT COUNT(*) FROM merges WHERE root=node";
    stmt = NULL;
    rc = sqlite3_prepare_v2(db->handle, cycle_sql, -1, &stmt, NULL);
    size_t cycle_rows = 0;
    bool cycles_ok = rc == SQLITE_OK;
    while (cycles_ok && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cycles_ok = sqlite3_column_int64(stmt, 0) == 0;
        cycle_rows++;
    }
    cycles_ok = cycles_ok && rc == SQLITE_DONE && cycle_rows == 2;
    (void)sqlite3_finalize(stmt);
    if (!cycles_ok) {
        ledger_error(err, err_size, "category hierarchy contains a cycle");
        return false;
    }

    static const char *invalid_sql =
        "SELECT COUNT(*) FROM entries e WHERE"
        " (e.entry_type=1 AND ("
        "   (SELECT COUNT(*) FROM postings p WHERE p.entry_id=e.id)<>1 OR"
        "   (SELECT COUNT(*) FROM allocations a JOIN postings p ON p.id=a.posting_id WHERE p.entry_id=e.id)"
        "      NOT BETWEEN 1 AND 256 OR"
        "   (SELECT SUM(a.amount_minor) FROM allocations a JOIN postings p ON p.id=a.posting_id"
        "      WHERE p.entry_id=e.id)<>(SELECT amount_minor FROM postings p WHERE p.entry_id=e.id) OR"
        "   EXISTS(SELECT 1 FROM allocations a JOIN postings p ON p.id=a.posting_id"
        "      WHERE p.entry_id=e.id AND ((a.amount_minor<0 AND p.amount_minor>0)"
        "      OR (a.amount_minor>0 AND p.amount_minor<0))) OR"
        "   EXISTS(SELECT 1 FROM allocations a JOIN postings p ON p.id=a.posting_id"
        "      JOIN categories c ON c.id=a.category_id WHERE p.entry_id=e.id"
        "      AND ((a.amount_minor<0 AND (c.kind_mask&1)=0) OR (a.amount_minor>0 AND (c.kind_mask&2)=0)))))"
        " OR (e.entry_type=2 AND ("
        "   (SELECT COUNT(*) FROM postings p WHERE p.entry_id=e.id)<>2 OR"
        "   (SELECT SUM(p.amount_minor) FROM postings p WHERE p.entry_id=e.id)<>0 OR"
        "   (SELECT COUNT(DISTINCT p.account_id) FROM postings p WHERE p.entry_id=e.id)<>2 OR"
        "   EXISTS(SELECT 1 FROM allocations a JOIN postings p ON p.id=a.posting_id WHERE p.entry_id=e.id)))"
        " OR (e.entry_type=3 AND ((SELECT COUNT(*) FROM postings p WHERE p.entry_id=e.id)<>1 OR"
        "   EXISTS(SELECT 1 FROM allocations a JOIN postings p ON p.id=a.posting_id WHERE p.entry_id=e.id)))"
        " OR (SELECT COUNT(*) FROM entry_tags et WHERE et.entry_id=e.id)>256";
    stmt = NULL;
    rc = sqlite3_prepare_v2(db->handle, invalid_sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_int64(stmt, 0) == 0 &&
              sqlite3_step(stmt) == SQLITE_DONE;
    (void)sqlite3_finalize(stmt);
    if (!ok) {
        ledger_error(err, err_size, "normalized ledger invariants failed");
        return false;
    }
    rc = sqlite3_prepare_v2(db->handle, "SELECT COUNT(*) FROM pragma_foreign_key_check", -1,
                            &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    ok = rc == SQLITE_ROW && sqlite3_column_int64(stmt, 0) == 0 &&
         sqlite3_step(stmt) == SQLITE_DONE;
    (void)sqlite3_finalize(stmt);
    if (!ok) ledger_error(err, err_size, "ledger foreign-key integrity failed");
    return ok;
}
