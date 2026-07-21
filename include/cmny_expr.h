#ifndef CMNY_EXPR_H
#define CMNY_EXPR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CMNY_AMOUNT_EXPRESSION_MAX 256U
#define CMNY_DATE_EXPRESSION_MAX 128U

/*
 * Parse an exact two-decimal money expression into cents.
 *
 * Grammar:
 *   expression := product { ("+" | "-") product }
 *   product    := unary { ("*" | "/") signed_integer }
 *   unary      := { "+" | "-" } (amount | "(" expression ")")
 *   amount     := digits [ "." digit [ digit ] ]
 *
 * Multiplication and division therefore accept integer scalars only. Division
 * rounds to the nearest cent, with an exact half-cent rounded away from zero.
 * The final result must be greater than zero. Inputs longer than
 * CMNY_AMOUNT_EXPRESSION_MAX bytes are rejected. On failure, *cents is
 * unchanged.
 */
bool cmny_amount_expression_parse(const unsigned char *input, size_t length,
                                  int64_t *cents);

/* Return true when date is exactly YYYY-MM-DD in the supported 1900..9999 range. */
bool cmny_expression_date_valid(const char *date);

/*
 * Shift an ISO date by days ('d'), weeks ('w'), months ('m'), or years ('y').
 * Month and year shifts clamp the day to the destination month's last day.
 * On failure, out is unchanged.
 */
bool cmny_expression_date_shift(const char *date, int64_t amount, char unit,
                                char out[11]);

/* Compare two supported ISO dates, producing -1, 0, or 1. */
bool cmny_expression_date_compare(const char *left, const char *right,
                                  int *comparison);

/* Produce to - from in calendar days. */
bool cmny_expression_date_days_between(const char *from, const char *to,
                                       int64_t *days);

/*
 * Parse an anchored date expression. Anchors are an ISO date, today, tomorrow,
 * or yesterday. A leading relative shift (for example "+2w") is anchored to
 * base_date. Shifts may be chained, as in "today +1m -2d". Keywords are ASCII
 * case-insensitive. base_date supplies the deterministic meaning of "today".
 * Inputs longer than CMNY_DATE_EXPRESSION_MAX bytes are rejected. On failure,
 * out is unchanged.
 */
bool cmny_date_expression_parse(const unsigned char *input, size_t length,
                                const char *base_date, char out[11]);

#endif
