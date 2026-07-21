#ifndef CMNY_H
#define CMNY_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef CMNY_VERSION
#define CMNY_VERSION "dev"
#endif
#define CMNY_CATEGORY_MAX 32
#define CMNY_NOTE_MAX 120
#define CMNY_TX_LIMIT 256
#define CMNY_CATEGORY_LIMIT 16
#define CMNY_TREND_MONTHS 6
#define CMNY_BUDGET_LIMIT 16
#define CMNY_RECURRING_LIMIT 9
#define CMNY_ACCOUNT_NAME_MAX 64
#define CMNY_INSTITUTION_MAX 64
#define CMNY_TAG_NAME_MAX 32
#define CMNY_CATEGORY_MARKER_MAX 8
#define CMNY_CATEGORY_COLOR_MAX 16
#define CMNY_PAYEE_MAX 96
#define CMNY_LEDGER_NOTE_MAX 240
#define CMNY_ENTRY_CHILD_LIMIT 256
#define CMNY_AMOUNT_MAX 9000000000000000LL

typedef enum {
    CMNY_EXPENSE = 1,
    CMNY_INCOME = 2
} CmnyKind;

typedef enum {
    CMNY_THEME_OCEAN,
    CMNY_THEME_VIOLET,
    CMNY_THEME_AMBER,
    CMNY_THEME_HIGH_CONTRAST,
    CMNY_THEME_MONOCHROME,
    CMNY_THEME_COUNT
} CmnyTheme;

typedef enum {
    CMNY_ACCOUNT_CASH = 1,
    CMNY_ACCOUNT_CHECKING,
    CMNY_ACCOUNT_SAVINGS,
    CMNY_ACCOUNT_CREDIT,
    CMNY_ACCOUNT_LOAN,
    CMNY_ACCOUNT_INVESTMENT,
    CMNY_ACCOUNT_OTHER
} CmnyAccountType;

typedef enum {
    CMNY_ENTRY_NORMAL = 1,
    CMNY_ENTRY_TRANSFER,
    CMNY_ENTRY_ADJUSTMENT
} CmnyEntryType;

enum {
    CMNY_CATEGORY_EXPENSE = 1,
    CMNY_CATEGORY_INCOME = 2,
    CMNY_CATEGORY_BOTH = 3
};

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
    char category[CMNY_CATEGORY_MAX + 1];
    int64_t limit_cents;
    int64_t spent_cents;
} CmnyBudget;

typedef struct {
    int64_t id;
    CmnyKind kind;
    int64_t amount_cents;
    char category[CMNY_CATEGORY_MAX + 1];
    char note[CMNY_NOTE_MAX + 1];
    int day_of_month;
} CmnyRecurring;

typedef struct {
    size_t transaction_count;
    int64_t income_cents;
    int64_t expense_cents;
} CmnyImportPreview;

typedef struct {
    sqlite3 *handle;
} CmnyDb;

typedef struct {
    int64_t id;
    int64_t balance_minor;
    int64_t revision;
    CmnyAccountType type;
    int sort_order;
    bool archived;
    char name[CMNY_ACCOUNT_NAME_MAX + 1];
    char institution[CMNY_INSTITUTION_MAX + 1];
    char currency[4];
} CmnyAccount;

typedef struct {
    int64_t id;
    int64_t parent_id;
    int64_t merged_into_id;
    int64_t revision;
    int kind_mask;
    bool archived;
    char name[CMNY_CATEGORY_MAX + 1];
    char marker[CMNY_CATEGORY_MARKER_MAX + 1];
    char color[CMNY_CATEGORY_COLOR_MAX + 1];
} CmnyCategory;

typedef struct {
    int64_t id;
    int64_t revision;
    bool archived;
    char name[CMNY_TAG_NAME_MAX + 1];
} CmnyTag;

typedef struct {
    int64_t category_id;
    int64_t amount_minor;
    const char *note;
} CmnySplitDraft;

/* Draft strings and arrays are borrowed for the duration of the call only. */
typedef struct {
    int64_t account_id;
    int64_t amount_minor;
    size_t split_count;
    size_t tag_count;
    const char *occurred_on;
    const char *payee;
    const char *note;
    const CmnySplitDraft *splits;
    const int64_t *tag_ids;
} CmnyNormalEntryDraft;

typedef struct {
    int64_t from_account_id;
    int64_t to_account_id;
    int64_t amount_minor;
    size_t tag_count;
    const char *occurred_on;
    const char *payee;
    const char *note;
    const int64_t *tag_ids;
} CmnyTransferDraft;

typedef struct {
    int64_t id;
    int64_t account_id;
    int64_t amount_minor;
    int clear_state;
    int sort_order;
} CmnyPosting;

typedef struct {
    int64_t id;
    int64_t posting_id;
    int64_t category_id;
    int64_t amount_minor;
    int sort_order;
    char note[CMNY_NOTE_MAX + 1];
} CmnyAllocation;

/* Owned aggregate. Release all child arrays with cmny_ledger_entry_destroy(). */
typedef struct {
    int64_t id;
    int64_t revision;
    CmnyPosting *postings;
    CmnyAllocation *allocations;
    int64_t *tag_ids;
    size_t posting_count;
    size_t allocation_count;
    size_t tag_count;
    CmnyEntryType type;
    char occurred_on[11];
    char payee[CMNY_PAYEE_MAX + 1];
    char note[CMNY_LEDGER_NOTE_MAX + 1];
} CmnyLedgerEntry;

typedef struct {
    bool demo;
    bool ascii;
    bool no_color;
    bool theme_explicit;
    CmnyTheme theme;
    const char *db_override;
    const char *currency;
} CmnyOptions;

bool cmny_money_parse(const char *text, int64_t *cents);
void cmny_money_format(int64_t cents, char *out, size_t out_size);
void cmny_money_format_plain(int64_t cents, char *out, size_t out_size);
bool cmny_currency_supported(const char *input, char output[4]);
bool cmny_theme_parse(const char *input, CmnyTheme *theme);
const char *cmny_theme_name(CmnyTheme theme);
bool cmny_date_valid(const char *date);
bool cmny_month_valid(const char *month);
void cmny_today(char out[11]);
void cmny_current_month(char out[8]);
bool cmny_month_shift(const char *month, int delta, char out[8]);
bool cmny_date_for_month_day(const char *month, int day, char out[11]);
void cmny_month_label(const char *month, char *out, size_t out_size);
bool cmny_text_valid(const char *text, size_t maximum, bool allow_empty);
bool cmny_transaction_valid(const CmnyTransaction *tx, bool require_id);

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
bool cmny_db_setting_get(CmnyDb *db, const char *key, char *out, size_t out_size);
bool cmny_db_setting_set(CmnyDb *db, const char *key, const char *value,
                         char *err, size_t err_size);
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
bool cmny_db_budget_set(CmnyDb *db, const char *month, const char *category,
                        int64_t limit_cents, char *err, size_t err_size);
bool cmny_db_budget_list(CmnyDb *db, const char *month, CmnyBudget *out, size_t cap,
                         size_t *count, char *err, size_t err_size);
bool cmny_db_recurring_add(CmnyDb *db, const CmnyTransaction *tx,
                           char *err, size_t err_size);
bool cmny_db_recurring_list(CmnyDb *db, CmnyRecurring *out, size_t cap,
                            size_t *count, char *err, size_t err_size);
bool cmny_db_recurring_delete(CmnyDb *db, int64_t id, char *err, size_t err_size);
bool cmny_db_backup(CmnyDb *db, const char *path, char *err, size_t err_size);
bool cmny_db_restore(CmnyDb *db, const char *path, char currency_out[4],
                     char *err, size_t err_size);
bool cmny_db_check(CmnyDb *db, char *err, size_t err_size);
bool cmny_db_seed_demo(CmnyDb *db, char *err, size_t err_size);

bool cmny_account_create(CmnyDb *db, const char *name, CmnyAccountType type,
                         const char *institution, int64_t *new_id,
                         char *err, size_t err_size);
bool cmny_account_create_with_opening(CmnyDb *db, const char *name, CmnyAccountType type,
                                      const char *institution, int64_t opening_balance_minor,
                                      const char *balance_on, int64_t *new_id,
                                      char *err, size_t err_size);
bool cmny_account_update(CmnyDb *db, int64_t id, int64_t expected_revision,
                         const char *name, CmnyAccountType type, const char *institution,
                         char *err, size_t err_size);
bool cmny_account_set_archived(CmnyDb *db, int64_t id, int64_t expected_revision,
                               bool archived, char *err, size_t err_size);
bool cmny_account_list(CmnyDb *db, bool include_archived, CmnyAccount *out, size_t cap,
                       size_t *count, char *err, size_t err_size);
bool cmny_account_balance(CmnyDb *db, int64_t account_id, int64_t *balance_minor,
                          char *err, size_t err_size);

bool cmny_category_create(CmnyDb *db, const char *name, int kind_mask, int64_t parent_id,
                          int64_t *new_id, char *err, size_t err_size);
bool cmny_category_create_styled(CmnyDb *db, const char *name, int kind_mask, int64_t parent_id,
                                 const char *marker, const char *color, int64_t *new_id,
                                 char *err, size_t err_size);
bool cmny_category_update(CmnyDb *db, int64_t id, int64_t expected_revision,
                          const char *name, int kind_mask, int64_t parent_id,
                          char *err, size_t err_size);
bool cmny_category_update_styled(CmnyDb *db, int64_t id, int64_t expected_revision,
                                 const char *name, int kind_mask, int64_t parent_id,
                                 const char *marker, const char *color,
                                 char *err, size_t err_size);
bool cmny_category_set_archived(CmnyDb *db, int64_t id, int64_t expected_revision,
                                bool archived, char *err, size_t err_size);
bool cmny_category_merge(CmnyDb *db, int64_t source_id, int64_t target_id,
                         char *err, size_t err_size);
bool cmny_category_find(CmnyDb *db, const char *name, CmnyCategory *out,
                        char *err, size_t err_size);
bool cmny_category_list(CmnyDb *db, bool include_archived, CmnyCategory *out, size_t cap,
                        size_t *count, char *err, size_t err_size);

bool cmny_tag_create(CmnyDb *db, const char *name, int64_t *new_id,
                     char *err, size_t err_size);
bool cmny_tag_update(CmnyDb *db, int64_t id, int64_t expected_revision,
                     const char *name, char *err, size_t err_size);
bool cmny_tag_set_archived(CmnyDb *db, int64_t id, int64_t expected_revision,
                           bool archived, char *err, size_t err_size);
bool cmny_tag_list(CmnyDb *db, bool include_archived, CmnyTag *out, size_t cap,
                   size_t *count, char *err, size_t err_size);

bool cmny_entry_create_normal(CmnyDb *db, const CmnyNormalEntryDraft *draft,
                              int64_t *new_id, char *err, size_t err_size);
bool cmny_entry_update_normal(CmnyDb *db, int64_t id, int64_t expected_revision,
                              const CmnyNormalEntryDraft *draft,
                              char *err, size_t err_size);
bool cmny_entry_delete(CmnyDb *db, int64_t id, int64_t expected_revision,
                       char *err, size_t err_size);
bool cmny_transfer_create(CmnyDb *db, const CmnyTransferDraft *draft,
                          int64_t *new_id, char *err, size_t err_size);
bool cmny_ledger_entry_get(CmnyDb *db, int64_t id, CmnyLedgerEntry *out,
                           char *err, size_t err_size);
void cmny_ledger_entry_destroy(CmnyLedgerEntry *entry);

bool cmny_csv_export(CmnyDb *db, const char *path, size_t *count,
                     char *err, size_t err_size);
bool cmny_csv_import(CmnyDb *db, const char *path, bool apply,
                     CmnyImportPreview *preview, char *err, size_t err_size);

int cmny_ui_run(CmnyDb *db, const CmnyOptions *options, const char *db_path);

#endif
