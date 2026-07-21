#include "cmny_bank_csv.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    CmnyCsvReader upstream;
    void *upstream_context;
    CmnyBankCsvCancelHandler cancel;
    void *cancel_context;
    unsigned char prefix[CMNY_BANK_DELIMITER_DETECT_CAP];
    size_t prefix_length;
    size_t prefix_offset;
    bool upstream_ended;
} ReplayReader;

typedef struct {
    CmnyBankCsvPreviewHandler handler;
    void *handler_context;
    CmnyBankCsvCancelHandler cancel;
    void *cancel_context;
    size_t row_limit;
    CmnyBankCsvPreview result;
    bool saw_header;
    bool callback_cancelled;
    bool callback_failed;
} PreviewContext;

typedef struct {
    uint64_t magnitude;
    bool negative;
} ParsedAmount;

typedef struct {
    const CmnyBankCsvImportOptions *options;
    CmnyBankCsvRowHandler row_handler;
    void *row_context;
    CmnyBankCsvDiagnosticHandler diagnostic_handler;
    void *diagnostic_context;
    CmnyBankCsvCancelHandler cancel;
    void *cancel_context;
    CmnyBankCsvSummary result;
    size_t header_fields;
    bool saw_header;
    bool fatal;
    bool callback_cancelled;
    bool callback_failed;
    size_t fatal_line;
    size_t fatal_record;
    size_t fatal_column;
    const char *fatal_message;
} NormalizeContext;

static void clear_error(CmnyBankCsvError *error) {
    if (error != NULL) *error = (CmnyBankCsvError){.column = CMNY_BANK_COLUMN_NONE};
}

static bool bank_fail(CmnyBankCsvError *error, size_t byte_offset,
                      size_t physical_line, size_t record_number, size_t column,
                      const char *message) {
    if (error != NULL) {
        error->byte_offset = byte_offset;
        error->physical_line = physical_line;
        error->record_number = record_number;
        error->column = column;
        error->message = message;
    }
    return false;
}

static bool source_options_valid(const CmnyBankCsvSourceOptions *options) {
    if (options == NULL ||
        (options->delimiter != '\0' && options->delimiter != ',' &&
         options->delimiter != ';' && options->delimiter != '\t')) {
        return false;
    }
    const CmnyCsvLimits *limits = &options->limits;
    return limits->max_bytes > 0 && limits->max_records > 0 &&
           limits->max_fields > 0 && limits->max_fields <= CMNY_CSV_MAX_FIELDS &&
           limits->max_field_bytes > 0 && limits->max_field_bytes < CMNY_CSV_FIELD_CAP;
}

void cmny_bank_csv_source_options_default(CmnyBankCsvSourceOptions *options) {
    if (options == NULL) return;
    *options = (CmnyBankCsvSourceOptions){0};
    cmny_csv_limits_default(&options->limits);
    options->preview_rows = 5;
}

void cmny_bank_csv_import_options_default(CmnyBankCsvImportOptions *options) {
    if (options == NULL) return;
    *options = (CmnyBankCsvImportOptions){0};
    cmny_bank_csv_source_options_default(&options->source);
    options->mapping = (CmnyBankCsvMapping){
        .date_column = CMNY_BANK_COLUMN_NONE,
        .amount_column = CMNY_BANK_COLUMN_NONE,
        .debit_column = CMNY_BANK_COLUMN_NONE,
        .credit_column = CMNY_BANK_COLUMN_NONE,
        .payee_column = CMNY_BANK_COLUMN_NONE,
        .note_column = CMNY_BANK_COLUMN_NONE,
        .external_id_column = CMNY_BANK_COLUMN_NONE,
    };
    options->date_format = CMNY_BANK_DATE_ISO;
    options->sign_convention = CMNY_BANK_SIGNED_INFLOW_POSITIVE;
    options->decimal_separator = '.';
    options->thousands_separator = '\0';
}

static bool scan_first_record(const unsigned char *bytes, size_t start, size_t length,
                              bool *quoted, bool *after_quote) {
    for (size_t offset = start; offset < length; offset++) {
        unsigned char byte = bytes[offset];
        if (*quoted) {
            if (byte == '"') {
                *quoted = false;
                *after_quote = true;
            }
        } else if (*after_quote) {
            if (byte == '"') {
                *quoted = true;
                *after_quote = false;
            } else if (byte == '\r' || byte == '\n') {
                return true;
            } else {
                *after_quote = false;
            }
        } else if (byte == '"') {
            *quoted = true;
        } else if (byte == '\r' || byte == '\n') {
            return true;
        }
    }
    return false;
}

static bool prepare_reader(CmnyCsvReader reader, void *reader_context,
                           const CmnyBankCsvSourceOptions *options,
                           CmnyBankCsvCancelHandler cancel, void *cancel_context,
                           ReplayReader *replay, char *delimiter,
                           CmnyBankCsvError *error) {
    if (reader == NULL || replay == NULL || delimiter == NULL ||
        !source_options_valid(options)) {
        return bank_fail(error, 0, 0, 0, CMNY_BANK_COLUMN_NONE,
                         "invalid bank CSV source configuration");
    }
    *replay = (ReplayReader){
        .upstream = reader,
        .upstream_context = reader_context,
        .cancel = cancel,
        .cancel_context = cancel_context,
    };
    if (options->delimiter != '\0') {
        *delimiter = options->delimiter;
        return true;
    }

    size_t scanned = 0;
    bool quoted = false;
    bool after_quote = false;
    bool complete = false;
    while (!complete && !replay->upstream_ended) {
        if (cancel != NULL && cancel(cancel_context)) {
            return bank_fail(error, replay->prefix_length, 1, 1,
                             CMNY_BANK_COLUMN_NONE, "bank CSV operation was cancelled");
        }
        size_t capacity = sizeof(replay->prefix) - replay->prefix_length;
        if (capacity == 0 || replay->prefix_length >= options->limits.max_bytes) {
            return bank_fail(error, replay->prefix_length, 1, 1,
                             CMNY_BANK_COLUMN_NONE,
                             "bank CSV header exceeds delimiter detection limits");
        }
        size_t byte_limit = options->limits.max_bytes - replay->prefix_length;
        if (capacity > byte_limit) capacity = byte_limit;
        size_t amount = 0;
        CmnyCsvReadStatus status = reader(reader_context,
                                          replay->prefix + replay->prefix_length,
                                          capacity, &amount);
        if (status == CMNY_CSV_READ_CANCELLED) {
            return bank_fail(error, replay->prefix_length, 1, 1,
                             CMNY_BANK_COLUMN_NONE, "bank CSV reading was cancelled");
        }
        if (status == CMNY_CSV_READ_ERROR) {
            return bank_fail(error, replay->prefix_length, 1, 1,
                             CMNY_BANK_COLUMN_NONE, "bank CSV reader failed");
        }
        if (status == CMNY_CSV_READ_END) {
            if (amount != 0) {
                return bank_fail(error, replay->prefix_length, 1, 1,
                                 CMNY_BANK_COLUMN_NONE, "bank CSV reader contract violation");
            }
            replay->upstream_ended = true;
            break;
        }
        if (status != CMNY_CSV_READ_DATA || amount == 0 || amount > capacity) {
            return bank_fail(error, replay->prefix_length, 1, 1,
                             CMNY_BANK_COLUMN_NONE, "bank CSV reader contract violation");
        }
        replay->prefix_length += amount;
        complete = scan_first_record(replay->prefix, scanned, replay->prefix_length,
                                     &quoted, &after_quote);
        scanned = replay->prefix_length;
    }
    CmnyCsvParseError detect_error = {0};
    char detected = '\0';
    if (!cmny_csv_detect_delimiter(replay->prefix, replay->prefix_length,
                                   &detected, &detect_error)) {
        return bank_fail(error, detect_error.byte_offset, 1, 1,
                         CMNY_BANK_COLUMN_NONE,
                         detect_error.message != NULL ? detect_error.message
                                                      : "cannot detect bank CSV delimiter");
    }
    *delimiter = detected;
    return true;
}

static CmnyCsvReadStatus replay_read(void *context, unsigned char *buffer,
                                     size_t capacity, size_t *length) {
    ReplayReader *reader = context;
    if (reader->cancel != NULL && reader->cancel(reader->cancel_context)) {
        *length = 0;
        return CMNY_CSV_READ_CANCELLED;
    }
    if (reader->prefix_offset < reader->prefix_length) {
        size_t remaining = reader->prefix_length - reader->prefix_offset;
        size_t amount = remaining < capacity ? remaining : capacity;
        memcpy(buffer, reader->prefix + reader->prefix_offset, amount);
        reader->prefix_offset += amount;
        *length = amount;
        return CMNY_CSV_READ_DATA;
    }
    if (reader->upstream_ended) {
        *length = 0;
        return CMNY_CSV_READ_END;
    }
    return reader->upstream(reader->upstream_context, buffer, capacity, length);
}

static void error_from_csv(CmnyBankCsvError *error, const CmnyCsvParseError *csv_error) {
    if (error == NULL) return;
    *error = (CmnyBankCsvError){
        .byte_offset = csv_error->byte_offset,
        .physical_line = csv_error->physical_line,
        .record_number = csv_error->record_number,
        .column = CMNY_BANK_COLUMN_NONE,
        .message = csv_error->message,
    };
}

static CmnyCsvVisitResult preview_record(const CmnyCsvRecord *record, void *context) {
    PreviewContext *preview = context;
    if (preview->cancel != NULL && preview->cancel(preview->cancel_context)) {
        preview->callback_cancelled = true;
        return CMNY_CSV_VISIT_CANCEL;
    }
    bool header = !preview->saw_header;
    if (header) {
        preview->saw_header = true;
        preview->result.header_fields = record->field_count;
    }
    if (preview->handler != NULL) {
        CmnyBankCsvAction action = preview->handler(record, header, preview->handler_context);
        if (action == CMNY_BANK_CANCEL) {
            preview->callback_cancelled = true;
            return CMNY_CSV_VISIT_CANCEL;
        }
        if (action != CMNY_BANK_CONTINUE) {
            preview->callback_failed = true;
            return CMNY_CSV_VISIT_ERROR;
        }
    }
    if (header) return preview->row_limit == 0 ? CMNY_CSV_VISIT_STOP
                                               : CMNY_CSV_VISIT_CONTINUE;
    preview->result.previewed_rows++;
    return preview->result.previewed_rows >= preview->row_limit
               ? CMNY_CSV_VISIT_STOP
               : CMNY_CSV_VISIT_CONTINUE;
}

bool cmny_bank_csv_preview(CmnyCsvReader reader, void *reader_context,
                           const CmnyBankCsvSourceOptions *options,
                           CmnyBankCsvPreviewHandler handler, void *handler_context,
                           CmnyBankCsvCancelHandler cancel, void *cancel_context,
                           CmnyBankCsvPreview *preview, CmnyBankCsvError *error) {
    clear_error(error);
    if (preview == NULL) {
        return bank_fail(error, 0, 0, 0, CMNY_BANK_COLUMN_NONE,
                         "bank CSV preview output is required");
    }
    ReplayReader replay;
    char delimiter = '\0';
    if (!prepare_reader(reader, reader_context, options, cancel, cancel_context,
                        &replay, &delimiter, error)) {
        return false;
    }
    PreviewContext context = {
        .handler = handler,
        .handler_context = handler_context,
        .cancel = cancel,
        .cancel_context = cancel_context,
        .row_limit = options->preview_rows,
    };
    context.result.delimiter = delimiter;
    CmnyCsvParseError csv_error = {0};
    if (!cmny_csv_parse_stream(replay_read, &replay, delimiter, &options->limits,
                               preview_record, &context, &csv_error)) {
        if (context.callback_cancelled) {
            return bank_fail(error, csv_error.byte_offset, csv_error.physical_line,
                             csv_error.record_number, CMNY_BANK_COLUMN_NONE,
                             "bank CSV preview was cancelled");
        }
        if (context.callback_failed) {
            return bank_fail(error, csv_error.byte_offset, csv_error.physical_line,
                             csv_error.record_number, CMNY_BANK_COLUMN_NONE,
                             "bank CSV preview callback failed");
        }
        error_from_csv(error, &csv_error);
        return false;
    }
    if (!context.saw_header) {
        return bank_fail(error, 0, 1, 1, CMNY_BANK_COLUMN_NONE,
                         "bank CSV has no header record");
    }
    *preview = context.result;
    return true;
}

static bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int month_days(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    return month == 2 && leap_year(year) ? 29 : days[month - 1];
}

static bool parse_two_digits(const char *text, int *value) {
    if (text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9') return false;
    *value = (text[0] - '0') * 10 + (text[1] - '0');
    return true;
}

static bool parse_four_digits(const char *text, int *value) {
    int high = 0;
    int low = 0;
    if (!parse_two_digits(text, &high) || !parse_two_digits(text + 2, &low)) return false;
    *value = high * 100 + low;
    return true;
}

static bool normalize_date(const char *text, CmnyBankDateFormat format, char out[11]) {
    if (strlen(text) != 10) return false;
    int year = 0;
    int month = 0;
    int day = 0;
    if (format == CMNY_BANK_DATE_ISO) {
        if (text[4] != '-' || text[7] != '-' || !parse_four_digits(text, &year) ||
            !parse_two_digits(text + 5, &month) || !parse_two_digits(text + 8, &day)) {
            return false;
        }
    } else if (format == CMNY_BANK_DATE_DMY || format == CMNY_BANK_DATE_MDY) {
        int first = 0;
        int second = 0;
        if (text[2] != '/' || text[5] != '/' || !parse_two_digits(text, &first) ||
            !parse_two_digits(text + 3, &second) || !parse_four_digits(text + 6, &year)) {
            return false;
        }
        day = format == CMNY_BANK_DATE_DMY ? first : second;
        month = format == CMNY_BANK_DATE_DMY ? second : first;
    } else {
        return false;
    }
    if (year < 1 || year > 9999 || month < 1 || month > 12 ||
        day < 1 || day > month_days(year, month)) {
        return false;
    }
    int written = snprintf(out, 11, "%04d-%02d-%02d", year, month, day);
    return written == 10;
}

static void trim_ascii(const char *text, const char **start, size_t *length) {
    const char *begin = text;
    while (*begin == ' ' || *begin == '\t') begin++;
    const char *end = text + strlen(text);
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) end--;
    *start = begin;
    *length = (size_t)(end - begin);
}

static bool parse_amount_text(const char *text, char decimal_separator,
                              char thousands_separator, ParsedAmount *amount,
                              bool allow_empty, bool *empty) {
    const char *start = NULL;
    size_t length = 0;
    trim_ascii(text, &start, &length);
    *empty = length == 0;
    if (*empty) return allow_empty;

    size_t position = 0;
    bool negative = false;
    if (start[position] == '+' || start[position] == '-') {
        negative = start[position] == '-';
        position++;
        if (position == length) return false;
    }
    size_t decimal_position = length;
    for (size_t i = position; i < length; i++) {
        if (start[i] == decimal_separator) {
            if (decimal_position != length) return false;
            decimal_position = i;
        }
    }
    if (decimal_position == position) return false;
    size_t integer_end = decimal_position;
    uint64_t whole = 0;
    size_t group_digits = 0;
    bool grouped = false;
    for (size_t i = position; i < integer_end; i++) {
        char value = start[i];
        if (value >= '0' && value <= '9') {
            unsigned digit = (unsigned)(value - '0');
            if (whole > (UINT64_MAX - digit) / 10U) return false;
            whole = whole * 10U + digit;
            group_digits++;
        } else if (thousands_separator != '\0' && value == thousands_separator) {
            if (group_digits == 0 || (!grouped && group_digits > 3) ||
                (grouped && group_digits != 3)) {
                return false;
            }
            grouped = true;
            group_digits = 0;
        } else {
            return false;
        }
    }
    if (group_digits == 0 || (grouped && group_digits != 3)) return false;

    uint64_t fraction = 0;
    if (decimal_position < length) {
        size_t fraction_length = length - decimal_position - 1U;
        if (fraction_length < 1 || fraction_length > 2) return false;
        for (size_t i = decimal_position + 1U; i < length; i++) {
            if (start[i] < '0' || start[i] > '9') return false;
        }
        fraction = (uint64_t)(start[decimal_position + 1U] - '0') * 10U;
        if (fraction_length == 2) {
            fraction += (uint64_t)(start[decimal_position + 2U] - '0');
        }
    }
    uint64_t limit = (uint64_t)INT64_MAX + 1U;
    if (whole > (limit - fraction) / 100U) return false;
    amount->magnitude = whole * 100U + fraction;
    amount->negative = negative;
    return true;
}

static bool signed_cents(uint64_t magnitude, bool negative, int64_t *value) {
    if (negative) {
        if (magnitude > (uint64_t)INT64_MAX + 1U) return false;
        *value = magnitude == (uint64_t)INT64_MAX + 1U
                     ? INT64_MIN
                     : -(int64_t)magnitude;
        return true;
    }
    if (magnitude > (uint64_t)INT64_MAX) return false;
    *value = (int64_t)magnitude;
    return true;
}

static bool normalize_single_amount(const char *text,
                                    const CmnyBankCsvImportOptions *options,
                                    int64_t *cents) {
    ParsedAmount amount = {0};
    bool empty = false;
    if (!parse_amount_text(text, options->decimal_separator,
                           options->thousands_separator, &amount, false, &empty) ||
        empty || amount.magnitude == 0) {
        return false;
    }
    bool negative = amount.negative;
    if (options->sign_convention == CMNY_BANK_SIGNED_OUTFLOW_POSITIVE) {
        negative = !negative;
    } else if (options->sign_convention == CMNY_BANK_UNSIGNED_INFLOW ||
               options->sign_convention == CMNY_BANK_UNSIGNED_OUTFLOW) {
        if (amount.negative) return false;
        negative = options->sign_convention == CMNY_BANK_UNSIGNED_OUTFLOW;
    } else if (options->sign_convention != CMNY_BANK_SIGNED_INFLOW_POSITIVE) {
        return false;
    }
    return signed_cents(amount.magnitude, negative, cents);
}

static bool normalize_split_amount(const char *debit_text, const char *credit_text,
                                   const CmnyBankCsvImportOptions *options,
                                   int64_t *cents) {
    ParsedAmount debit = {0};
    ParsedAmount credit = {0};
    bool debit_empty = false;
    bool credit_empty = false;
    if (!parse_amount_text(debit_text, options->decimal_separator,
                           options->thousands_separator, &debit, true, &debit_empty) ||
        !parse_amount_text(credit_text, options->decimal_separator,
                           options->thousands_separator, &credit, true, &credit_empty) ||
        debit.negative || credit.negative || (debit_empty && credit_empty) ||
        debit.magnitude > (uint64_t)INT64_MAX ||
        credit.magnitude > (uint64_t)INT64_MAX) {
        return false;
    }
    int64_t debit_value = (int64_t)debit.magnitude;
    int64_t credit_value = (int64_t)credit.magnitude;
    *cents = credit_value - debit_value;
    return *cents != 0;
}

static bool valid_utf8(const unsigned char *text, size_t length) {
    size_t position = 0;
    while (position < length) {
        unsigned char first = text[position++];
        if (first <= 0x7FU) continue;
        size_t continuation = 0;
        uint32_t codepoint = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation = 1;
            codepoint = (uint32_t)(first & 0x1FU);
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation = 2;
            codepoint = (uint32_t)(first & 0x0FU);
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation = 3;
            codepoint = (uint32_t)(first & 0x07U);
        } else {
            return false;
        }
        if (continuation > length - position) return false;
        for (size_t i = 0; i < continuation; i++) {
            unsigned char next = text[position++];
            if ((next & 0xC0U) != 0x80U) return false;
            codepoint = codepoint << 6U | (uint32_t)(next & 0x3FU);
        }
        if ((continuation == 2 && codepoint < 0x800U) ||
            (continuation == 3 && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || codepoint > 0x10FFFFU) {
            return false;
        }
    }
    return true;
}

static bool normalize_text(const char *text, char *out, size_t capacity) {
    const char *start = NULL;
    size_t length = 0;
    trim_ascii(text, &start, &length);
    if (length >= capacity || !valid_utf8((const unsigned char *)start, length)) return false;
    memcpy(out, start, length);
    out[length] = '\0';
    return true;
}

static bool append_identity(char *identity, size_t capacity, size_t *used,
                            char label, const char *value, size_t length) {
    int written = snprintf(identity + *used, capacity - *used, "|%c%zu:", label, length);
    if (written < 0 || (size_t)written >= capacity - *used) return false;
    *used += (size_t)written;
    if (length >= capacity - *used) return false;
    memcpy(identity + *used, value, length);
    *used += length;
    identity[*used] = '\0';
    return true;
}

static bool build_identity(CmnyBankCsvRow *row) {
    (void)snprintf(row->identity, sizeof(row->identity), "v1");
    size_t used = 2;
    char amount[32];
    int written = snprintf(amount, sizeof(amount), "%" PRId64, row->amount_cents);
    if (written < 0 || (size_t)written >= sizeof(amount)) return false;
    return append_identity(row->identity, sizeof(row->identity), &used,
                           'D', row->date, strlen(row->date)) &&
           append_identity(row->identity, sizeof(row->identity), &used,
                           'A', amount, (size_t)written) &&
           append_identity(row->identity, sizeof(row->identity), &used,
                           'P', row->payee, strlen(row->payee)) &&
           append_identity(row->identity, sizeof(row->identity), &used,
                           'N', row->note, strlen(row->note)) &&
           append_identity(row->identity, sizeof(row->identity), &used,
                           'X', row->external_id, strlen(row->external_id));
}

static bool separator_configuration_valid(const CmnyBankCsvImportOptions *options) {
    bool decimal_ok = options->decimal_separator == '.' || options->decimal_separator == ',';
    bool thousands_ok = options->thousands_separator == '\0' ||
                        options->thousands_separator == '.' ||
                        options->thousands_separator == ',' ||
                        options->thousands_separator == ' ' ||
                        options->thousands_separator == '\'';
    bool date_ok = options->date_format == CMNY_BANK_DATE_ISO ||
                   options->date_format == CMNY_BANK_DATE_DMY ||
                   options->date_format == CMNY_BANK_DATE_MDY;
    bool sign_ok = options->sign_convention == CMNY_BANK_SIGNED_INFLOW_POSITIVE ||
                   options->sign_convention == CMNY_BANK_SIGNED_OUTFLOW_POSITIVE ||
                   options->sign_convention == CMNY_BANK_UNSIGNED_INFLOW ||
                   options->sign_convention == CMNY_BANK_UNSIGNED_OUTFLOW;
    return decimal_ok && thousands_ok && date_ok && sign_ok &&
           options->decimal_separator != options->thousands_separator;
}

static bool mapping_valid(const CmnyBankCsvMapping *mapping, size_t field_count) {
    bool amount_mode = mapping->amount_column != CMNY_BANK_COLUMN_NONE &&
                       mapping->debit_column == CMNY_BANK_COLUMN_NONE &&
                       mapping->credit_column == CMNY_BANK_COLUMN_NONE;
    bool split_mode = mapping->amount_column == CMNY_BANK_COLUMN_NONE &&
                      mapping->debit_column != CMNY_BANK_COLUMN_NONE &&
                      mapping->credit_column != CMNY_BANK_COLUMN_NONE;
    if (mapping->date_column == CMNY_BANK_COLUMN_NONE || (!amount_mode && !split_mode)) {
        return false;
    }
    size_t columns[] = {
        mapping->date_column, mapping->amount_column, mapping->debit_column,
        mapping->credit_column, mapping->payee_column, mapping->note_column,
        mapping->external_id_column,
    };
    size_t count = sizeof(columns) / sizeof(columns[0]);
    for (size_t i = 0; i < count; i++) {
        if (columns[i] == CMNY_BANK_COLUMN_NONE) continue;
        if (columns[i] >= field_count) return false;
        for (size_t j = i + 1U; j < count; j++) {
            if (columns[i] == columns[j]) return false;
        }
    }
    return true;
}

static void emit_diagnostic(NormalizeContext *context, const CmnyCsvRecord *record,
                            CmnyBankCsvDiagnosticCode code, size_t column,
                            const char *message) {
    if (context->diagnostic_handler == NULL) return;
    CmnyBankCsvDiagnostic diagnostic = {
        .code = code,
        .physical_line = record->physical_line_start,
        .record_number = record->record_number,
        .column = column,
        .message = message,
    };
    context->diagnostic_handler(&diagnostic, context->diagnostic_context);
}

static CmnyCsvVisitResult skip_row(NormalizeContext *context,
                                   const CmnyCsvRecord *record,
                                   CmnyBankCsvDiagnosticCode code, size_t column,
                                   const char *message) {
    context->result.skipped_rows++;
    emit_diagnostic(context, record, code, column, message);
    return CMNY_CSV_VISIT_CONTINUE;
}

static void set_fatal(NormalizeContext *context, const CmnyCsvRecord *record,
                      size_t column, const char *message) {
    context->fatal = true;
    context->fatal_line = record->physical_line_start;
    context->fatal_record = record->record_number;
    context->fatal_column = column;
    context->fatal_message = message;
}

static CmnyCsvVisitResult normalize_record(const CmnyCsvRecord *record, void *opaque) {
    NormalizeContext *context = opaque;
    if (context->cancel != NULL && context->cancel(context->cancel_context)) {
        context->callback_cancelled = true;
        return CMNY_CSV_VISIT_CANCEL;
    }
    if (!context->saw_header) {
        context->saw_header = true;
        context->header_fields = record->field_count;
        if (!mapping_valid(&context->options->mapping, record->field_count)) {
            set_fatal(context, record, CMNY_BANK_COLUMN_NONE,
                      "bank CSV column mapping does not match the header");
            return CMNY_CSV_VISIT_ERROR;
        }
        return CMNY_CSV_VISIT_CONTINUE;
    }

    context->result.input_rows++;
    if (record->field_count != context->header_fields) {
        return skip_row(context, record, CMNY_BANK_DIAG_FIELD_COUNT,
                        CMNY_BANK_COLUMN_NONE,
                        "row field count does not match the header");
    }
    const CmnyBankCsvMapping *mapping = &context->options->mapping;
    CmnyBankCsvRow row = {
        .physical_line = record->physical_line_start,
        .record_number = record->record_number,
    };
    if (!normalize_date(record->fields[mapping->date_column],
                        context->options->date_format, row.date)) {
        return skip_row(context, record, CMNY_BANK_DIAG_DATE, mapping->date_column,
                        "date is invalid for the configured format");
    }
    bool amount_ok = false;
    size_t amount_column = mapping->amount_column;
    if (amount_column != CMNY_BANK_COLUMN_NONE) {
        amount_ok = normalize_single_amount(record->fields[amount_column],
                                            context->options, &row.amount_cents);
    } else {
        amount_column = mapping->debit_column;
        amount_ok = normalize_split_amount(record->fields[mapping->debit_column],
                                           record->fields[mapping->credit_column],
                                           context->options, &row.amount_cents);
    }
    if (!amount_ok) {
        return skip_row(context, record, CMNY_BANK_DIAG_AMOUNT, amount_column,
                        "amount is invalid for the configured separators and sign convention");
    }
    if (mapping->payee_column != CMNY_BANK_COLUMN_NONE &&
        !normalize_text(record->fields[mapping->payee_column],
                        row.payee, sizeof(row.payee))) {
        return skip_row(context, record, CMNY_BANK_DIAG_PAYEE, mapping->payee_column,
                        "payee is too long or is not valid UTF-8");
    }
    if (mapping->note_column != CMNY_BANK_COLUMN_NONE &&
        !normalize_text(record->fields[mapping->note_column],
                        row.note, sizeof(row.note))) {
        return skip_row(context, record, CMNY_BANK_DIAG_NOTE, mapping->note_column,
                        "note is too long or is not valid UTF-8");
    }
    if (mapping->external_id_column != CMNY_BANK_COLUMN_NONE &&
        !normalize_text(record->fields[mapping->external_id_column],
                        row.external_id, sizeof(row.external_id))) {
        return skip_row(context, record, CMNY_BANK_DIAG_EXTERNAL_ID,
                        mapping->external_id_column,
                        "external ID is too long or is not valid UTF-8");
    }
    if (!build_identity(&row)) {
        set_fatal(context, record, CMNY_BANK_COLUMN_NONE,
                  "normalized duplicate identity exceeds its fixed bound");
        return CMNY_CSV_VISIT_ERROR;
    }
    if (context->row_handler != NULL) {
        CmnyBankCsvAction action = context->row_handler(&row, context->row_context);
        if (action == CMNY_BANK_CANCEL) {
            context->callback_cancelled = true;
            return CMNY_CSV_VISIT_CANCEL;
        }
        if (action != CMNY_BANK_CONTINUE) {
            context->callback_failed = true;
            return CMNY_CSV_VISIT_ERROR;
        }
    }
    context->result.normalized_rows++;
    return CMNY_CSV_VISIT_CONTINUE;
}

bool cmny_bank_csv_normalize(CmnyCsvReader reader, void *reader_context,
                             const CmnyBankCsvImportOptions *options,
                             CmnyBankCsvRowHandler row_handler, void *row_context,
                             CmnyBankCsvDiagnosticHandler diagnostic_handler,
                             void *diagnostic_context,
                             CmnyBankCsvCancelHandler cancel, void *cancel_context,
                             CmnyBankCsvSummary *summary, CmnyBankCsvError *error) {
    clear_error(error);
    if (summary == NULL || options == NULL || !source_options_valid(&options->source) ||
        !separator_configuration_valid(options)) {
        return bank_fail(error, 0, 0, 0, CMNY_BANK_COLUMN_NONE,
                         "invalid bank CSV normalization configuration");
    }
    ReplayReader replay;
    char delimiter = '\0';
    if (!prepare_reader(reader, reader_context, &options->source, cancel, cancel_context,
                        &replay, &delimiter, error)) {
        return false;
    }
    NormalizeContext context = {
        .options = options,
        .row_handler = row_handler,
        .row_context = row_context,
        .diagnostic_handler = diagnostic_handler,
        .diagnostic_context = diagnostic_context,
        .cancel = cancel,
        .cancel_context = cancel_context,
        .fatal_column = CMNY_BANK_COLUMN_NONE,
    };
    context.result.delimiter = delimiter;
    CmnyCsvParseError csv_error = {0};
    if (!cmny_csv_parse_stream(replay_read, &replay, delimiter, &options->source.limits,
                               normalize_record, &context, &csv_error)) {
        if (context.fatal) {
            return bank_fail(error, csv_error.byte_offset, context.fatal_line,
                             context.fatal_record, context.fatal_column,
                             context.fatal_message);
        }
        if (context.callback_cancelled) {
            return bank_fail(error, csv_error.byte_offset, csv_error.physical_line,
                             csv_error.record_number, CMNY_BANK_COLUMN_NONE,
                             "bank CSV normalization was cancelled");
        }
        if (context.callback_failed) {
            return bank_fail(error, csv_error.byte_offset, csv_error.physical_line,
                             csv_error.record_number, CMNY_BANK_COLUMN_NONE,
                             "bank CSV row callback failed");
        }
        error_from_csv(error, &csv_error);
        return false;
    }
    if (!context.saw_header) {
        return bank_fail(error, 0, 1, 1, CMNY_BANK_COLUMN_NONE,
                         "bank CSV has no header record");
    }
    *summary = context.result;
    return true;
}
