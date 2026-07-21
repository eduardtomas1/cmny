#include "cmny_expr.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    int64_t cents = 0;
    if (cmny_amount_expression_parse(data, size, &cents) && cents <= 0) abort();
    return 0;
}
