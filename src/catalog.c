#include "cmny_internal.h"

#include <stdio.h>
#include <string.h>

static void catalog_error(char *err, size_t err_size, const char *message) {
    if (err != NULL && err_size > 0) (void)snprintf(err, err_size, "%s", message);
}

static void catalog_db_error(CmnyDb *db, char *err, size_t err_size, const char *context) {
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s: %s", context,
                       db != NULL && db->handle != NULL ? sqlite3_errmsg(db->handle) : "database is not open");
    }
}

static bool catalog_ready(CmnyDb *db, char *err, size_t err_size) {
    if (db != NULL && db->handle != NULL) return true;
    catalog_error(err, err_size, "database is not open");
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

static bool exec_catalog(CmnyDb *db, const char *sql, char *err, size_t err_size,
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

static bool account_type_valid(CmnyAccountType type) {
    return type >= CMNY_ACCOUNT_CASH && type <= CMNY_ACCOUNT_OTHER;
}

static bool account_insert(CmnyDb *db, const char *name, CmnyAccountType type,
                           const char *institution, int64_t *new_id,
                           char *err, size_t err_size) {
    const char *place = institution != NULL ? institution : "";
    if (!catalog_ready(db, err, err_size) ||
        !cmny_text_valid(name, CMNY_ACCOUNT_NAME_MAX, false) ||
        !cmny_text_valid(place, CMNY_INSTITUTION_MAX, true) || !account_type_valid(type)) {
        catalog_error(err, err_size, "invalid account data");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO accounts(name,account_type,currency_code,institution,sort_order)"
        " SELECT ?,?,value,?,COALESCE((SELECT MAX(sort_order)+1 FROM accounts),0)"
        " FROM settings WHERE key='currency'", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, (int)type);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, place, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (ok && new_id != NULL) *new_id = sqlite3_last_insert_rowid(db->handle);
    if (!ok) catalog_db_error(db, err, err_size, "cannot create account");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_account_create_with_opening(CmnyDb *db, const char *name, CmnyAccountType type,
                                      const char *institution, int64_t opening_balance_minor,
                                      const char *balance_on, int64_t *new_id,
                                      char *err, size_t err_size) {
    if (opening_balance_minor < -CMNY_AMOUNT_MAX || opening_balance_minor > CMNY_AMOUNT_MAX ||
        (opening_balance_minor != 0 && !cmny_date_valid(balance_on))) {
        catalog_error(err, err_size, "invalid opening balance");
        return false;
    }
    if (!catalog_ready(db, err, err_size) ||
        !exec_catalog(db, "BEGIN IMMEDIATE", err, err_size, "cannot begin account creation"))
        return false;
    int64_t account_id = 0;
    bool ok = account_insert(db, name, type, institution, &account_id, err, err_size);
    sqlite3_stmt *stmt = NULL;
    int rc = SQLITE_OK;
    int64_t entry_id = 0;
    if (ok && opening_balance_minor != 0) {
        rc = sqlite3_prepare_v2(db->handle,
            "INSERT INTO entries(entry_type,occurred_on,note) VALUES(3,?,'Opening balance')",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, balance_on, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        if (ok) entry_id = sqlite3_last_insert_rowid(db->handle);
        (void)sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (ok && opening_balance_minor != 0) {
        rc = sqlite3_prepare_v2(db->handle,
            "INSERT INTO postings(entry_id,account_id,amount_minor,sort_order) VALUES(?,?,?,0)",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, entry_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, account_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, opening_balance_minor);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
    }
    if (!ok && err != NULL && err_size > 0 && err[0] == '\0')
        catalog_db_error(db, err, err_size, "cannot create opening balance");
    if (ok && exec_catalog(db, "COMMIT", err, err_size,
                           "cannot commit account creation")) {
        ok = true;
    } else {
        (void)sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
        ok = false;
    }
    if (ok && new_id != NULL) *new_id = account_id;
    return ok;
}

bool cmny_account_create(CmnyDb *db, const char *name, CmnyAccountType type,
                         const char *institution, int64_t *new_id,
                         char *err, size_t err_size) {
    return cmny_account_create_with_opening(db, name, type, institution, 0, NULL,
                                            new_id, err, err_size);
}

bool cmny_account_update(CmnyDb *db, int64_t id, int64_t expected_revision,
                         const char *name, CmnyAccountType type, const char *institution,
                         char *err, size_t err_size) {
    const char *place = institution != NULL ? institution : "";
    if (!catalog_ready(db, err, err_size) || id <= 0 || expected_revision <= 0 ||
        !cmny_text_valid(name, CMNY_ACCOUNT_NAME_MAX, false) ||
        !cmny_text_valid(place, CMNY_INSTITUTION_MAX, true) || !account_type_valid(type)) {
        catalog_error(err, err_size, "invalid account data");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "UPDATE accounts SET name=?,account_type=?,institution=?,revision=revision+1,"
        "updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE id=? AND revision=?", -1,
        &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, (int)type);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, place, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 4, id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 5, expected_revision);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) catalog_error(err, err_size, rc == SQLITE_DONE ? "account changed; reload and retry"
                                                           : sqlite3_errmsg(db->handle));
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_account_set_archived(CmnyDb *db, int64_t id, int64_t expected_revision,
                               bool archived, char *err, size_t err_size) {
    if (!catalog_ready(db, err, err_size) || id <= 0 || expected_revision <= 0) {
        catalog_error(err, err_size, "invalid account revision");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "UPDATE accounts SET archived=?,revision=revision+1,"
        "updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE id=? AND revision=?", -1,
        &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, archived ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, expected_revision);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) catalog_error(err, err_size, rc == SQLITE_DONE ? "account changed; reload and retry"
                                                           : sqlite3_errmsg(db->handle));
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_account_list(CmnyDb *db, bool include_archived, CmnyAccount *out, size_t cap,
                       size_t *count, char *err, size_t err_size) {
    if (!catalog_ready(db, err, err_size) || out == NULL || count == NULL) {
        catalog_error(err, err_size, "invalid account query");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT a.id,a.account_type,a.currency_code,a.institution,a.archived,a.sort_order,a.revision,a.name,"
        "COALESCE((SELECT SUM(p.amount_minor) FROM postings p JOIN entries e ON e.id=p.entry_id"
        " WHERE p.account_id=a.id AND e.voided_at IS NULL),0)"
        " FROM accounts a WHERE (? OR a.archived=0) ORDER BY a.sort_order,a.id LIMIT ?", -1,
        &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, include_archived ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)cap);
    if (rc != SQLITE_OK) {
        catalog_db_error(db, err, err_size, "cannot prepare account list");
        *count = 0;
        (void)sqlite3_finalize(stmt);
        return false;
    }
    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap) {
        CmnyAccount *account = &out[used];
        account->id = sqlite3_column_int64(stmt, 0);
        account->type = (CmnyAccountType)sqlite3_column_int(stmt, 1);
        account->archived = sqlite3_column_int(stmt, 4) != 0;
        account->sort_order = sqlite3_column_int(stmt, 5);
        account->revision = sqlite3_column_int64(stmt, 6);
        account->balance_minor = sqlite3_column_int64(stmt, 8);
        if (!copy_text(stmt, 2, account->currency, sizeof(account->currency)) ||
            !copy_text(stmt, 3, account->institution, sizeof(account->institution)) ||
            !copy_text(stmt, 7, account->name, sizeof(account->name))) {
            rc = SQLITE_CORRUPT;
            break;
        }
        used++;
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) catalog_db_error(db, err, err_size, "cannot list accounts");
    *count = used;
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_account_balance(CmnyDb *db, int64_t account_id, int64_t *balance_minor,
                          char *err, size_t err_size) {
    if (!catalog_ready(db, err, err_size) || account_id <= 0 || balance_minor == NULL) {
        catalog_error(err, err_size, "invalid account balance query");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT COALESCE(SUM(CASE WHEN e.id IS NOT NULL THEN p.amount_minor ELSE 0 END),0)"
        " FROM accounts a LEFT JOIN postings p ON p.account_id=a.id"
        " LEFT JOIN entries e ON e.id=p.entry_id AND e.voided_at IS NULL"
        " WHERE a.id=? GROUP BY a.id", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, account_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW;
    if (ok) *balance_minor = sqlite3_column_int64(stmt, 0);
    if (!ok) catalog_error(err, err_size, rc == SQLITE_DONE ? "account does not exist"
                                                           : sqlite3_errmsg(db->handle));
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool category_row(sqlite3_stmt *stmt, CmnyCategory *out) {
    out->id = sqlite3_column_int64(stmt, 0);
    out->parent_id = sqlite3_column_type(stmt, 1) == SQLITE_NULL ? 0 : sqlite3_column_int64(stmt, 1);
    out->merged_into_id = sqlite3_column_type(stmt, 2) == SQLITE_NULL ? 0 : sqlite3_column_int64(stmt, 2);
    out->kind_mask = sqlite3_column_int(stmt, 3);
    out->archived = sqlite3_column_int(stmt, 4) != 0;
    out->revision = sqlite3_column_int64(stmt, 5);
    return copy_text(stmt, 6, out->name, sizeof(out->name)) &&
           copy_text(stmt, 7, out->marker, sizeof(out->marker)) &&
           copy_text(stmt, 8, out->color, sizeof(out->color));
}

static bool category_parent_valid(CmnyDb *db, int64_t id, int64_t parent_id,
                                  char *err, size_t err_size) {
    if (parent_id == 0) return true;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "WITH RECURSIVE descendants(id) AS ("
        " SELECT id FROM categories WHERE parent_id=?"
        " UNION SELECT c.id FROM categories c JOIN descendants d ON c.parent_id=d.id)"
        " SELECT NOT EXISTS(SELECT 1 FROM descendants WHERE id=?)", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, parent_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1 &&
              sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) catalog_error(err, err_size, rc == SQLITE_ROW || rc == SQLITE_DONE
        ? "category parent would create a cycle" : sqlite3_errmsg(db->handle));
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_category_create_styled(CmnyDb *db, const char *name, int kind_mask, int64_t parent_id,
                                 const char *marker, const char *color, int64_t *new_id,
                                 char *err, size_t err_size) {
    const char *visual_marker = marker != NULL ? marker : "";
    const char *visual_color = color != NULL ? color : "";
    if (!catalog_ready(db, err, err_size) ||
        !cmny_text_valid(name, CMNY_CATEGORY_MAX, false) || kind_mask < 1 || kind_mask > 3 ||
        parent_id < 0 || !cmny_text_valid(visual_marker, CMNY_CATEGORY_MARKER_MAX, true) ||
        !cmny_text_valid(visual_color, CMNY_CATEGORY_COLOR_MAX, true)) {
        catalog_error(err, err_size, "invalid category data");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO categories(name,kind_mask,parent_id,marker,color) VALUES(?,?,NULLIF(?,0),?,?)",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, kind_mask);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, parent_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, visual_marker, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, visual_color, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (ok && new_id != NULL) *new_id = sqlite3_last_insert_rowid(db->handle);
    if (!ok) catalog_db_error(db, err, err_size, "cannot create category");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_category_create(CmnyDb *db, const char *name, int kind_mask, int64_t parent_id,
                          int64_t *new_id, char *err, size_t err_size) {
    return cmny_category_create_styled(db, name, kind_mask, parent_id, "", "", new_id,
                                       err, err_size);
}

bool cmny_category_update(CmnyDb *db, int64_t id, int64_t expected_revision,
                          const char *name, int kind_mask, int64_t parent_id,
                          char *err, size_t err_size) {
    if (!catalog_ready(db, err, err_size) || id <= 0 || expected_revision <= 0 ||
        !cmny_text_valid(name, CMNY_CATEGORY_MAX, false) || kind_mask < 1 || kind_mask > 3 ||
        parent_id < 0 || parent_id == id) {
        catalog_error(err, err_size, "invalid category data");
        return false;
    }
    if (!category_parent_valid(db, id, parent_id, err, err_size)) return false;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "UPDATE categories SET name=?,kind_mask=?,parent_id=NULLIF(?,0),revision=revision+1,"
        "updated_at=CAST(strftime('%s','now') AS INTEGER)"
        " WHERE id=? AND revision=? AND merged_into_id IS NULL", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, kind_mask);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, parent_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 4, id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 5, expected_revision);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) catalog_error(err, err_size, rc == SQLITE_DONE ? "category changed; reload and retry"
                                                           : sqlite3_errmsg(db->handle));
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_category_update_styled(CmnyDb *db, int64_t id, int64_t expected_revision,
                                 const char *name, int kind_mask, int64_t parent_id,
                                 const char *marker, const char *color,
                                 char *err, size_t err_size) {
    const char *visual_marker = marker != NULL ? marker : "";
    const char *visual_color = color != NULL ? color : "";
    if (!catalog_ready(db, err, err_size) || id <= 0 || expected_revision <= 0 ||
        !cmny_text_valid(name, CMNY_CATEGORY_MAX, false) || kind_mask < 1 || kind_mask > 3 ||
        parent_id < 0 || parent_id == id ||
        !cmny_text_valid(visual_marker, CMNY_CATEGORY_MARKER_MAX, true) ||
        !cmny_text_valid(visual_color, CMNY_CATEGORY_COLOR_MAX, true)) {
        catalog_error(err, err_size, "invalid category data");
        return false;
    }
    if (!category_parent_valid(db, id, parent_id, err, err_size)) return false;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "UPDATE categories SET name=?,kind_mask=?,parent_id=NULLIF(?,0),marker=?,color=?,"
        "revision=revision+1,updated_at=CAST(strftime('%s','now') AS INTEGER)"
        " WHERE id=? AND revision=? AND merged_into_id IS NULL", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, kind_mask);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, parent_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, visual_marker, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, visual_color, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 6, id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 7, expected_revision);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) catalog_error(err, err_size, rc == SQLITE_DONE ? "category changed; reload and retry"
                                                           : sqlite3_errmsg(db->handle));
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_category_set_archived(CmnyDb *db, int64_t id, int64_t expected_revision,
                                bool archived, char *err, size_t err_size) {
    if (!catalog_ready(db, err, err_size) || id <= 0 || expected_revision <= 0) {
        catalog_error(err, err_size, "invalid category revision");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "UPDATE categories SET archived=?,revision=revision+1,updated_at=CAST(strftime('%s','now') AS INTEGER)"
        " WHERE id=? AND revision=? AND merged_into_id IS NULL", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, archived ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, expected_revision);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) catalog_error(err, err_size, rc == SQLITE_DONE ? "category changed; reload and retry"
                                                           : sqlite3_errmsg(db->handle));
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_category_find(CmnyDb *db, const char *name, CmnyCategory *out,
                        char *err, size_t err_size) {
    if (!catalog_ready(db, err, err_size) ||
        !cmny_text_valid(name, CMNY_CATEGORY_MAX, false) || out == NULL) {
        catalog_error(err, err_size, "invalid category lookup");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT id,parent_id,merged_into_id,kind_mask,archived,revision,name,marker,color"
        " FROM categories WHERE name=?",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && category_row(stmt, out);
    if (!ok) catalog_error(err, err_size, rc == SQLITE_DONE ? "category does not exist"
                                                           : sqlite3_errmsg(db->handle));
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_category_list(CmnyDb *db, bool include_archived, CmnyCategory *out, size_t cap,
                        size_t *count, char *err, size_t err_size) {
    if (!catalog_ready(db, err, err_size) || out == NULL || count == NULL) {
        catalog_error(err, err_size, "invalid category query");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT id,parent_id,merged_into_id,kind_mask,archived,revision,name,marker,color FROM categories"
        " WHERE (? OR archived=0) ORDER BY name LIMIT ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, include_archived ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)cap);
    if (rc != SQLITE_OK) {
        catalog_db_error(db, err, err_size, "cannot prepare category list");
        *count = 0;
        (void)sqlite3_finalize(stmt);
        return false;
    }
    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap) {
        if (!category_row(stmt, &out[used])) { rc = SQLITE_CORRUPT; break; }
        used++;
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) catalog_db_error(db, err, err_size, "cannot list categories");
    *count = used;
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_internal_category_ensure(CmnyDb *db, const char *name, int kind_mask,
                                   int64_t *category_id, char *err, size_t err_size) {
    if (!catalog_ready(db, err, err_size) || category_id == NULL ||
        !cmny_text_valid(name, CMNY_CATEGORY_MAX, false) || kind_mask < 1 || kind_mask > 3) {
        catalog_error(err, err_size, "invalid category");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO categories(name,kind_mask) VALUES(?,?)"
        " ON CONFLICT(name) DO UPDATE SET kind_mask=(categories.kind_mask | excluded.kind_mask)"
        " RETURNING COALESCE(merged_into_id,id)", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, kind_mask);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW;
    if (ok) *category_id = sqlite3_column_int64(stmt, 0);
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) catalog_db_error(db, err, err_size, "cannot resolve category");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_category_merge(CmnyDb *db, int64_t source_id, int64_t target_id,
                         char *err, size_t err_size) {
    if (!catalog_ready(db, err, err_size) || source_id <= 0 || target_id <= 0 ||
        source_id == target_id) {
        catalog_error(err, err_size, "invalid category merge");
        return false;
    }
    if (!category_parent_valid(db, source_id, target_id, err, err_size)) return false;
    if (!exec_catalog(db, "BEGIN IMMEDIATE", err, err_size, "cannot begin category merge")) return false;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT s.name,t.name FROM categories s JOIN categories t ON t.id=?"
        " WHERE s.id=? AND s.merged_into_id IS NULL AND t.merged_into_id IS NULL AND t.archived=0",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, target_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, source_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    char source_name[CMNY_CATEGORY_MAX + 1] = {0};
    char target_name[CMNY_CATEGORY_MAX + 1] = {0};
    bool ok = rc == SQLITE_ROW && copy_text(stmt, 0, source_name, sizeof(source_name)) &&
              copy_text(stmt, 1, target_name, sizeof(target_name));
    (void)sqlite3_finalize(stmt);
    if (!ok) catalog_error(err, err_size, "source or target category is unavailable");

    const char *sql[] = {
        "UPDATE allocations SET category_id=? WHERE category_id=?",
        "UPDATE categories SET parent_id=? WHERE parent_id=?",
        "UPDATE categories SET archived=1,merged_into_id=?,revision=revision+1,"
        "updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE id=?",
        "UPDATE categories SET kind_mask=(kind_mask | (SELECT kind_mask FROM categories WHERE id=?)),"
        "revision=revision+1,updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE id=?"
    };
    for (size_t i = 0; ok && i < sizeof(sql) / sizeof(sql[0]); i++) {
        rc = sqlite3_prepare_v2(db->handle, sql[i], -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, i == 3 ? source_id : target_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, i == 3 ? target_id : source_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "INSERT INTO budgets(month,category,limit_cents)"
            " SELECT month,?,limit_cents FROM budgets WHERE category=? AND true"
            " ON CONFLICT(month,category) DO UPDATE SET limit_cents=limit_cents+excluded.limit_cents",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, target_name, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, source_name, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
    }
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle, "DELETE FROM budgets WHERE category=?", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, source_name, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
    }
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "INSERT OR IGNORE INTO recurring(kind,amount_cents,category,note,day_of_month,created_at)"
            " SELECT kind,amount_cents,?,note,day_of_month,created_at FROM recurring WHERE category=?",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, target_name, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, source_name, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
    }
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle, "DELETE FROM recurring WHERE category=?", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, source_name, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
    }
    if (!ok) catalog_db_error(db, err, err_size, "cannot merge categories");
    if (ok && exec_catalog(db, "COMMIT", err, err_size,
                           "cannot commit category merge")) {
        ok = true;
    } else {
        (void)sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
        ok = false;
    }
    return ok;
}

static bool tag_change(CmnyDb *db, int64_t id, int64_t revision, const char *name,
                       int archived, char *err, size_t err_size) {
    const char *sql = name != NULL
        ? "UPDATE tags SET name=?,revision=revision+1,updated_at=CAST(strftime('%s','now') AS INTEGER)"
          " WHERE id=? AND revision=?"
        : "UPDATE tags SET archived=?,revision=revision+1,updated_at=CAST(strftime('%s','now') AS INTEGER)"
          " WHERE id=? AND revision=?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK && name != NULL) rc = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK && name == NULL) rc = sqlite3_bind_int(stmt, 1, archived);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, revision);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) catalog_error(err, err_size, rc == SQLITE_DONE ? "tag changed; reload and retry"
                                                           : sqlite3_errmsg(db->handle));
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_tag_create(CmnyDb *db, const char *name, int64_t *new_id,
                     char *err, size_t err_size) {
    if (!catalog_ready(db, err, err_size) || !cmny_text_valid(name, CMNY_TAG_NAME_MAX, false)) {
        catalog_error(err, err_size, "invalid tag name");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, "INSERT INTO tags(name) VALUES(?)", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (ok && new_id != NULL) *new_id = sqlite3_last_insert_rowid(db->handle);
    if (!ok) catalog_db_error(db, err, err_size, "cannot create tag");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_tag_update(CmnyDb *db, int64_t id, int64_t expected_revision,
                     const char *name, char *err, size_t err_size) {
    if (!catalog_ready(db, err, err_size) || id <= 0 || expected_revision <= 0 ||
        !cmny_text_valid(name, CMNY_TAG_NAME_MAX, false)) {
        catalog_error(err, err_size, "invalid tag data");
        return false;
    }
    return tag_change(db, id, expected_revision, name, 0, err, err_size);
}

bool cmny_tag_set_archived(CmnyDb *db, int64_t id, int64_t expected_revision,
                           bool archived, char *err, size_t err_size) {
    if (!catalog_ready(db, err, err_size) || id <= 0 || expected_revision <= 0) {
        catalog_error(err, err_size, "invalid tag revision");
        return false;
    }
    return tag_change(db, id, expected_revision, NULL, archived ? 1 : 0, err, err_size);
}

bool cmny_tag_list(CmnyDb *db, bool include_archived, CmnyTag *out, size_t cap,
                   size_t *count, char *err, size_t err_size) {
    if (!catalog_ready(db, err, err_size) || out == NULL || count == NULL) {
        catalog_error(err, err_size, "invalid tag query");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT id,revision,archived,name FROM tags WHERE (? OR archived=0) ORDER BY name LIMIT ?",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, include_archived ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)cap);
    if (rc != SQLITE_OK) {
        catalog_db_error(db, err, err_size, "cannot prepare tag list");
        *count = 0;
        (void)sqlite3_finalize(stmt);
        return false;
    }
    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap) {
        out[used].id = sqlite3_column_int64(stmt, 0);
        out[used].revision = sqlite3_column_int64(stmt, 1);
        out[used].archived = sqlite3_column_int(stmt, 2) != 0;
        if (!copy_text(stmt, 3, out[used].name, sizeof(out[used].name))) { rc = SQLITE_CORRUPT; break; }
        used++;
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) catalog_db_error(db, err, err_size, "cannot list tags");
    *count = used;
    (void)sqlite3_finalize(stmt);
    return ok;
}
