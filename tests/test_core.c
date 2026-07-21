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
    cmny_money_format_plain(123456, value, sizeof(value));
    ASSERT_TRUE(strcmp(value, "1234.56") == 0);
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

    char date[11];
    ASSERT_TRUE(cmny_date_for_month_day("2026-02", 31, date));
    ASSERT_TRUE(strcmp(date, "2026-02-28") == 0);
    ASSERT_TRUE(cmny_date_for_month_day("2024-02", 29, date));
    ASSERT_TRUE(strcmp(date, "2024-02-29") == 0);
}

static void test_transaction_validation(void) {
    CmnyTransaction tx = {0};
    tx.kind = CMNY_EXPENSE;
    tx.amount_cents = 1250;
    (void)snprintf(tx.category, sizeof(tx.category), "Food");
    (void)snprintf(tx.note, sizeof(tx.note), "Lunch");
    (void)snprintf(tx.occurred_on, sizeof(tx.occurred_on), "2026-07-21");
    ASSERT_TRUE(cmny_transaction_valid(&tx, false));
    ASSERT_TRUE(!cmny_transaction_valid(&tx, true));
    tx.id = 1;
    ASSERT_TRUE(cmny_transaction_valid(&tx, true));
    tx.note[0] = '\n';
    ASSERT_TRUE(!cmny_transaction_valid(&tx, true));
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
    test_transaction_validation();
    (void)printf("ok - core tests\n");
    return 0;
}
