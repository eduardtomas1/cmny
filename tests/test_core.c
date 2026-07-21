#include "cmny.h"
#include "test.h"

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void test_money_parse(void) {
    int64_t cents = 0;
    ASSERT_TRUE(cmny_money_parse("12", &cents));
    ASSERT_EQ_I64(1200, cents);
    ASSERT_TRUE(cmny_money_parse("  +12.3 ", &cents));
    ASSERT_EQ_I64(1230, cents);
    ASSERT_TRUE(cmny_money_parse("12,34", &cents));
    ASSERT_EQ_I64(1234, cents);
    ASSERT_TRUE(cmny_money_parse("92233720368547758.07", &cents));
    ASSERT_EQ_I64(INT64_MAX, cents);

    ASSERT_TRUE(!cmny_money_parse("", &cents));
    ASSERT_TRUE(!cmny_money_parse("0", &cents));
    ASSERT_TRUE(!cmny_money_parse("-1.00", &cents));
    ASSERT_TRUE(!cmny_money_parse("1.234", &cents));
    ASSERT_TRUE(!cmny_money_parse("1e3", &cents));
    ASSERT_TRUE(!cmny_money_parse("92233720368547758.08", &cents));
}

static void test_money_format(void) {
    char value[80];
    cmny_money_format(0, value, sizeof(value));
    ASSERT_TRUE(strcmp(value, "0.00") == 0);
    cmny_money_format(123456789, value, sizeof(value));
    ASSERT_TRUE(strcmp(value, "1,234,567.89") == 0);
    cmny_money_format(-430, value, sizeof(value));
    ASSERT_TRUE(strcmp(value, "-4.30") == 0);
    cmny_money_format(INT64_MIN, value, sizeof(value));
    ASSERT_TRUE(strcmp(value, "-92,233,720,368,547,758.08") == 0);
}

static void test_dates(void) {
    ASSERT_TRUE(cmny_date_valid("2024-02-29"));
    ASSERT_TRUE(cmny_date_valid("2000-02-29"));
    ASSERT_TRUE(!cmny_date_valid("1900-02-29"));
    ASSERT_TRUE(!cmny_date_valid("2026-02-30"));
    ASSERT_TRUE(!cmny_date_valid("2026-7-01"));
    ASSERT_TRUE(cmny_month_valid("2026-07"));
    ASSERT_TRUE(!cmny_month_valid("2026-13"));

    char month[8];
    ASSERT_TRUE(cmny_month_shift("2026-01", -1, month));
    ASSERT_TRUE(strcmp(month, "2025-12") == 0);
    ASSERT_TRUE(cmny_month_shift("2026-12", 1, month));
    ASSERT_TRUE(strcmp(month, "2027-01") == 0);
    ASSERT_TRUE(!cmny_month_shift("2026-07", INT_MAX, month));
    ASSERT_TRUE(!cmny_month_shift("2026-07", INT_MIN, month));
}

static void test_currency(void) {
    char currency[4];
    ASSERT_TRUE(cmny_currency_supported("eur", currency));
    ASSERT_TRUE(strcmp(currency, "EUR") == 0);
    ASSERT_TRUE(cmny_currency_supported("USD", currency));
    ASSERT_TRUE(!cmny_currency_supported("JPY", currency));
    ASSERT_TRUE(!cmny_currency_supported("123", currency));
}

static void test_themes(void) {
    CmnyTheme theme = CMNY_THEME_OCEAN;
    ASSERT_TRUE(cmny_theme_parse("violet", &theme));
    ASSERT_EQ_I64(CMNY_THEME_VIOLET, theme);
    ASSERT_TRUE(strcmp(cmny_theme_name(CMNY_THEME_AMBER), "amber") == 0);
    ASSERT_TRUE(!cmny_theme_parse("neon", &theme));
}

int main(void) {
    test_money_parse();
    test_money_format();
    test_dates();
    test_currency();
    test_themes();
    (void)printf("ok - core tests\n");
    return 0;
}
