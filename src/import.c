#include "cmny_internal.h"
#include "cmny_import.h"
#include "cmny_rules.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    CmnyDb *db;
    const CmnyImportRequest *request;
    CmnyImportResult *result;
    int64_t batch_id;
    int64_t default_category_id;
    bool failed;
    bool validation_failed;
    char *err;
    size_t err_size;
} ApplyContext;

static void import_error(char *err, size_t err_size, const char *message) {
    if (err != NULL && err_size > 0) (void)snprintf(err, err_size, "%s", message);
}

static void import_db_error(CmnyDb *db, char *err, size_t err_size, const char *context) {
    if (err != NULL && err_size > 0) {
        (void)snprintf(err, err_size, "%s: %s", context,
                       db != NULL && db->handle != NULL ? sqlite3_errmsg(db->handle)
                                                       : "database is not open");
    }
}

static bool import_exec(CmnyDb *db, const char *sql, char *err, size_t err_size,
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

static bool mapping_column_valid(size_t column) {
    return column == CMNY_BANK_COLUMN_NONE || column < CMNY_CSV_MAX_FIELDS;
}

static bool profile_config_valid(const CmnyImportProfileConfig *config) {
    if (config == NULL ||
        (config->delimiter != '\0' && config->delimiter != ',' &&
         config->delimiter != ';' && config->delimiter != '\t') ||
        (config->date_format != CMNY_BANK_DATE_ISO &&
         config->date_format != CMNY_BANK_DATE_DMY &&
         config->date_format != CMNY_BANK_DATE_MDY) ||
        (config->sign_convention < CMNY_BANK_SIGNED_INFLOW_POSITIVE ||
         config->sign_convention > CMNY_BANK_UNSIGNED_OUTFLOW) ||
        (config->decimal_separator != '.' && config->decimal_separator != ',') ||
        (config->thousands_separator != '\0' && config->thousands_separator != '.' &&
         config->thousands_separator != ',' && config->thousands_separator != ' ' &&
         config->thousands_separator != '\'') ||
        config->decimal_separator == config->thousands_separator) return false;
    const CmnyBankCsvMapping *mapping = &config->mapping;
    if (!mapping_column_valid(mapping->date_column) ||
        !mapping_column_valid(mapping->amount_column) ||
        !mapping_column_valid(mapping->debit_column) ||
        !mapping_column_valid(mapping->credit_column) ||
        !mapping_column_valid(mapping->payee_column) ||
        !mapping_column_valid(mapping->note_column) ||
        !mapping_column_valid(mapping->external_id_column) ||
        mapping->date_column == CMNY_BANK_COLUMN_NONE) return false;
    bool amount_mode = mapping->amount_column != CMNY_BANK_COLUMN_NONE &&
                       mapping->debit_column == CMNY_BANK_COLUMN_NONE &&
                       mapping->credit_column == CMNY_BANK_COLUMN_NONE;
    bool split_mode = mapping->amount_column == CMNY_BANK_COLUMN_NONE &&
                      mapping->debit_column != CMNY_BANK_COLUMN_NONE &&
                      mapping->credit_column != CMNY_BANK_COLUMN_NONE;
    if (!amount_mode && !split_mode) return false;
    size_t columns[] = {
        mapping->date_column, mapping->amount_column, mapping->debit_column,
        mapping->credit_column, mapping->payee_column, mapping->note_column,
        mapping->external_id_column,
    };
    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++) {
        if (columns[i] == CMNY_BANK_COLUMN_NONE) continue;
        for (size_t j = i + 1; j < sizeof(columns) / sizeof(columns[0]); j++)
            if (columns[i] == columns[j]) return false;
    }
    return true;
}

static CmnyImportProfileConfig config_from_options(const CmnyBankCsvImportOptions *options) {
    return (CmnyImportProfileConfig){
        .mapping = options->mapping,
        .date_format = options->date_format,
        .sign_convention = options->sign_convention,
        .delimiter = options->source.delimiter,
        .decimal_separator = options->decimal_separator,
        .thousands_separator = options->thousands_separator,
    };
}

static bool source_options_valid(const CmnyBankCsvImportOptions *options) {
    if (options == NULL) return false;
    CmnyImportProfileConfig config = config_from_options(options);
    if (!profile_config_valid(&config)) return false;
    const CmnyCsvLimits *limits = &options->source.limits;
    return limits->max_bytes > 0 && limits->max_records > 0 && limits->max_fields > 0 &&
           limits->max_fields <= CMNY_CSV_MAX_FIELDS && limits->max_field_bytes > 0 &&
           limits->max_field_bytes < CMNY_CSV_FIELD_CAP;
}

static sqlite3_int64 encode_column(size_t column) {
    return column == CMNY_BANK_COLUMN_NONE ? -1 : (sqlite3_int64)column;
}

static size_t decode_column(sqlite3_int64 column) {
    return column < 0 ? CMNY_BANK_COLUMN_NONE : (size_t)column;
}

static bool bind_profile(sqlite3_stmt *stmt, const CmnyImportProfileDraft *draft,
                         int start) {
    const CmnyImportProfileConfig *config = &draft->config;
    int rc = sqlite3_bind_text(stmt, start, draft->name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, start + 1, (unsigned char)config->delimiter);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, start + 2,
                                                  encode_column(config->mapping.date_column));
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, start + 3,
                                                  encode_column(config->mapping.amount_column));
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, start + 4,
                                                  encode_column(config->mapping.debit_column));
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, start + 5,
                                                  encode_column(config->mapping.credit_column));
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, start + 6,
                                                  encode_column(config->mapping.payee_column));
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, start + 7,
                                                  encode_column(config->mapping.note_column));
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, start + 8,
                                                  encode_column(config->mapping.external_id_column));
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, start + 9, (int)config->date_format);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, start + 10, (int)config->sign_convention);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt, start + 11, (unsigned char)config->decimal_separator);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt, start + 12, (unsigned char)config->thousands_separator);
    return rc == SQLITE_OK;
}

bool cmny_import_profile_create(CmnyDb *db, const CmnyImportProfileDraft *draft,
                                int64_t *new_id, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || draft == NULL ||
        !cmny_text_valid(draft->name, CMNY_IMPORT_PROFILE_NAME_MAX, false) ||
        !profile_config_valid(&draft->config)) {
        import_error(err, err_size, "invalid bank import profile");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "INSERT INTO import_profiles(name,delimiter,date_column,amount_column,debit_column,"
        "credit_column,payee_column,note_column,external_id_column,date_format,"
        "sign_convention,decimal_separator,thousands_separator)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt, NULL);
    bool ok = rc == SQLITE_OK && bind_profile(stmt, draft, 1);
    if (ok) {
        rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
    }
    if (ok && new_id != NULL) *new_id = sqlite3_last_insert_rowid(db->handle);
    if (!ok) import_db_error(db, err, err_size, "cannot create bank import profile");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool read_profile(sqlite3_stmt *stmt, CmnyImportProfile *out) {
    *out = (CmnyImportProfile){0};
    out->id = sqlite3_column_int64(stmt, 0);
    out->revision = sqlite3_column_int64(stmt, 1);
    out->created_at = sqlite3_column_int64(stmt, 2);
    out->updated_at = sqlite3_column_int64(stmt, 3);
    out->config.mapping.date_column = decode_column(sqlite3_column_int64(stmt, 4));
    out->config.mapping.amount_column = decode_column(sqlite3_column_int64(stmt, 5));
    out->config.mapping.debit_column = decode_column(sqlite3_column_int64(stmt, 6));
    out->config.mapping.credit_column = decode_column(sqlite3_column_int64(stmt, 7));
    out->config.mapping.payee_column = decode_column(sqlite3_column_int64(stmt, 8));
    out->config.mapping.note_column = decode_column(sqlite3_column_int64(stmt, 9));
    out->config.mapping.external_id_column = decode_column(sqlite3_column_int64(stmt, 10));
    out->config.date_format = (CmnyBankDateFormat)sqlite3_column_int(stmt, 11);
    out->config.sign_convention = (CmnyBankSignConvention)sqlite3_column_int(stmt, 12);
    out->config.delimiter = (char)sqlite3_column_int(stmt, 13);
    out->config.decimal_separator = (char)sqlite3_column_int(stmt, 14);
    out->config.thousands_separator = (char)sqlite3_column_int(stmt, 15);
    return copy_text(stmt, 16, out->name, sizeof(out->name));
}

static const char *profile_select(void) {
    return "SELECT id,revision,created_at,updated_at,date_column,amount_column,debit_column,"
           "credit_column,payee_column,note_column,external_id_column,date_format,"
           "sign_convention,delimiter,decimal_separator,thousands_separator,name"
           " FROM import_profiles";
}

bool cmny_import_profile_get(CmnyDb *db, int64_t id, CmnyImportProfile *out,
                             char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || id <= 0 || out == NULL) {
        import_error(err, err_size, "invalid bank import profile lookup");
        return false;
    }
    char sql[512];
    (void)snprintf(sql, sizeof(sql), "%s WHERE id=?", profile_select());
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && read_profile(stmt, out) &&
              profile_config_valid(&out->config) && sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) import_error(err, err_size, rc == SQLITE_DONE
        ? "bank import profile does not exist" : "cannot read bank import profile");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_import_profile_update(CmnyDb *db, int64_t id, int64_t expected_revision,
                                const CmnyImportProfileDraft *draft,
                                char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || id <= 0 || expected_revision <= 0 ||
        draft == NULL || !cmny_text_valid(draft->name, CMNY_IMPORT_PROFILE_NAME_MAX, false) ||
        !profile_config_valid(&draft->config)) {
        import_error(err, err_size, "invalid bank import profile update");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "UPDATE import_profiles SET name=?,delimiter=?,date_column=?,amount_column=?,"
        "debit_column=?,credit_column=?,payee_column=?,note_column=?,external_id_column=?,"
        "date_format=?,sign_convention=?,decimal_separator=?,thousands_separator=?,"
        "revision=revision+1,updated_at=CAST(strftime('%s','now') AS INTEGER)"
        " WHERE id=? AND revision=?", -1, &stmt, NULL);
    bool ok = rc == SQLITE_OK && bind_profile(stmt, draft, 1);
    if (ok) rc = sqlite3_bind_int64(stmt, 14, id);
    if (rc == SQLITE_OK && ok) rc = sqlite3_bind_int64(stmt, 15, expected_revision);
    if (rc == SQLITE_OK && ok) rc = sqlite3_step(stmt);
    ok = ok && rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) import_error(err, err_size, rc == SQLITE_DONE
        ? "bank import profile changed; reload and retry" : "cannot update bank import profile");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_import_profile_delete(CmnyDb *db, int64_t id, int64_t expected_revision,
                                char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || id <= 0 || expected_revision <= 0) {
        import_error(err, err_size, "invalid bank import profile deletion");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "DELETE FROM import_profiles WHERE id=? AND revision=?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, expected_revision);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
    if (!ok) import_error(err, err_size, rc == SQLITE_DONE
        ? "bank import profile changed; reload and retry" : "cannot delete bank import profile");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_import_profile_list(CmnyDb *db, size_t offset, CmnyImportProfile *out,
                              size_t cap, size_t *count, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || out == NULL || count == NULL ||
        cap > CMNY_IMPORT_PROFILE_LIST_LIMIT) {
        import_error(err, err_size, "invalid bank import profile list");
        return false;
    }
    char sql[640];
    (void)snprintf(sql, sizeof(sql), "%s ORDER BY name COLLATE NOCASE,id LIMIT ? OFFSET ?",
                   profile_select());
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)cap);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)offset);
    if (rc != SQLITE_OK) {
        import_db_error(db, err, err_size, "cannot prepare bank import profile list");
        *count = 0;
        (void)sqlite3_finalize(stmt);
        return false;
    }
    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap) {
        if (!read_profile(stmt, &out[used]) || !profile_config_valid(&out[used].config)) {
            rc = SQLITE_CORRUPT;
            break;
        }
        used++;
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) import_db_error(db, err, err_size, "cannot list bank import profiles");
    *count = ok ? used : 0;
    (void)sqlite3_finalize(stmt);
    return ok;
}

void cmny_import_profile_options(const CmnyImportProfile *profile,
                                 CmnyBankCsvImportOptions *options) {
    if (profile == NULL || options == NULL) return;
    cmny_bank_csv_import_options_default(options);
    options->source.delimiter = profile->config.delimiter;
    options->mapping = profile->config.mapping;
    options->date_format = profile->config.date_format;
    options->sign_convention = profile->config.sign_convention;
    options->decimal_separator = profile->config.decimal_separator;
    options->thousands_separator = profile->config.thousands_separator;
}

static bool request_valid(const CmnyImportRequest *request) {
    return request != NULL && request->reader != NULL && request->account_id > 0 &&
           request->profile_id >= 0 &&
           (request->heuristic_policy == CMNY_IMPORT_HEURISTIC_SKIP ||
            request->heuristic_policy == CMNY_IMPORT_HEURISTIC_ALLOW) &&
           cmny_text_valid(request->source_name, CMNY_IMPORT_SOURCE_MAX, false) &&
           source_options_valid(request->options);
}

static bool request_references_available(CmnyDb *db, const CmnyImportRequest *request,
                                         char *err, size_t err_size) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle,
        "SELECT EXISTS(SELECT 1 FROM accounts a JOIN settings s ON s.key='currency'"
        " WHERE a.id=? AND a.archived=0 AND a.currency_code=s.value),"
        "(?=0 OR EXISTS(SELECT 1 FROM import_profiles WHERE id=?))", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, request->account_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, request->profile_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, request->profile_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0 &&
              sqlite3_column_int(stmt, 1) != 0 && sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) import_error(err, err_size, rc == SQLITE_ROW || rc == SQLITE_DONE
        ? "import account or profile is unavailable" : "cannot validate import ownership");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool insert_batch(ApplyContext *context) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(context->db->handle,
        "INSERT INTO import_batches(account_id,profile_id,source_name,delimiter,heuristic_policy)"
        " VALUES(?,?,?,?,?)", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, context->request->account_id);
    if (rc == SQLITE_OK) {
        rc = context->request->profile_id == 0 ? sqlite3_bind_null(stmt, 2)
             : sqlite3_bind_int64(stmt, 2, context->request->profile_id);
    }
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt, 3, context->request->source_name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt, 4, (unsigned char)context->request->options->source.delimiter);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 5, (int)context->request->heuristic_policy);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (ok) context->batch_id = sqlite3_last_insert_rowid(context->db->handle);
    if (!ok) import_db_error(context->db, context->err, context->err_size,
                             "cannot start bank import batch");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool row_domain_valid(const CmnyBankCsvRow *row) {
    return row != NULL && row->amount_cents != 0 && row->amount_cents >= -CMNY_AMOUNT_MAX &&
           row->amount_cents <= CMNY_AMOUNT_MAX && fixed_text(row->date, sizeof(row->date)) &&
           cmny_date_valid(row->date) && fixed_text(row->payee, sizeof(row->payee)) &&
           cmny_text_valid(row->payee, CMNY_PAYEE_MAX, true) &&
           fixed_text(row->note, sizeof(row->note)) &&
           cmny_text_valid(row->note, CMNY_LEDGER_NOTE_MAX, true) &&
           fixed_text(row->external_id, sizeof(row->external_id)) &&
           fixed_text(row->identity, sizeof(row->identity)) && row->identity[0] != '\0' &&
           row->physical_line > 0 && row->record_number > 0 &&
           row->physical_line <= (size_t)INT64_MAX && row->record_number <= (size_t)INT64_MAX;
}

static bool find_duplicate(CmnyDb *db, int64_t account_id, const char *column,
                           const char *value, int64_t *record_id,
                           char *err, size_t err_size) {
    char sql[256];
    int written = snprintf(sql, sizeof(sql),
        "SELECT id FROM import_records WHERE account_id=? AND decision=1"
        " AND dedupe_active=1 AND %s=? ORDER BY id LIMIT 1", column);
    if (written < 0 || (size_t)written >= sizeof(sql)) {
        import_error(err, err_size, "cannot prepare duplicate lookup");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, account_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW || rc == SQLITE_DONE;
    *record_id = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
    if (rc == SQLITE_ROW) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) import_db_error(db, err, err_size, "cannot inspect bank import duplicates");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool insert_record(ApplyContext *context, const CmnyBankCsvRow *row,
                          CmnyImportRecordDecision decision, int64_t entry_id,
                          int64_t duplicate_of, const CmnyRuleMatch *match,
                          int64_t category_id, bool heuristic_duplicate) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(context->db->handle,
        "INSERT INTO import_records(batch_id,account_id,entry_id,duplicate_of_record_id,"
        "rule_id,category_id,tag_id,decision,dedupe_active,heuristic_duplicate,occurred_on,"
        "amount_minor,payee,note,external_id,identity,physical_line,record_number)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, context->batch_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, context->request->account_id);
    if (rc == SQLITE_OK)
        rc = entry_id == 0 ? sqlite3_bind_null(stmt, 3) : sqlite3_bind_int64(stmt, 3, entry_id);
    if (rc == SQLITE_OK) rc = duplicate_of == 0 ? sqlite3_bind_null(stmt, 4)
                                                 : sqlite3_bind_int64(stmt, 4, duplicate_of);
    if (rc == SQLITE_OK) rc = match == NULL || !match->matched ? sqlite3_bind_null(stmt, 5)
                                                               : sqlite3_bind_int64(stmt, 5, match->rule_id);
    if (rc == SQLITE_OK) rc = category_id == 0 ? sqlite3_bind_null(stmt, 6)
                                                : sqlite3_bind_int64(stmt, 6, category_id);
    if (rc == SQLITE_OK) rc = match == NULL || match->tag_id == 0 ? sqlite3_bind_null(stmt, 7)
                                                                  : sqlite3_bind_int64(stmt, 7, match->tag_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 8, (int)decision);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 9,
                                                decision == CMNY_IMPORT_RECORD_IMPORTED ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 10, heuristic_duplicate ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 11, row->date, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 12, row->amount_cents);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 13, row->payee, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 14, row->note, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 15, row->external_id, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 16, row->identity, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 17, (sqlite3_int64)row->physical_line);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 18, (sqlite3_int64)row->record_number);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE;
    if (!ok) import_db_error(context->db, context->err, context->err_size,
                             "cannot record bank import row");
    (void)sqlite3_finalize(stmt);
    return ok;
}

static CmnyBankCsvAction apply_row(const CmnyBankCsvRow *row, void *opaque) {
    ApplyContext *context = opaque;
    if (!row_domain_valid(row)) {
        import_error(context->err, context->err_size,
                     "normalized bank row is outside the ledger domain");
        context->failed = true;
        return CMNY_BANK_ERROR;
    }
    int64_t hard_duplicate = 0;
    if (row->external_id[0] != '\0' &&
        !find_duplicate(context->db, context->request->account_id, "external_id",
                        row->external_id, &hard_duplicate, context->err, context->err_size)) {
        context->failed = true;
        return CMNY_BANK_ERROR;
    }
    if (hard_duplicate != 0) {
        if (!insert_record(context, row, CMNY_IMPORT_RECORD_HARD_DUPLICATE, 0,
                           hard_duplicate, NULL, 0, false)) {
            context->failed = true;
            return CMNY_BANK_ERROR;
        }
        context->result->hard_duplicates++;
        return CMNY_BANK_CONTINUE;
    }
    int64_t heuristic_anchor = 0;
    if (!find_duplicate(context->db, context->request->account_id, "identity",
                        row->identity, &heuristic_anchor, context->err, context->err_size)) {
        context->failed = true;
        return CMNY_BANK_ERROR;
    }
    bool heuristic_duplicate = heuristic_anchor != 0;
    if (heuristic_duplicate) context->result->heuristic_duplicates++;
    if (heuristic_duplicate &&
        context->request->heuristic_policy == CMNY_IMPORT_HEURISTIC_SKIP) {
        if (!insert_record(context, row, CMNY_IMPORT_RECORD_HEURISTIC_SKIPPED, 0,
                           heuristic_anchor, NULL, 0, true)) {
            context->failed = true;
            return CMNY_BANK_ERROR;
        }
        context->result->heuristic_skipped++;
        return CMNY_BANK_CONTINUE;
    }

    CmnyRuleMatch match = {0};
    if (!cmny_rule_match(context->db, context->request->account_id, row,
                         &match, context->err, context->err_size)) {
        context->failed = true;
        return CMNY_BANK_ERROR;
    }
    int64_t category_id = match.matched ? match.category_id : context->default_category_id;
    if (category_id == 0) {
        if (!cmny_internal_category_ensure(context->db, "Uncategorized", CMNY_CATEGORY_BOTH,
                                           &context->default_category_id,
                                           context->err, context->err_size)) {
            context->failed = true;
            return CMNY_BANK_ERROR;
        }
        category_id = context->default_category_id;
    }
    CmnySplitDraft split = {
        .category_id = category_id,
        .amount_minor = row->amount_cents,
        .note = "",
    };
    int64_t tag_id = match.tag_id;
    CmnyNormalEntryDraft draft = {
        .account_id = context->request->account_id,
        .amount_minor = row->amount_cents,
        .split_count = 1,
        .tag_count = tag_id == 0 ? 0U : 1U,
        .occurred_on = row->date,
        .payee = row->payee,
        .note = row->note,
        .splits = &split,
        .tag_ids = tag_id == 0 ? NULL : &tag_id,
    };
    int64_t entry_id = 0;
    if (!cmny_entry_create_normal(context->db, &draft, &entry_id,
                                  context->err, context->err_size) ||
        !insert_record(context, row, CMNY_IMPORT_RECORD_IMPORTED, entry_id,
                       heuristic_anchor, &match, category_id, heuristic_duplicate)) {
        context->failed = true;
        return CMNY_BANK_ERROR;
    }
    context->result->imported_rows++;
    return CMNY_BANK_CONTINUE;
}

static void capture_diagnostic(const CmnyBankCsvDiagnostic *diagnostic, void *opaque) {
    ApplyContext *context = opaque;
    context->validation_failed = true;
    if (context->result->diagnostic_count >= CMNY_IMPORT_DIAGNOSTIC_LIMIT) {
        context->result->diagnostics_truncated = true;
        return;
    }
    CmnyImportDiagnostic *copy =
        &context->result->diagnostics[context->result->diagnostic_count++];
    copy->code = diagnostic->code;
    copy->physical_line = diagnostic->physical_line;
    copy->record_number = diagnostic->record_number;
    copy->column = diagnostic->column;
    (void)snprintf(copy->message, sizeof(copy->message), "%s",
                   diagnostic->message != NULL ? diagnostic->message : "invalid bank row");
}

static bool finish_batch(ApplyContext *context, const CmnyBankCsvSummary *summary) {
    if (summary->input_rows > (size_t)INT64_MAX || summary->normalized_rows > (size_t)INT64_MAX ||
        context->result->imported_rows > (size_t)INT64_MAX ||
        context->result->hard_duplicates > (size_t)INT64_MAX ||
        context->result->heuristic_duplicates > (size_t)INT64_MAX ||
        context->result->heuristic_skipped > (size_t)INT64_MAX) {
        import_error(context->err, context->err_size, "bank import counters exceed SQLite limits");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(context->db->handle,
        "UPDATE import_batches SET delimiter=?,input_rows=?,normalized_rows=?,imported_rows=?,"
        "hard_duplicates=?,heuristic_duplicates=?,heuristic_skipped=?,"
        "updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE id=? AND status=1",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 1, (unsigned char)summary->delimiter);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)summary->input_rows);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, (sqlite3_int64)summary->normalized_rows);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt, 4, (sqlite3_int64)context->result->imported_rows);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt, 5, (sqlite3_int64)context->result->hard_duplicates);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt, 6, (sqlite3_int64)context->result->heuristic_duplicates);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt, 7, (sqlite3_int64)context->result->heuristic_skipped);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 8, context->batch_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(context->db->handle) == 1;
    if (!ok) import_db_error(context->db, context->err, context->err_size,
                             "cannot finalize bank import batch");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_import_apply(CmnyDb *db, const CmnyImportRequest *request,
                       CmnyImportResult *result, char *err, size_t err_size) {
    if (err != NULL && err_size > 0) err[0] = '\0';
    if (result != NULL) *result = (CmnyImportResult){0};
    if (db == NULL || db->handle == NULL || result == NULL || !request_valid(request) ||
        sqlite3_get_autocommit(db->handle) == 0) {
        import_error(err, err_size, "invalid atomic bank import request");
        return false;
    }
    if (!import_exec(db, "BEGIN IMMEDIATE", err, err_size, "cannot begin bank import"))
        return false;
    ApplyContext context = {
        .db = db,
        .request = request,
        .result = result,
        .err = err,
        .err_size = err_size,
    };
    bool ok = request_references_available(db, request, err, err_size) &&
              insert_batch(&context);
    CmnyBankCsvSummary summary = {0};
    CmnyBankCsvError bank_error = {0};
    if (ok) {
        ok = cmny_bank_csv_normalize(request->reader, request->reader_context,
                                     request->options, apply_row, &context,
                                     capture_diagnostic, &context,
                                     request->cancel, request->cancel_context,
                                     &summary, &bank_error);
    }
    if (!ok && !context.failed && err != NULL && err_size > 0 && err[0] == '\0') {
        import_error(err, err_size, bank_error.message != NULL
            ? bank_error.message : "bank CSV normalization failed");
    }
    if (ok && context.validation_failed) {
        import_error(err, err_size, "bank CSV contains invalid rows");
        ok = false;
    }
    if (ok && request->cancel != NULL && request->cancel(request->cancel_context)) {
        import_error(err, err_size, "bank import was cancelled");
        ok = false;
    }
    result->input_rows = summary.input_rows;
    result->normalized_rows = summary.normalized_rows;
    if (ok) ok = finish_batch(&context, &summary);
    if (ok) ok = import_exec(db, "COMMIT", err, err_size, "cannot commit bank import");
    if (!ok) {
        (void)sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
        result->batch_id = 0;
        return false;
    }
    result->batch_id = context.batch_id;
    return true;
}

static bool read_batch(sqlite3_stmt *stmt, CmnyImportBatch *out) {
    *out = (CmnyImportBatch){0};
    out->id = sqlite3_column_int64(stmt, 0);
    out->account_id = sqlite3_column_int64(stmt, 1);
    out->profile_id = sqlite3_column_type(stmt, 2) == SQLITE_NULL
        ? 0 : sqlite3_column_int64(stmt, 2);
    out->revision = sqlite3_column_int64(stmt, 3);
    out->input_rows = sqlite3_column_int64(stmt, 4);
    out->normalized_rows = sqlite3_column_int64(stmt, 5);
    out->imported_rows = sqlite3_column_int64(stmt, 6);
    out->hard_duplicates = sqlite3_column_int64(stmt, 7);
    out->heuristic_duplicates = sqlite3_column_int64(stmt, 8);
    out->heuristic_skipped = sqlite3_column_int64(stmt, 9);
    out->created_at = sqlite3_column_int64(stmt, 10);
    out->rolled_back_at = sqlite3_column_type(stmt, 11) == SQLITE_NULL
        ? 0 : sqlite3_column_int64(stmt, 11);
    out->heuristic_policy = (CmnyImportHeuristicPolicy)sqlite3_column_int(stmt, 12);
    out->status = (CmnyImportBatchStatus)sqlite3_column_int(stmt, 13);
    out->delimiter = (char)sqlite3_column_int(stmt, 14);
    return copy_text(stmt, 15, out->source_name, sizeof(out->source_name));
}

static const char *batch_select(void) {
    return "SELECT id,account_id,profile_id,revision,input_rows,normalized_rows,imported_rows,"
           "hard_duplicates,heuristic_duplicates,heuristic_skipped,created_at,rolled_back_at,"
           "heuristic_policy,status,delimiter,source_name FROM import_batches";
}

bool cmny_import_batch_get(CmnyDb *db, int64_t id, CmnyImportBatch *out,
                           char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || id <= 0 || out == NULL) {
        import_error(err, err_size, "invalid bank import batch lookup");
        return false;
    }
    char sql[512];
    (void)snprintf(sql, sizeof(sql), "%s WHERE id=?", batch_select());
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool ok = rc == SQLITE_ROW && read_batch(stmt, out) &&
              sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) import_error(err, err_size, rc == SQLITE_DONE
        ? "bank import batch does not exist" : "cannot read bank import batch");
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_import_batch_list(CmnyDb *db, int64_t account_id, size_t offset,
                            CmnyImportBatch *out, size_t cap, size_t *count,
                            char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || account_id < 0 || out == NULL || count == NULL ||
        cap > CMNY_IMPORT_BATCH_LIST_LIMIT) {
        import_error(err, err_size, "invalid bank import batch list");
        return false;
    }
    char sql[640];
    (void)snprintf(sql, sizeof(sql),
                   "%s WHERE (?=0 OR account_id=?) ORDER BY id DESC LIMIT ? OFFSET ?",
                   batch_select());
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, account_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, account_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, (sqlite3_int64)cap);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 4, (sqlite3_int64)offset);
    if (rc != SQLITE_OK) {
        import_db_error(db, err, err_size, "cannot prepare bank import batch list");
        *count = 0;
        (void)sqlite3_finalize(stmt);
        return false;
    }
    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap) {
        if (!read_batch(stmt, &out[used])) {
            rc = SQLITE_CORRUPT;
            break;
        }
        used++;
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) import_db_error(db, err, err_size, "cannot list bank import batches");
    *count = ok ? used : 0;
    (void)sqlite3_finalize(stmt);
    return ok;
}

static bool read_record(sqlite3_stmt *stmt, CmnyImportRecord *out) {
    *out = (CmnyImportRecord){0};
    out->id = sqlite3_column_int64(stmt, 0);
    out->batch_id = sqlite3_column_int64(stmt, 1);
    out->account_id = sqlite3_column_int64(stmt, 2);
    out->entry_id = sqlite3_column_type(stmt, 3) == SQLITE_NULL
        ? 0 : sqlite3_column_int64(stmt, 3);
    out->duplicate_of_record_id = sqlite3_column_type(stmt, 4) == SQLITE_NULL
        ? 0 : sqlite3_column_int64(stmt, 4);
    out->rule_id = sqlite3_column_type(stmt, 5) == SQLITE_NULL
        ? 0 : sqlite3_column_int64(stmt, 5);
    out->category_id = sqlite3_column_type(stmt, 6) == SQLITE_NULL
        ? 0 : sqlite3_column_int64(stmt, 6);
    out->tag_id = sqlite3_column_type(stmt, 7) == SQLITE_NULL
        ? 0 : sqlite3_column_int64(stmt, 7);
    out->amount_minor = sqlite3_column_int64(stmt, 8);
    sqlite3_int64 physical_line = sqlite3_column_int64(stmt, 9);
    sqlite3_int64 record_number = sqlite3_column_int64(stmt, 10);
    if (physical_line <= 0 || record_number <= 0) return false;
    out->physical_line = (size_t)physical_line;
    out->record_number = (size_t)record_number;
    out->decision = (CmnyImportRecordDecision)sqlite3_column_int(stmt, 11);
    out->dedupe_active = sqlite3_column_int(stmt, 12) != 0;
    out->heuristic_duplicate = sqlite3_column_int(stmt, 13) != 0;
    return copy_text(stmt, 14, out->occurred_on, sizeof(out->occurred_on)) &&
           copy_text(stmt, 15, out->payee, sizeof(out->payee)) &&
           copy_text(stmt, 16, out->note, sizeof(out->note)) &&
           copy_text(stmt, 17, out->external_id, sizeof(out->external_id)) &&
           copy_text(stmt, 18, out->identity, sizeof(out->identity));
}

bool cmny_import_record_list(CmnyDb *db, int64_t batch_id, size_t offset,
                             CmnyImportRecord *out, size_t cap, size_t *count,
                             char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || batch_id <= 0 || out == NULL || count == NULL ||
        cap > CMNY_IMPORT_RECORD_LIST_LIMIT) {
        import_error(err, err_size, "invalid bank import record list");
        return false;
    }
    static const char *sql =
        "SELECT id,batch_id,account_id,entry_id,duplicate_of_record_id,rule_id,category_id,"
        "tag_id,amount_minor,physical_line,record_number,decision,dedupe_active,"
        "heuristic_duplicate,occurred_on,payee,note,external_id,identity"
        " FROM import_records WHERE batch_id=? ORDER BY id LIMIT ? OFFSET ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, batch_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)cap);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, (sqlite3_int64)offset);
    if (rc != SQLITE_OK) {
        import_db_error(db, err, err_size, "cannot prepare bank import record list");
        *count = 0;
        (void)sqlite3_finalize(stmt);
        return false;
    }
    size_t used = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < cap) {
        if (!read_record(stmt, &out[used])) {
            rc = SQLITE_CORRUPT;
            break;
        }
        used++;
    }
    bool ok = rc == SQLITE_DONE;
    if (!ok) import_db_error(db, err, err_size, "cannot list bank import records");
    *count = ok ? used : 0;
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_import_batch_rollback(CmnyDb *db, int64_t batch_id,
                                int64_t expected_revision,
                                char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL || batch_id <= 0 || expected_revision <= 0 ||
        sqlite3_get_autocommit(db->handle) == 0) {
        import_error(err, err_size, "invalid bank import rollback");
        return false;
    }
    if (!import_exec(db, "BEGIN IMMEDIATE", err, err_size,
                     "cannot begin bank import rollback")) return false;
    CmnyImportBatch batch = {0};
    bool ok = cmny_import_batch_get(db, batch_id, &batch, err, err_size) &&
              batch.status == CMNY_IMPORT_BATCH_APPLIED &&
              batch.revision == expected_revision;
    if (!ok && err != NULL && err_size > 0 && err[0] == '\0')
        import_error(err, err_size, "bank import batch changed; reload and retry");
    int64_t last_record_id = 0;
    while (ok) {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(db->handle,
            "SELECT r.id,e.id,e.revision,e.entry_type FROM import_records r"
            " JOIN entries e ON e.id=r.entry_id WHERE r.batch_id=? AND r.decision=1"
            " AND r.id>? AND e.voided_at IS NULL ORDER BY r.id LIMIT 1", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, batch_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, last_record_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        bool done = rc == SQLITE_DONE;
        int64_t entry_id = 0;
        int64_t revision = 0;
        if (rc == SQLITE_ROW) {
            last_record_id = sqlite3_column_int64(stmt, 0);
            entry_id = sqlite3_column_int64(stmt, 1);
            revision = sqlite3_column_int64(stmt, 2);
            ok = sqlite3_column_int(stmt, 3) == CMNY_ENTRY_NORMAL;
        } else if (!done) {
            ok = false;
        }
        (void)sqlite3_finalize(stmt);
        if (!ok || done) break;
        ok = cmny_entry_delete(db, entry_id, revision, err, err_size);
    }
    sqlite3_stmt *stmt = NULL;
    int rc = SQLITE_OK;
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "UPDATE import_records SET dedupe_active=0 WHERE batch_id=?", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, batch_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (ok) {
        rc = sqlite3_prepare_v2(db->handle,
            "UPDATE import_batches SET status=2,revision=revision+1,"
            "updated_at=CAST(strftime('%s','now') AS INTEGER),"
            "rolled_back_at=CAST(strftime('%s','now') AS INTEGER)"
            " WHERE id=? AND status=1 AND revision=?", -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, batch_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, expected_revision);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        ok = rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1;
        (void)sqlite3_finalize(stmt);
    }
    if (!ok && err != NULL && err_size > 0 && err[0] == '\0')
        import_error(err, err_size, "cannot roll back bank import batch");
    if (ok) ok = import_exec(db, "COMMIT", err, err_size,
                             "cannot commit bank import rollback");
    if (!ok) (void)sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
    return ok;
}

static bool profiles_integrity(CmnyDb *db) {
    char sql[512];
    (void)snprintf(sql, sizeof(sql), "%s ORDER BY id", profile_select());
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            CmnyImportProfile profile = {0};
            if (!read_profile(stmt, &profile) || profile.id <= 0 || profile.revision <= 0 ||
                !cmny_text_valid(profile.name, CMNY_IMPORT_PROFILE_NAME_MAX, false) ||
                !profile_config_valid(&profile.config)) {
                rc = SQLITE_CORRUPT;
                break;
            }
        }
    }
    bool ok = rc == SQLITE_DONE;
    (void)sqlite3_finalize(stmt);
    return ok;
}

bool cmny_import_integrity_check(CmnyDb *db, char *err, size_t err_size) {
    if (db == NULL || db->handle == NULL) {
        import_error(err, err_size, "database is not open");
        return false;
    }
    if (!profiles_integrity(db)) {
        import_error(err, err_size, "bank import profiles are inconsistent");
        return false;
    }
    static const char *batch_sql =
        "SELECT COUNT(*) FROM import_batches b WHERE b.id<=0 OR b.account_id<=0"
        " OR b.revision<=0 OR b.delimiter NOT IN(9,44,59)"
        " OR b.heuristic_policy NOT IN(1,2) OR b.status NOT IN(1,2)"
        " OR length(b.source_name) NOT BETWEEN 1 AND 255"
        " OR b.source_name GLOB '*[^ -~]*' OR instr(CAST(b.source_name AS BLOB),x'00')>0"
        " OR b.input_rows<>b.normalized_rows"
        " OR b.normalized_rows<>b.imported_rows+b.hard_duplicates+b.heuristic_skipped"
        " OR b.heuristic_skipped>b.heuristic_duplicates"
        " OR b.heuristic_duplicates>b.imported_rows+b.heuristic_skipped"
        " OR ((b.status=2)<>(b.rolled_back_at IS NOT NULL))"
        " OR b.normalized_rows<>(SELECT COUNT(*) FROM import_records r WHERE r.batch_id=b.id)"
        " OR b.imported_rows<>(SELECT COUNT(*) FROM import_records r"
        "  WHERE r.batch_id=b.id AND r.decision=1)"
        " OR b.hard_duplicates<>(SELECT COUNT(*) FROM import_records r"
        "  WHERE r.batch_id=b.id AND r.decision=2)"
        " OR b.heuristic_skipped<>(SELECT COUNT(*) FROM import_records r"
        "  WHERE r.batch_id=b.id AND r.decision=3)"
        " OR b.heuristic_duplicates<>(SELECT COUNT(*) FROM import_records r"
        "  WHERE r.batch_id=b.id AND r.heuristic_duplicate=1)";
    static const char *record_sql =
        "SELECT COUNT(*) FROM import_records r JOIN import_batches b ON b.id=r.batch_id"
        " WHERE r.id<=0 OR r.account_id<>b.account_id OR r.decision NOT BETWEEN 1 AND 3"
        " OR r.dedupe_active NOT IN(0,1) OR r.heuristic_duplicate NOT IN(0,1)"
        " OR (b.status=1 AND r.decision=1 AND r.dedupe_active<>1)"
        " OR (b.status=2 AND r.dedupe_active<>0)"
        " OR (r.decision<>1 AND r.dedupe_active<>0)"
        " OR r.amount_minor=0 OR r.amount_minor NOT BETWEEN"
        "  -9000000000000000 AND 9000000000000000"
        " OR r.occurred_on NOT GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'"
        " OR date(r.occurred_on,'+0 days')<>r.occurred_on"
        " OR length(r.payee)>256 OR length(r.note)>512 OR length(r.external_id)>128"
        " OR length(r.identity) NOT BETWEEN 1 AND 1099"
        " OR instr(CAST(r.occurred_on AS BLOB),x'00')>0"
        " OR instr(CAST(r.payee AS BLOB),x'00')>0 OR instr(CAST(r.note AS BLOB),x'00')>0"
        " OR instr(CAST(r.external_id AS BLOB),x'00')>0"
        " OR instr(CAST(r.identity AS BLOB),x'00')>0"
        " OR r.physical_line<=0 OR r.record_number<=0"
        " OR (r.decision=1 AND (r.entry_id IS NULL OR r.category_id IS NULL"
        "  OR NOT EXISTS(SELECT 1 FROM entries e WHERE e.id=r.entry_id AND e.entry_type=1)))"
        " OR (r.decision IN(2,3) AND (r.entry_id IS NOT NULL OR r.category_id IS NOT NULL"
        "  OR r.tag_id IS NOT NULL))"
        " OR (r.decision=2 AND (r.external_id='' OR r.duplicate_of_record_id IS NULL"
        "  OR NOT EXISTS(SELECT 1 FROM import_records d WHERE d.id=r.duplicate_of_record_id"
        "   AND d.decision=1 AND d.account_id=r.account_id AND d.external_id=r.external_id)))"
        " OR (r.heuristic_duplicate=1 AND (r.duplicate_of_record_id IS NULL"
        "  OR NOT EXISTS(SELECT 1 FROM import_records d WHERE d.id=r.duplicate_of_record_id"
        "   AND d.decision=1 AND d.account_id=r.account_id AND d.identity=r.identity)))"
        " OR (r.heuristic_duplicate=0 AND r.decision<>2 AND r.duplicate_of_record_id IS NOT NULL)"
        " OR (r.decision=3 AND r.heuristic_duplicate<>1)";
    const char *queries[] = {batch_sql, record_sql};
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(db->handle, queries[i], -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        bool ok = rc == SQLITE_ROW && sqlite3_column_int64(stmt, 0) == 0 &&
                  sqlite3_step(stmt) == SQLITE_DONE;
        (void)sqlite3_finalize(stmt);
        if (!ok) {
            import_error(err, err_size, "bank import data is inconsistent");
            return false;
        }
    }
    return true;
}
