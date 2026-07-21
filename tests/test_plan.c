#include "cmny_expr.h"
#include "cmny_plan.h"
#include "test.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(offsetof(CmnyCashFlowSchedule, amount_cents) % _Alignof(int64_t) == 0,
               "schedule amount alignment");
_Static_assert(offsetof(CmnyForecastOccurrence, balance_after_cents) %
                   _Alignof(int64_t) == 0,
               "forecast balance alignment");

static CmnyRecurrence recurrence(CmnyRecurrenceFrequency frequency, uint32_t interval,
                                 const char *start, const char *end) {
    CmnyRecurrence result = {.frequency = frequency, .interval = interval};
    (void)snprintf(result.start, sizeof(result.start), "%s", start);
    if (end != NULL) (void)snprintf(result.end, sizeof(result.end), "%s", end);
    return result;
}

static void assert_next(const CmnyRecurrence *value, const char *from,
                        bool found, const char *date, uint64_t ordinal) {
    CmnyRecurrenceMatch match = {0};
    ASSERT_TRUE(cmny_recurrence_next(value, from, &match));
    ASSERT_TRUE(match.found == found);
    if (found) {
        ASSERT_TRUE(strcmp(match.date, date) == 0);
        ASSERT_EQ_I64(ordinal, match.ordinal);
    }
}

static void test_recurrence_next_tables(void) {
    CmnyRecurrence once = recurrence(CMNY_RECURRENCE_ONE_TIME, 1,
                                     "2024-02-29", NULL);
    ASSERT_TRUE(cmny_recurrence_valid(&once));
    assert_next(&once, "2024-01-01", true, "2024-02-29", 0);
    assert_next(&once, "2024-02-29", true, "2024-02-29", 0);
    assert_next(&once, "2024-03-01", false, NULL, 0);

    CmnyRecurrence daily = recurrence(CMNY_RECURRENCE_DAILY, 2,
                                      "2024-02-27", "2024-03-04");
    assert_next(&daily, "2024-02-28", true, "2024-02-29", 1);
    assert_next(&daily, "2024-03-01", true, "2024-03-02", 2);
    assert_next(&daily, "2024-03-04", true, "2024-03-04", 3);
    assert_next(&daily, "2024-03-05", false, NULL, 0);

    CmnyRecurrence weekly = recurrence(CMNY_RECURRENCE_WEEKLY, 2,
                                       "2024-01-01", NULL);
    assert_next(&weekly, "2024-01-02", true, "2024-01-15", 1);
    assert_next(&weekly, "2024-01-15", true, "2024-01-15", 1);

    CmnyRecurrence monthly = recurrence(CMNY_RECURRENCE_MONTHLY, 1,
                                        "2024-01-31", NULL);
    assert_next(&monthly, "2024-02-01", true, "2024-02-29", 1);
    assert_next(&monthly, "2024-03-01", true, "2024-03-31", 2);
    assert_next(&monthly, "2024-04-01", true, "2024-04-30", 3);
    assert_next(&monthly, "2025-02-28", true, "2025-02-28", 13);

    CmnyRecurrence every_three_months = recurrence(CMNY_RECURRENCE_MONTHLY, 3,
                                                    "2023-11-30", NULL);
    assert_next(&every_three_months, "2024-02-29", true, "2024-02-29", 1);
    assert_next(&every_three_months, "2024-03-01", true, "2024-05-30", 2);

    CmnyRecurrence yearly = recurrence(CMNY_RECURRENCE_YEARLY, 1,
                                       "2024-02-29", NULL);
    assert_next(&yearly, "2025-01-01", true, "2025-02-28", 1);
    assert_next(&yearly, "2027-03-01", true, "2028-02-29", 4);

    CmnyRecurrence huge = recurrence(CMNY_RECURRENCE_DAILY, UINT32_MAX,
                                     "1900-01-01", NULL);
    assert_next(&huge, "1900-01-01", true, "1900-01-01", 0);
    assert_next(&huge, "1900-01-02", false, NULL, 0);

    CmnyRecurrence boundary = recurrence(CMNY_RECURRENCE_DAILY, 1,
                                         "9999-12-31", NULL);
    assert_next(&boundary, "9999-12-31", true, "9999-12-31", 0);

    CmnyRecurrence invalid = recurrence(CMNY_RECURRENCE_MONTHLY, 0,
                                        "2024-01-01", NULL);
    ASSERT_TRUE(!cmny_recurrence_valid(&invalid));
    invalid = recurrence(CMNY_RECURRENCE_ONE_TIME, 2, "2024-01-01", NULL);
    ASSERT_TRUE(!cmny_recurrence_valid(&invalid));
    invalid = recurrence(CMNY_RECURRENCE_DAILY, 1,
                         "2024-01-02", "2024-01-01");
    ASSERT_TRUE(!cmny_recurrence_valid(&invalid));
    CmnyRecurrenceMatch sentinel = {.ordinal = 99, .found = true, .date = "unchanged"};
    CmnyRecurrenceMatch original = sentinel;
    ASSERT_TRUE(!cmny_recurrence_next(&invalid, "2024-01-01", &sentinel));
    ASSERT_TRUE(memcmp(&sentinel, &original, sizeof(sentinel)) == 0);
}

static void test_recurrence_expansion_and_properties(void) {
    CmnyRecurrence monthly = recurrence(CMNY_RECURRENCE_MONTHLY, 1,
                                        "2024-01-31", "2024-06-30");
    CmnyRecurrenceMatch dates[6] = {{0}};
    size_t count = 0;
    ASSERT_TRUE(cmny_recurrence_expand(&monthly, "2024-02-01", "2024-05-31",
                                       dates, 6, &count));
    ASSERT_EQ_I64(4, count);
    static const char *expected[] = {
        "2024-02-29", "2024-03-31", "2024-04-30", "2024-05-31"
    };
    for (size_t index = 0; index < count; index++) {
        ASSERT_TRUE(strcmp(dates[index].date, expected[index]) == 0);
        ASSERT_EQ_I64(index + 1U, dates[index].ordinal);
        char direct[11];
        ASSERT_TRUE(cmny_expression_date_shift(monthly.start,
                                               (int64_t)dates[index].ordinal,
                                               'm', direct));
        ASSERT_TRUE(strcmp(direct, dates[index].date) == 0);
    }

    count = 999;
    ASSERT_TRUE(cmny_recurrence_expand(&monthly, "2024-01-01", "2024-12-31",
                                       NULL, 0, &count));
    ASSERT_EQ_I64(6, count);

    CmnyRecurrenceMatch too_small[1] = {{.ordinal = 88, .found = true,
                                         .date = "unchanged"}};
    CmnyRecurrenceMatch original = too_small[0];
    count = 77;
    ASSERT_TRUE(!cmny_recurrence_expand(&monthly, "2024-01-01", "2024-12-31",
                                        too_small, 1, &count));
    ASSERT_TRUE(memcmp(&too_small[0], &original, sizeof(original)) == 0);
    ASSERT_EQ_I64(77, count);

    CmnyRecurrence daily = recurrence(CMNY_RECURRENCE_DAILY, 17,
                                      "1900-01-01", "2100-12-31");
    CmnyRecurrenceMatch sample[5000];
    ASSERT_TRUE(cmny_recurrence_expand(&daily, "1900-01-01", "2100-12-31",
                                       sample, 5000, &count));
    for (size_t index = 0; index < count; index++) {
        int64_t distance = 0;
        ASSERT_TRUE(cmny_expression_date_days_between(daily.start,
                                                      sample[index].date,
                                                      &distance));
        ASSERT_EQ_I64((int64_t)index * 17, distance);
    }
}

static void test_budget_rollover(void) {
    CmnyBudgetRollover result = {0};
    ASSERT_TRUE(cmny_budget_rollover(CMNY_BUDGET_ROLLOVER_NONE,
                                     10000, 6000, 20000, &result));
    ASSERT_EQ_I64(0, result.carry_cents);
    ASSERT_EQ_I64(20000, result.available_cents);
    ASSERT_TRUE(cmny_budget_rollover(CMNY_BUDGET_ROLLOVER_POSITIVE_UNUSED,
                                     10000, 6000, 20000, &result));
    ASSERT_EQ_I64(4000, result.carry_cents);
    ASSERT_EQ_I64(24000, result.available_cents);
    ASSERT_TRUE(cmny_budget_rollover(CMNY_BUDGET_ROLLOVER_POSITIVE_UNUSED,
                                     10000, 15000, 20000, &result));
    ASSERT_EQ_I64(0, result.carry_cents);
    ASSERT_TRUE(cmny_budget_rollover(CMNY_BUDGET_ROLLOVER_FULL,
                                     10000, 15000, 20000, &result));
    ASSERT_EQ_I64(-5000, result.carry_cents);
    ASSERT_EQ_I64(15000, result.available_cents);
    ASSERT_TRUE(cmny_budget_rollover(CMNY_BUDGET_ROLLOVER_FULL,
                                     0, INT64_MAX, 0, &result));
    ASSERT_EQ_I64(-INT64_MAX, result.available_cents);

    result = (CmnyBudgetRollover){.carry_cents = 77, .available_cents = 88};
    CmnyBudgetRollover original = result;
    ASSERT_TRUE(!cmny_budget_rollover(CMNY_BUDGET_ROLLOVER_FULL,
                                      INT64_MAX, 0, 1, &result));
    ASSERT_TRUE(memcmp(&result, &original, sizeof(result)) == 0);
    ASSERT_TRUE(!cmny_budget_rollover(CMNY_BUDGET_ROLLOVER_NONE,
                                      -1, 0, 0, &result));
    ASSERT_TRUE(memcmp(&result, &original, sizeof(result)) == 0);
}

static void test_goal_pacing(void) {
    CmnyGoalPace pace = {0};
    ASSERT_TRUE(cmny_goal_required_monthly(100, 1000, "2024-01-15",
                                           "2024-04-15", &pace));
    ASSERT_EQ_I64(3, pace.months_remaining);
    ASSERT_EQ_I64(300, pace.contribution_cents);
    ASSERT_TRUE(cmny_goal_required_monthly(100, 1001, "2024-01-15",
                                           "2024-04-15", &pace));
    ASSERT_EQ_I64(301, pace.contribution_cents);
    ASSERT_TRUE(cmny_goal_required_monthly(0, 501, "2024-01-01",
                                           "2024-01-31", &pace));
    ASSERT_EQ_I64(1, pace.months_remaining);
    ASSERT_EQ_I64(501, pace.contribution_cents);
    ASSERT_TRUE(cmny_goal_required_monthly(1000, 1000, "2024-01-01",
                                           "2024-01-01", &pace));
    ASSERT_EQ_I64(0, pace.months_remaining);
    ASSERT_EQ_I64(0, pace.contribution_cents);
    ASSERT_TRUE(cmny_goal_required_monthly(0, INT64_MAX, "2024-01-01",
                                           "2024-02-01", &pace));
    ASSERT_EQ_I64(INT64_MAX, pace.contribution_cents);

    CmnyGoalProjection projection = {0};
    ASSERT_TRUE(cmny_goal_projected_date(100, 1001, 300, "2024-01-31",
                                         &projection));
    ASSERT_EQ_I64(4, projection.months_needed);
    ASSERT_TRUE(strcmp(projection.target_date, "2024-05-31") == 0);
    ASSERT_TRUE(cmny_goal_projected_date(1000, 1000, 0, "2024-02-29",
                                         &projection));
    ASSERT_EQ_I64(0, projection.months_needed);
    ASSERT_TRUE(strcmp(projection.target_date, "2024-02-29") == 0);

    pace = (CmnyGoalPace){.contribution_cents = 77, .months_remaining = 88};
    CmnyGoalPace pace_original = pace;
    ASSERT_TRUE(!cmny_goal_required_monthly(-1, 100, "2024-01-01",
                                            "2024-02-01", &pace));
    ASSERT_TRUE(memcmp(&pace, &pace_original, sizeof(pace)) == 0);
    ASSERT_TRUE(!cmny_goal_required_monthly(0, 100, "2024-02-01",
                                            "2024-01-01", &pace));
    ASSERT_TRUE(memcmp(&pace, &pace_original, sizeof(pace)) == 0);

    projection = (CmnyGoalProjection){.months_needed = 77,
                                      .target_date = "unchanged"};
    CmnyGoalProjection projection_original = projection;
    ASSERT_TRUE(!cmny_goal_projected_date(0, 100, 0, "2024-01-01",
                                          &projection));
    ASSERT_TRUE(memcmp(&projection, &projection_original,
                       sizeof(projection)) == 0);
    ASSERT_TRUE(!cmny_goal_projected_date(0, INT64_MAX, 1, "9999-12-31",
                                          &projection));
    ASSERT_TRUE(memcmp(&projection, &projection_original,
                       sizeof(projection)) == 0);
}

static void test_forecast(void) {
    CmnyCashFlowSchedule schedules[3] = {
        {
            .amount_cents = 1000,
            .recurrence = {
                .frequency = CMNY_RECURRENCE_MONTHLY, .interval = 1,
                .start = "2024-01-31",
            },
        },
        {
            .amount_cents = -300,
            .recurrence = {
                .frequency = CMNY_RECURRENCE_WEEKLY, .interval = 1,
                .start = "2024-02-01", .end = "2024-02-15",
            },
        },
        {
            .amount_cents = -900,
            .recurrence = {
                .frequency = CMNY_RECURRENCE_ONE_TIME, .interval = 1,
                .start = "2024-02-01",
            },
        },
    };
    CmnyForecastOccurrence rows[8] = {{0}};
    CmnyForecastSummary summary = {0};
    ASSERT_TRUE(cmny_cash_flow_forecast(500, schedules, 3,
                                        "2024-01-31", "2024-02-29",
                                        rows, 8, &summary));
    ASSERT_EQ_I64(6, summary.occurrence_count);
    ASSERT_EQ_I64(700, summary.ending_balance_cents);
    ASSERT_EQ_I64(-300, summary.minimum_balance_cents);
    ASSERT_TRUE(summary.has_below_zero);
    ASSERT_TRUE(strcmp(summary.first_below_zero_date, "2024-02-15") == 0);
    static const char *dates[] = {
        "2024-01-31", "2024-02-01", "2024-02-01",
        "2024-02-08", "2024-02-15", "2024-02-29"
    };
    const size_t indexes[] = {0, 1, 2, 1, 1, 0};
    const int64_t balances[] = {1500, 1200, 300, 0, -300, 700};
    for (size_t index = 0; index < summary.occurrence_count; index++) {
        ASSERT_TRUE(strcmp(rows[index].date, dates[index]) == 0);
        ASSERT_EQ_I64(indexes[index], rows[index].schedule_index);
        ASSERT_EQ_I64(balances[index], rows[index].balance_after_cents);
    }

    CmnyForecastSummary summary_only = {0};
    ASSERT_TRUE(cmny_cash_flow_forecast(500, schedules, 3,
                                        "2024-01-31", "2024-02-29",
                                        NULL, 0, &summary_only));
    ASSERT_TRUE(memcmp(&summary, &summary_only, sizeof(summary)) == 0);

    CmnyForecastOccurrence small[1] = {{.amount_cents = 77,
                                        .balance_after_cents = 88,
                                        .date = "unchanged"}};
    CmnyForecastOccurrence small_original = small[0];
    summary = (CmnyForecastSummary){.ending_balance_cents = 91,
                                    .minimum_balance_cents = 92,
                                    .occurrence_count = 93};
    CmnyForecastSummary summary_original = summary;
    ASSERT_TRUE(!cmny_cash_flow_forecast(500, schedules, 3,
                                         "2024-01-31", "2024-02-29",
                                         small, 1, &summary));
    ASSERT_TRUE(memcmp(&small[0], &small_original, sizeof(small[0])) == 0);
    ASSERT_TRUE(memcmp(&summary, &summary_original, sizeof(summary)) == 0);

    CmnyCashFlowSchedule overflow = {
        .amount_cents = 1,
        .recurrence = {
            .frequency = CMNY_RECURRENCE_ONE_TIME,
            .interval = 1,
            .start = "2024-01-01",
        },
    };
    ASSERT_TRUE(!cmny_cash_flow_forecast(INT64_MAX, &overflow, 1,
                                         "2024-01-01", "2024-01-01",
                                         NULL, 0, &summary));
    ASSERT_TRUE(memcmp(&summary, &summary_original, sizeof(summary)) == 0);
    overflow.amount_cents = 0;
    ASSERT_TRUE(!cmny_cash_flow_forecast(0, &overflow, 1,
                                         "2024-01-01", "2024-01-01",
                                         NULL, 0, &summary));
    overflow.amount_cents = -1;
    ASSERT_TRUE(!cmny_cash_flow_forecast(INT64_MIN, &overflow, 1,
                                         "2024-01-01", "2024-01-01",
                                         NULL, 0, &summary));
    ASSERT_TRUE(memcmp(&summary, &summary_original, sizeof(summary)) == 0);

    ASSERT_TRUE(cmny_cash_flow_forecast(-1, NULL, 0,
                                        "2024-01-01", "2024-01-31",
                                        NULL, 0, &summary));
    ASSERT_EQ_I64(-1, summary.minimum_balance_cents);
    ASSERT_TRUE(summary.has_below_zero);
    ASSERT_TRUE(strcmp(summary.first_below_zero_date, "2024-01-01") == 0);
}

static void test_bill_status(void) {
    CmnyBillDue due = {0};
    ASSERT_TRUE(cmny_bill_due_status("2024-03-01", "2024-02-28", &due));
    ASSERT_TRUE(due.status == CMNY_BILL_UPCOMING);
    ASSERT_EQ_I64(2, due.days_until_due);
    ASSERT_TRUE(cmny_bill_due_status("2024-02-29", "2024-02-29", &due));
    ASSERT_TRUE(due.status == CMNY_BILL_DUE_TODAY);
    ASSERT_EQ_I64(0, due.days_until_due);
    ASSERT_TRUE(cmny_bill_due_status("2024-02-28", "2024-03-01", &due));
    ASSERT_TRUE(due.status == CMNY_BILL_OVERDUE);
    ASSERT_EQ_I64(-2, due.days_until_due);
    ASSERT_TRUE(cmny_bill_due_status("", "2024-03-01", &due));
    ASSERT_TRUE(due.status == CMNY_BILL_NO_DUE_DATE);

    due = (CmnyBillDue){.status = CMNY_BILL_OVERDUE, .days_until_due = 77};
    CmnyBillDue original = due;
    ASSERT_TRUE(!cmny_bill_due_status("2024-02-30", "2024-03-01", &due));
    ASSERT_TRUE(memcmp(&due, &original, sizeof(due)) == 0);
}

int main(void) {
    test_recurrence_next_tables();
    test_recurrence_expansion_and_properties();
    test_budget_rollover();
    test_goal_pacing();
    test_forecast();
    test_bill_status();
    (void)printf("ok - planning primitive tests\n");
    return 0;
}
