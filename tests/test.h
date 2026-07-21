#ifndef CMNY_TEST_H
#define CMNY_TEST_H

#include <stdio.h>
#include <stdlib.h>

#define ASSERT_TRUE(expression) do { \
    if (!(expression)) { \
        (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        exit(1); \
    } \
} while (0)

#define ASSERT_EQ_I64(expected, actual) do { \
    long long expected_value = (long long)(expected); \
    long long actual_value = (long long)(actual); \
    if (expected_value != actual_value) { \
        (void)fprintf(stderr, "FAIL %s:%d: expected %lld, got %lld\n", \
                      __FILE__, __LINE__, expected_value, actual_value); \
        exit(1); \
    } \
} while (0)

#endif
