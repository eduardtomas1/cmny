#ifndef CMNY_PLAN_H
#define CMNY_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CMNY_RECURRENCE_ONE_TIME,
    CMNY_RECURRENCE_DAILY,
    CMNY_RECURRENCE_WEEKLY,
    CMNY_RECURRENCE_MONTHLY,
    CMNY_RECURRENCE_YEARLY
} CmnyRecurrenceFrequency;

typedef struct {
    CmnyRecurrenceFrequency frequency;
    uint32_t interval;
    char start[11];
    char end[11]; /* Empty means no explicit end. */
} CmnyRecurrence;

typedef struct {
    uint64_t ordinal; /* Zero is the start occurrence. */
    bool found;
    char date[11];
} CmnyRecurrenceMatch;

bool cmny_recurrence_valid(const CmnyRecurrence *recurrence);

/*
 * Find the first occurrence on or after on_or_after. interval must be positive
 * (and exactly one for one-time schedules). Month and year occurrences always
 * shift from start, preserving natural anchor alignment after a clamped month.
 * found=false is a successful no-match.
 */
bool cmny_recurrence_next(const CmnyRecurrence *recurrence,
                          const char *on_or_after, CmnyRecurrenceMatch *match);

/*
 * Expand occurrences in the inclusive range. Passing dates=NULL and cap=0 is
 * a count-only query. A non-NULL output with insufficient cap fails without
 * modifying dates or count.
 */
bool cmny_recurrence_expand(const CmnyRecurrence *recurrence,
                            const char *range_start, const char *range_end,
                            CmnyRecurrenceMatch *dates, size_t cap, size_t *count);

typedef enum {
    CMNY_BUDGET_ROLLOVER_NONE,
    CMNY_BUDGET_ROLLOVER_POSITIVE_UNUSED,
    CMNY_BUDGET_ROLLOVER_FULL
} CmnyBudgetRolloverMode;

typedef struct {
    int64_t carry_cents;
    int64_t available_cents;
} CmnyBudgetRollover;

bool cmny_budget_rollover(CmnyBudgetRolloverMode mode,
                          int64_t prior_budget_cents, int64_t prior_spent_cents,
                          int64_t next_budget_cents, CmnyBudgetRollover *rollover);

typedef struct {
    int64_t contribution_cents;
    uint32_t months_remaining;
} CmnyGoalPace;

/* Calendar-month distance is used; a future date in the same month is one month. */
bool cmny_goal_required_monthly(int64_t current_cents, int64_t target_cents,
                                const char *as_of, const char *target_date,
                                CmnyGoalPace *pace);

typedef struct {
    uint64_t months_needed;
    char target_date[11];
} CmnyGoalProjection;

bool cmny_goal_projected_date(int64_t current_cents, int64_t target_cents,
                              int64_t monthly_contribution_cents,
                              const char *as_of, CmnyGoalProjection *projection);

typedef struct {
    int64_t amount_cents; /* Signed: inflow positive, outflow negative. */
    CmnyRecurrence recurrence;
} CmnyCashFlowSchedule;

typedef struct {
    int64_t amount_cents;
    int64_t balance_after_cents;
    size_t schedule_index;
    uint64_t recurrence_ordinal;
    char date[11];
} CmnyForecastOccurrence;

typedef struct {
    int64_t ending_balance_cents;
    int64_t minimum_balance_cents;
    size_t occurrence_count;
    bool has_below_zero;
    char first_below_zero_date[11];
} CmnyForecastSummary;

/*
 * Forecast an inclusive range. Same-day schedules apply in array order.
 * rows=NULL and cap=0 requests only the summary; otherwise cap must fit every
 * occurrence. All outputs remain unchanged on failure.
 */
bool cmny_cash_flow_forecast(int64_t opening_balance_cents,
                             const CmnyCashFlowSchedule *schedules,
                             size_t schedule_count,
                             const char *range_start, const char *range_end,
                             CmnyForecastOccurrence *rows, size_t cap,
                             CmnyForecastSummary *summary);

typedef enum {
    CMNY_BILL_NO_DUE_DATE,
    CMNY_BILL_UPCOMING,
    CMNY_BILL_DUE_TODAY,
    CMNY_BILL_OVERDUE
} CmnyBillStatus;

typedef struct {
    CmnyBillStatus status;
    int64_t days_until_due; /* Negative means overdue. */
} CmnyBillDue;

/* An empty due_date produces NO_DUE_DATE; a recurrence match date can be passed directly. */
bool cmny_bill_due_status(const char *due_date, const char *as_of,
                          CmnyBillDue *due);

#endif
