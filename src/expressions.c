#include "cmny_expr.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define AMOUNT_TOKEN_MAX 128U
#define AMOUNT_DEPTH_MAX 16U
#define DATE_TOKEN_MAX 64U
#define DATE_YEAR_MIN 1900
#define DATE_YEAR_MAX 9999

typedef struct {
    const unsigned char *input;
    size_t length;
    size_t position;
    size_t tokens;
    unsigned int depth;
    bool failed;
} AmountParser;

typedef struct {
    int year;
    int month;
    int day;
} CivilDate;

typedef struct {
    const unsigned char *input;
    size_t length;
    size_t position;
    size_t tokens;
    bool failed;
} DateParser;

static bool ascii_space(unsigned char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
           value == '\f' || value == '\v';
}

static bool ascii_digit(unsigned char value) {
    return value >= '0' && value <= '9';
}

static unsigned char ascii_lower(unsigned char value) {
    if (value >= 'A' && value <= 'Z') return (unsigned char)(value + ('a' - 'A'));
    return value;
}

static void amount_skip_spaces(AmountParser *parser) {
    while (parser->position < parser->length &&
           ascii_space(parser->input[parser->position])) {
        parser->position++;
    }
}

static bool amount_add_token(AmountParser *parser) {
    parser->tokens++;
    if (parser->tokens > AMOUNT_TOKEN_MAX) {
        parser->failed = true;
        return false;
    }
    return true;
}

static bool checked_add(int64_t left, int64_t right, int64_t *out) {
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right)) {
        return false;
    }
    *out = left + right;
    return true;
}

static bool checked_subtract(int64_t left, int64_t right, int64_t *out) {
    if ((right > 0 && left < INT64_MIN + right) ||
        (right < 0 && left > INT64_MAX + right)) {
        return false;
    }
    *out = left - right;
    return true;
}

static bool checked_negate(int64_t value, int64_t *out) {
    if (value == INT64_MIN) return false;
    *out = -value;
    return true;
}

static bool checked_multiply(int64_t value, int64_t scalar, int64_t *out) {
    if (value == 0 || scalar == 0) {
        *out = 0;
        return true;
    }
    if ((value == INT64_MIN && scalar == -1) ||
        (scalar == INT64_MIN && value == -1)) {
        return false;
    }
    if (value > 0) {
        if ((scalar > 0 && value > INT64_MAX / scalar) ||
            (scalar < 0 && scalar < INT64_MIN / value)) {
            return false;
        }
    } else {
        if ((scalar > 0 && value < INT64_MIN / scalar) ||
            (scalar < 0 && value < INT64_MAX / scalar)) {
            return false;
        }
    }
    *out = value * scalar;
    return true;
}

static bool divide_rounded(int64_t value, int64_t scalar, int64_t *out) {
    if (scalar == 0 || (value == INT64_MIN && scalar == -1)) return false;
    int64_t quotient = value / scalar;
    int64_t remainder = value % scalar;
    if (remainder != 0) {
        int64_t denominator = scalar < 0 ? -scalar : scalar;
        int64_t magnitude = remainder < 0 ? -remainder : remainder;
        int64_t threshold = denominator / 2 + denominator % 2;
        if (magnitude >= threshold) {
            bool negative = (value < 0) != (scalar < 0);
            if ((negative && quotient == INT64_MIN) ||
                (!negative && quotient == INT64_MAX)) {
                return false;
            }
            quotient += negative ? -1 : 1;
        }
    }
    *out = quotient;
    return true;
}

static bool amount_parse_expression(AmountParser *parser, int64_t *value);

static bool amount_parse_literal(AmountParser *parser, int64_t *value) {
    amount_skip_spaces(parser);
    if (parser->position >= parser->length ||
        !ascii_digit(parser->input[parser->position]) ||
        !amount_add_token(parser)) {
        return false;
    }

    int64_t whole = 0;
    while (parser->position < parser->length &&
           ascii_digit(parser->input[parser->position])) {
        int digit = (int)(parser->input[parser->position] - '0');
        if (whole > (INT64_MAX - digit) / 10) return false;
        whole = whole * 10 + digit;
        parser->position++;
    }

    int64_t fraction = 0;
    if (parser->position < parser->length && parser->input[parser->position] == '.') {
        parser->position++;
        if (parser->position >= parser->length ||
            !ascii_digit(parser->input[parser->position])) {
            return false;
        }
        fraction = (int64_t)(parser->input[parser->position] - '0') * 10;
        parser->position++;
        if (parser->position < parser->length &&
            ascii_digit(parser->input[parser->position])) {
            fraction += (int64_t)(parser->input[parser->position] - '0');
            parser->position++;
            if (parser->position < parser->length &&
                ascii_digit(parser->input[parser->position])) {
                return false;
            }
        }
    }
    if (whole > (INT64_MAX - fraction) / 100) return false;
    *value = whole * 100 + fraction;
    return true;
}

static bool amount_parse_unary(AmountParser *parser, int64_t *value) {
    amount_skip_spaces(parser);
    bool negative = false;
    while (parser->position < parser->length &&
           (parser->input[parser->position] == '+' ||
            parser->input[parser->position] == '-')) {
        if (!amount_add_token(parser)) return false;
        if (parser->input[parser->position] == '-') negative = !negative;
        parser->position++;
        amount_skip_spaces(parser);
    }

    if (parser->position < parser->length && parser->input[parser->position] == '(') {
        if (!amount_add_token(parser) || parser->depth >= AMOUNT_DEPTH_MAX) return false;
        parser->position++;
        parser->depth++;
        if (!amount_parse_expression(parser, value)) return false;
        amount_skip_spaces(parser);
        if (parser->position >= parser->length || parser->input[parser->position] != ')' ||
            !amount_add_token(parser)) {
            return false;
        }
        parser->position++;
        parser->depth--;
    } else if (!amount_parse_literal(parser, value)) {
        return false;
    }

    return !negative || checked_negate(*value, value);
}

static bool amount_parse_scalar(AmountParser *parser, int64_t *value) {
    amount_skip_spaces(parser);
    bool negative = false;
    if (parser->position < parser->length &&
        (parser->input[parser->position] == '+' ||
         parser->input[parser->position] == '-')) {
        if (!amount_add_token(parser)) return false;
        negative = parser->input[parser->position] == '-';
        parser->position++;
        amount_skip_spaces(parser);
    }
    if (parser->position >= parser->length ||
        !ascii_digit(parser->input[parser->position]) ||
        !amount_add_token(parser)) {
        return false;
    }
    int64_t scalar = 0;
    while (parser->position < parser->length &&
           ascii_digit(parser->input[parser->position])) {
        int digit = (int)(parser->input[parser->position] - '0');
        if (scalar > (INT64_MAX - digit) / 10) return false;
        scalar = scalar * 10 + digit;
        parser->position++;
    }
    *value = negative ? -scalar : scalar;
    return true;
}

static bool amount_parse_product(AmountParser *parser, int64_t *value) {
    if (!amount_parse_unary(parser, value)) return false;
    for (;;) {
        amount_skip_spaces(parser);
        if (parser->position >= parser->length ||
            (parser->input[parser->position] != '*' &&
             parser->input[parser->position] != '/')) {
            return true;
        }
        unsigned char operation = parser->input[parser->position++];
        if (!amount_add_token(parser)) return false;
        int64_t scalar = 0;
        if (!amount_parse_scalar(parser, &scalar)) return false;
        int64_t result = 0;
        bool ok = operation == '*' ? checked_multiply(*value, scalar, &result)
                                   : divide_rounded(*value, scalar, &result);
        if (!ok) return false;
        *value = result;
    }
}

static bool amount_parse_expression(AmountParser *parser, int64_t *value) {
    if (!amount_parse_product(parser, value)) return false;
    for (;;) {
        amount_skip_spaces(parser);
        if (parser->position >= parser->length ||
            (parser->input[parser->position] != '+' &&
             parser->input[parser->position] != '-')) {
            return true;
        }
        unsigned char operation = parser->input[parser->position++];
        if (!amount_add_token(parser)) return false;
        int64_t right = 0;
        if (!amount_parse_product(parser, &right)) return false;
        int64_t result = 0;
        bool ok = operation == '+' ? checked_add(*value, right, &result)
                                   : checked_subtract(*value, right, &result);
        if (!ok) return false;
        *value = result;
    }
}

bool cmny_amount_expression_parse(const unsigned char *input, size_t length,
                                  int64_t *cents) {
    if (input == NULL || cents == NULL || length == 0 ||
        length > CMNY_AMOUNT_EXPRESSION_MAX) {
        return false;
    }
    AmountParser parser = {
        .input = input,
        .length = length,
        .position = 0,
        .tokens = 0,
        .depth = 0,
        .failed = false,
    };
    int64_t result = 0;
    if (!amount_parse_expression(&parser, &result)) return false;
    amount_skip_spaces(&parser);
    if (parser.failed || parser.position != parser.length || parser.depth != 0 || result <= 0) {
        return false;
    }
    *cents = result;
    return true;
}

static bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int days_in_month(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && leap_year(year)) return 29;
    return days[month - 1];
}

static bool parse_iso_date(const unsigned char *input, size_t length, CivilDate *date) {
    if (input == NULL || date == NULL || length != 10 || input[4] != '-' || input[7] != '-') {
        return false;
    }
    static const size_t digit_positions[] = {0, 1, 2, 3, 5, 6, 8, 9};
    for (size_t i = 0; i < sizeof(digit_positions) / sizeof(digit_positions[0]); i++) {
        if (!ascii_digit(input[digit_positions[i]])) return false;
    }
    int year = (int)(input[0] - '0') * 1000 + (int)(input[1] - '0') * 100 +
               (int)(input[2] - '0') * 10 + (int)(input[3] - '0');
    int month = (int)(input[5] - '0') * 10 + (int)(input[6] - '0');
    int day = (int)(input[8] - '0') * 10 + (int)(input[9] - '0');
    if (year < DATE_YEAR_MIN || year > DATE_YEAR_MAX || month < 1 || month > 12 ||
        day < 1 || day > days_in_month(year, month)) {
        return false;
    }
    date->year = year;
    date->month = month;
    date->day = day;
    return true;
}

bool cmny_expression_date_valid(const char *date) {
    if (date == NULL || strlen(date) != 10) return false;
    unsigned char bytes[10];
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)date[i];
    CivilDate parsed = {0};
    return parse_iso_date(bytes, sizeof(bytes), &parsed);
}

static void format_iso_date(const CivilDate *date, char out[11]) {
    out[0] = (char)('0' + date->year / 1000);
    out[1] = (char)('0' + date->year / 100 % 10);
    out[2] = (char)('0' + date->year / 10 % 10);
    out[3] = (char)('0' + date->year % 10);
    out[4] = '-';
    out[5] = (char)('0' + date->month / 10);
    out[6] = (char)('0' + date->month % 10);
    out[7] = '-';
    out[8] = (char)('0' + date->day / 10);
    out[9] = (char)('0' + date->day % 10);
    out[10] = '\0';
}

static int64_t days_from_civil(const CivilDate *date) {
    int64_t year = date->year;
    int64_t month = date->month;
    int64_t day = date->day;
    year -= month <= 2 ? 1 : 0;
    int64_t era = year / 400;
    int64_t year_of_era = year - era * 400;
    int64_t shifted_month = month + (month > 2 ? -3 : 9);
    int64_t day_of_year = (153 * shifted_month + 2) / 5 + day - 1;
    int64_t day_of_era = year_of_era * 365 + year_of_era / 4 -
                         year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era;
}

static CivilDate civil_from_days(int64_t value) {
    int64_t era = value / 146097;
    int64_t day_of_era = value - era * 146097;
    int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
                           day_of_era / 146096) / 365;
    int64_t year = year_of_era + era * 400;
    int64_t day_of_year = day_of_era -
                          (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    int64_t month_piece = (5 * day_of_year + 2) / 153;
    int64_t day = day_of_year - (153 * month_piece + 2) / 5 + 1;
    int64_t month = month_piece + (month_piece < 10 ? 3 : -9);
    year += month <= 2 ? 1 : 0;
    CivilDate result = {(int)year, (int)month, (int)day};
    return result;
}

static bool shift_civil(CivilDate *date, int64_t amount, char unit) {
    unit = (char)ascii_lower((unsigned char)unit);
    if (unit == 'd' || unit == 'w') {
        int64_t days = amount;
        if (unit == 'w') {
            if (amount > INT64_MAX / 7 || amount < INT64_MIN / 7) return false;
            days = amount * 7;
        }
        int64_t current = days_from_civil(date);
        int64_t minimum = days_from_civil(&(CivilDate){DATE_YEAR_MIN, 1, 1});
        int64_t maximum = days_from_civil(&(CivilDate){DATE_YEAR_MAX, 12, 31});
        if (days > maximum - current || days < minimum - current) return false;
        *date = civil_from_days(current + days);
        return true;
    }
    if (unit == 'm') {
        int64_t current = (int64_t)date->year * 12 + (date->month - 1);
        int64_t minimum = (int64_t)DATE_YEAR_MIN * 12;
        int64_t maximum = (int64_t)DATE_YEAR_MAX * 12 + 11;
        if (amount > maximum - current || amount < minimum - current) return false;
        int64_t target = current + amount;
        date->year = (int)(target / 12);
        date->month = (int)(target % 12) + 1;
    } else if (unit == 'y') {
        if (amount > DATE_YEAR_MAX - date->year || amount < DATE_YEAR_MIN - date->year) {
            return false;
        }
        date->year += (int)amount;
    } else {
        return false;
    }
    int maximum_day = days_in_month(date->year, date->month);
    if (date->day > maximum_day) date->day = maximum_day;
    return true;
}

bool cmny_expression_date_shift(const char *date, int64_t amount, char unit,
                                char out[11]) {
    if (date == NULL || out == NULL || strlen(date) != 10) return false;
    unsigned char bytes[10];
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)date[i];
    CivilDate parsed = {0};
    if (!parse_iso_date(bytes, sizeof(bytes), &parsed) ||
        !shift_civil(&parsed, amount, unit)) {
        return false;
    }
    char result[11];
    format_iso_date(&parsed, result);
    memcpy(out, result, sizeof(result));
    return true;
}

bool cmny_expression_date_compare(const char *left, const char *right,
                                  int *comparison) {
    if (left == NULL || right == NULL || comparison == NULL ||
        !cmny_expression_date_valid(left) || !cmny_expression_date_valid(right)) {
        return false;
    }
    int result = strcmp(left, right);
    int normalized = result < 0 ? -1 : result > 0 ? 1 : 0;
    *comparison = normalized;
    return true;
}

bool cmny_expression_date_days_between(const char *from, const char *to,
                                       int64_t *days) {
    if (from == NULL || to == NULL || days == NULL || strlen(from) != 10 ||
        strlen(to) != 10) {
        return false;
    }
    CivilDate parsed_from = {0};
    CivilDate parsed_to = {0};
    if (!parse_iso_date((const unsigned char *)from, 10, &parsed_from) ||
        !parse_iso_date((const unsigned char *)to, 10, &parsed_to)) {
        return false;
    }
    int64_t result = days_from_civil(&parsed_to) - days_from_civil(&parsed_from);
    *days = result;
    return true;
}

static void date_skip_spaces(DateParser *parser) {
    while (parser->position < parser->length &&
           ascii_space(parser->input[parser->position])) {
        parser->position++;
    }
}

static bool date_add_token(DateParser *parser) {
    parser->tokens++;
    if (parser->tokens > DATE_TOKEN_MAX) {
        parser->failed = true;
        return false;
    }
    return true;
}

static bool date_match_word(DateParser *parser, const char *word) {
    size_t word_length = strlen(word);
    if (parser->length - parser->position < word_length) return false;
    for (size_t i = 0; i < word_length; i++) {
        if (ascii_lower(parser->input[parser->position + i]) !=
            (unsigned char)word[i]) {
            return false;
        }
    }
    parser->position += word_length;
    return date_add_token(parser);
}

static bool date_parse_base(const char *base_date, CivilDate *base) {
    if (base_date == NULL || strlen(base_date) != 10) return false;
    unsigned char bytes[10];
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)base_date[i];
    return parse_iso_date(bytes, sizeof(bytes), base);
}

static bool date_parse_anchor(DateParser *parser, const CivilDate *base,
                              CivilDate *date) {
    date_skip_spaces(parser);
    if (parser->length - parser->position >= 10 &&
        parse_iso_date(parser->input + parser->position, 10, date)) {
        parser->position += 10;
        return date_add_token(parser);
    }
    *date = *base;
    if (date_match_word(parser, "tomorrow")) return shift_civil(date, 1, 'd');
    if (date_match_word(parser, "yesterday")) return shift_civil(date, -1, 'd');
    if (date_match_word(parser, "today")) return true;
    return false;
}

static bool date_parse_magnitude(DateParser *parser, int64_t *magnitude) {
    date_skip_spaces(parser);
    if (parser->position >= parser->length ||
        !ascii_digit(parser->input[parser->position]) ||
        !date_add_token(parser)) {
        return false;
    }
    int64_t result = 0;
    while (parser->position < parser->length &&
           ascii_digit(parser->input[parser->position])) {
        int digit = (int)(parser->input[parser->position] - '0');
        if (result > (INT64_MAX - digit) / 10) return false;
        result = result * 10 + digit;
        parser->position++;
    }
    *magnitude = result;
    return true;
}

bool cmny_date_expression_parse(const unsigned char *input, size_t length,
                                const char *base_date, char out[11]) {
    if (input == NULL || out == NULL || length == 0 ||
        length > CMNY_DATE_EXPRESSION_MAX) {
        return false;
    }
    CivilDate base = {0};
    if (!date_parse_base(base_date, &base)) return false;
    DateParser parser = {
        .input = input,
        .length = length,
        .position = 0,
        .tokens = 0,
        .failed = false,
    };
    date_skip_spaces(&parser);
    CivilDate date = base;
    bool relative_only = parser.position < parser.length &&
                         (parser.input[parser.position] == '+' ||
                          parser.input[parser.position] == '-');
    if (!relative_only && !date_parse_anchor(&parser, &base, &date)) return false;

    bool shifted = false;
    for (;;) {
        date_skip_spaces(&parser);
        if (parser.position >= parser.length) break;
        unsigned char sign = parser.input[parser.position];
        if ((sign != '+' && sign != '-') || !date_add_token(&parser)) return false;
        parser.position++;
        int64_t magnitude = 0;
        if (!date_parse_magnitude(&parser, &magnitude)) return false;
        date_skip_spaces(&parser);
        if (parser.position >= parser.length) return false;
        char unit = (char)ascii_lower(parser.input[parser.position++]);
        if (!date_add_token(&parser) ||
            (unit != 'd' && unit != 'w' && unit != 'm' && unit != 'y')) {
            return false;
        }
        int64_t amount = sign == '-' ? -magnitude : magnitude;
        if (!shift_civil(&date, amount, unit)) return false;
        shifted = true;
    }
    if (parser.failed || parser.position != parser.length ||
        (relative_only && !shifted)) {
        return false;
    }
    char result[11];
    format_iso_date(&date, result);
    memcpy(out, result, sizeof(result));
    return true;
}
