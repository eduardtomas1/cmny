#include "cmny_history.h"
#include "cmny_internal.h"

#include <stdio.h>
#include <string.h>

static void history_error(char *err, size_t err_size, const char *message) {
    if (err != NULL && err_size > 0) (void)snprintf(err, err_size, "%s", message);
}

static void history_db_error(CmnyDb *db, char *err, size_t err_size, const char *context) {
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s: %s", context,
                       db != NULL && db->handle != NULL ? sqlite3_errmsg(db->handle)
                                                       : "database is not open");
    }
}

static bool history_exec(CmnyDb *db, const char *sql, char *err, size_t err_size,
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

static bool history_begin(CmnyDb *db, char *err, size_t err_size) {
    return history_exec(db, "BEGIN IMMEDIATE", err, err_size, "cannot begin history change");
}

static bool history_finish(CmnyDb *db, bool ok, char *err, size_t err_size) {
    if (ok && history_exec(db, "COMMIT", err, err_size, "cannot commit history change")) return true;
    (void)sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
    return false;
}

static bool snapshot_entry(CmnyDb *db, int64_t action_id, int side, int64_t entry_id,
                           char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO history_entry_snapshots(action_id,snapshot_side,entry_id,entry_type,"
        "occurred_on,payee,note,origin,voided_at,created_at,updated_at,revision)"
        " SELECT ?,?,id,entry_type,occurred_on,payee,note,origin,voided_at,created_at,updated_at,revision"
        " FROM entries WHERE id=?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, action_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, side);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, entry_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) history_db_error(db, err, err_size, "cannot snapshot entry");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool snapshot_postings(CmnyDb *db, int64_t action_id, int side, int64_t entry_id,
                              char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO history_posting_snapshots(action_id,snapshot_side,posting_ordinal,"
        "original_posting_id,account_id,amount_minor,clear_state,memo,sort_order)"
        " SELECT ?,?,(SELECT COUNT(*) FROM postings q WHERE q.entry_id=p.entry_id AND"
        " (q.sort_order<p.sort_order OR (q.sort_order=p.sort_order AND q.id<p.id))),"
        " p.id,p.account_id,p.amount_minor,p.clear_state,p.memo,p.sort_order"
        " FROM postings p WHERE p.entry_id=? ORDER BY p.sort_order,p.id", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, action_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, side);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, entry_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (!ok) history_db_error(db, err, err_size, "cannot snapshot postings");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool snapshot_allocations(CmnyDb *db, int64_t action_id, int side,
                                 char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO history_allocation_snapshots(action_id,snapshot_side,posting_ordinal,"
        "allocation_ordinal,original_allocation_id,category_id,amount_minor,note,sort_order)"
        " SELECT ?,?,ps.posting_ordinal,"
        " (SELECT COUNT(*) FROM allocations q WHERE q.posting_id=a.posting_id AND"
        " (q.sort_order<a.sort_order OR (q.sort_order=a.sort_order AND q.id<a.id))),"
        " a.id,a.category_id,a.amount_minor,a.note,a.sort_order"
        " FROM history_posting_snapshots ps JOIN allocations a"
        " ON a.posting_id=ps.original_posting_id"
        " WHERE ps.action_id=? AND ps.snapshot_side=? ORDER BY ps.posting_ordinal,a.sort_order,a.id",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, action_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, side);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, action_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 4, side);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (!ok) history_db_error(db, err, err_size, "cannot snapshot allocations");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool snapshot_tags(CmnyDb *db, int64_t action_id, int side, int64_t entry_id,
                          char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO history_tag_snapshots(action_id,snapshot_side,tag_id)"
        " SELECT ?,?,tag_id FROM entry_tags WHERE entry_id=?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, action_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, side);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, entry_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (!ok) history_db_error(db, err, err_size, "cannot snapshot tags");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool capture_snapshot(CmnyDb *db, int64_t action_id, int side, int64_t entry_id,
                             char *err, size_t err_size) {
    return snapshot_entry(db, action_id, side, entry_id, err, err_size) &&
           snapshot_postings(db, action_id, side, entry_id, err, err_size) &&
           snapshot_allocations(db, action_id, side, err, err_size) &&
           snapshot_tags(db, action_id, side, entry_id, err, err_size);
}

bool cmny_history_action_begin(CmnyDb *db, CmnyHistoryActionType type, int64_t entry_id,
                               bool capture_before, int64_t *action_id,
                               char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || action_id == NULL || entry_id <= 0 ||
        type < CMNY_HISTORY_CREATE || type > CMNY_HISTORY_TRANSFER ||
        sqlite3_get_autocommit(db->handle) != 0) {
        history_error(err, err_size, "invalid history action context");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO history_actions(action_type,entry_id,entry_type,before_revision)"
        " SELECT ?,id,entry_type,CASE WHEN ? THEN revision END FROM entries WHERE id=?",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, (int)type);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, capture_before ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, entry_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (ok) *action_id = sqlite3_last_insert_rowid(db->handle);
    if (!ok) history_db_error(db, err, err_size, "cannot begin history action");
    (void)sqlite3_finalize(stmt);
    if (ok && capture_before)
        ok = capture_snapshot(db, *action_id, 1, entry_id, err, err_size);
    return ok;
}

bool cmny_history_action_finish(CmnyDb *db, int64_t action_id, int64_t entry_id,
                                char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || action_id <= 0 || entry_id <= 0 ||
        sqlite3_get_autocommit(db->handle) != 0) {
        history_error(err, err_size, "invalid history action context");
        return false;
    }
    if (!capture_snapshot(db, action_id, 2, entry_id, err, err_size)) return false;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "UPDATE history_actions SET after_revision=(SELECT revision FROM entries WHERE id=?)"
        " WHERE id=? AND entry_id=? AND after_revision IS NULL", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, entry_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, action_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, entry_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) history_db_error(db, err, err_size, "cannot finish history action");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_history_list(CmnyDb *db, bool include_undone, size_t offset,
                       CmnyHistoryAction *out, size_t cap, size_t *count,
                       char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || out == NULL || count == NULL ||
        cap > CMNY_HISTORY_LIST_LIMIT) {
        history_error(err, err_size, "invalid history query");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT id,entry_id,before_revision,after_revision,created_at,undone_at,"
        "action_type,entry_type FROM history_actions WHERE (? OR undone_at IS NULL)"
        " ORDER BY id DESC LIMIT ? OFFSET ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, include_undone ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)cap);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, (sqlite3_int64)offset);
    if (rc != SQLITE_OK) {
        history_db_error(db, err, err_size, "cannot prepare history query");
        (void)sqlite3_finalize(stmt);
        return false;
    }
    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap) {
        out[used].id = sqlite3_column_int64(stmt, 0);
        out[used].entry_id = sqlite3_column_int64(stmt, 1);
        out[used].before_revision = sqlite3_column_type(stmt, 2) == SQLITE_NULL
            ? 0 : sqlite3_column_int64(stmt, 2);
        out[used].after_revision = sqlite3_column_int64(stmt, 3);
        out[used].created_at = sqlite3_column_int64(stmt, 4);
        out[used].undone_at = sqlite3_column_type(stmt, 5) == SQLITE_NULL
            ? 0 : sqlite3_column_int64(stmt, 5);
        out[used].type = (CmnyHistoryActionType)sqlite3_column_int(stmt, 6);
        out[used].entry_type = (CmnyEntryType)sqlite3_column_int(stmt, 7);
        out[used].undone = sqlite3_column_type(stmt, 5) != SQLITE_NULL;
        used++;
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) history_db_error(db, err, err_size, "cannot list history");
    *count = used;
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool snapshot_matches(CmnyDb *db, int64_t action_id, int64_t entry_id,
                             char *err, size_t err_size) {
    static const char *sql =
        "SELECT"
        " EXISTS(SELECT 1 FROM history_entry_snapshots s JOIN entries e ON e.id=s.entry_id"
        " WHERE s.action_id=? AND s.snapshot_side=2 AND e.id=?"
        " AND e.entry_type=s.entry_type AND e.occurred_on=s.occurred_on AND e.payee=s.payee"
        " AND e.note=s.note AND e.origin=s.origin AND e.voided_at IS s.voided_at"
        " AND e.created_at=s.created_at AND e.updated_at=s.updated_at AND e.revision=s.revision)"
        " AND (SELECT COUNT(*) FROM postings WHERE entry_id=?)="
        "     (SELECT COUNT(*) FROM history_posting_snapshots WHERE action_id=? AND snapshot_side=2)"
        " AND NOT EXISTS(SELECT 1 FROM postings p LEFT JOIN history_posting_snapshots s"
        " ON s.action_id=? AND s.snapshot_side=2 AND s.original_posting_id=p.id"
        " AND s.account_id=p.account_id AND s.amount_minor=p.amount_minor"
        " AND s.clear_state=p.clear_state AND s.memo=p.memo AND s.sort_order=p.sort_order"
        " WHERE p.entry_id=? AND s.original_posting_id IS NULL)"
        " AND (SELECT COUNT(*) FROM allocations a JOIN postings p ON p.id=a.posting_id WHERE p.entry_id=?)="
        "     (SELECT COUNT(*) FROM history_allocation_snapshots WHERE action_id=? AND snapshot_side=2)"
        " AND NOT EXISTS(SELECT 1 FROM allocations a JOIN postings p ON p.id=a.posting_id"
        " LEFT JOIN history_allocation_snapshots s ON s.action_id=? AND s.snapshot_side=2"
        " AND s.original_allocation_id=a.id AND s.category_id=a.category_id"
        " AND s.amount_minor=a.amount_minor AND s.note=a.note AND s.sort_order=a.sort_order"
        " WHERE p.entry_id=? AND s.original_allocation_id IS NULL)"
        " AND (SELECT COUNT(*) FROM entry_tags WHERE entry_id=?)="
        "     (SELECT COUNT(*) FROM history_tag_snapshots WHERE action_id=? AND snapshot_side=2)"
        " AND NOT EXISTS(SELECT 1 FROM entry_tags et LEFT JOIN history_tag_snapshots s"
        " ON s.action_id=? AND s.snapshot_side=2 AND s.tag_id=et.tag_id"
        " WHERE et.entry_id=? AND s.tag_id IS NULL)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    int bind = 1;
    int64_t values[] = {action_id,entry_id,entry_id,action_id,action_id,entry_id,
                        entry_id,action_id,action_id,entry_id,entry_id,action_id,
                        action_id,entry_id};
    for (size_t i = 0; rc == SQLITE_OK && i < sizeof(values) / sizeof(values[0]); i++)
        rc = sqlite3_bind_int64(stmt, bind++, values[i]);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1 &&
              sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) history_error(err, err_size, rc == SQLITE_ROW || rc == SQLITE_DONE
        ? "entry changed after this history action" : "cannot compare history snapshot");
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
    if (!ok) history_error(err, err_size, rc == SQLITE_ROW || rc == SQLITE_DONE
        ? "entry belongs to an active or finalized reconciliation" : "cannot inspect reconciliation state");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool restore_entry_header(CmnyDb *db, int64_t action_id, int64_t entry_id,
                                 char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "UPDATE entries SET (entry_type,occurred_on,payee,note,origin,voided_at)=("
        " SELECT entry_type,occurred_on,payee,note,origin,voided_at"
        " FROM history_entry_snapshots WHERE action_id=? AND snapshot_side=1),"
        " revision=revision+1,updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE id=?",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, action_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, entry_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) history_db_error(db, err, err_size, "cannot restore entry header");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool clear_entry_children(CmnyDb *db, int64_t entry_id, char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, "DELETE FROM postings WHERE entry_id=?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, entry_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    (void)sqlite3_finalize(stmt);
    stmt = NULL;
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle, "DELETE FROM entry_tags WHERE entry_id=?", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, entry_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
    }
    if (!ok) history_db_error(db, err, err_size, "cannot clear current entry details");
    return ok;
}

static bool restore_postings(CmnyDb *db, int64_t action_id, int64_t entry_id,
                             int64_t posting_ids[CMNY_ENTRY_CHILD_LIMIT], size_t *posting_count,
                             char *err, size_t err_size) {
    sqlite3_stmt *read = NULL;
    sqlite3_stmt *insert = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT posting_ordinal,account_id,amount_minor,clear_state,memo,sort_order"
        " FROM history_posting_snapshots WHERE action_id=? AND snapshot_side=1"
        " ORDER BY posting_ordinal", -1, &read, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(read, 1, action_id);
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO postings(entry_id,account_id,amount_minor,clear_state,memo,sort_order)"
        " VALUES(?,?,?,?,?,?)", -1, &insert, NULL);
    size_t used = 0;
    while (rc == SQLITE_OK && (rc = sqlite3_step(read)) == SQLITE_ROW) {
        int ordinal = sqlite3_column_int(read, 0);
        if (ordinal < 0 || ordinal >= (int)CMNY_ENTRY_CHILD_LIMIT || (size_t)ordinal != used) {
            rc = SQLITE_CORRUPT;
            break;
        }
        rc = sqlite3_bind_int64(insert, 1, entry_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(insert, 2, sqlite3_column_int64(read, 1));
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(insert, 3, sqlite3_column_int64(read, 2));
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(insert, 4, sqlite3_column_int(read, 3));
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(insert, 5,
            (const char *)sqlite3_column_text(read, 4), sqlite3_column_bytes(read, 4), SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(insert, 6, sqlite3_column_int(read, 5));
        if (rc == SQLITE_OK) rc = sqlite3_step(insert);
        if (rc == SQLITE_DONE) {
            posting_ids[used++] = sqlite3_last_insert_rowid(db->handle);
            rc = sqlite3_reset(insert);
            if (rc == SQLITE_OK) rc = sqlite3_clear_bindings(insert);
        }
    }
    bool ok = rc == SQLITE_DONE;
    if (ok) *posting_count = used;
    if (!ok) history_db_error(db, err, err_size, "cannot restore postings");
    (void)sqlite3_finalize(read);
    (void)sqlite3_finalize(insert);
    return ok;
}

static bool restore_allocations(CmnyDb *db, int64_t action_id,
                                const int64_t posting_ids[CMNY_ENTRY_CHILD_LIMIT],
                                size_t posting_count, char *err, size_t err_size) {
    sqlite3_stmt *read = NULL;
    sqlite3_stmt *insert = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT posting_ordinal,category_id,amount_minor,note,sort_order"
        " FROM history_allocation_snapshots WHERE action_id=? AND snapshot_side=1"
        " ORDER BY posting_ordinal,allocation_ordinal", -1, &read, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(read, 1, action_id);
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO allocations(posting_id,category_id,amount_minor,note,sort_order) VALUES(?,?,?,?,?)",
        -1, &insert, NULL);
    while (rc == SQLITE_OK && (rc = sqlite3_step(read)) == SQLITE_ROW) {
        int ordinal = sqlite3_column_int(read, 0);
        if (ordinal < 0 || (size_t)ordinal >= posting_count) { rc = SQLITE_CORRUPT; break; }
        rc = sqlite3_bind_int64(insert, 1, posting_ids[ordinal]);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(insert, 2, sqlite3_column_int64(read, 1));
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(insert, 3, sqlite3_column_int64(read, 2));
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(insert, 4,
            (const char *)sqlite3_column_text(read, 3), sqlite3_column_bytes(read, 3), SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(insert, 5, sqlite3_column_int(read, 4));
        if (rc == SQLITE_OK) rc = sqlite3_step(insert);
        if (rc == SQLITE_DONE) {
            rc = sqlite3_reset(insert);
            if (rc == SQLITE_OK) rc = sqlite3_clear_bindings(insert);
        }
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) history_db_error(db, err, err_size, "cannot restore allocations");
    (void)sqlite3_finalize(read);
    (void)sqlite3_finalize(insert);
    return ok;
}

static bool restore_tags(CmnyDb *db, int64_t action_id, int64_t entry_id,
                         char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO entry_tags(entry_id,tag_id) SELECT ?,tag_id FROM history_tag_snapshots"
        " WHERE action_id=? AND snapshot_side=1", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, entry_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, action_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (!ok) history_db_error(db, err, err_size, "cannot restore entry tags");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool restore_before_snapshot(CmnyDb *db, int64_t action_id, int64_t entry_id,
                                    char *err, size_t err_size) {
    int64_t posting_ids[CMNY_ENTRY_CHILD_LIMIT] = {0};
    size_t posting_count = 0;
    return restore_entry_header(db, action_id, entry_id, err, err_size) &&
           clear_entry_children(db, entry_id, err, err_size) &&
           restore_postings(db, action_id, entry_id, posting_ids, &posting_count, err, err_size) &&
           restore_allocations(db, action_id, posting_ids, posting_count, err, err_size) &&
           restore_tags(db, action_id, entry_id, err, err_size);
}

static bool current_revision(CmnyDb *db, int64_t entry_id, int64_t *revision,
                             char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, "SELECT revision FROM entries WHERE id=?", -1,
                                &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, entry_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW;
    if (ok) *revision = sqlite3_column_int64(stmt, 0);
    if (!ok) history_db_error(db, err, err_size, "cannot read restored revision");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_history_undo(CmnyDb *db, int64_t action_id, int64_t expected_after_revision,
                       int64_t *new_entry_revision, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || action_id <= 0 || expected_after_revision <= 0) {
        history_error(err, err_size, "invalid history undo request");
        return false;
    }
    if (!history_begin(db, err, err_size)) return false;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT action_type,entry_id,after_revision FROM history_actions"
        " WHERE id=? AND undone_at IS NULL", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, action_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW;
    CmnyHistoryActionType type = ok ? (CmnyHistoryActionType)sqlite3_column_int(stmt, 0) : 0;
    int64_t entry_id = ok ? sqlite3_column_int64(stmt, 1) : 0;
    int64_t after_revision = ok ? sqlite3_column_int64(stmt, 2) : 0;
    (void)sqlite3_finalize(stmt);
    stmt = NULL;
    if (!ok) history_error(err, err_size, "history action is missing or already undone");
    if (ok && after_revision != expected_after_revision) {
        history_error(err, err_size, "history revision does not match");
        ok = false;
    }
    if (ok) ok = snapshot_matches(db, action_id, entry_id, err, err_size) &&
                 entry_not_reconciling(db, entry_id, err, err_size);
    if (ok && (type == CMNY_HISTORY_CREATE || type == CMNY_HISTORY_TRANSFER)) {
        rc = sqlite3_prepare_v2(db->handle,
            "UPDATE entries SET voided_at=CAST(strftime('%s','now') AS INTEGER),"
            "revision=revision+1,updated_at=CAST(strftime('%s','now') AS INTEGER)"
            " WHERE id=? AND revision=? AND voided_at IS NULL", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, entry_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, after_revision);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
        if (!ok) history_db_error(db, err, err_size, "cannot undo created entry");
        (void)sqlite3_finalize(stmt);
        stmt = NULL;
    } else if (ok) {
        ok = restore_before_snapshot(db, action_id, entry_id, err, err_size);
    }
    int64_t revision = 0;
    if (ok) ok = current_revision(db, entry_id, &revision, err, err_size);
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "UPDATE history_actions SET undone_at=CAST(strftime('%s','now') AS INTEGER)"
            " WHERE id=? AND undone_at IS NULL", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, action_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
        if (!ok) history_db_error(db, err, err_size, "cannot mark history action undone");
        (void)sqlite3_finalize(stmt);
    }
    ok = history_finish(db, ok, err, err_size);
    if (ok && new_entry_revision != NULL) *new_entry_revision = revision;
    return ok;
}

bool cmny_history_undo_latest(CmnyDb *db, int64_t *undone_action_id,
                              int64_t *new_entry_revision, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL) {
        history_error(err, err_size, "database is not open");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT id,after_revision FROM history_actions WHERE undone_at IS NULL ORDER BY id DESC LIMIT 1",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int64_t action_id = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
    int64_t revision = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 1) : 0;
    bool found = rc == SQLITE_ROW;
    (void)sqlite3_finalize(stmt);
    if (!found) {
        history_error(err, err_size, "there is no action to undo");
        return false;
    }
    bool ok = cmny_history_undo(db, action_id, revision, new_entry_revision, err, err_size);
    if (ok && undone_action_id != NULL) *undone_action_id = action_id;
    return ok;
}

bool cmny_history_prune(CmnyDb *db, size_t keep_newest, size_t *removed,
                        char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || removed == NULL ||
        keep_newest > CMNY_HISTORY_KEEP_LIMIT) {
        history_error(err, err_size, "invalid history retention limit");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "DELETE FROM history_actions WHERE id IN(SELECT id FROM history_actions"
        " ORDER BY id DESC LIMIT -1 OFFSET ?)", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)keep_newest);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (ok) *removed = (size_t)sqlite3_changes(db->handle);
    if (!ok) history_db_error(db, err, err_size, "cannot prune history");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_history_integrity_check(CmnyDb *db, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL) {
        history_error(err, err_size, "database is not open");
        return false;
    }
    static const char *sql =
        "SELECT COUNT(*) FROM history_actions h WHERE h.id<=0 OR h.entry_id<=0"
        " OR h.action_type NOT BETWEEN 1 AND 4 OR h.entry_type NOT BETWEEN 1 AND 3"
        " OR h.after_revision IS NULL OR h.after_revision<=0"
        " OR ((h.action_type IN(1,4))<>(h.before_revision IS NULL))"
        " OR (SELECT COUNT(*) FROM history_entry_snapshots s"
        "     WHERE s.action_id=h.id AND s.snapshot_side=2)<>1"
        " OR ((h.before_revision IS NOT NULL)<>(SELECT COUNT(*) FROM history_entry_snapshots s"
        "     WHERE s.action_id=h.id AND s.snapshot_side=1)=1)"
        " OR EXISTS(SELECT 1 FROM history_entry_snapshots s WHERE s.action_id=h.id"
        "     AND (s.entry_id<>h.entry_id OR s.entry_type<>h.entry_type OR s.revision<=0))"
        " OR EXISTS(SELECT 1 FROM history_posting_snapshots s WHERE s.action_id=h.id"
        "     GROUP BY s.snapshot_side HAVING COUNT(*)>256)"
        " OR EXISTS(SELECT 1 FROM history_allocation_snapshots s WHERE s.action_id=h.id"
        "     GROUP BY s.snapshot_side,s.posting_ordinal HAVING COUNT(*)>256)"
        " OR EXISTS(SELECT 1 FROM history_tag_snapshots s WHERE s.action_id=h.id"
        "     GROUP BY s.snapshot_side HAVING COUNT(*)>256)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_int64(stmt, 0) == 0 &&
              sqlite3_step(stmt) == SQLITE_DONE;
    (void)sqlite3_finalize(stmt);
    if (!ok) {
        history_error(err, err_size, "history actions are inconsistent");
        return false;
    }
    static const char *snapshot_sql =
        "SELECT COUNT(*) FROM history_entry_snapshots s WHERE s.entry_id<=0"
        " OR s.entry_type NOT BETWEEN 1 AND 3 OR s.revision<=0 OR s.origin NOT BETWEEN 1 AND 4"
        " OR s.occurred_on NOT GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'"
        " OR date(s.occurred_on,'+0 days') IS NULL OR date(s.occurred_on,'+0 days')<>s.occurred_on"
        " OR length(s.payee)>96 OR s.payee GLOB '*[^ -~]*'"
        " OR length(s.note)>240 OR s.note GLOB '*[^ -~]*'"
        " OR s.revision<>(SELECT CASE s.snapshot_side WHEN 1 THEN h.before_revision"
        "     ELSE h.after_revision END FROM history_actions h WHERE h.id=s.action_id)"
        " OR (s.entry_type=1 AND ((SELECT COUNT(*) FROM history_posting_snapshots p"
        "     WHERE p.action_id=s.action_id AND p.snapshot_side=s.snapshot_side)<>1"
        "   OR (SELECT COUNT(*) FROM history_allocation_snapshots a"
        "     WHERE a.action_id=s.action_id AND a.snapshot_side=s.snapshot_side) NOT BETWEEN 1 AND 256"
        "   OR (SELECT SUM(a.amount_minor) FROM history_allocation_snapshots a"
        "     WHERE a.action_id=s.action_id AND a.snapshot_side=s.snapshot_side)<>"
        "      (SELECT amount_minor FROM history_posting_snapshots p"
        "       WHERE p.action_id=s.action_id AND p.snapshot_side=s.snapshot_side)))"
        " OR (s.entry_type=2 AND ((SELECT COUNT(*) FROM history_posting_snapshots p"
        "     WHERE p.action_id=s.action_id AND p.snapshot_side=s.snapshot_side)<>2"
        "   OR (SELECT SUM(p.amount_minor) FROM history_posting_snapshots p"
        "     WHERE p.action_id=s.action_id AND p.snapshot_side=s.snapshot_side)<>0"
        "   OR (SELECT COUNT(DISTINCT p.account_id) FROM history_posting_snapshots p"
        "     WHERE p.action_id=s.action_id AND p.snapshot_side=s.snapshot_side)<>2"
        "   OR EXISTS(SELECT 1 FROM history_allocation_snapshots a"
        "     WHERE a.action_id=s.action_id AND a.snapshot_side=s.snapshot_side)))"
        " OR (s.entry_type=3 AND ((SELECT COUNT(*) FROM history_posting_snapshots p"
        "     WHERE p.action_id=s.action_id AND p.snapshot_side=s.snapshot_side)<>1"
        "   OR EXISTS(SELECT 1 FROM history_allocation_snapshots a"
        "     WHERE a.action_id=s.action_id AND a.snapshot_side=s.snapshot_side)))"
        " OR EXISTS(SELECT 1 FROM history_posting_snapshots p WHERE p.action_id=s.action_id"
        "   AND p.snapshot_side=s.snapshot_side AND (p.account_id<=0 OR p.amount_minor=0"
        "   OR p.amount_minor NOT BETWEEN -9000000000000000 AND 9000000000000000"
        "   OR p.clear_state NOT BETWEEN 0 AND 2 OR length(p.memo)>120"
        "   OR p.memo GLOB '*[^ -~]*' OR p.sort_order<0"
        "   OR NOT EXISTS(SELECT 1 FROM accounts x WHERE x.id=p.account_id)))"
        " OR EXISTS(SELECT 1 FROM history_allocation_snapshots a WHERE a.action_id=s.action_id"
        "   AND a.snapshot_side=s.snapshot_side AND (a.category_id<=0 OR a.amount_minor=0"
        "   OR a.amount_minor NOT BETWEEN -9000000000000000 AND 9000000000000000"
        "   OR length(a.note)>120 OR a.note GLOB '*[^ -~]*' OR a.sort_order<0"
        "   OR NOT EXISTS(SELECT 1 FROM categories c WHERE c.id=a.category_id)"
        "   OR NOT EXISTS(SELECT 1 FROM history_posting_snapshots p WHERE p.action_id=a.action_id"
        "      AND p.snapshot_side=a.snapshot_side AND p.posting_ordinal=a.posting_ordinal)))"
        " OR EXISTS(SELECT 1 FROM history_tag_snapshots t WHERE t.action_id=s.action_id"
        "   AND t.snapshot_side=s.snapshot_side"
        "   AND NOT EXISTS(SELECT 1 FROM tags x WHERE x.id=t.tag_id))";
    stmt = NULL;
    rc = sqlite3_prepare_v2(db->handle, snapshot_sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    ok = rc == SQLITE_ROW && sqlite3_column_int64(stmt, 0) == 0 &&
         sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) history_error(err, err_size, "history snapshots are inconsistent");
    (void)sqlite3_finalize(stmt);
    return ok;
}
