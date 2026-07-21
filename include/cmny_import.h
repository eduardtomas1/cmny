#ifndef CMNY_IMPORT_H
#define CMNY_IMPORT_H

#include "cmny.h"
#include "cmny_bank_csv.h"

#define CMNY_IMPORT_PROFILE_NAME_MAX 64U
#define CMNY_IMPORT_PROFILE_LIST_LIMIT 256U
#define CMNY_IMPORT_SOURCE_MAX 255U
#define CMNY_IMPORT_BATCH_LIST_LIMIT 256U
#define CMNY_IMPORT_RECORD_LIST_LIMIT 64U
#define CMNY_IMPORT_DIAGNOSTIC_LIMIT 64U
#define CMNY_IMPORT_DIAGNOSTIC_MESSAGE_MAX 127U

typedef struct {
    CmnyBankCsvMapping mapping;
    CmnyBankDateFormat date_format;
    CmnyBankSignConvention sign_convention;
    char delimiter; /* ',', ';', '\t', or zero for auto-detection. */
    char decimal_separator;
    char thousands_separator;
} CmnyImportProfileConfig;

typedef struct {
    const char *name;
    CmnyImportProfileConfig config;
} CmnyImportProfileDraft;

typedef struct {
    int64_t id;
    int64_t revision;
    int64_t created_at;
    int64_t updated_at;
    CmnyImportProfileConfig config;
    char name[CMNY_IMPORT_PROFILE_NAME_MAX + 1U];
} CmnyImportProfile;

typedef enum {
    CMNY_IMPORT_HEURISTIC_SKIP = 1,
    CMNY_IMPORT_HEURISTIC_ALLOW
} CmnyImportHeuristicPolicy;

typedef enum {
    CMNY_IMPORT_BATCH_APPLIED = 1,
    CMNY_IMPORT_BATCH_ROLLED_BACK
} CmnyImportBatchStatus;

typedef enum {
    CMNY_IMPORT_RECORD_IMPORTED = 1,
    CMNY_IMPORT_RECORD_HARD_DUPLICATE,
    CMNY_IMPORT_RECORD_HEURISTIC_SKIPPED
} CmnyImportRecordDecision;

typedef struct {
    CmnyCsvReader reader;
    void *reader_context;
    const CmnyBankCsvImportOptions *options;
    int64_t account_id;
    int64_t profile_id; /* Zero means no remembered profile was used. */
    const char *source_name;
    CmnyImportHeuristicPolicy heuristic_policy;
    CmnyBankCsvCancelHandler cancel;
    void *cancel_context;
} CmnyImportRequest;

typedef struct {
    CmnyBankCsvDiagnosticCode code;
    size_t physical_line;
    size_t record_number;
    size_t column;
    char message[CMNY_IMPORT_DIAGNOSTIC_MESSAGE_MAX + 1U];
} CmnyImportDiagnostic;

typedef struct {
    int64_t batch_id;
    size_t input_rows;
    size_t normalized_rows;
    size_t imported_rows;
    size_t hard_duplicates;
    size_t heuristic_duplicates;
    size_t heuristic_skipped;
    size_t diagnostic_count;
    bool diagnostics_truncated;
    CmnyImportDiagnostic diagnostics[CMNY_IMPORT_DIAGNOSTIC_LIMIT];
} CmnyImportResult;

typedef struct {
    int64_t id;
    int64_t account_id;
    int64_t profile_id;
    int64_t revision;
    int64_t input_rows;
    int64_t normalized_rows;
    int64_t imported_rows;
    int64_t hard_duplicates;
    int64_t heuristic_duplicates;
    int64_t heuristic_skipped;
    int64_t created_at;
    int64_t rolled_back_at;
    CmnyImportHeuristicPolicy heuristic_policy;
    CmnyImportBatchStatus status;
    char delimiter;
    char source_name[CMNY_IMPORT_SOURCE_MAX + 1U];
} CmnyImportBatch;

typedef struct {
    int64_t id;
    int64_t batch_id;
    int64_t account_id;
    int64_t entry_id;
    int64_t duplicate_of_record_id;
    int64_t rule_id;
    int64_t category_id;
    int64_t tag_id;
    int64_t amount_minor;
    size_t physical_line;
    size_t record_number;
    CmnyImportRecordDecision decision;
    bool dedupe_active;
    bool heuristic_duplicate;
    char occurred_on[11];
    char payee[CMNY_BANK_PAYEE_CAP];
    char note[CMNY_BANK_NOTE_CAP];
    char external_id[CMNY_BANK_EXTERNAL_ID_CAP];
    char identity[CMNY_BANK_IDENTITY_CAP];
} CmnyImportRecord;

bool cmny_import_profile_create(CmnyDb *db, const CmnyImportProfileDraft *draft,
                                int64_t *new_id, char *err, size_t err_size);
bool cmny_import_profile_get(CmnyDb *db, int64_t id, CmnyImportProfile *out,
                             char *err, size_t err_size);
bool cmny_import_profile_update(CmnyDb *db, int64_t id, int64_t expected_revision,
                                const CmnyImportProfileDraft *draft,
                                char *err, size_t err_size);
bool cmny_import_profile_delete(CmnyDb *db, int64_t id, int64_t expected_revision,
                                char *err, size_t err_size);
bool cmny_import_profile_list(CmnyDb *db, size_t offset, CmnyImportProfile *out,
                              size_t cap, size_t *count, char *err, size_t err_size);
void cmny_import_profile_options(const CmnyImportProfile *profile,
                                 CmnyBankCsvImportOptions *options);

bool cmny_import_apply(CmnyDb *db, const CmnyImportRequest *request,
                       CmnyImportResult *result, char *err, size_t err_size);
bool cmny_import_batch_get(CmnyDb *db, int64_t id, CmnyImportBatch *out,
                           char *err, size_t err_size);
bool cmny_import_batch_list(CmnyDb *db, int64_t account_id, size_t offset,
                            CmnyImportBatch *out, size_t cap, size_t *count,
                            char *err, size_t err_size);
bool cmny_import_record_list(CmnyDb *db, int64_t batch_id, size_t offset,
                             CmnyImportRecord *out, size_t cap, size_t *count,
                             char *err, size_t err_size);
bool cmny_import_batch_rollback(CmnyDb *db, int64_t batch_id,
                                int64_t expected_revision,
                                char *err, size_t err_size);

#endif
