#include "cmny_csv_parser.h"

#include <string.h>

typedef enum {
    FIELD_START,
    FIELD_UNQUOTED,
    FIELD_QUOTED,
    FIELD_AFTER_QUOTE
} FieldState;

typedef struct {
    CmnyCsvRecord record;
    CmnyCsvLimits limits;
    CmnyCsvRecordHandler handler;
    void *handler_context;
    size_t field;
    size_t used;
    size_t byte_offset;
    size_t physical_line;
    size_t records_emitted;
    char delimiter;
    FieldState state;
    bool record_started;
    bool skip_lf;
    bool cr_ended_record;
    bool stopped;
} Parser;

typedef struct {
    const unsigned char *bytes;
    size_t length;
    size_t offset;
} MemoryReader;

typedef struct {
    CmnyCsvRecordVisitor visitor;
    void *context;
} VisitorAdapter;

static bool fail(CmnyCsvParseError *error, size_t offset, const Parser *parser,
                 const char *message) {
    if (error != NULL) {
        error->byte_offset = offset;
        error->physical_line = parser != NULL ? parser->physical_line : 0;
        error->record_number = parser != NULL ? parser->record.record_number : 0;
        error->message = message;
    }
    return false;
}

void cmny_csv_limits_default(CmnyCsvLimits *limits) {
    if (limits == NULL) return;
    limits->max_bytes = CMNY_CSV_DEFAULT_MAX_BYTES;
    limits->max_records = CMNY_CSV_DEFAULT_MAX_RECORDS;
    limits->max_fields = CMNY_CSV_MAX_FIELDS;
    limits->max_field_bytes = CMNY_CSV_FIELD_CAP - 1U;
}

static bool limits_valid(const CmnyCsvLimits *limits) {
    return limits != NULL && limits->max_bytes > 0 && limits->max_records > 0 &&
           limits->max_fields > 0 && limits->max_fields <= CMNY_CSV_MAX_FIELDS &&
           limits->max_field_bytes > 0 &&
           limits->max_field_bytes < CMNY_CSV_FIELD_CAP;
}

static bool append_byte(Parser *parser, unsigned char byte, size_t offset,
                        CmnyCsvParseError *error) {
    if (parser->used >= parser->limits.max_field_bytes) {
        return fail(error, offset, parser, "CSV field exceeds the supported length");
    }
    parser->record.fields[parser->field][parser->used++] = (char)byte;
    return true;
}

static bool finish_field(Parser *parser, size_t offset, CmnyCsvParseError *error) {
    if (parser->field >= parser->limits.max_fields) {
        return fail(error, offset, parser, "CSV record has too many fields");
    }
    parser->record.fields[parser->field][parser->used] = '\0';
    parser->record.field_count = parser->field + 1U;
    return true;
}

static bool next_field(Parser *parser, size_t offset, CmnyCsvParseError *error) {
    if (!finish_field(parser, offset, error)) return false;
    parser->field++;
    parser->used = 0;
    if (parser->field >= parser->limits.max_fields) {
        return fail(error, offset, parser, "CSV record has too many fields");
    }
    parser->record.fields[parser->field][0] = '\0';
    parser->state = FIELD_START;
    parser->record_started = true;
    return true;
}

static bool emit_record(Parser *parser, size_t offset, CmnyCsvParseError *error) {
    if (parser->records_emitted >= parser->limits.max_records) {
        return fail(error, offset, parser, "CSV exceeds the supported record count");
    }
    if (!finish_field(parser, offset, error)) return false;
    parser->record.physical_line_end = parser->physical_line;
    if (parser->handler != NULL) {
        CmnyCsvVisitResult result = parser->handler(&parser->record, parser->handler_context);
        if (result == CMNY_CSV_VISIT_STOP) parser->stopped = true;
        else if (result == CMNY_CSV_VISIT_CANCEL) {
            return fail(error, offset, parser, "CSV parsing was cancelled");
        } else if (result == CMNY_CSV_VISIT_ERROR) {
            return fail(error, offset, parser, "CSV handler rejected the record");
        } else if (result != CMNY_CSV_VISIT_CONTINUE) {
            return fail(error, offset, parser, "CSV handler returned an invalid result");
        }
    }
    parser->records_emitted++;
    parser->record.record_number++;
    parser->record.field_count = 0;
    parser->field = 0;
    parser->used = 0;
    parser->record.fields[0][0] = '\0';
    parser->state = FIELD_START;
    parser->record_started = false;
    return true;
}

static bool process_newline(Parser *parser, unsigned char byte, size_t offset,
                            CmnyCsvParseError *error) {
    bool quoted_newline = parser->state == FIELD_QUOTED;
    if (quoted_newline) {
        if (!append_byte(parser, '\n', offset, error)) return false;
    } else {
        if (!emit_record(parser, offset, error)) return false;
    }
    parser->physical_line++;
    parser->skip_lf = byte == '\r';
    parser->cr_ended_record = byte == '\r' && !quoted_newline;
    if (!quoted_newline) {
        parser->record.physical_line_start = parser->physical_line;
        parser->record.byte_offset_start = offset + 1U;
    }
    return true;
}

static bool process_byte(Parser *parser, unsigned char byte, size_t offset,
                         CmnyCsvParseError *error) {
    if (parser->skip_lf) {
        parser->skip_lf = false;
        if (byte == '\n') {
            if (parser->cr_ended_record) parser->record.byte_offset_start = offset + 1U;
            parser->cr_ended_record = false;
            return true;
        }
        parser->cr_ended_record = false;
    }
    if (byte == '\0') return fail(error, offset, parser, "CSV contains a NUL byte");

    if (parser->state == FIELD_QUOTED) {
        if (byte == '"') {
            parser->state = FIELD_AFTER_QUOTE;
            return true;
        }
        if (byte == '\r' || byte == '\n') {
            return process_newline(parser, byte, offset, error);
        }
        return append_byte(parser, byte, offset, error);
    }

    if (parser->state == FIELD_AFTER_QUOTE) {
        if (byte == '"') {
            if (!append_byte(parser, '"', offset, error)) return false;
            parser->state = FIELD_QUOTED;
            return true;
        }
        if ((char)byte == parser->delimiter) return next_field(parser, offset, error);
        if (byte == '\r' || byte == '\n') {
            return process_newline(parser, byte, offset, error);
        }
        return fail(error, offset, parser, "unexpected text after a closing quote");
    }

    if ((char)byte == parser->delimiter) return next_field(parser, offset, error);
    if (byte == '\r' || byte == '\n') return process_newline(parser, byte, offset, error);
    if (byte == '"') {
        if (parser->state != FIELD_START || parser->used != 0) {
            return fail(error, offset, parser, "quote appears inside an unquoted CSV field");
        }
        parser->state = FIELD_QUOTED;
        parser->record_started = true;
        return true;
    }
    if (!append_byte(parser, byte, offset, error)) return false;
    parser->state = FIELD_UNQUOTED;
    parser->record_started = true;
    return true;
}

static bool process_initial_bytes(Parser *parser, const unsigned char bytes[3],
                                  size_t count, CmnyCsvParseError *error) {
    size_t start = count == 3 && bytes[0] == 0xEFU && bytes[1] == 0xBBU &&
                           bytes[2] == 0xBFU
                       ? 3U : 0U;
    if (start == 3U) {
        parser->record.byte_offset_start = 3;
        return true;
    }
    for (size_t i = 0; i < count; i++) {
        if (!process_byte(parser, bytes[i], i, error)) return false;
        if (parser->stopped) return true;
    }
    return true;
}

bool cmny_csv_parse_stream(CmnyCsvReader reader, void *reader_context,
                           char delimiter, const CmnyCsvLimits *limits,
                           CmnyCsvRecordHandler handler, void *handler_context,
                           CmnyCsvParseError *error) {
    if (error != NULL) *error = (CmnyCsvParseError){0};
    CmnyCsvLimits effective_limits;
    if (limits == NULL) {
        cmny_csv_limits_default(&effective_limits);
        limits = &effective_limits;
    }
    if (reader == NULL || delimiter == '\0' || delimiter == '"' ||
        delimiter == '\r' || delimiter == '\n' || !limits_valid(limits)) {
        return fail(error, 0, NULL, "invalid CSV parser input");
    }

    Parser parser = {0};
    parser.limits = *limits;
    parser.handler = handler;
    parser.handler_context = handler_context;
    parser.delimiter = delimiter;
    parser.physical_line = 1;
    parser.record.record_number = 1;
    parser.record.physical_line_start = 1;

    unsigned char initial[3];
    size_t initial_count = 0;
    bool initial_processed = false;
    unsigned char buffer[CMNY_CSV_STREAM_CHUNK];
    for (;;) {
        size_t length = 0;
        CmnyCsvReadStatus status = reader(reader_context, buffer, sizeof(buffer), &length);
        if (status == CMNY_CSV_READ_CANCELLED) {
            return fail(error, parser.byte_offset, &parser, "CSV reading was cancelled");
        }
        if (status == CMNY_CSV_READ_ERROR) {
            return fail(error, parser.byte_offset, &parser, "CSV reader failed");
        }
        if (status == CMNY_CSV_READ_END) {
            if (length != 0) return fail(error, parser.byte_offset, &parser, "CSV reader contract violation");
            if (!initial_processed &&
                !process_initial_bytes(&parser, initial, initial_count, error)) {
                return false;
            }
            break;
        }
        if (status != CMNY_CSV_READ_DATA || length == 0 || length > sizeof(buffer)) {
            return fail(error, parser.byte_offset, &parser, "CSV reader contract violation");
        }
        if (length > parser.limits.max_bytes - parser.byte_offset) {
            return fail(error, parser.byte_offset, &parser, "CSV exceeds the supported byte count");
        }
        for (size_t i = 0; i < length; i++) {
            if (!initial_processed) {
                initial[initial_count++] = buffer[i];
                parser.byte_offset++;
                if (initial_count == sizeof(initial)) {
                    if (!process_initial_bytes(&parser, initial, initial_count, error)) return false;
                    initial_processed = true;
                    if (parser.stopped) return true;
                }
                continue;
            }
            size_t offset = parser.byte_offset;
            parser.byte_offset++;
            if (!process_byte(&parser, buffer[i], offset, error)) return false;
            if (parser.stopped) return true;
        }
    }

    if (parser.state == FIELD_QUOTED) {
        return fail(error, parser.byte_offset, &parser, "CSV quoted field is not closed");
    }
    if (parser.record_started || parser.state == FIELD_AFTER_QUOTE ||
        parser.field != 0 || parser.used != 0) {
        if (!emit_record(&parser, parser.byte_offset, error)) return false;
    }
    return true;
}

static CmnyCsvReadStatus memory_read(void *context, unsigned char *buffer,
                                     size_t capacity, size_t *length) {
    MemoryReader *reader = context;
    size_t remaining = reader->length - reader->offset;
    if (remaining == 0) {
        *length = 0;
        return CMNY_CSV_READ_END;
    }
    size_t amount = remaining < capacity ? remaining : capacity;
    memcpy(buffer, reader->bytes + reader->offset, amount);
    reader->offset += amount;
    *length = amount;
    return CMNY_CSV_READ_DATA;
}

static CmnyCsvVisitResult adapt_visitor(const CmnyCsvRecord *record, void *context) {
    VisitorAdapter *adapter = context;
    return adapter->visitor == NULL || adapter->visitor(record, adapter->context)
               ? CMNY_CSV_VISIT_CONTINUE
               : CMNY_CSV_VISIT_ERROR;
}

bool cmny_csv_parse_memory(const unsigned char *bytes, size_t length, char delimiter,
                           CmnyCsvRecordVisitor visitor, void *context,
                           CmnyCsvParseError *error) {
    if (bytes == NULL && length != 0) {
        if (error != NULL) *error = (CmnyCsvParseError){.message = "invalid CSV parser input"};
        return false;
    }
    MemoryReader reader = {.bytes = bytes, .length = length, .offset = 0};
    VisitorAdapter adapter = {.visitor = visitor, .context = context};
    return cmny_csv_parse_stream(memory_read, &reader, delimiter, NULL,
                                 adapt_visitor, &adapter, error);
}

bool cmny_csv_detect_delimiter(const unsigned char *bytes, size_t length,
                               char *delimiter, CmnyCsvParseError *error) {
    if (error != NULL) *error = (CmnyCsvParseError){0};
    if ((bytes == NULL && length != 0) || delimiter == NULL || length == 0) {
        return fail(error, 0, NULL, "invalid delimiter detection input");
    }
    size_t offset = length >= 3 && bytes[0] == 0xEFU && bytes[1] == 0xBBU &&
                            bytes[2] == 0xBFU
                        ? 3U : 0U;
    size_t counts[3] = {0};
    bool quoted = false;
    bool after_quote = false;
    bool complete = false;
    for (; offset < length; offset++) {
        unsigned char byte = bytes[offset];
        if (byte == '\0') return fail(error, offset, NULL, "CSV contains a NUL byte");
        if (quoted) {
            if (byte == '"') {
                quoted = false;
                after_quote = true;
            }
            continue;
        }
        if (after_quote) {
            if (byte == '"') {
                quoted = true;
                after_quote = false;
            } else if (byte == ',' || byte == ';' || byte == '\t') {
                counts[byte == ',' ? 0U : byte == ';' ? 1U : 2U]++;
                after_quote = false;
            } else if (byte == '\r' || byte == '\n') {
                complete = true;
                break;
            } else {
                return fail(error, offset, NULL, "invalid quoted field while detecting delimiter");
            }
            continue;
        }
        if (byte == '"') quoted = true;
        else if (byte == ',' || byte == ';' || byte == '\t') {
            counts[byte == ',' ? 0U : byte == ';' ? 1U : 2U]++;
        } else if (byte == '\r' || byte == '\n') {
            complete = true;
            break;
        }
    }
    if (quoted || (!complete && offset == length && length == 0)) {
        return fail(error, offset, NULL, "delimiter sample has no complete record");
    }
    size_t best = 0;
    bool tie = false;
    for (size_t i = 1; i < 3; i++) {
        if (counts[i] > counts[best]) {
            best = i;
            tie = false;
        } else if (counts[i] == counts[best] && counts[i] != 0) {
            tie = true;
        }
    }
    if (counts[best] == 0 || tie) {
        return fail(error, offset, NULL, "CSV delimiter is ambiguous");
    }
    static const char candidates[3] = {',', ';', '\t'};
    *delimiter = candidates[best];
    return true;
}
