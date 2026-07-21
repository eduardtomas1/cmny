#include "cmny_plan.h"

#include "cmny_expr.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(CmnyCashFlowSchedule, amount_cents) % _Alignof(int64_t) == 0,
               "cash-flow amounts must be naturally aligned");
_Static_assert(offsetof(CmnyForecastOccurrence, amount_cents) % _Alignof(int64_t) == 0,
               "forecast amounts must be naturally aligned");
_Static_assert(offsetof(CmnyBudgetRollover, available_cents) % _Alignof(int64_t) == 0,
               "budget amounts must be naturally aligned");

static bool checked_add(int64_t left, int64_t right, int64_t *out) {
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right)) {
        return false;
    }
    *out = left + right;
    return true;
}

static int year_of(const char *date) {
    return (date[0] - '0') * 1000 + (date[1] - '0') * 100 +
           (date[2] - '0') * 10 + (date[3] - '0');
}

static int month_of(const char *date) {
    return (date[5] - '0') * 10 + (date[6] - '0');
}

static int64_t month_index(const char *date) {
    return (int64_t)year_of(date) * 12 + month_of(date) - 1;
}

static bool date_not_after(const char *left, const char *right) {
    int comparison = 0;
    return cmny_expression_date_compare(left, right, &comparison) && comparison <= 0;
}

bool cmny_recurrence_valid(const CmnyRecurrence *recurrence) {
    if (recurrence == NULL || recurrence->interval == 0 ||
        !cmny_expression_date_valid(recurrence->start) ||
        recurrence->frequency < CMNY_RECURRENCE_ONE_TIME ||
        recurrence->frequency > CMNY_RECURRENCE_YEARLY ||
        (recurrence->frequency == CMNY_RECURRENCE_ONE_TIME &&
         recurrence->interval != 1)) {
        return false;
    }
    if (recurrence->end[0] == '\0') return true;
    return cmny_expression_date_valid(recurrence->end) &&
           date_not_after(recurrence->start, recurrence->end);
}

static bool occurrence_at(const CmnyRecurrence *recurrence, uint64_t ordinal,
                          char date[11]) {
    if (recurrence->frequency == CMNY_RECURRENCE_ONE_TIME) {
        if (ordinal != 0) return false;
        memcpy(date, recurrence->start, 11);
    } else {
        if (ordinal > (uint64_t)INT64_MAX / recurrence->interval) return false;
        int64_t amount = (int64_t)(ordinal * recurrence->interval);
        char unit = recurrence->frequency == CMNY_RECURRENCE_DAILY ? 'd' :
                    recurrence->frequency == CMNY_RECURRENCE_WEEKLY ? 'w' :
                    recurrence->frequency == CMNY_RECURRENCE_MONTHLY ? 'm' : 'y';
        if (!cmny_expression_date_shift(recurrence->start, amount, unit, date)) {
            return false;
        }
    }
    return recurrence->end[0] == '\0' || date_not_after(date, recurrence->end);
}

static uint64_t ceil_positive_days(int64_t days, uint64_t step) {
    uint64_t magnitude = (uint64_t)days;
    return magnitude / step + (magnitude % step != 0 ? 1U : 0U);
}

static bool recurrence_next_internal(const CmnyRecurrence *recurrence,
                                     const char *on_or_after,
                                     CmnyRecurrenceMatch *match) {
    CmnyRecurrenceMatch result = {0};
    if (recurrence->end[0] != '\0' && !date_not_after(on_or_after, recurrence->end)) {
        *match = result;
        return true;
    }
    int start_comparison = 0;
    if (!cmny_expression_date_compare(on_or_after, recurrence->start,
                                      &start_comparison)) {
        return false;
    }
    uint64_t ordinal = 0;
    if (start_comparison > 0) {
        if (recurrence->frequency == CMNY_RECURRENCE_ONE_TIME) {
            *match = result;
            return true;
        }
        if (recurrence->frequency == CMNY_RECURRENCE_DAILY ||
            recurrence->frequency == CMNY_RECURRENCE_WEEKLY) {
            int64_t days = 0;
            if (!cmny_expression_date_days_between(recurrence->start,
                                                    on_or_after, &days) || days <= 0) {
                return false;
            }
            uint64_t step = recurrence->interval;
            if (recurrence->frequency == CMNY_RECURRENCE_WEEKLY) step *= 7U;
            ordinal = ceil_positive_days(days, step);
        } else if (recurrence->frequency == CMNY_RECURRENCE_MONTHLY) {
            int64_t months = month_index(on_or_after) - month_index(recurrence->start);
            ordinal = months > 0 ? (uint64_t)months / recurrence->interval : 0;
        } else {
            int years = year_of(on_or_after) - year_of(recurrence->start);
            ordinal = years > 0 ? (uint64_t)years / recurrence->interval : 0;
        }
    }

    char candidate[11];
    if (!occurrence_at(recurrence, ordinal, candidate)) {
        *match = result;
        return true;
    }
    int candidate_comparison = 0;
    if (!cmny_expression_date_compare(candidate, on_or_after,
                                      &candidate_comparison)) {
        return false;
    }
    if (candidate_comparison < 0) {
        if (ordinal == UINT64_MAX ||
            !occurrence_at(recurrence, ordinal + 1U, candidate)) {
            *match = result;
            return true;
        }
        ordinal++;
    }
    result.ordinal = ordinal;
    result.found = true;
    memcpy(result.date, candidate, sizeof(result.date));
    *match = result;
    return true;
}

bool cmny_recurrence_next(const CmnyRecurrence *recurrence,
                          const char *on_or_after, CmnyRecurrenceMatch *match) {
    if (!cmny_recurrence_valid(recurrence) || on_or_after == NULL ||
        !cmny_expression_date_valid(on_or_after) || match == NULL) {
        return false;
    }
    CmnyRecurrenceMatch result = {0};
    if (!recurrence_next_internal(recurrence, on_or_after, &result)) return false;
    *match = result;
    return true;
}

static bool count_range(const CmnyRecurrence *recurrence, const char *range_start,
                        const char *range_end, size_t *required) {
    CmnyRecurrenceMatch match = {0};
    if (!recurrence_next_internal(recurrence, range_start, &match)) return false;
    size_t count = 0;
    while (match.found && date_not_after(match.date, range_end)) {
        if (count == SIZE_MAX) return false;
        count++;
        if (match.ordinal == UINT64_MAX) break;
        char next[11];
        if (!occurrence_at(recurrence, match.ordinal + 1U, next)) break;
        match.ordinal++;
        memcpy(match.date, next, sizeof(match.date));
    }
    *required = count;
    return true;
}

bool cmny_recurrence_expand(const CmnyRecurrence *recurrence,
                            const char *range_start, const char *range_end,
                            CmnyRecurrenceMatch *dates, size_t cap, size_t *count) {
    if (!cmny_recurrence_valid(recurrence) || range_start == NULL ||
        range_end == NULL || !cmny_expression_date_valid(range_start) ||
        !cmny_expression_date_valid(range_end) ||
        !date_not_after(range_start, range_end) || count == NULL ||
        (dates == NULL && cap != 0)) {
        return false;
    }
    size_t required = 0;
    if (!count_range(recurrence, range_start, range_end, &required)) return false;
    if (dates != NULL && required > cap) return false;
    if (dates != NULL) {
        CmnyRecurrenceMatch match = {0};
        if (!recurrence_next_internal(recurrence, range_start, &match)) return false;
        for (size_t index = 0; index < required; index++) {
            dates[index] = match;
            if (index + 1U < required) {
                char next[11];
                if (match.ordinal == UINT64_MAX ||
                    !occurrence_at(recurrence, match.ordinal + 1U, next)) {
                    return false;
                }
                match.ordinal++;
                memcpy(match.date, next, sizeof(match.date));
            }
        }
    }
    *count = required;
    return true;
}

bool cmny_budget_rollover(CmnyBudgetRolloverMode mode,
                          int64_t prior_budget_cents, int64_t prior_spent_cents,
                          int64_t next_budget_cents, CmnyBudgetRollover *rollover) {
    if (rollover == NULL || prior_budget_cents < 0 || prior_spent_cents < 0 ||
        next_budget_cents < 0 || mode < CMNY_BUDGET_ROLLOVER_NONE ||
        mode > CMNY_BUDGET_ROLLOVER_FULL) {
        return false;
    }
    int64_t difference = prior_budget_cents - prior_spent_cents;
    int64_t carry = mode == CMNY_BUDGET_ROLLOVER_NONE ? 0 : difference;
    if (mode == CMNY_BUDGET_ROLLOVER_POSITIVE_UNUSED && carry < 0) carry = 0;
    int64_t available = 0;
    if (!checked_add(next_budget_cents, carry, &available)) return false;
    CmnyBudgetRollover result = {
        .carry_cents = carry,
        .available_cents = available,
    };
    *rollover = result;
    return true;
}

bool cmny_goal_required_monthly(int64_t current_cents, int64_t target_cents,
                                const char *as_of, const char *target_date,
                                CmnyGoalPace *pace) {
    if (pace == NULL || current_cents < 0 || target_cents < 0 ||
        as_of == NULL || target_date == NULL ||
        !cmny_expression_date_valid(as_of) ||
        !cmny_expression_date_valid(target_date) ||
        !date_not_after(as_of, target_date)) {
        return false;
    }
    CmnyGoalPace result = {0};
    if (current_cents < target_cents) {
        int64_t month_difference = month_index(target_date) - month_index(as_of);
        if (month_difference == 0) month_difference = 1;
        if (month_difference < 0 || month_difference > UINT32_MAX) return false;
        int64_t remaining = target_cents - current_cents;
        int64_t contribution = remaining / month_difference;
        if (remaining % month_difference != 0) contribution++;
        result.contribution_cents = contribution;
        result.months_remaining = (uint32_t)month_difference;
    }
    *pace = result;
    return true;
}

bool cmny_goal_projected_date(int64_t current_cents, int64_t target_cents,
                              int64_t monthly_contribution_cents,
                              const char *as_of, CmnyGoalProjection *projection) {
    if (projection == NULL || current_cents < 0 || target_cents < 0 ||
        monthly_contribution_cents < 0 || as_of == NULL ||
        !cmny_expression_date_valid(as_of)) {
        return false;
    }
    CmnyGoalProjection result = {0};
    memcpy(result.target_date, as_of, sizeof(result.target_date));
    if (current_cents < target_cents) {
        if (monthly_contribution_cents == 0) return false;
        uint64_t remaining = (uint64_t)(target_cents - current_cents);
        uint64_t contribution = (uint64_t)monthly_contribution_cents;
        uint64_t months = remaining / contribution +
                          (remaining % contribution != 0 ? 1U : 0U);
        if (months > (uint64_t)INT64_MAX ||
            !cmny_expression_date_shift(as_of, (int64_t)months, 'm',
                                        result.target_date)) {
            return false;
        }
        result.months_needed = months;
    }
    *projection = result;
    return true;
}

static bool candidate_for_schedule(const CmnyCashFlowSchedule *schedule,
                                   size_t schedule_index,
                                   const char *range_start, const char *range_end,
                                   bool has_last, const char *last_date,
                                   size_t last_index, CmnyRecurrenceMatch *candidate) {
    const char *search = has_last ? last_date : range_start;
    if (!recurrence_next_internal(&schedule->recurrence, search, candidate)) return false;
    if (candidate->found && has_last && strcmp(candidate->date, last_date) == 0 &&
        schedule_index <= last_index) {
        char next[11];
        if (candidate->ordinal == UINT64_MAX ||
            !occurrence_at(&schedule->recurrence, candidate->ordinal + 1U, next)) {
            *candidate = (CmnyRecurrenceMatch){0};
            return true;
        }
        candidate->ordinal++;
        memcpy(candidate->date, next, sizeof(candidate->date));
    }
    if (candidate->found && !date_not_after(candidate->date, range_end)) {
        *candidate = (CmnyRecurrenceMatch){0};
    }
    return true;
}

static bool next_forecast_event(const CmnyCashFlowSchedule *schedules,
                                size_t schedule_count,
                                const char *range_start, const char *range_end,
                                bool has_last, const char *last_date,
                                size_t last_index, CmnyRecurrenceMatch *match,
                                size_t *schedule_index) {
    CmnyRecurrenceMatch best = {0};
    size_t best_index = 0;
    for (size_t index = 0; index < schedule_count; index++) {
        CmnyRecurrenceMatch candidate = {0};
        if (!candidate_for_schedule(&schedules[index], index, range_start, range_end,
                                    has_last, last_date, last_index, &candidate)) {
            return false;
        }
        if (!candidate.found) continue;
        if (!best.found || strcmp(candidate.date, best.date) < 0 ||
            (strcmp(candidate.date, best.date) == 0 && index < best_index)) {
            best = candidate;
            best_index = index;
        }
    }
    *match = best;
    *schedule_index = best_index;
    return true;
}

static bool forecast_pass(int64_t opening_balance_cents,
                          const CmnyCashFlowSchedule *schedules,
                          size_t schedule_count,
                          const char *range_start, const char *range_end,
                          CmnyForecastOccurrence *rows, size_t cap,
                          CmnyForecastSummary *summary) {
    CmnyForecastSummary result = {
        .ending_balance_cents = opening_balance_cents,
        .minimum_balance_cents = opening_balance_cents,
    };
    if (opening_balance_cents < 0) {
        result.has_below_zero = true;
        memcpy(result.first_below_zero_date, range_start,
               sizeof(result.first_below_zero_date));
    }
    bool has_last = false;
    char last_date[11] = {0};
    size_t last_index = 0;
    for (;;) {
        CmnyRecurrenceMatch match = {0};
        size_t index = 0;
        if (!next_forecast_event(schedules, schedule_count, range_start, range_end,
                                 has_last, last_date, last_index, &match, &index)) {
            return false;
        }
        if (!match.found) break;
        int64_t balance = 0;
        if (!checked_add(result.ending_balance_cents,
                         schedules[index].amount_cents, &balance)) {
            return false;
        }
        if (result.occurrence_count == SIZE_MAX) return false;
        if (rows != NULL) {
            if (result.occurrence_count >= cap) return false;
            rows[result.occurrence_count] = (CmnyForecastOccurrence){
                .amount_cents = schedules[index].amount_cents,
                .balance_after_cents = balance,
                .schedule_index = index,
                .recurrence_ordinal = match.ordinal,
            };
            memcpy(rows[result.occurrence_count].date, match.date,
                   sizeof(rows[result.occurrence_count].date));
        }
        result.occurrence_count++;
        result.ending_balance_cents = balance;
        if (balance < result.minimum_balance_cents) result.minimum_balance_cents = balance;
        if (balance < 0 && !result.has_below_zero) {
            result.has_below_zero = true;
            memcpy(result.first_below_zero_date, match.date,
                   sizeof(result.first_below_zero_date));
        }
        has_last = true;
        memcpy(last_date, match.date, sizeof(last_date));
        last_index = index;
    }
    *summary = result;
    return true;
}

bool cmny_cash_flow_forecast(int64_t opening_balance_cents,
                             const CmnyCashFlowSchedule *schedules,
                             size_t schedule_count,
                             const char *range_start, const char *range_end,
                             CmnyForecastOccurrence *rows, size_t cap,
                             CmnyForecastSummary *summary) {
    if ((schedules == NULL && schedule_count != 0) || range_start == NULL ||
        range_end == NULL || !cmny_expression_date_valid(range_start) ||
        !cmny_expression_date_valid(range_end) ||
        !date_not_after(range_start, range_end) || summary == NULL ||
        (rows == NULL && cap != 0)) {
        return false;
    }
    if (schedule_count == 0) {
        CmnyForecastSummary empty = {
            .ending_balance_cents = opening_balance_cents,
            .minimum_balance_cents = opening_balance_cents,
        };
        if (opening_balance_cents < 0) {
            empty.has_below_zero = true;
            memcpy(empty.first_below_zero_date, range_start,
                   sizeof(empty.first_below_zero_date));
        }
        *summary = empty;
        return true;
    }
    for (size_t index = 0; index < schedule_count; index++) {
        if (schedules[index].amount_cents == 0 ||
            !cmny_recurrence_valid(&schedules[index].recurrence)) {
            return false;
        }
    }
    CmnyForecastSummary result = {0};
    if (!forecast_pass(opening_balance_cents, schedules, schedule_count,
                       range_start, range_end, NULL, 0, &result)) {
        return false;
    }
    if (rows != NULL && result.occurrence_count > cap) return false;
    if (rows != NULL) {
        CmnyForecastSummary filled = {0};
        if (!forecast_pass(opening_balance_cents, schedules, schedule_count,
                           range_start, range_end, rows, cap, &filled)) {
            return false;
        }
        result = filled;
    }
    *summary = result;
    return true;
}

bool cmny_bill_due_status(const char *due_date, const char *as_of,
                          CmnyBillDue *due) {
    if (due_date == NULL || as_of == NULL || due == NULL ||
        !cmny_expression_date_valid(as_of)) {
        return false;
    }
    CmnyBillDue result = {0};
    if (due_date[0] == '\0') {
        result.status = CMNY_BILL_NO_DUE_DATE;
    } else {
        int64_t days = 0;
        if (!cmny_expression_date_days_between(as_of, due_date, &days)) return false;
        result.days_until_due = days;
        result.status = days < 0 ? CMNY_BILL_OVERDUE :
                        days == 0 ? CMNY_BILL_DUE_TODAY : CMNY_BILL_UPCOMING;
    }
    *due = result;
    return true;
}
