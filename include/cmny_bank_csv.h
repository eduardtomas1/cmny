#ifndef CMNY_BANK_CSV_H
#define CMNY_BANK_CSV_H

#include "cmny_csv_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CMNY_BANK_COLUMN_NONE SIZE_MAX
#define CMNY_BANK_PAYEE_CAP 257U
#define CMNY_BANK_NOTE_CAP 513U
#define CMNY_BANK_EXTERNAL_ID_CAP 129U
#define CMNY_BANK_IDENTITY_CAP 1100U
#define CMNY_BANK_DELIMITER_DETECT_CAP (64U * 1024U)

typedef enum {
    CMNY_BANK_DATE_ISO,
    CMNY_BANK_DATE_DMY,
    CMNY_BANK_DATE_MDY
} CmnyBankDateFormat;

typedef enum {
    CMNY_BANK_SIGNED_INFLOW_POSITIVE,
    CMNY_BANK_SIGNED_OUTFLOW_POSITIVE,
    CMNY_BANK_UNSIGNED_INFLOW,
    CMNY_BANK_UNSIGNED_OUTFLOW
} CmnyBankSignConvention;

typedef struct {
    size_t date_column;
    size_t amount_column;
    size_t debit_column;
    size_t credit_column;
    size_t payee_column;
    size_t note_column;
    size_t external_id_column;
} CmnyBankCsvMapping;

typedef struct {
    char delimiter; /* ',', ';', '\t', or zero for bounded auto-detection. */
    CmnyCsvLimits limits;
    size_t preview_rows;
} CmnyBankCsvSourceOptions;

typedef struct {
    CmnyBankCsvSourceOptions source;
    CmnyBankCsvMapping mapping;
    CmnyBankDateFormat date_format;
    /* Applies to amount_column. Debit/credit mode is always credit - debit. */
    CmnyBankSignConvention sign_convention;
    char decimal_separator;
    char thousands_separator; /* Zero disables grouping. */
} CmnyBankCsvImportOptions;

typedef struct {
    char date[11];
    int64_t amount_cents; /* Canonical: inflow positive, outflow negative. */
    char payee[CMNY_BANK_PAYEE_CAP];
    char note[CMNY_BANK_NOTE_CAP];
    char external_id[CMNY_BANK_EXTERNAL_ID_CAP];
    /* Length-prefixed normalized identity; explicitly not a security hash. */
    char identity[CMNY_BANK_IDENTITY_CAP];
    size_t physical_line;
    size_t record_number;
} CmnyBankCsvRow;

typedef enum {
    CMNY_BANK_DIAG_FIELD_COUNT,
    CMNY_BANK_DIAG_DATE,
    CMNY_BANK_DIAG_AMOUNT,
    CMNY_BANK_DIAG_PAYEE,
    CMNY_BANK_DIAG_NOTE,
    CMNY_BANK_DIAG_EXTERNAL_ID
} CmnyBankCsvDiagnosticCode;

typedef struct {
    CmnyBankCsvDiagnosticCode code;
    size_t physical_line;
    size_t record_number;
    size_t column; /* CMNY_BANK_COLUMN_NONE when the whole record is invalid. */
    const char *message; /* Static storage; borrowed for the callback call. */
} CmnyBankCsvDiagnostic;

typedef enum {
    CMNY_BANK_CONTINUE,
    CMNY_BANK_CANCEL,
    CMNY_BANK_ERROR
} CmnyBankCsvAction;

/* All row/record/diagnostic arguments are borrowed only for the callback call. */
typedef CmnyBankCsvAction (*CmnyBankCsvPreviewHandler)(const CmnyCsvRecord *record,
                                                       bool header, void *context);
typedef CmnyBankCsvAction (*CmnyBankCsvRowHandler)(const CmnyBankCsvRow *row,
                                                   void *context);
typedef void (*CmnyBankCsvDiagnosticHandler)(const CmnyBankCsvDiagnostic *diagnostic,
                                             void *context);
typedef bool (*CmnyBankCsvCancelHandler)(void *context);

typedef struct {
    char delimiter;
    size_t header_fields;
    size_t previewed_rows;
} CmnyBankCsvPreview;

typedef struct {
    char delimiter;
    size_t input_rows;
    size_t normalized_rows;
    size_t skipped_rows;
} CmnyBankCsvSummary;

typedef struct {
    size_t byte_offset;
    size_t physical_line;
    size_t record_number;
    size_t column;
    const char *message; /* Static storage; never owned by the caller. */
} CmnyBankCsvError;

void cmny_bank_csv_source_options_default(CmnyBankCsvSourceOptions *options);
void cmny_bank_csv_import_options_default(CmnyBankCsvImportOptions *options);

/* Preview and normalize require separate fresh readers; neither rewinds a source. */
bool cmny_bank_csv_preview(CmnyCsvReader reader, void *reader_context,
                           const CmnyBankCsvSourceOptions *options,
                           CmnyBankCsvPreviewHandler handler, void *handler_context,
                           CmnyBankCsvCancelHandler cancel, void *cancel_context,
                           CmnyBankCsvPreview *preview, CmnyBankCsvError *error);

bool cmny_bank_csv_normalize(CmnyCsvReader reader, void *reader_context,
                             const CmnyBankCsvImportOptions *options,
                             CmnyBankCsvRowHandler row_handler, void *row_context,
                             CmnyBankCsvDiagnosticHandler diagnostic_handler,
                             void *diagnostic_context,
                             CmnyBankCsvCancelHandler cancel, void *cancel_context,
                             CmnyBankCsvSummary *summary, CmnyBankCsvError *error);

#endif
