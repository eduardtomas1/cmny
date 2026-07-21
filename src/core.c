#include "cmny.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

bool cmny_money_parse(const char *text, int64_t *cents) {
    if (text == NULL || cents == NULL) {
        return false;
    }

    while (isspace((unsigned char)*text)) {
        text++;
    }
    if (*text == '+') {
        text++;
    }
    if (!isdigit((unsigned char)*text)) {
        return false;
    }

    uint64_t whole = 0;
    while (isdigit((unsigned char)*text)) {
        unsigned digit = (unsigned)(*text - '0');
        if (whole > ((uint64_t)INT64_MAX / 100U - digit) / 10U) {
            return false;
        }
        whole = whole * 10U + digit;
        text++;
    }

    unsigned fraction = 0;
    if (*text == '.' || *text == ',') {
        text++;
        if (!isdigit((unsigned char)*text)) {
            return false;
        }
        fraction = (unsigned)(*text - '0') * 10U;
        text++;
        if (isdigit((unsigned char)*text)) {
            fraction += (unsigned)(*text - '0');
            text++;
        }
        if (isdigit((unsigned char)*text)) {
            return false;
        }
    }

    while (isspace((unsigned char)*text)) {
        text++;
    }
    if (*text != '\0') {
        return false;
    }

    uint64_t value = whole * 100U + fraction;
    if (value == 0 || value > (uint64_t)INT64_MAX) {
        return false;
    }
    *cents = (int64_t)value;
    return true;
}

void cmny_money_format(int64_t cents, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }

    bool negative = cents < 0;
    uint64_t magnitude = negative ? (uint64_t)(-(cents + 1)) + 1U : (uint64_t)cents;
    uint64_t whole = magnitude / 100U;
    unsigned fraction = (unsigned)(magnitude % 100U);

    char digits[32];
    (void)snprintf(digits, sizeof(digits), "%" PRIu64, whole);
    size_t len = strlen(digits);
    char grouped[48];
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 1 < sizeof(grouped); i++) {
        if (i > 0 && (len - i) % 3 == 0 && pos + 1 < sizeof(grouped)) {
            grouped[pos++] = ',';
        }
        grouped[pos++] = digits[i];
    }
    grouped[pos] = '\0';
    (void)snprintf(out, out_size, "%s%s.%02u", negative ? "-" : "", grouped, fraction);
}

void cmny_money_format_plain(int64_t cents, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    bool negative = cents < 0;
    uint64_t magnitude = negative ? (uint64_t)(-(cents + 1)) + 1U : (uint64_t)cents;
    (void)snprintf(out, out_size, "%s%" PRIu64 ".%02u", negative ? "-" : "",
                   magnitude / 100U, (unsigned)(magnitude % 100U));
}

bool cmny_currency_supported(const char *input, char output[4]) {
    if (input == NULL || output == NULL || strlen(input) != 3) return false;
    for (size_t i = 0; i < 3; i++) {
        if (!isalpha((unsigned char)input[i])) return false;
        output[i] = (char)toupper((unsigned char)input[i]);
    }
    output[3] = '\0';
    static const char *supported[] = {
        "AED", "AUD", "BRL", "CAD", "CHF", "CNY", "CZK", "DKK", "EUR", "GBP",
        "HKD", "HUF", "INR", "ILS", "MXN", "NOK", "NZD", "PLN", "RON", "SEK",
        "SGD", "TRY", "USD", "ZAR"
    };
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (strcmp(output, supported[i]) == 0) return true;
    }
    return false;
}

bool cmny_theme_parse(const char *input, CmnyTheme *theme) {
    if (input == NULL || theme == NULL) return false;
    for (int i = 0; i < (int)CMNY_THEME_COUNT; i++) {
        CmnyTheme candidate = (CmnyTheme)i;
        if (strcmp(input, cmny_theme_name(candidate)) == 0) {
            *theme = candidate;
            return true;
        }
    }
    return false;
}

const char *cmny_theme_name(CmnyTheme theme) {
    static const char *names[] = {
        "ocean", "violet", "amber", "high-contrast", "monochrome"
    };
    return theme >= CMNY_THEME_OCEAN && theme < CMNY_THEME_COUNT ? names[theme] : "unknown";
}

static bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool cmny_date_valid(const char *date) {
    if (date == NULL || strlen(date) != 10 || date[4] != '-' || date[7] != '-') {
        return false;
    }
    for (size_t i = 0; i < 10; i++) {
        if (i != 4 && i != 7 && !isdigit((unsigned char)date[i])) {
            return false;
        }
    }
    int year = (date[0] - '0') * 1000 + (date[1] - '0') * 100 +
               (date[2] - '0') * 10 + (date[3] - '0');
    int month = (date[5] - '0') * 10 + (date[6] - '0');
    int day = (date[8] - '0') * 10 + (date[9] - '0');
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1900 || month < 1 || month > 12 || day < 1) {
        return false;
    }
    int limit = days[month] + (month == 2 && leap_year(year) ? 1 : 0);
    return day <= limit;
}

bool cmny_month_valid(const char *month) {
    if (month == NULL || strlen(month) != 7 || month[4] != '-') {
        return false;
    }
    char date[11];
    (void)snprintf(date, sizeof(date), "%s-01", month);
    return cmny_date_valid(date);
}

void cmny_today(char out[11]) {
    time_t now = time(NULL);
    struct tm local = {0};
#ifdef _WIN32
    if (localtime_s(&local, &now) != 0) {
#else
    if (localtime_r(&now, &local) == NULL) {
#endif
        (void)snprintf(out, 11, "1970-01-01");
        return;
    }
    (void)strftime(out, 11, "%Y-%m-%d", &local);
}

void cmny_current_month(char out[8]) {
    char today[11];
    cmny_today(today);
    memcpy(out, today, 7);
    out[7] = '\0';
}

bool cmny_month_shift(const char *month, int delta, char out[8]) {
    if (!cmny_month_valid(month) || out == NULL) {
        return false;
    }
    int year = (month[0] - '0') * 1000 + (month[1] - '0') * 100 +
               (month[2] - '0') * 10 + (month[3] - '0');
    int mon = (month[5] - '0') * 10 + (month[6] - '0');
    int64_t serial = (int64_t)year * 12 + (mon - 1) + (int64_t)delta;
    int64_t new_year = serial / 12;
    int64_t new_mon = serial % 12;
    if (new_mon < 0) {
        new_mon += 12;
        new_year--;
    }
    if (new_year < 1900 || new_year > 9999) {
        return false;
    }
    (void)snprintf(out, 8, "%04lld-%02lld", (long long)new_year, (long long)new_mon + 1);
    return true;
}

bool cmny_date_for_month_day(const char *month, int day, char out[11]) {
    if (!cmny_month_valid(month) || out == NULL || day < 1 || day > 31) return false;
    for (int candidate = day; candidate >= 1; candidate--) {
        (void)snprintf(out, 11, "%s-%02d", month, candidate);
        if (cmny_date_valid(out)) return true;
    }
    return false;
}

void cmny_month_label(const char *month, char *out, size_t out_size) {
    static const char *names[] = {"", "January", "February", "March", "April", "May", "June",
                                  "July", "August", "September", "October", "November", "December"};
    if (!cmny_month_valid(month)) {
        (void)snprintf(out, out_size, "Unknown month");
        return;
    }
    int mon = (month[5] - '0') * 10 + (month[6] - '0');
    (void)snprintf(out, out_size, "%s %.4s", names[mon], month);
}

bool cmny_text_valid(const char *text, size_t maximum, bool allow_empty) {
    if (text == NULL) return false;
    size_t len = strlen(text);
    if (len > maximum || (!allow_empty && len == 0)) return false;
    bool has_non_space = allow_empty;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 32 || ch > 126) return false;
        if (!isspace(ch)) has_non_space = true;
    }
    return has_non_space;
}

bool cmny_transaction_valid(const CmnyTransaction *tx, bool require_id) {
    return tx != NULL && (!require_id || tx->id > 0) &&
           (tx->kind == CMNY_EXPENSE || tx->kind == CMNY_INCOME) &&
           tx->amount_cents > 0 && tx->amount_cents <= 9000000000000000LL &&
           cmny_text_valid(tx->category, CMNY_CATEGORY_MAX, false) &&
           cmny_text_valid(tx->note, CMNY_NOTE_MAX, true) &&
           cmny_date_valid(tx->occurred_on);
}
