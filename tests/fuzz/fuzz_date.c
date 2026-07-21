#include "cmny_expr.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char date[11];
    if (cmny_date_expression_parse(data, size, "2024-02-29", date)) {
        char unchanged[11];
        if (!cmny_expression_date_valid(date) ||
            !cmny_expression_date_shift(date, 0, 'd', unchanged)) {
            abort();
        }
    }
    return 0;
}
