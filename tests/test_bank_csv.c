#include "cmny_bank_csv.h"
#include "test.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const unsigned char *bytes;
    size_t length;
    size_t offset;
    size_t chunk;
    bool fail;
    bool violate_contract;
} MemorySource;

typedef struct {
    char fields[4][5][80];
    size_t field_counts[4];
    bool headers[4];
    size_t count;
    CmnyBankCsvAction action;
} PreviewCapture;

typedef struct {
    CmnyBankCsvRow rows[16];
    size_t count;
    size_t cancel_after;
    bool fail;
} RowCapture;

typedef struct {
    CmnyBankCsvDiagnostic diagnostics[16];
    size_t count;
} DiagnosticCapture;

typedef struct {
    size_t calls;
    size_t cancel_at;
} CancelState;

static CmnyCsvReadStatus memory_read(void *context, unsigned char *buffer,
                                     size_t capacity, size_t *length) {
    MemorySource *source = context;
    if (source->fail) {
        *length = 0;
        return CMNY_CSV_READ_ERROR;
    }
    if (source->violate_contract) {
        *length = 0;
        return CMNY_CSV_READ_DATA;
    }
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

static MemorySource source_for(const void *bytes, size_t length, size_t chunk) {
    return (MemorySource){
        .bytes = bytes,
        .length = length,
        .chunk = chunk,
    };
}

static CmnyBankCsvAction capture_preview(const CmnyCsvRecord *record, bool header,
                                         void *context) {
    PreviewCapture *capture = context;
    if (capture->count >= 4) return CMNY_BANK_ERROR;
    size_t index = capture->count++;
    capture->field_counts[index] = record->field_count;
    capture->headers[index] = header;
    size_t field_count = record->field_count < 5 ? record->field_count : 5;
    for (size_t field = 0; field < field_count; field++) {
        (void)snprintf(capture->fields[index][field],
                       sizeof(capture->fields[index][field]), "%s",
                       record->fields[field]);
    }
    return capture->action;
}

static CmnyBankCsvAction capture_row(const CmnyBankCsvRow *row, void *context) {
    RowCapture *capture = context;
    if (capture->fail) return CMNY_BANK_ERROR;
    if (capture->cancel_after > 0 && capture->count >= capture->cancel_after) {
        return CMNY_BANK_CANCEL;
    }
    if (capture->count >= sizeof(capture->rows) / sizeof(capture->rows[0])) {
        return CMNY_BANK_ERROR;
    }
    capture->rows[capture->count++] = *row;
    return CMNY_BANK_CONTINUE;
}

static void capture_diagnostic(const CmnyBankCsvDiagnostic *diagnostic, void *context) {
    DiagnosticCapture *capture = context;
    if (capture->count < sizeof(capture->diagnostics) /
                             sizeof(capture->diagnostics[0])) {
        capture->diagnostics[capture->count++] = *diagnostic;
    }
}

static bool cancel_when_called(void *context) {
    CancelState *state = context;
    state->calls++;
    return state->calls >= state->cancel_at;
}

static CmnyBankCsvImportOptions amount_options(char delimiter) {
    CmnyBankCsvImportOptions options;
    cmny_bank_csv_import_options_default(&options);
    options.source.delimiter = delimiter;
    options.mapping.date_column = 0;
    options.mapping.amount_column = 1;
    return options;
}

static bool normalize_document(const void *document, size_t length, size_t chunk,
                               CmnyBankCsvImportOptions *options,
                               RowCapture *rows, DiagnosticCapture *diagnostics,
                               CmnyBankCsvSummary *summary, CmnyBankCsvError *error) {
    MemorySource source = source_for(document, length, chunk);
    return cmny_bank_csv_normalize(memory_read, &source, options,
                                   rows != NULL ? capture_row : NULL, rows,
                                   diagnostics != NULL ? capture_diagnostic : NULL,
                                   diagnostics, NULL, NULL, summary, error);
}

static void test_preview_auto_detection(void) {
    static const unsigned char document[] =
        "\xEF\xBB\xBF" "date;payee;amount\r\n"
        "2026-01-01;\"Cafe, Central\";12,34\r\n"
        "2026-01-02;Market;-5,00\r\n"
        "2026-01-03;Salary;100,00\r\n";
    CmnyBankCsvSourceOptions options;
    cmny_bank_csv_source_options_default(&options);
    options.preview_rows = 2;
    MemorySource source = source_for(document, sizeof(document) - 1, 1);
    PreviewCapture capture = {0};
    CmnyBankCsvPreview preview = {0};
    CmnyBankCsvError error = {0};
    ASSERT_TRUE(cmny_bank_csv_preview(memory_read, &source, &options,
                                      capture_preview, &capture, NULL, NULL,
                                      &preview, &error));
    ASSERT_TRUE(preview.delimiter == ';');
    ASSERT_EQ_I64(3, preview.header_fields);
    ASSERT_EQ_I64(2, preview.previewed_rows);
    ASSERT_EQ_I64(3, capture.count);
    ASSERT_TRUE(capture.headers[0]);
    ASSERT_TRUE(!capture.headers[1]);
    ASSERT_TRUE(strcmp(capture.fields[1][1], "Cafe, Central") == 0);
    ASSERT_TRUE(source.offset < source.length);

    static const unsigned char tab_document[] = "date\tamount\n2026-01-01\t1.00\n";
    source = source_for(tab_document, sizeof(tab_document) - 1, 2);
    options.preview_rows = 1;
    preview = (CmnyBankCsvPreview){0};
    ASSERT_TRUE(cmny_bank_csv_preview(memory_read, &source, &options,
                                      NULL, NULL, NULL, NULL, &preview, &error));
    ASSERT_TRUE(preview.delimiter == '\t');

    static const unsigned char ambiguous[] = "date,amount;memo\n";
    source = source_for(ambiguous, sizeof(ambiguous) - 1, 3);
    preview = (CmnyBankCsvPreview){.delimiter = 'x', .header_fields = 77,
                                   .previewed_rows = 88};
    CmnyBankCsvPreview original = preview;
    ASSERT_TRUE(!cmny_bank_csv_preview(memory_read, &source, &options,
                                       NULL, NULL, NULL, NULL, &preview, &error));
    ASSERT_TRUE(memcmp(&preview, &original, sizeof(preview)) == 0);
    ASSERT_TRUE(error.message != NULL);
}

static void test_european_normalization_and_diagnostics(void) {
    static const unsigned char document[] =
        "\xEF\xBB\xBF" "date;payee;note;amount;id\r\n"
        "31/01/2026;Caf\xC3\xA9;\"first\r\nsecond\";\"-1.234,56\";x1\r\n"
        "29/02/2023;Bad date;;\"1,00\";x2\r\n"
        "01/02/2026;Bad amount;;\"12.34,56\";x3\r\n"
        "02/02/2026;Employer;Salary;\"2.000,00\";x4\r\n"
        "03/02/2026;short;row;1,00\r\n";
    CmnyBankCsvImportOptions options;
    cmny_bank_csv_import_options_default(&options);
    options.date_format = CMNY_BANK_DATE_DMY;
    options.decimal_separator = ',';
    options.thousands_separator = '.';
    options.mapping = (CmnyBankCsvMapping){
        .date_column = 0,
        .amount_column = 3,
        .debit_column = CMNY_BANK_COLUMN_NONE,
        .credit_column = CMNY_BANK_COLUMN_NONE,
        .payee_column = 1,
        .note_column = 2,
        .external_id_column = 4,
    };
    RowCapture rows = {0};
    DiagnosticCapture diagnostics = {0};
    CmnyBankCsvSummary summary = {0};
    CmnyBankCsvError error = {0};
    ASSERT_TRUE(normalize_document(document, sizeof(document) - 1, 1, &options,
                                   &rows, &diagnostics, &summary, &error));
    ASSERT_TRUE(summary.delimiter == ';');
    ASSERT_EQ_I64(5, summary.input_rows);
    ASSERT_EQ_I64(2, summary.normalized_rows);
    ASSERT_EQ_I64(3, summary.skipped_rows);
    ASSERT_EQ_I64(2, rows.count);
    ASSERT_TRUE(strcmp(rows.rows[0].date, "2026-01-31") == 0);
    ASSERT_EQ_I64(-123456, rows.rows[0].amount_cents);
    ASSERT_TRUE(strcmp(rows.rows[0].payee, "Caf\xC3\xA9") == 0);
    ASSERT_TRUE(strcmp(rows.rows[0].note, "first\nsecond") == 0);
    ASSERT_TRUE(strcmp(rows.rows[0].external_id, "x1") == 0);
    ASSERT_EQ_I64(2, rows.rows[0].physical_line);
    ASSERT_EQ_I64(2, rows.rows[0].record_number);
    ASSERT_EQ_I64(200000, rows.rows[1].amount_cents);
    ASSERT_EQ_I64(3, diagnostics.count);
    ASSERT_TRUE(diagnostics.diagnostics[0].code == CMNY_BANK_DIAG_DATE);
    ASSERT_EQ_I64(4, diagnostics.diagnostics[0].physical_line);
    ASSERT_EQ_I64(3, diagnostics.diagnostics[0].record_number);
    ASSERT_TRUE(diagnostics.diagnostics[1].code == CMNY_BANK_DIAG_AMOUNT);
    ASSERT_EQ_I64(5, diagnostics.diagnostics[1].physical_line);
    ASSERT_TRUE(diagnostics.diagnostics[2].code == CMNY_BANK_DIAG_FIELD_COUNT);
    ASSERT_EQ_I64(7, diagnostics.diagnostics[2].physical_line);
}

static void test_split_debit_credit(void) {
    static const unsigned char document[] =
        "date,debit,credit,payee\r\n"
        "01/31/2026,12.34,,Coffee\r\n"
        "02/01/2026,,\"1,234.56\",Salary\r\n"
        "02/02/2026,5.00,2.00,Net debit\r\n"
        "02/03/2026,0.00,0.00,Zero\r\n"
        "02/04/2026,-1.00,,Negative debit\r\n";
    CmnyBankCsvImportOptions options;
    cmny_bank_csv_import_options_default(&options);
    options.source.delimiter = ',';
    options.date_format = CMNY_BANK_DATE_MDY;
    options.thousands_separator = ',';
    options.mapping = (CmnyBankCsvMapping){
        .date_column = 0,
        .amount_column = CMNY_BANK_COLUMN_NONE,
        .debit_column = 1,
        .credit_column = 2,
        .payee_column = 3,
        .note_column = CMNY_BANK_COLUMN_NONE,
        .external_id_column = CMNY_BANK_COLUMN_NONE,
    };
    options.sign_convention = CMNY_BANK_UNSIGNED_OUTFLOW;
    RowCapture rows = {0};
    DiagnosticCapture diagnostics = {0};
    CmnyBankCsvSummary summary = {0};
    CmnyBankCsvError error = {0};
    ASSERT_TRUE(normalize_document(document, sizeof(document) - 1, 2, &options,
                                   &rows, &diagnostics, &summary, &error));
    ASSERT_EQ_I64(5, summary.input_rows);
    ASSERT_EQ_I64(3, summary.normalized_rows);
    ASSERT_EQ_I64(2, summary.skipped_rows);
    ASSERT_EQ_I64(-1234, rows.rows[0].amount_cents);
    ASSERT_EQ_I64(123456, rows.rows[1].amount_cents);
    ASSERT_EQ_I64(-300, rows.rows[2].amount_cents);
    ASSERT_TRUE(diagnostics.diagnostics[0].code == CMNY_BANK_DIAG_AMOUNT);
    ASSERT_TRUE(diagnostics.diagnostics[1].code == CMNY_BANK_DIAG_AMOUNT);
}

static void test_dates_signs_grouping_and_bounds(void) {
    static const unsigned char positive[] =
        "date,amount\n2026-02-28,12.34\n";
    const int64_t expected[] = {1234, -1234, 1234, -1234};
    for (int convention = CMNY_BANK_SIGNED_INFLOW_POSITIVE;
         convention <= CMNY_BANK_UNSIGNED_OUTFLOW; convention++) {
        CmnyBankCsvImportOptions options = amount_options(',');
        options.sign_convention = (CmnyBankSignConvention)convention;
        RowCapture rows = {0};
        CmnyBankCsvSummary summary = {0};
        CmnyBankCsvError error = {0};
        ASSERT_TRUE(normalize_document(positive, sizeof(positive) - 1, 7, &options,
                                       &rows, NULL, &summary, &error));
        ASSERT_EQ_I64(1, rows.count);
        ASSERT_EQ_I64(expected[convention], rows.rows[0].amount_cents);
    }

    static const unsigned char negative[] =
        "date,amount\n2026-02-28,-12.34\n";
    CmnyBankCsvImportOptions options = amount_options(',');
    options.sign_convention = CMNY_BANK_SIGNED_OUTFLOW_POSITIVE;
    RowCapture rows = {0};
    CmnyBankCsvSummary summary = {0};
    CmnyBankCsvError error = {0};
    ASSERT_TRUE(normalize_document(negative, sizeof(negative) - 1, 1, &options,
                                   &rows, NULL, &summary, &error));
    ASSERT_EQ_I64(1234, rows.rows[0].amount_cents);
    options.sign_convention = CMNY_BANK_UNSIGNED_INFLOW;
    rows = (RowCapture){0};
    ASSERT_TRUE(normalize_document(negative, sizeof(negative) - 1, 9, &options,
                                   &rows, NULL, &summary, &error));
    ASSERT_EQ_I64(0, rows.count);
    ASSERT_EQ_I64(1, summary.skipped_rows);

    static const unsigned char amount_edges[] =
        "date,amount\n"
        "2024-02-29,-92233720368547758.08\n"
        "2026-01-01,92233720368547758.08\n"
        "2026-01-02,\"1,234.56\"\n"
        "2026-01-03,\"12,34.56\"\n"
        "2023-02-29,1.00\n";
    options = amount_options(',');
    options.thousands_separator = ',';
    rows = (RowCapture){0};
    DiagnosticCapture diagnostics = {0};
    ASSERT_TRUE(normalize_document(amount_edges, sizeof(amount_edges) - 1, 3,
                                   &options, &rows, &diagnostics, &summary, &error));
    ASSERT_EQ_I64(2, rows.count);
    ASSERT_EQ_I64(INT64_MIN, rows.rows[0].amount_cents);
    ASSERT_EQ_I64(123456, rows.rows[1].amount_cents);
    ASSERT_EQ_I64(3, summary.skipped_rows);

    static const unsigned char mdy[] =
        "date,amount\n02/29/2024,1.00\n29/02/2024,2.00\n";
    options = amount_options(',');
    options.date_format = CMNY_BANK_DATE_MDY;
    rows = (RowCapture){0};
    ASSERT_TRUE(normalize_document(mdy, sizeof(mdy) - 1, 2, &options,
                                   &rows, NULL, &summary, &error));
    ASSERT_EQ_I64(1, rows.count);
    ASSERT_TRUE(strcmp(rows.rows[0].date, "2024-02-29") == 0);
}

static void test_identity_is_stable_and_unambiguous(void) {
    static const unsigned char eu[] =
        "date;payee;note;amount;id\n"
        "31/01/2026;\"A|N1:x\";\"B|X1:y\";\"1.234,56\";same\n";
    static const unsigned char us[] =
        "date,payee,note,amount,id\n"
        "2026-01-31,A|N1:x,B|X1:y,1234.56,same\n";
    CmnyBankCsvImportOptions options;
    cmny_bank_csv_import_options_default(&options);
    options.source.delimiter = ';';
    options.date_format = CMNY_BANK_DATE_DMY;
    options.decimal_separator = ',';
    options.thousands_separator = '.';
    options.mapping = (CmnyBankCsvMapping){
        .date_column = 0, .amount_column = 3,
        .debit_column = CMNY_BANK_COLUMN_NONE,
        .credit_column = CMNY_BANK_COLUMN_NONE,
        .payee_column = 1, .note_column = 2, .external_id_column = 4,
    };
    RowCapture first = {0};
    CmnyBankCsvSummary summary = {0};
    CmnyBankCsvError error = {0};
    ASSERT_TRUE(normalize_document(eu, sizeof(eu) - 1, 1, &options,
                                   &first, NULL, &summary, &error));
    options.source.delimiter = ',';
    options.date_format = CMNY_BANK_DATE_ISO;
    options.decimal_separator = '.';
    options.thousands_separator = '\0';
    RowCapture second = {0};
    ASSERT_TRUE(normalize_document(us, sizeof(us) - 1, 64, &options,
                                   &second, NULL, &summary, &error));
    ASSERT_EQ_I64(1, first.count);
    ASSERT_EQ_I64(1, second.count);
    ASSERT_TRUE(strcmp(first.rows[0].identity, second.rows[0].identity) == 0);
    ASSERT_TRUE(strstr(first.rows[0].identity, "|P6:A|N1:x|N6:B|X1:y") != NULL);
}

static void test_utf8_and_text_bounds(void) {
    static const unsigned char malformed[] = {
        'd','a','t','e',',','a','m','o','u','n','t',',','p','a','y','e','e','\n',
        '2','0','2','6','-','0','1','-','0','1',',','1','.','0','0',',',0xC3U,0x28U,'\n'
    };
    CmnyBankCsvImportOptions options = amount_options(',');
    options.mapping.payee_column = 2;
    RowCapture rows = {0};
    DiagnosticCapture diagnostics = {0};
    CmnyBankCsvSummary summary = {0};
    CmnyBankCsvError error = {0};
    ASSERT_TRUE(normalize_document(malformed, sizeof(malformed), 1, &options,
                                   &rows, &diagnostics, &summary, &error));
    ASSERT_EQ_I64(0, rows.count);
    ASSERT_EQ_I64(1, diagnostics.count);
    ASSERT_TRUE(diagnostics.diagnostics[0].code == CMNY_BANK_DIAG_PAYEE);

    char document[700];
    const char *header = "date,amount,payee\n2026-01-01,1.00,";
    size_t header_length = strlen(header);
    memcpy(document, header, header_length);
    memset(document + header_length, 'p', CMNY_BANK_PAYEE_CAP);
    document[header_length + CMNY_BANK_PAYEE_CAP] = '\n';
    size_t length = header_length + CMNY_BANK_PAYEE_CAP + 1U;
    rows = (RowCapture){0};
    diagnostics = (DiagnosticCapture){0};
    ASSERT_TRUE(normalize_document(document, length, 17, &options,
                                   &rows, &diagnostics, &summary, &error));
    ASSERT_EQ_I64(0, rows.count);
    ASSERT_TRUE(diagnostics.diagnostics[0].code == CMNY_BANK_DIAG_PAYEE);
}

static void test_failures_cancellation_and_unchanged_outputs(void) {
    static const unsigned char document[] =
        "date,amount\n2026-01-01,1.00\n2026-01-02,2.00\n";
    CmnyBankCsvImportOptions options = amount_options(',');
    CmnyBankCsvSummary sentinel = {
        .delimiter = 'x', .input_rows = 11, .normalized_rows = 22, .skipped_rows = 33,
    };
    CmnyBankCsvSummary summary = sentinel;
    CmnyBankCsvError error = {0};
    RowCapture rows = {0};

    options.date_format = (CmnyBankDateFormat)99;
    ASSERT_TRUE(!normalize_document(document, sizeof(document) - 1, 4, &options,
                                    &rows, NULL, &summary, &error));
    ASSERT_TRUE(memcmp(&summary, &sentinel, sizeof(summary)) == 0);
    ASSERT_EQ_I64(0, rows.count);

    options = amount_options(',');
    options.mapping.payee_column = 9;
    rows = (RowCapture){0};
    ASSERT_TRUE(!normalize_document(document, sizeof(document) - 1, 4, &options,
                                    &rows, NULL, &summary, &error));
    ASSERT_TRUE(memcmp(&summary, &sentinel, sizeof(summary)) == 0);
    ASSERT_EQ_I64(1, error.physical_line);
    ASSERT_EQ_I64(1, error.record_number);

    options = amount_options(',');
    options.source.limits.max_records = 2;
    rows = (RowCapture){0};
    ASSERT_TRUE(!normalize_document(document, sizeof(document) - 1, 1, &options,
                                    &rows, NULL, &summary, &error));
    ASSERT_TRUE(memcmp(&summary, &sentinel, sizeof(summary)) == 0);
    ASSERT_TRUE(strstr(error.message, "record count") != NULL);

    options = amount_options(',');
    rows = (RowCapture){.cancel_after = 1};
    ASSERT_TRUE(!normalize_document(document, sizeof(document) - 1, 1, &options,
                                    &rows, NULL, &summary, &error));
    ASSERT_TRUE(memcmp(&summary, &sentinel, sizeof(summary)) == 0);
    ASSERT_EQ_I64(1, rows.count);
    ASSERT_TRUE(strstr(error.message, "cancelled") != NULL);

    MemorySource source = source_for(document, sizeof(document) - 1, 2);
    CancelState cancel = {.cancel_at = 1};
    ASSERT_TRUE(!cmny_bank_csv_normalize(memory_read, &source, &options,
                                         capture_row, &rows, NULL, NULL,
                                         cancel_when_called, &cancel,
                                         &summary, &error));
    ASSERT_EQ_I64(0, source.offset);

    source = (MemorySource){.fail = true, .chunk = 1};
    ASSERT_TRUE(!cmny_bank_csv_normalize(memory_read, &source, &options,
                                         NULL, NULL, NULL, NULL, NULL, NULL,
                                         &summary, &error));
    ASSERT_TRUE(strstr(error.message, "reader") != NULL);

    source = (MemorySource){.violate_contract = true, .chunk = 1};
    ASSERT_TRUE(!cmny_bank_csv_normalize(memory_read, &source, &options,
                                         NULL, NULL, NULL, NULL, NULL, NULL,
                                         &summary, &error));
    ASSERT_TRUE(strstr(error.message, "contract") != NULL);

    static const unsigned char malformed_csv[] =
        "date,amount\n2026-01-01,\"1.00\n";
    ASSERT_TRUE(!normalize_document(malformed_csv, sizeof(malformed_csv) - 1, 1,
                                    &options, NULL, NULL, &summary, &error));
    ASSERT_TRUE(strstr(error.message, "not closed") != NULL);
}

static void test_generated_amount_property(void) {
    for (int major = 1; major <= 120; major++) {
        char document[96];
        int minor = (major * 37) % 100;
        int length = snprintf(document, sizeof(document),
                              "date;amount\n2026-06-15;%d,%02d\n", major, minor);
        ASSERT_TRUE(length > 0 && (size_t)length < sizeof(document));
        CmnyBankCsvImportOptions options = amount_options(';');
        options.decimal_separator = ',';
        RowCapture rows = {0};
        CmnyBankCsvSummary summary = {0};
        CmnyBankCsvError error = {0};
        ASSERT_TRUE(normalize_document(document, (size_t)length,
                                       (size_t)(major % 11 + 1), &options,
                                       &rows, NULL, &summary, &error));
        ASSERT_EQ_I64(1, rows.count);
        ASSERT_EQ_I64((int64_t)major * 100 + minor, rows.rows[0].amount_cents);
    }
}

int main(void) {
    test_preview_auto_detection();
    test_european_normalization_and_diagnostics();
    test_split_debit_credit();
    test_dates_signs_grouping_and_bounds();
    test_identity_is_stable_and_unambiguous();
    test_utf8_and_text_bounds();
    test_failures_cancellation_and_unchanged_outputs();
    test_generated_amount_property();
    (void)printf("ok - bank CSV normalization tests\n");
    return 0;
}
