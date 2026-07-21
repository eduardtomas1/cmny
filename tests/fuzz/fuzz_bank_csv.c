#include "cmny_bank_csv.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
    size_t chunk;
} FuzzReader;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static CmnyCsvReadStatus fuzz_read(void *context, unsigned char *buffer,
                                   size_t capacity, size_t *length) {
    FuzzReader *reader = context;
    if (reader->offset == reader->size) {
        *length = 0;
        return CMNY_CSV_READ_END;
    }
    size_t remaining = reader->size - reader->offset;
    size_t amount = remaining < reader->chunk ? remaining : reader->chunk;
    if (amount > capacity) amount = capacity;
    memcpy(buffer, reader->data + reader->offset, amount);
    reader->offset += amount;
    *length = amount;
    return CMNY_CSV_READ_DATA;
}

static CmnyBankCsvAction check_preview(const CmnyCsvRecord *record, bool header,
                                       void *context) {
    (void)header;
    (void)context;
    if (record->field_count == 0 || record->field_count > CMNY_CSV_MAX_FIELDS ||
        record->record_number == 0 || record->physical_line_start == 0 ||
        record->physical_line_end < record->physical_line_start) {
        abort();
    }
    for (size_t field = 0; field < record->field_count; field++) {
        if (memchr(record->fields[field], '\0', CMNY_CSV_FIELD_CAP) == NULL) abort();
    }
    return CMNY_BANK_CONTINUE;
}

static CmnyBankCsvAction check_row(const CmnyBankCsvRow *row, void *context) {
    (void)context;
    if (strlen(row->date) != 10 || row->date[4] != '-' || row->date[7] != '-' ||
        row->amount_cents == 0 || row->physical_line == 0 || row->record_number < 2 ||
        memchr(row->payee, '\0', sizeof(row->payee)) == NULL ||
        memchr(row->note, '\0', sizeof(row->note)) == NULL ||
        memchr(row->external_id, '\0', sizeof(row->external_id)) == NULL ||
        memchr(row->identity, '\0', sizeof(row->identity)) == NULL ||
        strncmp(row->identity, "v1|D10:", 7) != 0) {
        abort();
    }
    return CMNY_BANK_CONTINUE;
}

static void check_diagnostic(const CmnyBankCsvDiagnostic *diagnostic, void *context) {
    (void)context;
    if (diagnostic->physical_line == 0 || diagnostic->record_number < 2 ||
        diagnostic->message == NULL) {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t selector = size > 0 ? data[0] : 0;
    const uint8_t *payload = data;
    size_t payload_size = size;
    size_t chunk = (size_t)(selector % 31U) + 1U;
    static const char delimiters[] = {'\0', ',', ';', '\t'};

    CmnyBankCsvSourceOptions preview_options;
    cmny_bank_csv_source_options_default(&preview_options);
    preview_options.delimiter = delimiters[selector % 4U];
    preview_options.preview_rows = 3;
    preview_options.limits.max_bytes = 65536;
    preview_options.limits.max_records = 256;
    preview_options.limits.max_fields = 8;
    preview_options.limits.max_field_bytes = 128;
    FuzzReader reader = {.data = payload, .size = payload_size, .chunk = chunk};
    CmnyBankCsvPreview preview = {.delimiter = 'x', .header_fields = 13,
                                  .previewed_rows = 17};
    CmnyBankCsvPreview preview_before = preview;
    CmnyBankCsvError error = {0};
    bool preview_ok = cmny_bank_csv_preview(fuzz_read, &reader, &preview_options,
                                            check_preview, NULL, NULL, NULL,
                                            &preview, &error);
    if (!preview_ok && memcmp(&preview, &preview_before, sizeof(preview)) != 0) abort();
    if (!preview_ok && error.message == NULL) abort();

    CmnyBankCsvImportOptions options;
    cmny_bank_csv_import_options_default(&options);
    options.source = preview_options;
    options.date_format = (CmnyBankDateFormat)((selector >> 2U) % 3U);
    options.sign_convention = (CmnyBankSignConvention)((selector >> 4U) % 4U);
    options.decimal_separator = (selector & 0x40U) != 0 ? ',' : '.';
    options.thousands_separator = (selector & 0x80U) != 0
                                      ? (options.decimal_separator == '.' ? ',' : '.')
                                      : '\0';
    options.mapping = (CmnyBankCsvMapping){
        .date_column = 0,
        .amount_column = 1,
        .debit_column = CMNY_BANK_COLUMN_NONE,
        .credit_column = CMNY_BANK_COLUMN_NONE,
        .payee_column = 2,
        .note_column = 3,
        .external_id_column = 4,
    };
    reader = (FuzzReader){.data = payload, .size = payload_size, .chunk = chunk};
    CmnyBankCsvSummary summary = {
        .delimiter = 'x', .input_rows = 23, .normalized_rows = 29, .skipped_rows = 31,
    };
    CmnyBankCsvSummary summary_before = summary;
    error = (CmnyBankCsvError){0};
    bool normalize_ok = cmny_bank_csv_normalize(
        fuzz_read, &reader, &options, check_row, NULL, check_diagnostic, NULL,
        NULL, NULL, &summary, &error);
    if (!normalize_ok && memcmp(&summary, &summary_before, sizeof(summary)) != 0) abort();
    if (!normalize_ok && error.message == NULL) abort();
    if (normalize_ok && summary.normalized_rows + summary.skipped_rows !=
                            summary.input_rows) {
        abort();
    }
    return 0;
}
