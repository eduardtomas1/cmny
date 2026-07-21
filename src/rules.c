#include "cmny_internal.h"
#include "cmny_rules.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static void rules_error(char *err, size_t err_size, const char *message) {
    if (err != NULL && err_size > 0) (void)snprintf(err, err_size, "%s", message);
}

static void rules_db_error(CmnyDb *db, char *err, size_t err_size, const char *context) {
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s: %s", context,
                       db != NULL && db->handle != NULL ? sqlite3_errmsg(db->handle)
                                                       : "database is not open");
    }
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

static bool fixed_text(const char *text, size_t capacity) {
    return text != NULL && memchr(text, '\0', capacity) != NULL;
}

static bool text_mode_valid(CmnyRuleTextMode mode, const char *pattern,
                            size_t maximum) {
    if (mode < CMNY_RULE_TEXT_ANY || mode > CMNY_RULE_TEXT_CONTAINS || pattern == NULL)
        return false;
    if (mode == CMNY_RULE_TEXT_ANY) return pattern[0] == '\0';
    return cmny_text_valid(pattern, maximum, false);
}

static bool draft_valid(const CmnyRuleDraft *draft) {
    if (draft == NULL || draft->account_id < 0 || draft->category_id <= 0 ||
        draft->tag_id < 0 || draft->sort_order < 0 ||
        !cmny_text_valid(draft->name, CMNY_RULE_NAME_MAX, false) ||
        !text_mode_valid(draft->payee_mode, draft->payee_pattern,
                         CMNY_RULE_PAYEE_PATTERN_MAX) ||
        !text_mode_valid(draft->note_mode, draft->note_pattern,
                         CMNY_RULE_NOTE_PATTERN_MAX)) return false;
    if (draft->has_minimum_amount &&
        (draft->minimum_amount_minor < -CMNY_AMOUNT_MAX ||
         draft->minimum_amount_minor > CMNY_AMOUNT_MAX)) return false;
    if (draft->has_maximum_amount &&
        (draft->maximum_amount_minor < -CMNY_AMOUNT_MAX ||
         draft->maximum_amount_minor > CMNY_AMOUNT_MAX)) return false;
    return !draft->has_minimum_amount || !draft->has_maximum_amount ||
           draft->minimum_amount_minor <= draft->maximum_amount_minor;
}

static bool references_available(CmnyDb *db, const CmnyRuleDraft *draft,
                                 char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT EXISTS(SELECT 1 FROM categories WHERE id=? AND archived=0"
        " AND merged_into_id IS NULL),"
        "(?=0 OR EXISTS(SELECT 1 FROM accounts WHERE id=? AND archived=0)),"
        "(?=0 OR EXISTS(SELECT 1 FROM tags WHERE id=? AND archived=0))", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, draft->category_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, draft->account_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, draft->account_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 4, draft->tag_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 5, draft->tag_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0 &&
              sqlite3_column_int(stmt, 1) != 0 && sqlite3_column_int(stmt, 2) != 0 &&
              sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) rules_error(err, err_size, rc == SQLITE_ROW || rc == SQLITE_DONE
        ? "rule account, category, or tag is unavailable"
        : "cannot validate rule references");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static int bind_optional_id(sqlite3_stmt *stmt, int index, int64_t value) {
    return value == 0 ? sqlite3_bind_null(stmt, index)
                      : sqlite3_bind_int64(stmt, index, value);
}

static int bind_optional_amount(sqlite3_stmt *stmt, int index, bool present,
                                int64_t value) {
    return present ? sqlite3_bind_int64(stmt, index, value)
                   : sqlite3_bind_null(stmt, index);
}

static bool bind_draft(sqlite3_stmt *stmt, const CmnyRuleDraft *draft, int start) {
    int rc = sqlite3_bind_text(stmt, start, draft->name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, start + 1, draft->enabled ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, start + 2, draft->sort_order);
    if (rc == SQLITE_OK) rc = bind_optional_id(stmt, start + 3, draft->account_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, start + 4, (int)draft->payee_mode);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt, start + 5, draft->payee_pattern, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, start + 6, (int)draft->note_mode);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt, start + 7, draft->note_pattern, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = bind_optional_amount(stmt, start + 8, draft->has_minimum_amount,
                                  draft->minimum_amount_minor);
    if (rc == SQLITE_OK)
        rc = bind_optional_amount(stmt, start + 9, draft->has_maximum_amount,
                                  draft->maximum_amount_minor);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, start + 10, draft->category_id);
    if (rc == SQLITE_OK) rc = bind_optional_id(stmt, start + 11, draft->tag_id);
    return rc == SQLITE_OK;
}

bool cmny_rule_create(CmnyDb *db, const CmnyRuleDraft *draft, int64_t *new_id,
                      char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || !draft_valid(draft)) {
        rules_error(err, err_size, "invalid categorization rule");
        return false;
    }
    if (!references_available(db, draft, err, err_size)) return false;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO categorization_rules(name,enabled,sort_order,account_id,"
        "payee_mode,payee_pattern,note_mode,note_pattern,minimum_amount_minor,"
        "maximum_amount_minor,category_id,tag_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
        -1, &stmt, NULL);
    bool ok = rc == SQLITE_OK && bind_draft(stmt, draft, 1);
    if (ok) {
        rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
    }
    if (ok && new_id != NULL) *new_id = sqlite3_last_insert_rowid(db->handle);
    if (!ok) rules_db_error(db, err, err_size, "cannot create categorization rule");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool read_rule(sqlite3_stmt *stmt, CmnyRule *out) {
    *out = (CmnyRule){0};
    out->id = sqlite3_column_int64(stmt, 0);
    out->account_id = sqlite3_column_type(stmt, 1) == SQLITE_NULL
        ? 0 : sqlite3_column_int64(stmt, 1);
    out->category_id = sqlite3_column_int64(stmt, 2);
    out->tag_id = sqlite3_column_type(stmt, 3) == SQLITE_NULL
        ? 0 : sqlite3_column_int64(stmt, 3);
    out->has_minimum_amount = sqlite3_column_type(stmt, 4) != SQLITE_NULL;
    out->minimum_amount_minor = out->has_minimum_amount ? sqlite3_column_int64(stmt, 4) : 0;
    out->has_maximum_amount = sqlite3_column_type(stmt, 5) != SQLITE_NULL;
    out->maximum_amount_minor = out->has_maximum_amount ? sqlite3_column_int64(stmt, 5) : 0;
    out->revision = sqlite3_column_int64(stmt, 6);
    out->created_at = sqlite3_column_int64(stmt, 7);
    out->updated_at = sqlite3_column_int64(stmt, 8);
    out->sort_order = sqlite3_column_int(stmt, 9);
    out->enabled = sqlite3_column_int(stmt, 10) != 0;
    out->payee_mode = (CmnyRuleTextMode)sqlite3_column_int(stmt, 11);
    out->note_mode = (CmnyRuleTextMode)sqlite3_column_int(stmt, 12);
    return copy_text(stmt, 13, out->name, sizeof(out->name)) &&
           copy_text(stmt, 14, out->payee_pattern, sizeof(out->payee_pattern)) &&
           copy_text(stmt, 15, out->note_pattern, sizeof(out->note_pattern));
}

static const char *rule_select(void) {
    return "SELECT id,account_id,category_id,tag_id,minimum_amount_minor,"
           "maximum_amount_minor,revision,created_at,updated_at,sort_order,enabled,"
           "payee_mode,note_mode,name,payee_pattern,note_pattern"
           " FROM categorization_rules";
}

bool cmny_rule_get(CmnyDb *db, int64_t id, CmnyRule *out,
                   char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || id <= 0 || out == NULL) {
        rules_error(err, err_size, "invalid categorization rule lookup");
        return false;
    }
    char sql[512];
    (void)snprintf(sql, sizeof(sql), "%s WHERE id=?", rule_select());
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && read_rule(stmt, out) &&
              sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) rules_error(err, err_size, rc == SQLITE_DONE
        ? "categorization rule does not exist" : "cannot read categorization rule");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_rule_update(CmnyDb *db, int64_t id, int64_t expected_revision,
                      const CmnyRuleDraft *draft, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || id <= 0 || expected_revision <= 0 ||
        !draft_valid(draft)) {
        rules_error(err, err_size, "invalid categorization rule update");
        return false;
    }
    if (!references_available(db, draft, err, err_size)) return false;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "UPDATE categorization_rules SET name=?,enabled=?,sort_order=?,account_id=?,"
        "payee_mode=?,payee_pattern=?,note_mode=?,note_pattern=?,minimum_amount_minor=?,"
        "maximum_amount_minor=?,category_id=?,tag_id=?,revision=revision+1,"
        "updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE id=? AND revision=?",
        -1, &stmt, NULL);
    bool ok = rc == SQLITE_OK && bind_draft(stmt, draft, 1);
    if (ok) rc = sqlite3_bind_int64(stmt, 13, id);
    if (rc == SQLITE_OK && ok) rc = sqlite3_bind_int64(stmt, 14, expected_revision);
    if (rc == SQLITE_OK && ok) rc = sqlite3_step(stmt);
    ok = ok && rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) rules_error(err, err_size, rc == SQLITE_DONE
        ? "categorization rule changed; reload and retry"
        : "cannot update categorization rule");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_rule_delete(CmnyDb *db, int64_t id, int64_t expected_revision,
                      char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || id <= 0 || expected_revision <= 0) {
        rules_error(err, err_size, "invalid categorization rule deletion");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "DELETE FROM categorization_rules WHERE id=? AND revision=?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, expected_revision);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) rules_error(err, err_size, rc == SQLITE_DONE
        ? "categorization rule changed; reload and retry"
        : "cannot delete categorization rule");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_rule_list(CmnyDb *db, size_t offset, CmnyRule *out, size_t cap,
                    size_t *count, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || out == NULL || count == NULL ||
        cap > CMNY_RULE_LIST_LIMIT) {
        rules_error(err, err_size, "invalid categorization rule list");
        return false;
    }
    char sql[640];
    (void)snprintf(sql, sizeof(sql), "%s ORDER BY sort_order,id LIMIT ? OFFSET ?", rule_select());
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)cap);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)offset);
    if (rc != SQLITE_OK) {
        rules_db_error(db, err, err_size, "cannot prepare categorization rule list");
        *count = 0;
        (void)sqlite3_finalize(stmt);
        return false;
    }
    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap) {
        if (!read_rule(stmt, &out[used])) {
            rc = SQLITE_CORRUPT;
            break;
        }
        used++;
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) rules_db_error(db, err, err_size, "cannot list categorization rules");
    *count = ok ? used : 0;
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_rule_match(CmnyDb *db, int64_t account_id, const CmnyBankCsvRow *row,
                     CmnyRuleMatch *out, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || account_id <= 0 || row == NULL || out == NULL ||
        row->amount_cents == 0 || row->amount_cents < -CMNY_AMOUNT_MAX ||
        row->amount_cents > CMNY_AMOUNT_MAX || !fixed_text(row->date, sizeof(row->date)) ||
        !cmny_date_valid(row->date) || !fixed_text(row->payee, sizeof(row->payee)) ||
        !fixed_text(row->note, sizeof(row->note))) {
        rules_error(err, err_size, "invalid categorization match input");
        return false;
    }
    *out = (CmnyRuleMatch){0};
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT r.id,r.category_id,r.tag_id FROM categorization_rules r"
        " WHERE r.enabled=1 AND (r.account_id IS NULL OR r.account_id=?)"
        " AND (r.minimum_amount_minor IS NULL OR r.minimum_amount_minor<=?)"
        " AND (r.maximum_amount_minor IS NULL OR r.maximum_amount_minor>=?)"
        " AND (r.payee_mode=0 OR (r.payee_mode=1 AND r.payee_pattern=? COLLATE BINARY)"
        "  OR (r.payee_mode=2 AND instr(?,r.payee_pattern)>0))"
        " AND (r.note_mode=0 OR (r.note_mode=1 AND r.note_pattern=? COLLATE BINARY)"
        "  OR (r.note_mode=2 AND instr(?,r.note_pattern)>0))"
        " AND EXISTS(SELECT 1 FROM accounts a JOIN settings s ON s.key='currency'"
        "  WHERE a.id=? AND a.archived=0 AND a.currency_code=s.value)"
        " ORDER BY r.sort_order,r.id LIMIT 1", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, account_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, row->amount_cents);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, row->amount_cents);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, row->payee, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, row->payee, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 6, row->note, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 7, row->note, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 8, account_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW || rc == SQLITE_DONE;
    if (rc == SQLITE_ROW) {
        out->matched = true;
        out->rule_id = sqlite3_column_int64(stmt, 0);
        out->category_id = sqlite3_column_int64(stmt, 1);
        out->tag_id = sqlite3_column_type(stmt, 2) == SQLITE_NULL
            ? 0 : sqlite3_column_int64(stmt, 2);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    if (!ok) rules_db_error(db, err, err_size, "cannot evaluate categorization rules");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_rules_integrity_check(CmnyDb *db, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL) {
        rules_error(err, err_size, "database is not open");
        return false;
    }
    static const char *sql =
        "SELECT COUNT(*) FROM categorization_rules r WHERE r.id<=0 OR r.revision<=0"
        " OR r.enabled NOT IN(0,1) OR r.sort_order NOT BETWEEN 0 AND 2147483647"
        " OR length(r.name) NOT BETWEEN 1 AND 64 OR r.name GLOB '*[^ -~]*'"
        " OR instr(CAST(r.name AS BLOB),x'00')>0"
        " OR r.payee_mode NOT BETWEEN 0 AND 2 OR length(r.payee_pattern)>256"
        " OR r.payee_pattern GLOB '*[^ -~]*'"
        " OR instr(CAST(r.payee_pattern AS BLOB),x'00')>0"
        " OR (r.payee_mode=0)<>(r.payee_pattern='')"
        " OR r.note_mode NOT BETWEEN 0 AND 2 OR length(r.note_pattern)>512"
        " OR r.note_pattern GLOB '*[^ -~]*' OR instr(CAST(r.note_pattern AS BLOB),x'00')>0"
        " OR (r.note_mode=0)<>(r.note_pattern='')"
        " OR r.minimum_amount_minor NOT BETWEEN -9000000000000000 AND 9000000000000000"
        " OR r.maximum_amount_minor NOT BETWEEN -9000000000000000 AND 9000000000000000"
        " OR (r.minimum_amount_minor IS NOT NULL AND r.maximum_amount_minor IS NOT NULL"
        "  AND r.minimum_amount_minor>r.maximum_amount_minor)"
        " OR NOT EXISTS(SELECT 1 FROM categories c WHERE c.id=r.category_id)"
        " OR (r.account_id IS NOT NULL AND NOT EXISTS(SELECT 1 FROM accounts a"
        "  WHERE a.id=r.account_id))"
        " OR (r.tag_id IS NOT NULL AND NOT EXISTS(SELECT 1 FROM tags t WHERE t.id=r.tag_id))";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_int64(stmt, 0) == 0 &&
              sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) rules_error(err, err_size, "categorization rules are inconsistent");
    (void)sqlite3_finalize(stmt);
    return ok;
}
