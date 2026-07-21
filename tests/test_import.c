#include "cmny_history.h"
#include "cmny_import.h"
#include "cmny_rules.h"
#include "test.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

_Static_assert(_Alignof(CmnyImportProfile) >= _Alignof(int64_t),
               "import profiles must preserve int64 alignment");
_Static_assert(_Alignof(CmnyImportBatch) >= _Alignof(int64_t),
               "import batches must preserve int64 alignment");
_Static_assert(offsetof(CmnyImportBatch, status) > offsetof(CmnyImportBatch, rolled_back_at),
               "narrow batch fields belong after int64 fields");
_Static_assert(offsetof(CmnyImportRecord, decision) > offsetof(CmnyImportRecord, record_number),
               "narrow record fields belong after wide fields");

typedef struct {
    const unsigned char *bytes;
    size_t length;
    size_t offset;
    size_t chunk;
} MemorySource;

typedef struct {
    size_t calls;
    size_t cancel_at;
} CancelState;

static CmnyCsvReadStatus memory_read(void *opaque, unsigned char *buffer,
                                     size_t capacity, size_t *length) {
    MemorySource *source = opaque;
    if (source->offset == source->length) {
        *length = 0;
        return CMNY_CSV_READ_END;
    }
    size_t remaining = source->length - source->offset;
    size_t amount = remaining < source->chunk ? remaining : source->chunk;
    if (amount > capacity) amount = capacity;
    memcpy(buffer, source->bytes + source->offset, amount);
    source->offset += amount;
    *length = amount;
    return CMNY_CSV_READ_DATA;
}

static bool cancel_at_call(void *opaque) {
    CancelState *state = opaque;
    state->calls++;
    return state->calls >= state->cancel_at;
}

static CmnyBankCsvImportOptions options(bool external_id) {
    CmnyBankCsvImportOptions value;
    cmny_bank_csv_import_options_default(&value);
    value.source.delimiter = ',';
    value.mapping = (CmnyBankCsvMapping){
        .date_column = 0,
        .amount_column = 1,
        .debit_column = CMNY_BANK_COLUMN_NONE,
        .credit_column = CMNY_BANK_COLUMN_NONE,
        .payee_column = 2,
        .note_column = 3,
        .external_id_column = external_id ? 4U : CMNY_BANK_COLUMN_NONE,
    };
    return value;
}

static CmnyImportProfileConfig profile_config(const CmnyBankCsvImportOptions *value) {
    return (CmnyImportProfileConfig){
        .mapping = value->mapping,
        .date_format = value->date_format,
        .sign_convention = value->sign_convention,
        .delimiter = value->source.delimiter,
        .decimal_separator = value->decimal_separator,
        .thousands_separator = value->thousands_separator,
    };
}

static bool apply_document(CmnyDb *db, int64_t account_id, int64_t profile_id,
                           const void *document, size_t length,
                           const CmnyBankCsvImportOptions *value,
                           CmnyImportHeuristicPolicy policy, CancelState *cancel,
                           CmnyImportResult *result, char *err, size_t err_size) {
    MemorySource source = {
        .bytes = document,
        .length = length,
        .chunk = length,
    };
    CmnyImportRequest request = {
        .reader = memory_read,
        .reader_context = &source,
        .options = value,
        .account_id = account_id,
        .profile_id = profile_id,
        .source_name = "test-statement.csv",
        .heuristic_policy = policy,
        .cancel = cancel != NULL ? cancel_at_call : NULL,
        .cancel_context = cancel,
    };
    return cmny_import_apply(db, &request, result, err, err_size);
}

static int64_t first_account(CmnyDb *db, char *err, size_t err_size) {
    CmnyAccount accounts[4];
    size_t count = 0;
    ASSERT_TRUE(cmny_account_list(db, false, accounts, 4, &count, err, err_size));
    ASSERT_TRUE(count > 0);
    return accounts[0].id;
}

static bool entry_active(CmnyDb *db, int64_t entry_id, char *err, size_t err_size) {
    CmnyLedgerEntry entry = {0};
    bool active = cmny_ledger_entry_get(db, entry_id, &entry, err, err_size);
    cmny_ledger_entry_destroy(&entry);
    return active;
}

static size_t batch_count(CmnyDb *db, int64_t account_id, char *err, size_t err_size) {
    CmnyImportBatch batches[CMNY_IMPORT_BATCH_LIST_LIMIT];
    size_t count = 0;
    ASSERT_TRUE(cmny_import_batch_list(db, account_id, 0, batches,
                                      CMNY_IMPORT_BATCH_LIST_LIMIT, &count,
                                      err, err_size));
    return count;
}

int main(void) {
    static const unsigned char first_document[] =
        "date,amount,payee,note,id\n"
        "2026-12-01,-10.00,Coffee House,morning,ext-1\n"
        "2026-12-02,50.00,Employer,bonus,ext-2\n";
    static const unsigned char identical_document[] =
        "date,amount,payee,note\n"
        "2026-12-03,-5.00,Market,same\n"
        "2026-12-03,-5.00,Market,same\n";
    static const unsigned char one_identical[] =
        "date,amount,payee,note\n"
        "2026-12-03,-5.00,Market,same\n";
    static const unsigned char invalid_document[] =
        "date,amount,payee,note,id\n"
        "2026-12-04,-2.00,Valid,first,atomic-valid\n"
        "2026-99-99,-3.00,Bad,second,atomic-bad\n";
    static const unsigned char corrected_document[] =
        "date,amount,payee,note,id\n"
        "2026-12-04,-2.00,Valid,first,atomic-valid\n";
    static const unsigned char huge_document[] =
        "date,amount,payee,note,id\n"
        "2026-12-05,90000000000000.01,Huge,outside,huge-1\n";
    static const unsigned char rollback_failure_document[] =
        "date,amount,payee,note,id\n"
        "2026-12-06,-1.00,First,rollback,fail-1\n"
        "2026-12-07,-2.00,Second,rollback,fail-2\n";

    char path[] = "build/cmny-import-XXXXXX";
    int descriptor = mkstemp(path);
    ASSERT_TRUE(descriptor >= 0);
    ASSERT_TRUE(close(descriptor) == 0);
    CmnyDb db = {0};
    char currency[4] = {0};
    char err[256] = {0};
    ASSERT_TRUE(cmny_db_open(&db, path, false, "EUR", currency, err, sizeof(err)));
    int64_t cash_id = first_account(&db, err, sizeof(err));
    int64_t savings_id = 0;
    ASSERT_TRUE(cmny_account_create(&db, "Import savings", CMNY_ACCOUNT_SAVINGS, "",
                                    &savings_id, err, sizeof(err)));

    CmnyBankCsvImportOptions with_external = options(true);
    CmnyBankCsvImportOptions without_external = options(false);
    CmnyImportProfileDraft profile_draft = {
        .name = "Main bank CSV",
        .config = profile_config(&with_external),
    };
    int64_t profile_id = 0;
    ASSERT_TRUE(cmny_import_profile_create(&db, &profile_draft, &profile_id,
                                           err, sizeof(err)));
    ASSERT_TRUE(!cmny_import_profile_create(&db, &profile_draft, NULL, err, sizeof(err)));
    CmnyImportProfile profile = {0};
    ASSERT_TRUE(cmny_import_profile_get(&db, profile_id, &profile, err, sizeof(err)));
    ASSERT_TRUE(strcmp(profile.name, "Main bank CSV") == 0);
    CmnyBankCsvImportOptions remembered;
    cmny_import_profile_options(&profile, &remembered);
    ASSERT_TRUE(remembered.source.delimiter == ',');
    ASSERT_EQ_I64(4, remembered.mapping.external_id_column);
    int64_t profile_revision = profile.revision;
    profile_draft.name = "Main bank updated";
    ASSERT_TRUE(cmny_import_profile_update(&db, profile_id, profile_revision,
                                           &profile_draft, err, sizeof(err)));
    ASSERT_TRUE(!cmny_import_profile_update(&db, profile_id, profile_revision,
                                            &profile_draft, err, sizeof(err)));
    CmnyImportProfile profiles[4];
    size_t count = 0;
    ASSERT_TRUE(cmny_import_profile_list(&db, 0, profiles, 4, &count, err, sizeof(err)));
    ASSERT_EQ_I64(1, count);
    ASSERT_TRUE(!cmny_import_profile_list(&db, 0, profiles,
        CMNY_IMPORT_PROFILE_LIST_LIMIT + 1U, &count, err, sizeof(err)));

    int64_t coffee_category = 0;
    int64_t coffee_tag = 0;
    ASSERT_TRUE(cmny_category_create(&db, "Imported coffee", CMNY_CATEGORY_EXPENSE, 0,
                                     &coffee_category, err, sizeof(err)));
    ASSERT_TRUE(cmny_tag_create(&db, "Imported", &coffee_tag, err, sizeof(err)));
    CmnyRuleDraft coffee_rule = {
        .category_id = coffee_category,
        .tag_id = coffee_tag,
        .sort_order = 1,
        .payee_mode = CMNY_RULE_TEXT_CONTAINS,
        .note_mode = CMNY_RULE_TEXT_ANY,
        .enabled = true,
        .name = "Coffee import",
        .payee_pattern = "Coffee",
        .note_pattern = "",
    };
    int64_t coffee_rule_id = 0;
    ASSERT_TRUE(cmny_rule_create(&db, &coffee_rule, &coffee_rule_id, err, sizeof(err)));

    CmnyTransferDraft transfer = {
        .from_account_id = cash_id,
        .to_account_id = savings_id,
        .amount_minor = 100,
        .occurred_on = "2026-11-30",
        .payee = "Transfer stays",
        .note = "not an import",
    };
    int64_t transfer_id = 0;
    ASSERT_TRUE(cmny_transfer_create(&db, &transfer, &transfer_id, err, sizeof(err)));

    CmnyImportResult result = {0};
    ASSERT_TRUE(apply_document(&db, cash_id, profile_id, first_document,
                               sizeof(first_document) - 1U, &with_external,
                               CMNY_IMPORT_HEURISTIC_SKIP, NULL, &result,
                               err, sizeof(err)));
    int64_t first_batch_id = result.batch_id;
    ASSERT_EQ_I64(2, result.input_rows);
    ASSERT_EQ_I64(2, result.imported_rows);
    ASSERT_EQ_I64(0, result.hard_duplicates);
    CmnyImportRecord records[8];
    ASSERT_TRUE(cmny_import_record_list(&db, first_batch_id, 0, records, 8,
                                        &count, err, sizeof(err)));
    ASSERT_EQ_I64(2, count);
    ASSERT_EQ_I64(CMNY_IMPORT_RECORD_IMPORTED, records[0].decision);
    ASSERT_TRUE(records[0].dedupe_active);
    ASSERT_EQ_I64(coffee_rule_id, records[0].rule_id);
    ASSERT_EQ_I64(coffee_category, records[0].category_id);
    ASSERT_EQ_I64(coffee_tag, records[0].tag_id);
    int64_t first_entry = records[0].entry_id;
    int64_t second_entry = records[1].entry_id;
    ASSERT_TRUE(entry_active(&db, first_entry, err, sizeof(err)));
    ASSERT_TRUE(entry_active(&db, second_entry, err, sizeof(err)));
    ASSERT_TRUE(!cmny_import_record_list(&db, first_batch_id, 0, records,
        CMNY_IMPORT_RECORD_LIST_LIMIT + 1U, &count, err, sizeof(err)));

    cmny_db_close(&db);
    ASSERT_TRUE(cmny_db_open(&db, path, false, NULL, currency, err, sizeof(err)));
    CmnyImportBatch batch = {0};
    ASSERT_TRUE(cmny_import_batch_get(&db, first_batch_id, &batch, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_IMPORT_BATCH_APPLIED, batch.status);
    ASSERT_EQ_I64(profile_id, batch.profile_id);

    ASSERT_TRUE(apply_document(&db, cash_id, profile_id, first_document,
                               sizeof(first_document) - 1U, &with_external,
                               CMNY_IMPORT_HEURISTIC_ALLOW, NULL, &result,
                               err, sizeof(err)));
    ASSERT_EQ_I64(0, result.imported_rows);
    ASSERT_EQ_I64(2, result.hard_duplicates);
    ASSERT_TRUE(apply_document(&db, savings_id, 0, first_document,
                               sizeof(first_document) - 1U, &with_external,
                               CMNY_IMPORT_HEURISTIC_SKIP, NULL, &result,
                               err, sizeof(err)));
    ASSERT_EQ_I64(2, result.imported_rows);

    ASSERT_TRUE(apply_document(&db, cash_id, 0, identical_document,
                               sizeof(identical_document) - 1U, &without_external,
                               CMNY_IMPORT_HEURISTIC_SKIP, NULL, &result,
                               err, sizeof(err)));
    ASSERT_EQ_I64(1, result.imported_rows);
    ASSERT_EQ_I64(1, result.heuristic_duplicates);
    ASSERT_EQ_I64(1, result.heuristic_skipped);
    ASSERT_TRUE(apply_document(&db, cash_id, 0, one_identical,
                               sizeof(one_identical) - 1U, &without_external,
                               CMNY_IMPORT_HEURISTIC_ALLOW, NULL, &result,
                               err, sizeof(err)));
    ASSERT_EQ_I64(1, result.imported_rows);
    ASSERT_EQ_I64(1, result.heuristic_duplicates);
    ASSERT_TRUE(apply_document(&db, cash_id, 0, identical_document,
                               sizeof(identical_document) - 1U, &without_external,
                               CMNY_IMPORT_HEURISTIC_ALLOW, NULL, &result,
                               err, sizeof(err)));
    ASSERT_EQ_I64(2, result.imported_rows);
    ASSERT_EQ_I64(2, result.heuristic_duplicates);

    size_t before_failure = batch_count(&db, cash_id, err, sizeof(err));
    ASSERT_TRUE(!apply_document(&db, cash_id, 0, invalid_document,
                                sizeof(invalid_document) - 1U, &with_external,
                                CMNY_IMPORT_HEURISTIC_SKIP, NULL, &result,
                                err, sizeof(err)));
    ASSERT_EQ_I64(0, result.batch_id);
    ASSERT_EQ_I64(1, result.diagnostic_count);
    ASSERT_EQ_I64(before_failure, batch_count(&db, cash_id, err, sizeof(err)));
    ASSERT_TRUE(apply_document(&db, cash_id, 0, corrected_document,
                               sizeof(corrected_document) - 1U, &with_external,
                               CMNY_IMPORT_HEURISTIC_SKIP, NULL, &result,
                               err, sizeof(err)));
    ASSERT_EQ_I64(1, result.imported_rows);
    ASSERT_TRUE(!apply_document(&db, cash_id, 0, huge_document,
                                sizeof(huge_document) - 1U, &with_external,
                                CMNY_IMPORT_HEURISTIC_SKIP, NULL, &result,
                                err, sizeof(err)));

    CancelState cancel = {.cancel_at = 4};
    before_failure = batch_count(&db, cash_id, err, sizeof(err));
    ASSERT_TRUE(!apply_document(&db, cash_id, 0, rollback_failure_document,
                                sizeof(rollback_failure_document) - 1U, &with_external,
                                CMNY_IMPORT_HEURISTIC_SKIP, &cancel, &result,
                                err, sizeof(err)));
    ASSERT_EQ_I64(0, result.batch_id);
    ASSERT_EQ_I64(before_failure, batch_count(&db, cash_id, err, sizeof(err)));

    ASSERT_TRUE(cmny_import_batch_get(&db, first_batch_id, &batch, err, sizeof(err)));
    ASSERT_TRUE(cmny_import_batch_rollback(&db, first_batch_id, batch.revision,
                                           err, sizeof(err)));
    ASSERT_TRUE(cmny_import_batch_get(&db, first_batch_id, &batch, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_IMPORT_BATCH_ROLLED_BACK, batch.status);
    ASSERT_TRUE(batch.rolled_back_at > 0);
    ASSERT_TRUE(!entry_active(&db, first_entry, err, sizeof(err)));
    ASSERT_TRUE(!entry_active(&db, second_entry, err, sizeof(err)));
    ASSERT_TRUE(entry_active(&db, transfer_id, err, sizeof(err)));
    ASSERT_TRUE(cmny_import_record_list(&db, first_batch_id, 0, records, 8,
                                        &count, err, sizeof(err)));
    ASSERT_TRUE(!records[0].dedupe_active && !records[1].dedupe_active);
    CmnyHistoryAction actions[8];
    ASSERT_TRUE(cmny_history_list(&db, false, 0, actions, 8, &count, err, sizeof(err)));
    ASSERT_TRUE(count >= 2);
    ASSERT_EQ_I64(CMNY_HISTORY_DELETE, actions[0].type);
    ASSERT_EQ_I64(CMNY_HISTORY_DELETE, actions[1].type);

    ASSERT_TRUE(apply_document(&db, cash_id, 0, first_document,
                               sizeof(first_document) - 1U, &with_external,
                               CMNY_IMPORT_HEURISTIC_SKIP, NULL, &result,
                               err, sizeof(err)));
    ASSERT_EQ_I64(2, result.imported_rows);
    ASSERT_EQ_I64(0, result.hard_duplicates);

    ASSERT_TRUE(apply_document(&db, cash_id, 0, rollback_failure_document,
                               sizeof(rollback_failure_document) - 1U, &with_external,
                               CMNY_IMPORT_HEURISTIC_SKIP, NULL, &result,
                               err, sizeof(err)));
    int64_t failure_batch_id = result.batch_id;
    ASSERT_TRUE(cmny_import_record_list(&db, failure_batch_id, 0, records, 8,
                                        &count, err, sizeof(err)));
    ASSERT_EQ_I64(2, count);
    char trigger_sql[512];
    (void)snprintf(trigger_sql, sizeof(trigger_sql),
        "CREATE TRIGGER block_import_rollback BEFORE UPDATE OF voided_at ON entries"
        " WHEN OLD.id=%lld AND NEW.voided_at IS NOT NULL"
        " BEGIN SELECT RAISE(ABORT,'blocked rollback'); END",
        (long long)records[1].entry_id);
    ASSERT_TRUE(sqlite3_exec(db.handle, trigger_sql, NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(cmny_import_batch_get(&db, failure_batch_id, &batch, err, sizeof(err)));
    ASSERT_TRUE(!cmny_import_batch_rollback(&db, failure_batch_id, batch.revision,
                                            err, sizeof(err)));
    ASSERT_TRUE(entry_active(&db, records[0].entry_id, err, sizeof(err)));
    ASSERT_TRUE(entry_active(&db, records[1].entry_id, err, sizeof(err)));
    ASSERT_TRUE(cmny_import_batch_get(&db, failure_batch_id, &batch, err, sizeof(err)));
    ASSERT_EQ_I64(CMNY_IMPORT_BATCH_APPLIED, batch.status);
    ASSERT_TRUE(sqlite3_exec(db.handle, "DROP TRIGGER block_import_rollback",
                             NULL, NULL, NULL) == SQLITE_OK);

    ASSERT_TRUE(!cmny_import_batch_list(&db, 0, 0, &batch,
        CMNY_IMPORT_BATCH_LIST_LIMIT + 1U, &count, err, sizeof(err)));
    bool consistent = cmny_db_check(&db, err, sizeof(err));
    if (!consistent) (void)fprintf(stderr, "import integrity detail: %s\n", err);
    ASSERT_TRUE(consistent);
    ASSERT_TRUE(sqlite3_exec(db.handle,
        "PRAGMA ignore_check_constraints=ON;"
        "UPDATE import_batches SET imported_rows=999 WHERE id=1;"
        "PRAGMA ignore_check_constraints=OFF", NULL, NULL, NULL) == SQLITE_OK);
    ASSERT_TRUE(!cmny_db_check(&db, err, sizeof(err)));
    ASSERT_TRUE(sqlite3_exec(db.handle,
        "UPDATE import_batches SET imported_rows=2 WHERE id=1", NULL, NULL, NULL) == SQLITE_OK);
    bool integrity_ok = cmny_db_check(&db, err, sizeof(err));
    if (!integrity_ok) (void)fprintf(stderr, "import integrity failure: %s\n", err);
    ASSERT_TRUE(integrity_ok);

    ASSERT_TRUE(cmny_import_profile_get(&db, profile_id, &profile, err, sizeof(err)));
    ASSERT_TRUE(cmny_import_profile_delete(&db, profile_id, profile.revision,
                                           err, sizeof(err)));
    ASSERT_TRUE(cmny_import_batch_get(&db, first_batch_id, &batch, err, sizeof(err)));
    ASSERT_EQ_I64(0, batch.profile_id);
    ASSERT_TRUE(cmny_db_check(&db, err, sizeof(err)));
    cmny_db_close(&db);

    ASSERT_TRUE(unlink(path) == 0);
    char sidecar[4200];
    (void)snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    (void)unlink(sidecar);
    (void)snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    (void)unlink(sidecar);
    (void)printf("ok - bank import service tests\n");
    return 0;
}
