#ifndef CMNY_H
#define CMNY_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CMNY_VERSION "0.1.0"
#define CMNY_CATEGORY_MAX 32
#define CMNY_NOTE_MAX 120
#define CMNY_TX_LIMIT 256
#define CMNY_CATEGORY_LIMIT 16
#define CMNY_TREND_MONTHS 6

typedef enum {
    CMNY_EXPENSE = 1,
    CMNY_INCOME = 2
} CmnyKind;

typedef enum {
    CMNY_THEME_OCEAN,
    CMNY_THEME_VIOLET,
    CMNY_THEME_AMBER,
    CMNY_THEME_COUNT
} CmnyTheme;

typedef struct {
    int64_t id;
    CmnyKind kind;
    int64_t amount_cents;
    char category[CMNY_CATEGORY_MAX + 1];
    char note[CMNY_NOTE_MAX + 1];
    char occurred_on[11];
} CmnyTransaction;

typedef struct {
    int64_t income_cents;
    int64_t expense_cents;
    int transaction_count;
} CmnyMonthSummary;

typedef struct {
    char category[CMNY_CATEGORY_MAX + 1];
    int64_t amount_cents;
} CmnyCategoryTotal;

typedef struct {
    char month[8];
    int64_t income_cents;
    int64_t expense_cents;
} CmnyMonthTrend;

typedef struct {
    sqlite3 *handle;
} CmnyDb;

typedef struct {
    bool demo;
    bool ascii;
    bool no_color;
    CmnyTheme theme;
    const char *db_override;
    const char *currency;
} CmnyOptions;

bool cmny_money_parse(const char *text, int64_t *cents);
void cmny_money_format(int64_t cents, char *out, size_t out_size);
bool cmny_currency_supported(const char *input, char output[4]);
bool cmny_theme_parse(const char *input, CmnyTheme *theme);
const char *cmny_theme_name(CmnyTheme theme);
bool cmny_date_valid(const char *date);
bool cmny_month_valid(const char *month);
void cmny_today(char out[11]);
void cmny_current_month(char out[8]);
bool cmny_month_shift(const char *month, int delta, char out[8]);
void cmny_month_label(const char *month, char *out, size_t out_size);

bool cmny_resolve_db_path(const char *override_path, char *out, size_t out_size,
                          char *err, size_t err_size);
bool cmny_prepare_db_parent(const char *path, char *err, size_t err_size);

bool cmny_db_open(CmnyDb *db, const char *path, bool demo, const char *requested_currency,
                  char currency_out[4], char *err, size_t err_size);
void cmny_db_close(CmnyDb *db);
bool cmny_db_add(CmnyDb *db, const CmnyTransaction *tx, int64_t *new_id,
                 char *err, size_t err_size);
bool cmny_db_update(CmnyDb *db, const CmnyTransaction *tx, char *err, size_t err_size);
bool cmny_db_delete(CmnyDb *db, int64_t id, char *err, size_t err_size);
bool cmny_db_list(CmnyDb *db, const char *month, const char *search, int kind_filter,
                  size_t offset, CmnyTransaction *out, size_t cap, size_t *count,
                  char *err, size_t err_size);
bool cmny_db_count(CmnyDb *db, const char *month, const char *search, int kind_filter,
                   size_t *count, char *err, size_t err_size);
bool cmny_db_month_summary(CmnyDb *db, const char *month, CmnyMonthSummary *summary,
                           char *err, size_t err_size);
bool cmny_db_category_totals(CmnyDb *db, const char *month, CmnyCategoryTotal *out,
                             size_t cap, size_t *count, char *err, size_t err_size);
bool cmny_db_trend(CmnyDb *db, const char *end_month, CmnyMonthTrend *out,
                   size_t count, char *err, size_t err_size);
bool cmny_db_seed_demo(CmnyDb *db, char *err, size_t err_size);

int cmny_ui_run(CmnyDb *db, const CmnyOptions *options, const char *db_path);

#endif
