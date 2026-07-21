#include "cmny_csv_parser.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    CmnyCsvRecord records[8];
    size_t count;
} Capture;

typedef struct {
    const unsigned char *bytes;
    size_t length;
    size_t offset;
    size_t chunk;
    bool fail;
} ChunkReader;

typedef struct {
    Capture capture;
    size_t stop_after;
    bool cancel;
} StreamCapture;

static bool capture_record(const CmnyCsvRecord *record, void *context) {
    Capture *capture = context;
    if (capture->count >= sizeof(capture->records) / sizeof(capture->records[0])) return false;
    capture->records[capture->count++] = *record;
    return true;
}

static CmnyCsvReadStatus chunk_read(void *context, unsigned char *buffer,
                                   size_t capacity, size_t *length) {
    ChunkReader *reader = context;
    if (reader->fail) {
        *length = 0;
        return CMNY_CSV_READ_ERROR;
    }
    if (reader->offset == reader->length) {
        *length = 0;
        return CMNY_CSV_READ_END;
    }
    size_t remaining = reader->length - reader->offset;
    size_t amount = remaining < reader->chunk ? remaining : reader->chunk;
    if (amount > capacity) amount = capacity;
    memcpy(buffer, reader->bytes + reader->offset, amount);
    reader->offset += amount;
    *length = amount;
    return CMNY_CSV_READ_DATA;
}

static CmnyCsvVisitResult stream_capture(const CmnyCsvRecord *record, void *context) {
    StreamCapture *capture = context;
    if (capture->cancel) return CMNY_CSV_VISIT_CANCEL;
    if (!capture_record(record, &capture->capture)) return CMNY_CSV_VISIT_ERROR;
    return capture->stop_after > 0 && capture->capture.count >= capture->stop_after
               ? CMNY_CSV_VISIT_STOP
               : CMNY_CSV_VISIT_CONTINUE;
}

static bool parse(const char *text, Capture *capture, CmnyCsvParseError *error) {
    return cmny_csv_parse_memory((const unsigned char *)text, strlen(text), ',',
                                 capture_record, capture, error);
}

static void test_rfc_memory(void) {
    Capture capture = {0};
    CmnyCsvParseError error = {0};
    const char *document =
        "\xEF\xBB\xBF" "date,description,amount\r\n"
        "2026-07-01,\"Coffee, lunch\",-12.50\r\n"
        "2026-07-02,\"Quoted \"\"word\"\"\r\nand another line\",20.00\n";
    ASSERT_TRUE(parse(document, &capture, &error));
    ASSERT_EQ_I64(3, capture.count);
    ASSERT_EQ_I64(3, capture.records[0].field_count);
    ASSERT_TRUE(strcmp(capture.records[1].fields[1], "Coffee, lunch") == 0);
    ASSERT_TRUE(strcmp(capture.records[2].fields[1],
                       "Quoted \"word\"\nand another line") == 0);
    ASSERT_EQ_I64(3, capture.records[2].record_number);
    ASSERT_EQ_I64(3, capture.records[2].physical_line_start);
    ASSERT_EQ_I64(4, capture.records[2].physical_line_end);

    capture = (Capture){0};
    ASSERT_TRUE(parse("a,,c\n,,\n", &capture, &error));
    ASSERT_EQ_I64(2, capture.count);
    ASSERT_EQ_I64(3, capture.records[1].field_count);
    ASSERT_TRUE(strcmp(capture.records[0].fields[1], "") == 0);

    capture = (Capture){0};
    ASSERT_TRUE(!parse("a,\"unfinished\n", &capture, &error));
    ASSERT_TRUE(strstr(error.message, "not closed") != NULL);

    static const unsigned char nul_document[] = {'a', ',', 'b', '\0', 'c'};
    ASSERT_TRUE(!cmny_csv_parse_memory(nul_document, sizeof(nul_document), ',',
                                       capture_record, &capture, &error));
    ASSERT_TRUE(strstr(error.message, "NUL") != NULL);

    capture = (Capture){0};
    ASSERT_TRUE(!parse("a,\"b\"oops,c\n", &capture, &error));
    ASSERT_TRUE(strstr(error.message, "closing quote") != NULL);
}

static void test_one_byte_stream(void) {
    static const unsigned char document[] =
        "\xEF\xBB\xBF" "a;b;c\r\n1;\"two\r\nlines\";3\r\n";
    ChunkReader reader = {
        .bytes = document,
        .length = sizeof(document) - 1,
        .chunk = 1,
    };
    StreamCapture capture = {0};
    CmnyCsvParseError error = {0};
    ASSERT_TRUE(cmny_csv_parse_stream(chunk_read, &reader, ';', NULL,
                                      stream_capture, &capture, &error));
    ASSERT_EQ_I64(2, capture.capture.count);
    ASSERT_TRUE(strcmp(capture.capture.records[1].fields[1], "two\nlines") == 0);
    ASSERT_EQ_I64(2, capture.capture.records[1].physical_line_start);
    ASSERT_EQ_I64(3, capture.capture.records[1].physical_line_end);
    ASSERT_EQ_I64(sizeof(document) - 1, reader.offset);
}

static void test_stop_cancel_and_reader_error(void) {
    static const unsigned char document[] = "a,b\n1,2\n3,4\n";
    ChunkReader reader = {
        .bytes = document,
        .length = sizeof(document) - 1,
        .chunk = 1,
    };
    StreamCapture capture = {.stop_after = 1};
    CmnyCsvParseError error = {0};
    ASSERT_TRUE(cmny_csv_parse_stream(chunk_read, &reader, ',', NULL,
                                      stream_capture, &capture, &error));
    ASSERT_EQ_I64(1, capture.capture.count);
    ASSERT_TRUE(reader.offset < reader.length);

    static const unsigned char early_stop[] = {'\n', '\0', 'x'};
    reader = (ChunkReader){
        .bytes = early_stop,
        .length = sizeof(early_stop),
        .chunk = sizeof(early_stop),
    };
    capture = (StreamCapture){.stop_after = 1};
    ASSERT_TRUE(cmny_csv_parse_stream(chunk_read, &reader, ',', NULL,
                                      stream_capture, &capture, &error));
    ASSERT_EQ_I64(1, capture.capture.count);

    reader = (ChunkReader){
        .bytes = document,
        .length = sizeof(document) - 1,
        .chunk = 2,
    };
    capture = (StreamCapture){.cancel = true};
    ASSERT_TRUE(!cmny_csv_parse_stream(chunk_read, &reader, ',', NULL,
                                       stream_capture, &capture, &error));
    ASSERT_TRUE(strstr(error.message, "cancel") != NULL);

    reader = (ChunkReader){.fail = true, .chunk = 1};
    ASSERT_TRUE(!cmny_csv_parse_stream(chunk_read, &reader, ',', NULL,
                                       stream_capture, &capture, &error));
    ASSERT_TRUE(strstr(error.message, "reader") != NULL);
}

static void test_limits(void) {
    static const unsigned char document[] = "a,b,c\n1,2,3\n4,5,6\n";
    CmnyCsvLimits limits;
    cmny_csv_limits_default(&limits);
    CmnyCsvParseError error = {0};
    StreamCapture capture = {0};
    ChunkReader reader = {
        .bytes = document,
        .length = sizeof(document) - 1,
        .chunk = 4,
    };
    limits.max_bytes = sizeof(document) - 2;
    ASSERT_TRUE(!cmny_csv_parse_stream(chunk_read, &reader, ',', &limits,
                                       stream_capture, &capture, &error));
    ASSERT_TRUE(strstr(error.message, "byte") != NULL);

    cmny_csv_limits_default(&limits);
    limits.max_records = 2;
    reader.offset = 0;
    capture = (StreamCapture){0};
    ASSERT_TRUE(!cmny_csv_parse_stream(chunk_read, &reader, ',', &limits,
                                       stream_capture, &capture, &error));
    ASSERT_TRUE(strstr(error.message, "record count") != NULL);

    cmny_csv_limits_default(&limits);
    limits.max_fields = 2;
    reader.offset = 0;
    capture = (StreamCapture){0};
    ASSERT_TRUE(!cmny_csv_parse_stream(chunk_read, &reader, ',', &limits,
                                       stream_capture, &capture, &error));
    ASSERT_TRUE(strstr(error.message, "fields") != NULL);

    static const unsigned char long_field[] = "abcd\n";
    cmny_csv_limits_default(&limits);
    limits.max_field_bytes = 3;
    reader = (ChunkReader){
        .bytes = long_field,
        .length = sizeof(long_field) - 1,
        .chunk = 2,
    };
    ASSERT_TRUE(!cmny_csv_parse_stream(chunk_read, &reader, ',', &limits,
                                       stream_capture, &capture, &error));
    ASSERT_TRUE(strstr(error.message, "field") != NULL);
}

static void test_delimiter_detection(void) {
    CmnyCsvParseError error = {0};
    char delimiter = 'x';
    static const unsigned char semicolon[] =
        "\xEF\xBB\xBF" "date;\"payee, name\";amount\r\n";
    ASSERT_TRUE(cmny_csv_detect_delimiter(semicolon, sizeof(semicolon) - 1,
                                          &delimiter, &error));
    ASSERT_TRUE(delimiter == ';');
    static const unsigned char tab[] = "date\tpayee\tamount\n";
    ASSERT_TRUE(cmny_csv_detect_delimiter(tab, sizeof(tab) - 1, &delimiter, &error));
    ASSERT_TRUE(delimiter == '\t');
    static const unsigned char comma[] = "date,payee,amount";
    ASSERT_TRUE(cmny_csv_detect_delimiter(comma, sizeof(comma) - 1, &delimiter, &error));
    ASSERT_TRUE(delimiter == ',');

    static const unsigned char ambiguous[] = "a,b;c\n";
    delimiter = 'x';
    ASSERT_TRUE(!cmny_csv_detect_delimiter(ambiguous, sizeof(ambiguous) - 1,
                                           &delimiter, &error));
    ASSERT_TRUE(delimiter == 'x');
    ASSERT_TRUE(strstr(error.message, "ambiguous") != NULL);
}

int main(void) {
    test_rfc_memory();
    test_one_byte_stream();
    test_stop_cancel_and_reader_error();
    test_limits();
    test_delimiter_detection();
    (void)printf("ok - CSV parser tests\n");
    return 0;
}
