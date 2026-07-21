#include "cmny_internal.h"
#include "cmny_reconcile.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static void reconcile_error(char *err, size_t err_size, const char *message) {
    if (err != NULL && err_size > 0) (void)snprintf(err, err_size, "%s", message);
}

static void reconcile_db_error(CmnyDb *db, char *err, size_t err_size, const char *context) {
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s: %s", context,
                       db != NULL && db->handle != NULL ? sqlite3_errmsg(db->handle)
                                                       : "database is not open");
    }
}

static bool reconcile_exec(CmnyDb *db, const char *sql, char *err, size_t err_size,
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

static bool reconcile_begin(CmnyDb *db, char *err, size_t err_size) {
    return reconcile_exec(db, "BEGIN IMMEDIATE", err, err_size, "cannot begin reconciliation change");
}

static bool reconcile_finish(CmnyDb *db, bool ok, char *err, size_t err_size) {
    if (ok && reconcile_exec(db, "COMMIT", err, err_size,
                             "cannot commit reconciliation change")) return true;
    (void)sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
    return false;
}

static bool copy_text(sqlite3_stmt *stmt, int column, char *out, size_t out_size) {
    const unsigned char *value = sqlite3_column_text(stmt, column);
    int bytes = sqlite3_column_bytes(stmt, column);
    if (value == NULL || bytes < 0 || (size_t)bytes >= out_size ||
        memchr(value, '\0', (size_t)bytes) != NULL) return false;
    memcpy(out, value, (size_t)bytes);
    out[bytes] = '\0';
    return true;
}

static bool subtract_exact(int64_t left, int64_t right, int64_t *result) {
    if ((right > 0 && left < INT64_MIN + right) ||
        (right < 0 && left > INT64_MAX + right)) return false;
    *result = left - right;
    return true;
}

bool cmny_reconcile_start(CmnyDb *db, int64_t account_id, const char *statement_on,
                          int64_t statement_balance_minor, int64_t *session_id,
                          char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || account_id <= 0 || session_id == NULL ||
        !cmny_date_valid(statement_on) || statement_balance_minor < -CMNY_AMOUNT_MAX ||
        statement_balance_minor > CMNY_AMOUNT_MAX) {
        reconcile_error(err, err_size, "invalid reconciliation statement");
        return false;
    }
    if (!reconcile_begin(db, err, err_size)) return false;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO reconciliation_sessions(account_id,statement_on,statement_balance_minor)"
        " SELECT a.id,?,? FROM accounts a JOIN settings s ON s.key='currency'"
        " WHERE a.id=? AND a.archived=0 AND a.currency_code=s.value", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, statement_on, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, statement_balance_minor);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, account_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (ok) *session_id = sqlite3_last_insert_rowid(db->handle);
    if (!ok) reconcile_db_error(db, err, err_size, "cannot start reconciliation");
    (void)sqlite3_finalize(stmt);
    stmt = NULL;
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "INSERT INTO reconciliation_items(session_id,posting_id,prior_clear_state)"
            " SELECT ?,p.id,p.clear_state FROM postings p JOIN entries e ON e.id=p.entry_id"
            " WHERE p.account_id=? AND p.clear_state=1 AND e.voided_at IS NULL AND e.occurred_on<=?",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, *session_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, account_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, statement_on, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        if (!ok) reconcile_db_error(db, err, err_size, "cannot initialize reconciliation items");
        (void)sqlite3_finalize(stmt);
    }
    return reconcile_finish(db, ok, err, err_size);
}

bool cmny_reconcile_get(CmnyDb *db, int64_t session_id, CmnyReconcileSession *out,
                        char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || session_id <= 0 || out == NULL) {
        reconcile_error(err, err_size, "invalid reconciliation lookup");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT s.id,s.account_id,s.statement_balance_minor,s.revision,s.created_at,s.finalized_at,"
        "s.status,s.statement_on,"
        "CASE WHEN s.status=2 THEN s.final_account_balance_minor ELSE"
        " COALESCE((SELECT SUM(p.amount_minor) FROM postings p JOIN entries e ON e.id=p.entry_id"
        " WHERE p.account_id=s.account_id AND e.voided_at IS NULL),0) END,"
        "CASE WHEN s.status=2 THEN s.final_cleared_balance_minor ELSE"
        " COALESCE((SELECT SUM(p.amount_minor) FROM postings p JOIN entries e ON e.id=p.entry_id"
        " WHERE p.account_id=s.account_id AND p.clear_state IN(1,2)"
        " AND e.voided_at IS NULL AND e.occurred_on<=s.statement_on),0) END,"
        "s.final_discrepancy_minor"
        " FROM reconciliation_sessions s WHERE s.id=?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, session_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW;
    if (ok) {
        out->id = sqlite3_column_int64(stmt, 0);
        out->account_id = sqlite3_column_int64(stmt, 1);
        out->statement_balance_minor = sqlite3_column_int64(stmt, 2);
        out->revision = sqlite3_column_int64(stmt, 3);
        out->created_at = sqlite3_column_int64(stmt, 4);
        out->finalized_at = sqlite3_column_type(stmt, 5) == SQLITE_NULL
            ? 0 : sqlite3_column_int64(stmt, 5);
        out->status = (CmnyReconcileStatus)sqlite3_column_int(stmt, 6);
        ok = copy_text(stmt, 7, out->statement_on, sizeof(out->statement_on));
        if (ok && out->status == CMNY_RECONCILE_FINALIZED) {
            ok = sqlite3_column_type(stmt, 8) == SQLITE_INTEGER &&
                 sqlite3_column_type(stmt, 9) == SQLITE_INTEGER &&
                 sqlite3_column_type(stmt, 10) == SQLITE_INTEGER;
            if (ok) {
                out->account_balance_minor = sqlite3_column_int64(stmt, 8);
                out->cleared_balance_minor = sqlite3_column_int64(stmt, 9);
                out->discrepancy_minor = sqlite3_column_int64(stmt, 10);
            }
        } else if (ok) {
            ok = sqlite3_column_type(stmt, 8) == SQLITE_INTEGER &&
                 sqlite3_column_type(stmt, 9) == SQLITE_INTEGER;
            if (ok) {
                out->account_balance_minor = sqlite3_column_int64(stmt, 8);
                out->cleared_balance_minor = sqlite3_column_int64(stmt, 9);
                ok = subtract_exact(out->statement_balance_minor, out->cleared_balance_minor,
                                    &out->discrepancy_minor);
            }
        }
    }
    if (!ok) reconcile_error(err, err_size, rc == SQLITE_DONE
        ? "reconciliation session does not exist" : "cannot calculate reconciliation balances");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_reconcile_list(CmnyDb *db, bool include_closed, size_t offset,
                         CmnyReconcileSession *out, size_t cap, size_t *count,
                         char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || out == NULL || count == NULL ||
        cap > CMNY_RECONCILE_LIST_LIMIT) {
        reconcile_error(err, err_size, "invalid reconciliation query");
        return false;
    }
    int64_t ids[CMNY_RECONCILE_LIST_LIMIT];
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT id FROM reconciliation_sessions WHERE (? OR status=1)"
        " ORDER BY id DESC LIMIT ? OFFSET ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, include_closed ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)cap);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, (sqlite3_int64)offset);
    if (rc != SQLITE_OK) {
        reconcile_db_error(db, err, err_size, "cannot prepare reconciliation list");
        *count = 0;
        (void)sqlite3_finalize(stmt);
        return false;
    }
    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap)
        ids[used++] = sqlite3_column_int64(stmt, 0);
    bool ok = rc == SQLITE_DONE;
    if (!ok) reconcile_db_error(db, err, err_size, "cannot list reconciliation sessions");
    (void)sqlite3_finalize(stmt);
    for (size_t i = 0; ok && i < used; i++)
        ok = cmny_reconcile_get(db, ids[i], &out[i], err, err_size);
    *count = ok ? used : 0;
    return ok;
}

bool cmny_reconcile_postings(CmnyDb *db, int64_t session_id, size_t offset,
                             CmnyReconcilePosting *out, size_t cap, size_t *count,
                             char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || session_id <= 0 || out == NULL || count == NULL ||
        cap > CMNY_RECONCILE_LIST_LIMIT) {
        reconcile_error(err, err_size, "invalid reconciliation posting query");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT p.id,e.id,p.amount_minor,p.sort_order,p.clear_state,"
        "CASE WHEN ri.posting_id IS NOT NULL AND p.clear_state IN(1,2) THEN 1 ELSE 0 END,"
        "e.occurred_on,e.payee FROM reconciliation_sessions s"
        " JOIN postings p ON p.account_id=s.account_id JOIN entries e ON e.id=p.entry_id"
        " LEFT JOIN reconciliation_items ri ON ri.session_id=s.id AND ri.posting_id=p.id"
        " WHERE s.id=? AND e.voided_at IS NULL AND e.occurred_on<=s.statement_on"
        " AND ((s.status=1 AND p.clear_state<>2) OR ri.posting_id IS NOT NULL)"
        " ORDER BY e.occurred_on,p.id LIMIT ? OFFSET ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, session_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)cap);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, (sqlite3_int64)offset);
    if (rc != SQLITE_OK) {
        reconcile_db_error(db, err, err_size, "cannot prepare reconciliation posting list");
        *count = 0;
        (void)sqlite3_finalize(stmt);
        return false;
    }
    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap) {
        out[used].posting_id = sqlite3_column_int64(stmt, 0);
        out[used].entry_id = sqlite3_column_int64(stmt, 1);
        out[used].amount_minor = sqlite3_column_int64(stmt, 2);
        out[used].sort_order = sqlite3_column_int(stmt, 3);
        out[used].clear_state = (CmnyPostingClearState)sqlite3_column_int(stmt, 4);
        out[used].selected = sqlite3_column_int(stmt, 5) != 0;
        if (!copy_text(stmt, 6, out[used].occurred_on, sizeof(out[used].occurred_on)) ||
            !copy_text(stmt, 7, out[used].payee, sizeof(out[used].payee))) {
            rc = SQLITE_CORRUPT;
            break;
        }
        used++;
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) reconcile_db_error(db, err, err_size, "cannot list reconciliation postings");
    *count = used;
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_reconcile_set_cleared(CmnyDb *db, int64_t session_id,
                                int64_t expected_session_revision, int64_t posting_id,
                                bool cleared, int64_t *new_session_revision,
                                char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || session_id <= 0 ||
        expected_session_revision <= 0 || posting_id <= 0) {
        reconcile_error(err, err_size, "invalid reconciliation selection");
        return false;
    }
    if (!reconcile_begin(db, err, err_size)) return false;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO reconciliation_items(session_id,posting_id,prior_clear_state)"
        " SELECT s.id,p.id,p.clear_state FROM reconciliation_sessions s"
        " JOIN postings p ON p.account_id=s.account_id JOIN entries e ON e.id=p.entry_id"
        " WHERE s.id=? AND s.status=1 AND s.revision=? AND p.id=? AND p.clear_state<>2"
        " AND e.voided_at IS NULL AND e.occurred_on<=s.statement_on"
        " ON CONFLICT(session_id,posting_id) DO NOTHING", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, session_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, expected_session_revision);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, posting_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    (void)sqlite3_finalize(stmt);
    stmt = NULL;
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "UPDATE postings SET clear_state=? WHERE id=? AND clear_state<>2"
            " AND EXISTS(SELECT 1 FROM reconciliation_items WHERE session_id=? AND posting_id=?)",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, cleared ? 1 : 0);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, posting_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, session_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 4, posting_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
        (void)sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "UPDATE reconciliation_sessions SET revision=revision+1,"
            "updated_at=CAST(strftime('%s','now') AS INTEGER)"
            " WHERE id=? AND status=1 AND revision=? RETURNING revision", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, session_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, expected_session_revision);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_ROW;
        if (ok && new_session_revision != NULL)
            *new_session_revision = sqlite3_column_int64(stmt, 0);
        if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
    }
    if (!ok) reconcile_error(err, err_size, "posting is unavailable or reconciliation changed");
    return reconcile_finish(db, ok, err, err_size);
}

bool cmny_reconcile_finalize(CmnyDb *db, int64_t session_id,
                             int64_t expected_session_revision,
                             char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || session_id <= 0 || expected_session_revision <= 0) {
        reconcile_error(err, err_size, "invalid reconciliation finalization");
        return false;
    }
    if (!reconcile_begin(db, err, err_size)) return false;
    CmnyReconcileSession session = {0};
    bool ok = cmny_reconcile_get(db, session_id, &session, err, err_size) &&
              session.status == CMNY_RECONCILE_OPEN &&
              session.revision == expected_session_revision && session.discrepancy_minor == 0;
    if (!ok && err != NULL && err_size > 0 && err[0] == '\0')
        reconcile_error(err, err_size, "reconciliation is not balanced or changed");
    sqlite3_stmt *stmt = NULL;
    int rc = SQLITE_OK;
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "UPDATE postings SET clear_state=2 WHERE clear_state=1 AND id IN("
            " SELECT ri.posting_id FROM reconciliation_items ri"
            " JOIN reconciliation_sessions s ON s.id=ri.session_id"
            " JOIN postings p ON p.id=ri.posting_id JOIN entries e ON e.id=p.entry_id"
            " WHERE s.id=? AND p.account_id=s.account_id AND e.voided_at IS NULL"
            " AND e.occurred_on<=s.statement_on)", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, session_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "UPDATE reconciliation_sessions SET status=2,revision=revision+1,"
            "updated_at=CAST(strftime('%s','now') AS INTEGER),"
            "finalized_at=CAST(strftime('%s','now') AS INTEGER),"
            "final_account_balance_minor=?,final_cleared_balance_minor=?,"
            "final_discrepancy_minor=? WHERE id=? AND status=1 AND revision=?", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, session.account_balance_minor);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, session.cleared_balance_minor);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, session.discrepancy_minor);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 4, session_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 5, expected_session_revision);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
        (void)sqlite3_finalize(stmt);
    }
    if (!ok && err != NULL && err_size > 0 && err[0] == '\0')
        reconcile_error(err, err_size, "cannot finalize reconciliation");
    return reconcile_finish(db, ok, err, err_size);
}

bool cmny_reconcile_cancel(CmnyDb *db, int64_t session_id,
                           int64_t expected_session_revision,
                           char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || session_id <= 0 || expected_session_revision <= 0) {
        reconcile_error(err, err_size, "invalid reconciliation cancellation");
        return false;
    }
    if (!reconcile_begin(db, err, err_size)) return false;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "UPDATE postings SET clear_state=(SELECT ri.prior_clear_state FROM reconciliation_items ri"
        " WHERE ri.session_id=? AND ri.posting_id=postings.id)"
        " WHERE id IN(SELECT ri.posting_id FROM reconciliation_items ri"
        " JOIN reconciliation_sessions s ON s.id=ri.session_id"
        " JOIN postings p ON p.id=ri.posting_id WHERE s.id=? AND s.status=1 AND s.revision=?"
        " AND p.account_id=s.account_id)", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, session_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, session_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, expected_session_revision);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    (void)sqlite3_finalize(stmt);
    stmt = NULL;
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "DELETE FROM reconciliation_items WHERE session_id=?", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, session_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "UPDATE reconciliation_sessions SET status=3,revision=revision+1,"
            "updated_at=CAST(strftime('%s','now') AS INTEGER)"
            " WHERE id=? AND status=1 AND revision=?", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, session_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, expected_session_revision);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
        (void)sqlite3_finalize(stmt);
    }
    if (!ok) reconcile_error(err, err_size, "reconciliation changed or cannot be cancelled");
    return reconcile_finish(db, ok, err, err_size);
}

bool cmny_reconcile_integrity_check(CmnyDb *db, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL) {
        reconcile_error(err, err_size, "database is not open");
        return false;
    }
    static const char *sql =
        "SELECT COUNT(*) FROM reconciliation_sessions s WHERE s.id<=0 OR s.account_id<=0"
        " OR s.status NOT BETWEEN 1 AND 3 OR s.revision<=0"
        " OR s.statement_balance_minor NOT BETWEEN -9000000000000000 AND 9000000000000000"
        " OR s.statement_on NOT GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'"
        " OR date(s.statement_on,'+0 days') IS NULL OR date(s.statement_on,'+0 days')<>s.statement_on"
        " OR ((s.status=2)<>(s.finalized_at IS NOT NULL))"
        " OR ((s.status=2)<>(s.final_account_balance_minor IS NOT NULL"
        " AND s.final_cleared_balance_minor IS NOT NULL"
        " AND s.final_discrepancy_minor IS NOT NULL))"
        " OR (s.status<>2 AND (s.final_account_balance_minor IS NOT NULL"
        " OR s.final_cleared_balance_minor IS NOT NULL"
        " OR s.final_discrepancy_minor IS NOT NULL))"
        " OR (s.status=2 AND s.final_discrepancy_minor<>"
        " s.statement_balance_minor-s.final_cleared_balance_minor)"
        " OR EXISTS(SELECT 1 FROM reconciliation_items ri JOIN postings p ON p.id=ri.posting_id"
        " JOIN entries e ON e.id=p.entry_id WHERE ri.session_id=s.id"
        " AND (p.account_id<>s.account_id OR e.occurred_on>s.statement_on"
        " OR ri.prior_clear_state NOT BETWEEN 0 AND 2"
        " OR (s.status IN(1,2) AND e.voided_at IS NOT NULL)))";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_int64(stmt, 0) == 0 &&
              sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) reconcile_error(err, err_size, "reconciliation data is inconsistent");
    (void)sqlite3_finalize(stmt);
    return ok;
}
