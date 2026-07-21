#include "cmny_expr.h"
#include "cmny_plan.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static uint8_t byte_at(const uint8_t *data, size_t size, size_t index) {
    return size == 0 ? 0 : data[index % size];
}

static CmnyRecurrence make_recurrence(const uint8_t *data, size_t size,
                                      size_t offset) {
    CmnyRecurrence recurrence = {
        .frequency = (CmnyRecurrenceFrequency)(byte_at(data, size, offset) % 5U),
        .interval = (uint32_t)(byte_at(data, size, offset + 1U) % 12U) + 1U,
    };
    if (recurrence.frequency == CMNY_RECURRENCE_ONE_TIME) recurrence.interval = 1;
    static const char *starts[] = {
        "1900-01-01", "2024-01-31", "2024-02-29", "2026-07-21", "9999-12-31"
    };
    const char *start = starts[byte_at(data, size, offset + 2U) % 5U];
    memcpy(recurrence.start, start, sizeof(recurrence.start));
    if ((byte_at(data, size, offset + 3U) & 1U) != 0) {
        char end[11];
        int64_t days = byte_at(data, size, offset + 4U);
        if (cmny_expression_date_shift(start, days, 'd', end)) {
            memcpy(recurrence.end, end, sizeof(recurrence.end));
        }
    }
    return recurrence;
}

static char recurrence_unit(CmnyRecurrenceFrequency frequency) {
    return frequency == CMNY_RECURRENCE_DAILY ? 'd' :
           frequency == CMNY_RECURRENCE_WEEKLY ? 'w' :
           frequency == CMNY_RECURRENCE_MONTHLY ? 'm' : 'y';
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    CmnyRecurrence recurrence = make_recurrence(data, size, 0);
    CmnyRecurrenceMatch matches[64];
    memset(matches, 0xA5, sizeof(matches));
    CmnyRecurrenceMatch before[64];
    memcpy(before, matches, sizeof(matches));
    size_t count = 123;
    bool expanded = cmny_recurrence_expand(&recurrence, "2024-01-01", "2026-12-31",
                                           matches, 64, &count);
    if (!expanded) {
        if (count != 123 || memcmp(matches, before, sizeof(matches)) != 0) abort();
    } else {
        for (size_t index = 0; index < count; index++) {
            if (!matches[index].found ||
                !cmny_expression_date_valid(matches[index].date) ||
                strcmp(matches[index].date, "2024-01-01") < 0 ||
                strcmp(matches[index].date, "2026-12-31") > 0 ||
                (index > 0 && strcmp(matches[index - 1U].date,
                                     matches[index].date) >= 0)) {
                abort();
            }
            char direct[11];
            if (recurrence.frequency == CMNY_RECURRENCE_ONE_TIME) {
                if (matches[index].ordinal != 0 ||
                    strcmp(matches[index].date, recurrence.start) != 0) abort();
            } else {
                if (matches[index].ordinal > (uint64_t)INT64_MAX / recurrence.interval) abort();
                int64_t shift = (int64_t)(matches[index].ordinal * recurrence.interval);
                if (!cmny_expression_date_shift(recurrence.start, shift,
                                                recurrence_unit(recurrence.frequency),
                                                direct) ||
                    strcmp(direct, matches[index].date) != 0) {
                    abort();
                }
            }
        }
    }

    CmnyCashFlowSchedule schedules[3];
    for (size_t index = 0; index < 3; index++) {
        schedules[index].recurrence = make_recurrence(data, size, index * 5U);
        int64_t magnitude = (int64_t)byte_at(data, size, index + 7U) + 1;
        schedules[index].amount_cents = (byte_at(data, size, index + 10U) & 1U) != 0
                                            ? -magnitude : magnitude;
    }
    int64_t opening = (int64_t)(int8_t)byte_at(data, size, 20);
    CmnyForecastSummary summary = {
        .ending_balance_cents = 71,
        .minimum_balance_cents = 72,
        .occurrence_count = 73,
    };
    CmnyForecastSummary summary_before = summary;
    bool summarized = cmny_cash_flow_forecast(opening, schedules, 3,
                                               "2024-01-01", "2024-12-31",
                                               NULL, 0, &summary);
    if (!summarized) {
        if (memcmp(&summary, &summary_before, sizeof(summary)) != 0) abort();
    } else if (summary.occurrence_count <= 128) {
        CmnyForecastOccurrence rows[128];
        memset(rows, 0, sizeof(rows));
        CmnyForecastSummary with_rows = {0};
        if (!cmny_cash_flow_forecast(opening, schedules, 3,
                                     "2024-01-01", "2024-12-31",
                                     rows, 128, &with_rows) ||
            memcmp(&summary, &with_rows, sizeof(summary)) != 0) {
            abort();
        }
        int64_t balance = opening;
        for (size_t index = 0; index < with_rows.occurrence_count; index++) {
            if (rows[index].schedule_index >= 3 ||
                rows[index].amount_cents !=
                    schedules[rows[index].schedule_index].amount_cents ||
                (index > 0 && (strcmp(rows[index - 1U].date, rows[index].date) > 0 ||
                 (strcmp(rows[index - 1U].date, rows[index].date) == 0 &&
                  rows[index - 1U].schedule_index >= rows[index].schedule_index)))) {
                abort();
            }
            balance += rows[index].amount_cents;
            if (balance != rows[index].balance_after_cents) abort();
        }
        if (balance != with_rows.ending_balance_cents) abort();
    }

    CmnyBudgetRollover rollover = {.carry_cents = 81, .available_cents = 82};
    CmnyBudgetRollover rollover_before = rollover;
    int64_t budget = byte_at(data, size, 30);
    int64_t spent = byte_at(data, size, 31);
    int64_t next = byte_at(data, size, 32);
    CmnyBudgetRolloverMode mode =
        (CmnyBudgetRolloverMode)(byte_at(data, size, 33) % 3U);
    if (!cmny_budget_rollover(mode, budget, spent, next, &rollover) &&
        memcmp(&rollover, &rollover_before, sizeof(rollover)) != 0) {
        abort();
    }
    return 0;
}
