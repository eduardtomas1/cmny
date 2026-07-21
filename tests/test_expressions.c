#include "cmny_expr.h"
#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool parse_amount(const char *text, int64_t *cents) {
    size_t length = strlen(text);
    if (length > CMNY_AMOUNT_EXPRESSION_MAX) return false;
    unsigned char input[CMNY_AMOUNT_EXPRESSION_MAX];
    memcpy(input, text, length);
    return cmny_amount_expression_parse(input, length, cents);
}

static bool parse_date(const char *text, const char *base, char out[11]) {
    size_t length = strlen(text);
    if (length > CMNY_DATE_EXPRESSION_MAX) return false;
    unsigned char input[CMNY_DATE_EXPRESSION_MAX];
    memcpy(input, text, length);
    return cmny_date_expression_parse(input, length, base, out);
}

static void test_amount_basics(void) {
    int64_t cents = 0;
    ASSERT_TRUE(parse_amount("12", &cents));
    ASSERT_EQ_I64(1200, cents);
    ASSERT_TRUE(parse_amount("12.3 + 0.04", &cents));
    ASSERT_EQ_I64(1234, cents);
    ASSERT_TRUE(parse_amount("1 + 2 * 3", &cents));
    ASSERT_EQ_I64(700, cents);
    ASSERT_TRUE(parse_amount("(1 + 2) * 3", &cents));
    ASSERT_EQ_I64(900, cents);
    ASSERT_TRUE(parse_amount("10 / 4", &cents));
    ASSERT_EQ_I64(250, cents);
    ASSERT_TRUE(parse_amount(" - 2 + 5 ", &cents));
    ASSERT_EQ_I64(300, cents);
    ASSERT_TRUE(parse_amount("1--2", &cents));
    ASSERT_EQ_I64(300, cents);
    ASSERT_TRUE(parse_amount("2 * -3 + 7", &cents));
    ASSERT_EQ_I64(100, cents);
}

static void test_amount_rounding(void) {
    int64_t cents = 0;
    ASSERT_TRUE(parse_amount("1.00 / 3", &cents));
    ASSERT_EQ_I64(33, cents);
    ASSERT_TRUE(parse_amount("1.01 / 2", &cents));
    ASSERT_EQ_I64(51, cents);
    ASSERT_TRUE(parse_amount("-1.01 / 2 + 1", &cents));
    ASSERT_EQ_I64(49, cents);
    ASSERT_TRUE(parse_amount("1.04 / 8", &cents));
    ASSERT_EQ_I64(13, cents);
    ASSERT_TRUE(parse_amount("1.03 / 2", &cents));
    ASSERT_EQ_I64(52, cents);
}

static void test_amount_rejections(void) {
    int64_t cents = 777;
    ASSERT_TRUE(!cmny_amount_expression_parse(NULL, 0, &cents));
    ASSERT_TRUE(!parse_amount("", &cents));
    ASSERT_TRUE(!parse_amount("0", &cents));
    ASSERT_TRUE(!parse_amount("-1", &cents));
    ASSERT_TRUE(!parse_amount("1.234", &cents));
    ASSERT_TRUE(!parse_amount(".50", &cents));
    ASSERT_TRUE(!parse_amount("1.", &cents));
    ASSERT_TRUE(!parse_amount("1,00", &cents));
    ASSERT_TRUE(!parse_amount("1e3", &cents));
    ASSERT_TRUE(!parse_amount("1 / 0", &cents));
    ASSERT_TRUE(!parse_amount("1 * 2.5", &cents));
    ASSERT_TRUE(!parse_amount("1 * (2)", &cents));
    ASSERT_TRUE(!parse_amount("92233720368547758.07 + 0.01", &cents));
    ASSERT_TRUE(!parse_amount("92233720368547758.07 * 2", &cents));
    ASSERT_TRUE(!parse_amount("1 +", &cents));
    ASSERT_TRUE(!parse_amount("((1)", &cents));
    ASSERT_TRUE(!parse_amount("1 2", &cents));
    ASSERT_EQ_I64(777, cents);

    unsigned char embedded_nul[] = {'1', '.', '0', '0', '\0', '+', '1'};
    ASSERT_TRUE(!cmny_amount_expression_parse(embedded_nul, sizeof(embedded_nul), &cents));
    unsigned char too_long[257];
    memset(too_long, '1', sizeof(too_long));
    ASSERT_TRUE(!cmny_amount_expression_parse(too_long, sizeof(too_long), &cents));

    char too_deep[40];
    size_t position = 0;
    for (size_t i = 0; i < 17; i++) too_deep[position++] = '(';
    too_deep[position++] = '1';
    for (size_t i = 0; i < 17; i++) too_deep[position++] = ')';
    too_deep[position] = '\0';
    ASSERT_TRUE(!parse_amount(too_deep, &cents));

    char too_many_tokens[140];
    position = 0;
    for (size_t i = 0; i < 65; i++) {
        if (i != 0) too_many_tokens[position++] = '+';
        too_many_tokens[position++] = '1';
    }
    too_many_tokens[position] = '\0';
    ASSERT_TRUE(!parse_amount(too_many_tokens, &cents));
}

static void test_date_validation_and_shift(void) {
    ASSERT_TRUE(cmny_expression_date_valid("2024-02-29"));
    ASSERT_TRUE(!cmny_expression_date_valid("1900-02-29"));
    ASSERT_TRUE(!cmny_expression_date_valid("2024-2-29"));

    char date[11] = "unchanged";
    ASSERT_TRUE(cmny_expression_date_shift("2024-02-29", 1, 'y', date));
    ASSERT_TRUE(strcmp(date, "2025-02-28") == 0);
    ASSERT_TRUE(cmny_expression_date_shift("2024-01-31", 1, 'm', date));
    ASSERT_TRUE(strcmp(date, "2024-02-29") == 0);
    ASSERT_TRUE(cmny_expression_date_shift("2023-12-31", 1, 'd', date));
    ASSERT_TRUE(strcmp(date, "2024-01-01") == 0);
    ASSERT_TRUE(cmny_expression_date_shift("2024-02-29", -1, 'w', date));
    ASSERT_TRUE(strcmp(date, "2024-02-22") == 0);
    ASSERT_TRUE(!cmny_expression_date_shift("1900-01-01", -1, 'd', date));
    ASSERT_TRUE(strcmp(date, "2024-02-22") == 0);
    ASSERT_TRUE(!cmny_expression_date_shift("9999-12-31", 1, 'd', date));
    ASSERT_TRUE(!cmny_expression_date_shift("2024-01-01", INT64_MAX, 'w', date));
    ASSERT_TRUE(!cmny_expression_date_shift("2024-01-01", 1, 'q', date));
}

static void test_date_compare_and_distance(void) {
    int comparison = 77;
    int64_t days = 88;
    ASSERT_TRUE(cmny_expression_date_compare("2024-02-29", "2024-03-01",
                                             &comparison));
    ASSERT_EQ_I64(-1, comparison);
    ASSERT_TRUE(cmny_expression_date_compare("2024-02-29", "2024-02-29",
                                             &comparison));
    ASSERT_EQ_I64(0, comparison);
    ASSERT_TRUE(cmny_expression_date_compare("9999-12-31", "1900-01-01",
                                             &comparison));
    ASSERT_EQ_I64(1, comparison);
    ASSERT_TRUE(cmny_expression_date_days_between("2024-02-28", "2024-03-01",
                                                  &days));
    ASSERT_EQ_I64(2, days);
    ASSERT_TRUE(cmny_expression_date_days_between("2024-03-01", "2024-02-28",
                                                  &days));
    ASSERT_EQ_I64(-2, days);
    comparison = 77;
    days = 88;
    ASSERT_TRUE(!cmny_expression_date_compare("2024-02-30", "2024-03-01",
                                              &comparison));
    ASSERT_TRUE(!cmny_expression_date_days_between("2024-02-30", "2024-03-01",
                                                   &days));
    ASSERT_EQ_I64(77, comparison);
    ASSERT_EQ_I64(88, days);

    /* Brute-force reference: every checked daily successor advances by one. */
    char current[11] = "1900-01-01";
    for (int64_t expected = 0; expected <= 73413; expected++) {
        ASSERT_TRUE(cmny_expression_date_days_between("1900-01-01", current, &days));
        ASSERT_EQ_I64(expected, days);
        if (strcmp(current, "2100-12-31") == 0) break;
        char next[11];
        ASSERT_TRUE(cmny_expression_date_shift(current, 1, 'd', next));
        memcpy(current, next, sizeof(current));
    }
    ASSERT_TRUE(strcmp(current, "2100-12-31") == 0);
}

static void test_date_expressions(void) {
    const char *base = "2024-02-29";
    char date[11];
    ASSERT_TRUE(parse_date("today", base, date));
    ASSERT_TRUE(strcmp(date, "2024-02-29") == 0);
    ASSERT_TRUE(parse_date("Tomorrow", base, date));
    ASSERT_TRUE(strcmp(date, "2024-03-01") == 0);
    ASSERT_TRUE(parse_date("yesterday", base, date));
    ASSERT_TRUE(strcmp(date, "2024-02-28") == 0);
    ASSERT_TRUE(parse_date("2024-01-31 +1m", base, date));
    ASSERT_TRUE(strcmp(date, "2024-02-29") == 0);
    ASSERT_TRUE(parse_date("today + 1m - 2d", base, date));
    ASSERT_TRUE(strcmp(date, "2024-03-27") == 0);
    ASSERT_TRUE(parse_date("+2w-1d", base, date));
    ASSERT_TRUE(strcmp(date, "2024-03-13") == 0);
    ASSERT_TRUE(parse_date("today + 0d", base, date));
    ASSERT_TRUE(strcmp(date, base) == 0);
    ASSERT_TRUE(parse_date(" 2024-12-31 + 1D ", base, date));
    ASSERT_TRUE(strcmp(date, "2025-01-01") == 0);
}

static void test_date_rejections(void) {
    char date[11] = "unchanged";
    const char *base = "2024-02-29";
    ASSERT_TRUE(!cmny_date_expression_parse(NULL, 0, base, date));
    ASSERT_TRUE(!parse_date("", base, date));
    ASSERT_TRUE(!parse_date("today", "2024-02-30", date));
    ASSERT_TRUE(!parse_date("2024-02-30", base, date));
    ASSERT_TRUE(!parse_date("today +1", base, date));
    ASSERT_TRUE(!parse_date("today 1d", base, date));
    ASSERT_TRUE(!parse_date("today +d", base, date));
    ASSERT_TRUE(!parse_date("today +1q", base, date));
    ASSERT_TRUE(!parse_date("today +9223372036854775808d", base, date));
    ASSERT_TRUE(!parse_date("9999-12-31 +1d", base, date));
    ASSERT_TRUE(!parse_date("+", base, date));
    ASSERT_TRUE(strcmp(date, "unchanged") == 0);

    unsigned char embedded_nul[] = {'t', 'o', 'd', 'a', 'y', '\0', '+', '1', 'd'};
    ASSERT_TRUE(!cmny_date_expression_parse(embedded_nul, sizeof(embedded_nul), base, date));
    unsigned char too_long[129];
    memset(too_long, ' ', sizeof(too_long));
    ASSERT_TRUE(!cmny_date_expression_parse(too_long, sizeof(too_long), base, date));

    char too_many_tokens[100] = "today";
    size_t position = strlen(too_many_tokens);
    for (size_t i = 0; i < 22; i++) {
        too_many_tokens[position++] = '+';
        too_many_tokens[position++] = '0';
        too_many_tokens[position++] = 'd';
    }
    too_many_tokens[position] = '\0';
    ASSERT_TRUE(!parse_date(too_many_tokens, base, date));
}

int main(void) {
    test_amount_basics();
    test_amount_rounding();
    test_amount_rejections();
    test_date_validation_and_shift();
    test_date_compare_and_distance();
    test_date_expressions();
    test_date_rejections();
    (void)printf("ok - expression tests\n");
    return 0;
}
