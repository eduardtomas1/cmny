#include "cmny_csv_parser.h"

#include <stddef.h>
#include <stdint.h>

static bool accept_record(const CmnyCsvRecord *record, void *context) {
    (void)record;
    (void)context;
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    CmnyCsvParseError error = {0};
    (void)cmny_csv_parse_memory(data, size, ',', accept_record, NULL, &error);
    return 0;
}
