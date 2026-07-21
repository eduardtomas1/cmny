#ifndef CMNY_CSV_PARSER_H
#define CMNY_CSV_PARSER_H

#include <stdbool.h>
#include <stddef.h>

#define CMNY_CSV_MAX_FIELDS 64U
#define CMNY_CSV_FIELD_CAP 1025U
#define CMNY_CSV_STREAM_CHUNK 4096U
#define CMNY_CSV_DEFAULT_MAX_BYTES (32U * 1024U * 1024U)
#define CMNY_CSV_DEFAULT_MAX_RECORDS 100000U

typedef struct {
    char fields[CMNY_CSV_MAX_FIELDS][CMNY_CSV_FIELD_CAP];
    size_t field_count;
    size_t record_number;
    size_t physical_line_start;
    size_t physical_line_end;
    size_t byte_offset_start;
} CmnyCsvRecord;

typedef struct {
    size_t byte_offset;
    size_t physical_line;
    size_t record_number;
    const char *message; /* Static storage; never owned by the caller. */
} CmnyCsvParseError;

typedef struct {
    size_t max_bytes;
    size_t max_records;
    size_t max_fields;
    size_t max_field_bytes;
} CmnyCsvLimits;

typedef enum {
    CMNY_CSV_READ_DATA,
    CMNY_CSV_READ_END,
    CMNY_CSV_READ_ERROR,
    CMNY_CSV_READ_CANCELLED
} CmnyCsvReadStatus;

/* DATA must set *length to 1..capacity. Other statuses must set it to zero. */
typedef CmnyCsvReadStatus (*CmnyCsvReader)(void *context, unsigned char *buffer,
                                          size_t capacity, size_t *length);

typedef enum {
    CMNY_CSV_VISIT_CONTINUE,
    CMNY_CSV_VISIT_STOP,
    CMNY_CSV_VISIT_CANCEL,
    CMNY_CSV_VISIT_ERROR
} CmnyCsvVisitResult;

/* The record and all field pointers are borrowed only for the callback call. */
typedef CmnyCsvVisitResult (*CmnyCsvRecordHandler)(const CmnyCsvRecord *record,
                                                   void *context);
typedef bool (*CmnyCsvRecordVisitor)(const CmnyCsvRecord *record, void *context);

void cmny_csv_limits_default(CmnyCsvLimits *limits);

bool cmny_csv_parse_stream(CmnyCsvReader reader, void *reader_context,
                           char delimiter, const CmnyCsvLimits *limits,
                           CmnyCsvRecordHandler handler, void *handler_context,
                           CmnyCsvParseError *error);

/* Compatibility wrapper using default limits. */
bool cmny_csv_parse_memory(const unsigned char *bytes, size_t length, char delimiter,
                           CmnyCsvRecordVisitor visitor, void *context,
                           CmnyCsvParseError *error);

/*
 * Detect comma, semicolon, or tab from the first complete logical record in a
 * bounded memory sample. A unique non-zero separator count is required.
 * *delimiter is unchanged on failure.
 */
bool cmny_csv_detect_delimiter(const unsigned char *bytes, size_t length,
                               char *delimiter, CmnyCsvParseError *error);

#endif
