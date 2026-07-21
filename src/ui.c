#include "cmny.h"
#include "cmny_expr.h"
#include "cmny_history.h"
#include "cmny_reconcile.h"

#include <ctype.h>
#include <curses.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    COLOR_BRAND = 1,
    COLOR_INCOME,
    COLOR_EXPENSE,
    COLOR_MUTED,
    COLOR_SELECTED,
    COLOR_WARNING
};

typedef enum {
    SCREEN_OVERVIEW,
    SCREEN_ACTIVITY,
    SCREEN_PLAN,
    SCREEN_INSIGHTS,
    SCREEN_MANAGE,
    SCREEN_COUNT
} Screen;

typedef enum {
    ACTION_ADD,
    ACTION_EDIT,
    ACTION_DELETE,
    ACTION_UNDO,
    ACTION_SEARCH,
    ACTION_FILTER,
    ACTION_CLEAR,
    ACTION_BUDGET,
    ACTION_RECURRING,
    ACTION_TODAY,
    ACTION_SETTINGS,
    ACTION_TRANSFER,
    ACTION_ACCOUNT_FILTER,
    ACTION_ACCOUNTS,
    ACTION_RECONCILE,
    ACTION_COUNT
} UiAction;

typedef struct {
    const char *label;
    const char *setting;
    char default_key;
} BindingDefinition;

static const BindingDefinition binding_definitions[ACTION_COUNT] = {
    [ACTION_ADD] = {"Add entry", "key_add", 'a'},
    [ACTION_EDIT] = {"Edit entry", "key_edit", 'e'},
    [ACTION_DELETE] = {"Delete entry", "key_delete", 'd'},
    [ACTION_UNDO] = {"Undo latest change", "key_undo", 'u'},
    [ACTION_SEARCH] = {"Search", "key_search", '/'},
    [ACTION_FILTER] = {"Change filter", "key_filter", 'f'},
    [ACTION_CLEAR] = {"Clear search/filter", "key_clear", 'c'},
    [ACTION_BUDGET] = {"Set budget", "key_budget", 'b'},
    [ACTION_RECURRING] = {"Recurring entries", "key_recurring", 'r'},
    [ACTION_TODAY] = {"Current month", "key_today", 't'},
    [ACTION_SETTINGS] = {"Open Manage", "key_settings", 's'},
    [ACTION_TRANSFER] = {"New transfer", "key_transfer", 'v'},
    [ACTION_ACCOUNT_FILTER] = {"Filter by account", "key_account_filter", 'g'},
    [ACTION_ACCOUNTS] = {"Manage accounts", "key_accounts", 'm'},
    [ACTION_RECONCILE] = {"Reconcile account", "key_reconcile", 'l'}
};

enum {
    SETTINGS_THEME,
    SETTINGS_START_SCREEN,
    SETTINGS_MOUSE,
    SETTINGS_BINDING_FIRST,
    SETTINGS_ACCOUNTS = SETTINGS_BINDING_FIRST + ACTION_COUNT,
    SETTINGS_RECONCILE,
    SETTINGS_INTEGRITY,
    SETTINGS_TUTORIAL,
    SETTINGS_BACKUP,
    SETTINGS_RESET_KEYS,
    SETTINGS_ITEM_COUNT
};

enum { NAV_HISTORY_LIMIT = 16 };

typedef enum {
    UI_MOUSE_AUTO,
    UI_MOUSE_ON,
    UI_MOUSE_OFF,
    UI_MOUSE_MODE_COUNT
} UiMouseMode;

typedef enum {
    COMMAND_OVERVIEW,
    COMMAND_ACTIVITY,
    COMMAND_PLAN,
    COMMAND_INSIGHTS,
    COMMAND_MANAGE,
    COMMAND_BACK,
    COMMAND_ADD,
    COMMAND_VIEW,
    COMMAND_EDIT,
    COMMAND_DELETE,
    COMMAND_UNDO,
    COMMAND_SEARCH,
    COMMAND_FILTER,
    COMMAND_CLEAR,
    COMMAND_BUDGET,
    COMMAND_RECURRING,
    COMMAND_TRANSFER,
    COMMAND_ACCOUNT_FILTER,
    COMMAND_ACCOUNTS,
    COMMAND_RECONCILE,
    COMMAND_TODAY,
    COMMAND_PREVIOUS_MONTH,
    COMMAND_NEXT_MONTH,
    COMMAND_INTEGRITY,
    COMMAND_BACKUP,
    COMMAND_THEME_SETTINGS,
    COMMAND_START_SETTINGS,
    COMMAND_RESET_KEYS,
    COMMAND_TUTORIAL,
    COMMAND_HELP,
    COMMAND_QUIT,
    COMMAND_COUNT
} PaletteCommand;

typedef struct {
    short brand;
    short income;
    short expense;
    short muted;
    short selected_foreground;
    short selected_background;
    short warning;
} ThemePalette;

enum { UI_ACCOUNT_LIMIT = 64 };

typedef struct {
    int64_t revision;
    int64_t account_id;
    CmnyEntryType entry_type;
    CmnyPostingClearState clear_state;
    bool transfer_outgoing;
    char account[CMNY_ACCOUNT_NAME_MAX + 1];
    char counterparty[CMNY_ACCOUNT_NAME_MAX + 1];
    char payee[CMNY_PAYEE_MAX + 1];
} UiEntryMeta;

static const ThemePalette theme_palettes[CMNY_THEME_COUNT] = {
    [CMNY_THEME_OCEAN] = {COLOR_CYAN, COLOR_GREEN, COLOR_RED, COLOR_WHITE,
                          COLOR_BLACK, COLOR_CYAN, COLOR_YELLOW},
    [CMNY_THEME_VIOLET] = {COLOR_MAGENTA, COLOR_CYAN, COLOR_RED, COLOR_WHITE,
                           COLOR_WHITE, COLOR_MAGENTA, COLOR_YELLOW},
    [CMNY_THEME_AMBER] = {COLOR_YELLOW, COLOR_GREEN, COLOR_RED, COLOR_WHITE,
                          COLOR_BLACK, COLOR_YELLOW, COLOR_MAGENTA},
    [CMNY_THEME_HIGH_CONTRAST] = {COLOR_WHITE, COLOR_GREEN, COLOR_RED, COLOR_CYAN,
                                  COLOR_BLACK, COLOR_WHITE, COLOR_YELLOW},
    [CMNY_THEME_MONOCHROME] = {COLOR_WHITE, COLOR_WHITE, COLOR_WHITE, COLOR_WHITE,
                               COLOR_BLACK, COLOR_WHITE, COLOR_WHITE}
};

typedef struct {
    CmnyDb *db;
    const CmnyOptions *options;
    const char *db_path;
    Screen screen;
    Screen start_screen;
    Screen navigation_history[NAV_HISTORY_LIMIT];
    size_t navigation_depth;
    CmnyTheme theme;
    UiMouseMode mouse_mode;
    bool mouse_supported;
    bool mouse_active;
    bool tutorial_pending;
    char bindings[ACTION_COUNT];
    size_t settings_selected;
    char month[8];
    char search[CMNY_NOTE_MAX + 1];
    int kind_filter;
    size_t selected;
    CmnyMonthSummary summary;
    CmnyMonthSummary previous;
    CmnyCategoryTotal categories[CMNY_CATEGORY_LIMIT];
    size_t category_count;
    CmnyMonthTrend trend[CMNY_TREND_MONTHS];
    CmnyBudget budgets[CMNY_BUDGET_LIMIT];
    size_t budget_count;
    CmnyRecurring recurring[CMNY_RECURRING_LIMIT];
    size_t recurring_count;
    CmnyTransaction transactions[CMNY_TX_LIMIT];
    UiEntryMeta transaction_meta[CMNY_TX_LIMIT];
    size_t transaction_count;
    size_t activity_offset;
    size_t activity_total;
    CmnyTransaction recent[6];
    size_t recent_count;
    CmnyAccount accounts[UI_ACCOUNT_LIMIT];
    size_t account_count;
    int64_t account_filter_id;
    bool can_undo;
    char status[256];
    bool running;
} UiState;

static volatile sig_atomic_t interrupted = 0;
static bool colors_active = false;
static short theme_background = COLOR_BLACK;

static void handle_signal(int signal_number) {
    (void)signal_number;
    interrupted = 1;
}

static int min_int(int a, int b) {
    return a < b ? a : b;
}

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static bool has_visible_text(const char *text) {
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor != '\0'; cursor++) {
        if (!isspace(*cursor)) return true;
    }
    return false;
}

static const char *screen_name(Screen screen) {
    static const char *names[] = {"overview", "activity", "plan", "insights", "manage"};
    return screen >= SCREEN_OVERVIEW && screen < SCREEN_COUNT ? names[screen] : "overview";
}

static const char *screen_label(Screen screen) {
    static const char *labels[] = {"Overview", "Activity", "Plan", "Insights", "Manage"};
    return screen >= SCREEN_OVERVIEW && screen < SCREEN_COUNT ? labels[screen] : "Overview";
}

static bool screen_parse(const char *value, Screen *screen) {
    if (value == NULL || screen == NULL) return false;
    if (strcmp(value, "reports") == 0) {
        *screen = SCREEN_INSIGHTS;
        return true;
    }
    if (strcmp(value, "settings") == 0) {
        *screen = SCREEN_MANAGE;
        return true;
    }
    for (int i = SCREEN_OVERVIEW; i < SCREEN_COUNT; i++) {
        if (strcmp(value, screen_name((Screen)i)) == 0) {
            *screen = (Screen)i;
            return true;
        }
    }
    return false;
}

static const char *mouse_mode_name(UiMouseMode mode) {
    static const char *names[] = {"auto", "on", "off"};
    return mode >= UI_MOUSE_AUTO && mode < UI_MOUSE_MODE_COUNT ? names[mode] : "auto";
}

static bool mouse_mode_parse(const char *value, UiMouseMode *mode) {
    if (value == NULL || mode == NULL) return false;
    for (int i = UI_MOUSE_AUTO; i < UI_MOUSE_MODE_COUNT; i++) {
        if (strcmp(value, mouse_mode_name((UiMouseMode)i)) == 0) {
            *mode = (UiMouseMode)i;
            return true;
        }
    }
    return false;
}

static int normalize_binding_key(int ch) {
    if (ch >= 'A' && ch <= 'Z') ch = tolower(ch);
    if ((ch >= 'a' && ch <= 'z' && ch != 'q') || ch == '/') return ch;
    return 0;
}

static bool binding_conflicts(const UiState *state, UiAction action, int ch,
                              UiAction *conflict) {
    for (int i = 0; i < ACTION_COUNT; i++) {
        if (i != (int)action && state->bindings[i] == ch) {
            if (conflict != NULL) *conflict = (UiAction)i;
            return true;
        }
    }
    return false;
}

static bool action_pressed(const UiState *state, UiAction action, int ch) {
    int normalized = normalize_binding_key(ch);
    return normalized != 0 && state->bindings[action] == normalized;
}

static void load_bindings(UiState *state) {
    char requested[ACTION_COUNT];
    for (int i = 0; i < ACTION_COUNT; i++) {
        requested[i] = binding_definitions[i].default_key;
    }
    for (int i = 0; i < ACTION_COUNT; i++) {
        char value[8] = {0};
        if (!cmny_db_setting_get(state->db, binding_definitions[i].setting,
                                 value, sizeof(value)) || value[0] == '\0' || value[1] != '\0') {
            continue;
        }
        int key = normalize_binding_key((unsigned char)value[0]);
        if (key != 0) requested[i] = (char)key;
    }
    bool valid = true;
    for (int i = 0; i < ACTION_COUNT; i++) {
        for (int j = i + 1; j < ACTION_COUNT; j++) {
            if (requested[i] == requested[j]) valid = false;
        }
    }
    for (int i = 0; i < ACTION_COUNT; i++) {
        state->bindings[i] = valid ? requested[i] : binding_definitions[i].default_key;
    }
}

static void statusf(UiState *state, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static bool zero_amount(const char *text);
static void reconciliation_center(UiState *state);

static void statusf(UiState *state, const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(state->status, sizeof(state->status), format, args);
    va_end(args);
}

static bool configure_mouse(UiState *state) {
    state->mouse_supported = false;
    state->mouse_active = false;
#if defined(KEY_MOUSE) && defined(BUTTON1_CLICKED)
    mmask_t old_mask = 0;
    if (state->mouse_mode == UI_MOUSE_OFF) {
        (void)mousemask(0, &old_mask);
        return true;
    }
    mmask_t requested = BUTTON1_CLICKED;
#ifdef BUTTON1_DOUBLE_CLICKED
    requested |= BUTTON1_DOUBLE_CLICKED;
#endif
#ifdef BUTTON4_PRESSED
    requested |= BUTTON4_PRESSED;
#endif
#ifdef BUTTON5_PRESSED
    requested |= BUTTON5_PRESSED;
#endif
    mmask_t available = mousemask(requested, &old_mask);
    state->mouse_supported = (available & BUTTON1_CLICKED) != 0;
    state->mouse_active = state->mouse_supported;
    if (!state->mouse_active) {
        (void)mousemask(0, &old_mask);
        return false;
    }
#ifdef NCURSES_MOUSE_VERSION
    (void)mouseinterval(180);
#endif
    return true;
#else
    (void)state;
    return false;
#endif
}

static attr_t attr_color(int pair) {
    return colors_active ? (attr_t)COLOR_PAIR(pair) : (attr_t)0;
}

static attr_t selection_attr(void) {
    return colors_active ? attr_color(COLOR_SELECTED) : (attr_t)A_REVERSE;
}

static bool apply_theme(CmnyTheme theme, short background) {
    if (theme < CMNY_THEME_OCEAN || theme >= CMNY_THEME_COUNT) return false;
    const ThemePalette *palette = &theme_palettes[theme];
    return init_pair(COLOR_BRAND, palette->brand, background) == OK &&
           init_pair(COLOR_INCOME, palette->income, background) == OK &&
           init_pair(COLOR_EXPENSE, palette->expense, background) == OK &&
           init_pair(COLOR_MUTED, palette->muted, background) == OK &&
           init_pair(COLOR_SELECTED, palette->selected_foreground,
                     palette->selected_background) == OK &&
           init_pair(COLOR_WARNING, palette->warning, background) == OK;
}

static void put_clipped(int y, int x, int width, const char *text) {
    if (width <= 0 || text == NULL || y < 0 || x < 0) {
        return;
    }
    (void)mvaddnstr(y, x, text, width);
}

static void draw_box(int y, int x, int height, int width, const char *title) {
    if (height < 3 || width < 4) {
        return;
    }
    (void)mvaddch(y, x, '+');
    (void)mvhline(y, x + 1, '-', width - 2);
    (void)mvaddch(y, x + width - 1, '+');
    for (int row = 1; row < height - 1; row++) {
        (void)mvaddch(y + row, x, '|');
        (void)mvaddch(y + row, x + width - 1, '|');
    }
    (void)mvaddch(y + height - 1, x, '+');
    (void)mvhline(y + height - 1, x + 1, '-', width - 2);
    (void)mvaddch(y + height - 1, x + width - 1, '+');
    if (title != NULL && *title != '\0' && width > 8) {
        (void)attron(A_BOLD | attr_color(COLOR_BRAND));
        put_clipped(y, x + 2, width - 4, title);
        (void)attroff(A_BOLD | attr_color(COLOR_BRAND));
    }
}

static void format_value(const UiState *state, int64_t cents, char *out, size_t out_size) {
    char amount[64];
    cmny_money_format(cents, amount, sizeof(amount));
    (void)snprintf(out, out_size, "%s %s", amount, state->options->currency);
}

static bool text_contains_folded(const char *text, const char *needle) {
    if (needle == NULL || *needle == '\0') return true;
    if (text == NULL) return false;
    for (const char *start = text; *start != '\0'; start++) {
        const char *left = start;
        const char *right = needle;
        while (*left != '\0' && *right != '\0' &&
               tolower((unsigned char)*left) == tolower((unsigned char)*right)) {
            left++;
            right++;
        }
        if (*right == '\0') return true;
    }
    return false;
}

static const CmnyAccount *account_by_id(const UiState *state, int64_t id) {
    for (size_t i = 0; i < state->account_count; i++) {
        if (state->accounts[i].id == id) return &state->accounts[i];
    }
    return NULL;
}

static bool load_activity(UiState *state, char *err, size_t err_size) {
    static const char *sql =
        "SELECT e.id,e.revision,e.entry_type,e.occurred_on,e.payee,e.note,"
        "p.account_id,p.amount_minor,p.clear_state,a.name,"
        "CASE WHEN e.entry_type=1 THEN CASE WHEN "
        " (SELECT COUNT(*) FROM allocations x WHERE x.posting_id=p.id)=1 THEN "
        " (SELECT c.name FROM allocations x JOIN categories c ON c.id=x.category_id "
        "  WHERE x.posting_id=p.id LIMIT 1) ELSE 'Split' END "
        " WHEN e.entry_type=2 THEN 'Transfer' ELSE 'Balance adjustment' END,"
        "COALESCE((SELECT a2.name FROM postings p2 JOIN accounts a2 ON a2.id=p2.account_id "
        " WHERE p2.entry_id=e.id AND p2.id<>p.id ORDER BY p2.sort_order LIMIT 1),'') "
        "FROM entries e JOIN postings p ON p.entry_id=e.id "
        "JOIN accounts a ON a.id=p.account_id "
        "WHERE e.voided_at IS NULL AND "
        "((e.entry_type=2 AND ((?1=0 AND p.sort_order=0) OR (?1<>0 AND p.account_id=?1))) "
        " OR (e.entry_type<>2 AND p.sort_order=0)) "
        "ORDER BY e.occurred_on DESC,e.id DESC";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(state->db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        (void)snprintf(err, err_size, "%s", sqlite3_errmsg(state->db->handle));
        return false;
    }
    (void)sqlite3_bind_int64(stmt, 1, state->account_filter_id);
    state->transaction_count = 0;
    state->activity_total = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int64_t account_id = sqlite3_column_int64(stmt, 6);
        int64_t signed_amount = sqlite3_column_int64(stmt, 7);
        CmnyEntryType type = (CmnyEntryType)sqlite3_column_int(stmt, 2);
        const char *date = (const char *)sqlite3_column_text(stmt, 3);
        const char *payee = (const char *)sqlite3_column_text(stmt, 4);
        const char *note = (const char *)sqlite3_column_text(stmt, 5);
        const char *account = (const char *)sqlite3_column_text(stmt, 9);
        const char *category = (const char *)sqlite3_column_text(stmt, 10);
        const char *counterparty = (const char *)sqlite3_column_text(stmt, 11);
        if (state->account_filter_id != 0 && account_id != state->account_filter_id) continue;
        if (state->search[0] == '\0' && strncmp(date, state->month, 7) != 0) continue;
        if (state->kind_filter != 0 &&
            (type == CMNY_ENTRY_TRANSFER ||
             (state->kind_filter == CMNY_EXPENSE && signed_amount >= 0) ||
             (state->kind_filter == CMNY_INCOME && signed_amount <= 0))) continue;
        if (state->search[0] != '\0' &&
            !text_contains_folded(date, state->search) &&
            !text_contains_folded(payee, state->search) &&
            !text_contains_folded(note, state->search) &&
            !text_contains_folded(account, state->search) &&
            !text_contains_folded(category, state->search) &&
            !text_contains_folded(counterparty, state->search)) continue;
        size_t absolute_index = state->activity_total++;
        if (absolute_index < state->activity_offset ||
            state->transaction_count >= CMNY_TX_LIMIT) continue;
        size_t index = state->transaction_count++;
        CmnyTransaction *tx = &state->transactions[index];
        UiEntryMeta *meta = &state->transaction_meta[index];
        memset(tx, 0, sizeof(*tx));
        memset(meta, 0, sizeof(*meta));
        tx->id = sqlite3_column_int64(stmt, 0);
        tx->kind = signed_amount < 0 ? CMNY_EXPENSE : CMNY_INCOME;
        tx->amount_cents = signed_amount < 0 ? -signed_amount : signed_amount;
        (void)snprintf(tx->occurred_on, sizeof(tx->occurred_on), "%s", date);
        (void)snprintf(tx->category, sizeof(tx->category), "%s", category);
        (void)snprintf(tx->note, sizeof(tx->note), "%s", note);
        meta->revision = sqlite3_column_int64(stmt, 1);
        meta->entry_type = type;
        meta->account_id = account_id;
        meta->clear_state = (CmnyPostingClearState)sqlite3_column_int(stmt, 8);
        meta->transfer_outgoing = type == CMNY_ENTRY_TRANSFER && signed_amount < 0;
        (void)snprintf(meta->account, sizeof(meta->account), "%s", account);
        (void)snprintf(meta->payee, sizeof(meta->payee), "%s", payee);
        (void)snprintf(meta->counterparty, sizeof(meta->counterparty), "%s", counterparty);
    }
    (void)sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        (void)snprintf(err, err_size, "%s", sqlite3_errmsg(state->db->handle));
        return false;
    }
    return true;
}

static void refresh_state(UiState *state) {
    char err[256] = {0};
    char previous_month[8];
    state->status[0] = '\0';
    if (!cmny_month_shift(state->month, -1, previous_month) ||
        !cmny_db_month_summary(state->db, state->month, &state->summary, err, sizeof(err)) ||
        !cmny_db_month_summary(state->db, previous_month, &state->previous, err, sizeof(err)) ||
        !cmny_db_category_totals(state->db, state->month, state->categories,
                                 CMNY_CATEGORY_LIMIT, &state->category_count, err, sizeof(err)) ||
        !cmny_db_budget_list(state->db, state->month, state->budgets,
                             CMNY_BUDGET_LIMIT, &state->budget_count, err, sizeof(err)) ||
        !cmny_db_recurring_list(state->db, state->recurring, CMNY_RECURRING_LIMIT,
                                &state->recurring_count, err, sizeof(err)) ||
        !cmny_db_trend(state->db, state->month, state->trend, CMNY_TREND_MONTHS,
                       err, sizeof(err)) ||
        !cmny_account_list(state->db, true, state->accounts, UI_ACCOUNT_LIMIT,
                           &state->account_count, err, sizeof(err))) {
        statusf(state, "Database error: %s", err);
        return;
    }
    if (!load_activity(state, err, sizeof(err))) {
        statusf(state, "Database error: %s", err);
        return;
    }
    if (state->activity_total == 0) {
        state->activity_offset = 0;
    } else if (state->activity_offset >= state->activity_total) {
        state->activity_offset = ((state->activity_total - 1) / CMNY_TX_LIMIT) * CMNY_TX_LIMIT;
        if (!load_activity(state, err, sizeof(err))) {
            statusf(state, "Database error: %s", err);
            return;
        }
    }
    if (!cmny_db_list(state->db, state->month, "", 0, 0, state->recent, 6,
                      &state->recent_count, err, sizeof(err))) {
        statusf(state, "Database error: %s", err);
        return;
    }
    if (state->transaction_count == 0) {
        state->selected = 0;
    } else if (state->selected >= state->transaction_count) {
        state->selected = state->transaction_count - 1;
    }
    CmnyHistoryAction latest;
    size_t history_count = 0;
    state->can_undo = cmny_history_list(state->db, false, 0, &latest, 1,
                                        &history_count, err, sizeof(err)) && history_count > 0;
}

static void draw_header(const UiState *state, int columns) {
    char month_label[32];
    cmny_month_label(state->month, month_label, sizeof(month_label));
    (void)attron(A_REVERSE | A_BOLD | attr_color(COLOR_BRAND));
    (void)mvhline(0, 0, ' ', columns);
    put_clipped(0, 2, columns - 4,
                columns < 70 ? "CMNY" : "CMNY  //  YOUR MONEY, CLEARLY");
    int right = columns - (int)strlen(month_label) - 3;
    if (state->options->demo) {
        const char *badge = "[ DEMO - NOT SAVED ]";
        int badge_x = right - (int)strlen(badge) - 2;
        if (badge_x > (columns < 70 ? 7 : 34)) {
            put_clipped(0, badge_x, (int)strlen(badge), badge);
        }
    }
    if (right > (columns < 70 ? 7 : 34)) {
        put_clipped(0, right, columns - right - 1, month_label);
    }
    (void)attroff(A_REVERSE | A_BOLD | attr_color(COLOR_BRAND));

    if (columns < 58) {
        char compact[80];
        (void)snprintf(compact, sizeof(compact), "< [%d/%d] %s >  Tab screens",
                       (int)state->screen + 1, (int)SCREEN_COUNT,
                       screen_label(state->screen));
        (void)attron(A_BOLD | attr_color(COLOR_BRAND));
        put_clipped(1, 2, columns - 4, compact);
        (void)attroff(A_BOLD | attr_color(COLOR_BRAND));
        return;
    }

    const char *wide_tabs[] = {
        "[1] Overview", "[2] Activity", "[3] Plan", "[4] Insights", "[5] Manage"
    };
    const char *compact_tabs[] = {"1 Home", "2 Activity", "3 Plan", "4 Insights", "5 Manage"};
    const char *const *tabs = columns < 96 ? compact_tabs : wide_tabs;
    int x = 2;
    for (int i = 0; i < SCREEN_COUNT; i++) {
        bool active = (int)state->screen == i;
        if (active) (void)attron(A_BOLD | attr_color(COLOR_BRAND));
        put_clipped(1, x, columns - x - 1, tabs[i]);
        if (active) (void)attroff(A_BOLD | attr_color(COLOR_BRAND));
        x += (int)strlen(tabs[i]) + (columns < 96 ? 2 : 3);
        if (x >= columns - 4) break;
    }
}

static void draw_footer(const UiState *state, int rows, int columns) {
    char hints[256];
    (void)attron(A_REVERSE | attr_color(COLOR_MUTED));
    (void)mvhline(rows - 1, 0, ' ', columns);
    if (state->status[0] != '\0') {
        put_clipped(rows - 1, 1, columns - 2, state->status);
    } else {
        if (columns < 58) {
            if (state->screen == SCREEN_ACTIVITY) {
                (void)snprintf(hints, sizeof(hints), "%c add  %c transfer  Enter view  : more",
                               state->bindings[ACTION_ADD], state->bindings[ACTION_TRANSFER]);
            } else if (state->screen == SCREEN_PLAN) {
                (void)snprintf(hints, sizeof(hints), "%c budget  %c recurring  : more  ? help",
                               state->bindings[ACTION_BUDGET],
                               state->bindings[ACTION_RECURRING]);
            } else {
                (void)snprintf(hints, sizeof(hints), "Tab screens  : more  ? help  q quit");
            }
        } else if (state->screen == SCREEN_MANAGE &&
                   state->settings_selected == SETTINGS_MOUSE) {
            (void)snprintf(hints, sizeof(hints),
                           "Left/Right mouse mode  Shift-drag selects terminal text  : commands  ? help");
        } else if (state->screen == SCREEN_MANAGE &&
                   state->settings_selected == SETTINGS_TUTORIAL) {
            (void)snprintf(hints, sizeof(hints),
                           "Enter replay tutorial  Esc back  : commands  ? key guide  q quit");
        } else if (state->screen == SCREEN_MANAGE) {
            (void)snprintf(hints, sizeof(hints),
                           "Up/Down move  Enter/Left/Right change  Esc back  : commands  ? help  q quit");
        } else if (state->screen == SCREEN_ACTIVITY) {
            (void)snprintf(hints, sizeof(hints),
                           "%c add  %c transfer  %c account  Enter view  %c edit  %c delete  : commands",
                           state->bindings[ACTION_ADD], state->bindings[ACTION_TRANSFER],
                           state->bindings[ACTION_ACCOUNT_FILTER], state->bindings[ACTION_EDIT],
                           state->bindings[ACTION_DELETE]);
        } else if (state->screen == SCREEN_PLAN) {
            (void)snprintf(hints, sizeof(hints),
                           "%c budget  %c recurring  [/] month  Esc back  : commands  ? help",
                           state->bindings[ACTION_BUDGET], state->bindings[ACTION_RECURRING]);
        } else if (state->screen == SCREEN_INSIGHTS) {
            (void)snprintf(hints, sizeof(hints),
                           "[/] month  Enter activity  Esc back  : commands  ? help  q quit");
        } else {
            (void)snprintf(hints, sizeof(hints),
                           "%c add  Enter activity  [/] month  : commands  ? help  q quit",
                           state->bindings[ACTION_ADD]);
        }
        put_clipped(rows - 1, 1, columns - 2, hints);
    }
    (void)attroff(A_REVERSE | attr_color(COLOR_MUTED));
}

static void draw_card(const UiState *state, int y, int x, int width,
                      const char *label, int64_t cents, int color) {
    draw_box(y, x, 5, width, label);
    char value[80];
    format_value(state, cents, value, sizeof(value));
    int value_x = x + max_int(2, (width - (int)strlen(value)) / 2);
    (void)attron(A_BOLD | attr_color(color));
    put_clipped(y + 2, value_x, width - 4, value);
    (void)attroff(A_BOLD | attr_color(color));
}

static void draw_compact_summary(const UiState *state, int y, int columns) {
    char income[80], expense[80], saved[80], rate[32];
    int64_t net = state->summary.income_cents - state->summary.expense_cents;
    format_value(state, state->summary.income_cents, income, sizeof(income));
    format_value(state, state->summary.expense_cents, expense, sizeof(expense));
    format_value(state, net, saved, sizeof(saved));
    if (state->summary.income_cents > 0) {
        double percentage = (double)net * 100.0 / (double)state->summary.income_cents;
        (void)snprintf(rate, sizeof(rate), "%.1f%%", percentage);
    } else {
        (void)snprintf(rate, sizeof(rate), "n/a");
    }
    const char *labels[] = {"Income", "Spent", "Saved", "Rate"};
    const char *values[] = {income, expense, saved, rate};
    for (int i = 0; i < 4; i++) {
        (void)attron(A_BOLD);
        put_clipped(y + i, 2, 10, labels[i]);
        (void)attroff(A_BOLD);
        int color = i == 0 ? COLOR_INCOME : (i == 1 || (i == 2 && net < 0) ? COLOR_EXPENSE : COLOR_INCOME);
        (void)attron(attr_color(color));
        put_clipped(y + i, 12, columns - 14, values[i]);
        (void)attroff(attr_color(color));
    }
}

static void draw_categories(const UiState *state, int y, int x, int height, int width) {
    draw_box(y, x, height, width, "SPENDING BY CATEGORY");
    if (state->category_count == 0) {
        (void)attron(A_DIM | attr_color(COLOR_MUTED));
        put_clipped(y + 2, x + 2, width - 4, "No expenses in this month.");
        (void)attroff(A_DIM | attr_color(COLOR_MUTED));
        return;
    }
    int available = height - 2;
    int label_width = min_int(14, max_int(8, width / 4));
    int amount_width = min_int(16, max_int(10, width / 4));
    int bar_width = width - label_width - amount_width - 6;
    int64_t maximum = state->categories[0].amount_cents;
    size_t shown = state->category_count < (size_t)available ? state->category_count : (size_t)available;
    for (size_t i = 0; i < shown; i++) {
        int row = y + 1 + (int)i;
        put_clipped(row, x + 2, label_width, state->categories[i].category);
        if (bar_width >= 4 && maximum > 0) {
            int filled = (int)(((double)state->categories[i].amount_cents /
                                (double)maximum) * (double)bar_width);
            if (filled < 1) filled = 1;
            (void)attron(attr_color(COLOR_EXPENSE));
            (void)mvhline(row, x + 3 + label_width, '#', filled);
            (void)attroff(attr_color(COLOR_EXPENSE));
            if (filled < bar_width) {
                (void)attron(A_DIM);
                (void)mvhline(row, x + 3 + label_width + filled, '.', bar_width - filled);
                (void)attroff(A_DIM);
            }
        }
        char amount[64];
        cmny_money_format(state->categories[i].amount_cents, amount, sizeof(amount));
        int amount_x = x + width - (int)strlen(amount) - 2;
        if (amount_x > x + 2 + label_width) put_clipped(row, amount_x, amount_width, amount);
    }
}

static void draw_trend(const UiState *state, int y, int x, int height, int width) {
    draw_box(y, x, height, width, "SIX-MONTH SPEND");
    int64_t maximum = 0;
    for (size_t i = 0; i < CMNY_TREND_MONTHS; i++) {
        if (state->trend[i].expense_cents > maximum) maximum = state->trend[i].expense_cents;
    }
    if (maximum == 0) {
        (void)attron(A_DIM | attr_color(COLOR_MUTED));
        put_clipped(y + 2, x + 2, width - 4,
                    "No spending history yet. Add an expense to start this trend.");
        (void)attroff(A_DIM | attr_color(COLOR_MUTED));
        return;
    }
    int available = height - 2;
    size_t first = CMNY_TREND_MONTHS > (size_t)available ? CMNY_TREND_MONTHS - (size_t)available : 0;
    int bar_width = max_int(1, width - 26);
    int row = y + 1;
    for (size_t i = first; i < CMNY_TREND_MONTHS && row < y + height - 1; i++, row++) {
        put_clipped(row, x + 2, 7, state->trend[i].month);
        int filled = maximum > 0
                         ? (int)(((double)state->trend[i].expense_cents /
                                  (double)maximum) * (double)bar_width)
                         : 0;
        if (state->trend[i].expense_cents > 0 && filled < 1) filled = 1;
        (void)attron(attr_color(COLOR_BRAND));
        if (filled > 0) (void)mvhline(row, x + 10, '=', filled);
        (void)attroff(attr_color(COLOR_BRAND));
        char amount[48];
        cmny_money_format(state->trend[i].expense_cents, amount, sizeof(amount));
        int amount_x = x + width - (int)strlen(amount) - 2;
        if (amount_x > x + 10 + filled) put_clipped(row, amount_x, 14, amount);
    }
}

static void draw_budgets(const UiState *state, int y, int x, int height, int width) {
    char title[64];
    (void)snprintf(title, sizeof(title), "BUDGETS  [%c] SET",
                   state->bindings[ACTION_BUDGET]);
    draw_box(y, x, height, width, title);
    if (state->budget_count == 0) {
        char message[96];
        (void)snprintf(message, sizeof(message), "No budgets yet. Press %c to set one.",
                       state->bindings[ACTION_BUDGET]);
        (void)attron(A_DIM | attr_color(COLOR_MUTED));
        put_clipped(y + 2, x + 2, width - 4, message);
        (void)attroff(A_DIM | attr_color(COLOR_MUTED));
        return;
    }
    int bar_start = x + min_int(16, max_int(10, width / 3));
    int bar_width = max_int(1, width - (bar_start - x) - 19);
    size_t shown = state->budget_count < (size_t)(height - 2)
        ? state->budget_count : (size_t)(height - 2);
    for (size_t i = 0; i < shown; i++) {
        int row = y + 1 + (int)i;
        put_clipped(row, x + 2, bar_start - x - 3, state->budgets[i].category);
        double ratio = (double)state->budgets[i].spent_cents /
                       (double)state->budgets[i].limit_cents;
        int filled = (int)(ratio * (double)bar_width);
        if (state->budgets[i].spent_cents > 0 && filled < 1) filled = 1;
        if (filled > bar_width) filled = bar_width;
        int color = ratio > 1.0 ? COLOR_WARNING : COLOR_INCOME;
        (void)attron(attr_color(color));
        if (filled > 0) (void)mvhline(row, bar_start, '#', filled);
        (void)attroff(attr_color(color));
        if (filled < bar_width) {
            (void)attron(A_DIM);
            (void)mvhline(row, bar_start + filled, '.', bar_width - filled);
            (void)attroff(A_DIM);
        }
        char spent[32], limit[32], value[70];
        cmny_money_format_plain(state->budgets[i].spent_cents, spent, sizeof(spent));
        cmny_money_format_plain(state->budgets[i].limit_cents, limit, sizeof(limit));
        (void)snprintf(value, sizeof(value), "%s/%s", spent, limit);
        put_clipped(row, x + width - 17, 15, value);
    }
}

static void draw_recurring(const UiState *state, int y, int x, int height, int width) {
    char title[64];
    (void)snprintf(title, sizeof(title), "RECURRING  [%c] MANAGE",
                   state->bindings[ACTION_RECURRING]);
    draw_box(y, x, height, width, title);
    if (state->recurring_count == 0) {
        char message[96];
        (void)snprintf(message, sizeof(message), "No recurring entries. Press %c to open the manager.",
                       state->bindings[ACTION_RECURRING]);
        (void)attron(A_DIM | attr_color(COLOR_MUTED));
        put_clipped(y + 2, x + 2, width - 4, message);
        (void)attroff(A_DIM | attr_color(COLOR_MUTED));
        return;
    }
    size_t shown = state->recurring_count < (size_t)(height - 2)
        ? state->recurring_count : (size_t)(height - 2);
    for (size_t i = 0; i < shown; i++) {
        char amount[48];
        char line[180];
        cmny_money_format_plain(state->recurring[i].amount_cents, amount, sizeof(amount));
        (void)snprintf(line, sizeof(line), "%s  %s  %s  day %d",
                       state->recurring[i].kind == CMNY_INCOME ? "+" : "-",
                       state->recurring[i].category, amount,
                       state->recurring[i].day_of_month);
        put_clipped(y + 1 + (int)i, x + 2, width - 4, line);
    }
}

static void draw_transaction_rows(const UiState *state, const CmnyTransaction *items, size_t count,
                                  int y, int x, int height, int width, bool selectable) {
    if (count == 0) {
        char empty[128];
        if (state->search[0] != '\0' || state->kind_filter != 0 || state->account_filter_id != 0) {
            (void)snprintf(empty, sizeof(empty), "No matches. Press %c to clear filters.",
                           state->bindings[ACTION_CLEAR]);
        } else if (!selectable) {
            (void)snprintf(empty, sizeof(empty),
                           "This month is ready. Press %c to record your first entry.",
                           state->bindings[ACTION_ADD]);
        } else {
            (void)snprintf(empty, sizeof(empty),
                           "No activity yet. Press %c to record an expense or income.",
                           state->bindings[ACTION_ADD]);
        }
        (void)attron(A_DIM | attr_color(COLOR_MUTED));
        put_clipped(y, x, width, empty);
        (void)attroff(A_DIM | attr_color(COLOR_MUTED));
        return;
    }
    bool compact = width < 70;
    int item_height = compact ? 2 : 1;
    int rows_available = max_int(0, height / item_height);
    size_t start = 0;
    if (selectable && state->selected >= (size_t)rows_available && rows_available > 0) {
        start = state->selected - (size_t)rows_available + 1;
    }
    for (int row_index = 0; row_index < rows_available; row_index++) {
        size_t index = start + (size_t)row_index;
        if (index >= count) break;
        const CmnyTransaction *tx = &items[index];
        const UiEntryMeta *meta = selectable ? &state->transaction_meta[index] : NULL;
        int row = y + row_index * item_height;
        if (selectable && index == state->selected) {
            (void)attron(selection_attr());
            (void)mvhline(row, x, ' ', width);
            if (compact && row + 1 < y + height) (void)mvhline(row + 1, x, ' ', width);
        }
        int date_x = x + 1;
        if (meta != NULL) {
            const char *marker = meta->clear_state == CMNY_POSTING_RECONCILED ? "[R]" :
                                 meta->clear_state == CMNY_POSTING_CLEARED ? "[c]" : "[ ]";
            put_clipped(row, x + 1, 3, marker);
            date_x = x + 5;
        }
        put_clipped(row, date_x, min_int(10, width - (date_x - x) - 1), tx->occurred_on);
        char amount[64];
        char formatted[52];
        cmny_money_format(tx->amount_cents, formatted, sizeof(formatted));
        if (meta != NULL && meta->entry_type == CMNY_ENTRY_TRANSFER) {
            (void)snprintf(amount, sizeof(amount), "<>%s", formatted);
        } else {
            (void)snprintf(amount, sizeof(amount), "%c%s", tx->kind == CMNY_INCOME ? '+' : '-', formatted);
        }
        int amount_x = x + width - (int)strlen(amount) - 1;
        if (compact) {
            int category_x = meta != NULL ? x + 16 : x + 15;
            int category_width = amount_x - category_x - 1;
            if (category_width > 0) put_clipped(row, category_x, category_width, tx->category);
            if (row + 1 < y + height) {
                char context[256];
                if (meta != NULL) {
                    (void)snprintf(context, sizeof(context), "%s%s%s%s%s",
                                   meta->account,
                                   meta->counterparty[0] != '\0' ? " -> " : "",
                                   meta->counterparty,
                                   meta->payee[0] != '\0' ? "  " : "",
                                   meta->payee[0] != '\0' ? meta->payee : tx->note);
                    put_clipped(row + 1, x + 3, width - 5, context);
                } else {
                    put_clipped(row + 1, x + 3, width - 5,
                                tx->note[0] != '\0' ? tx->note : "No note");
                }
            }
        } else {
            int category_x = meta != NULL ? x + 16 : x + 15;
            if (meta != NULL && width >= 84) {
                put_clipped(row, category_x, 13, meta->account);
                put_clipped(row, x + 31, 13, tx->category);
                const char *description = meta->payee[0] != '\0' ? meta->payee :
                                          (meta->counterparty[0] != '\0' ? meta->counterparty : tx->note);
                put_clipped(row, x + 46, width - 68, description);
            } else {
                put_clipped(row, category_x, min_int(14, width - 30), tx->category);
                if (width >= 72) put_clipped(row, x + 32, width - 55, tx->note);
            }
        }
        if (!(selectable && index == state->selected)) {
            int color = meta != NULL && meta->entry_type == CMNY_ENTRY_TRANSFER
                ? COLOR_BRAND : (tx->kind == CMNY_INCOME ? COLOR_INCOME : COLOR_EXPENSE);
            (void)attron(attr_color(color));
        }
        if (amount_x > x + 14) put_clipped(row, amount_x, (int)strlen(amount), amount);
        if (!(selectable && index == state->selected)) {
            int color = meta != NULL && meta->entry_type == CMNY_ENTRY_TRANSFER
                ? COLOR_BRAND : (tx->kind == CMNY_INCOME ? COLOR_INCOME : COLOR_EXPENSE);
            (void)attroff(attr_color(color));
        }
        if (selectable && index == state->selected) {
            (void)attroff(selection_attr());
        }
    }
}

static void draw_recent(const UiState *state, int y, int x, int height, int width) {
    draw_box(y, x, height, width, "RECENT ACTIVITY");
    draw_transaction_rows(state, state->recent, state->recent_count, y + 1, x + 1,
                          height - 2, width - 2, false);
}

static void draw_overview(const UiState *state, int rows, int columns) {
    int content_bottom = rows - 1;
    int64_t saved = state->summary.income_cents - state->summary.expense_cents;
    if (columns < 70) {
        draw_compact_summary(state, 3, columns);
        int remaining = content_bottom - 8;
        if (remaining >= 5) draw_recent(state, 7, 0, remaining + 1, columns);
        return;
    }

    int gap = 1;
    int card_width = (columns - gap * 3) / 4;
    int x2 = card_width + gap;
    int x3 = x2 + card_width + gap;
    int x4 = x3 + card_width + gap;
    draw_card(state, 3, 0, card_width, "INCOME", state->summary.income_cents, COLOR_INCOME);
    draw_card(state, 3, x2, card_width, "SPENT", state->summary.expense_cents, COLOR_EXPENSE);
    draw_card(state, 3, x3, card_width, "SAVED", saved, saved >= 0 ? COLOR_INCOME : COLOR_EXPENSE);

    char rate_label[32];
    if (state->summary.income_cents > 0) {
        double rate = (double)saved * 100.0 / (double)state->summary.income_cents;
        (void)snprintf(rate_label, sizeof(rate_label), "%.1f%%", rate);
    } else {
        (void)snprintf(rate_label, sizeof(rate_label), "n/a");
    }
    draw_box(3, x4, 5, columns - x4, "SAVINGS RATE");
    int rate_x = x4 + max_int(2, (columns - x4 - (int)strlen(rate_label)) / 2);
    (void)attron(A_BOLD | attr_color(saved >= 0 ? COLOR_INCOME : COLOR_EXPENSE));
    put_clipped(5, rate_x, columns - rate_x - 2, rate_label);
    (void)attroff(A_BOLD | attr_color(saved >= 0 ? COLOR_INCOME : COLOR_EXPENSE));

    if (rows >= 28 && columns >= 90) {
        int panel_height = min_int(9, rows - 14);
        int left_width = columns / 2;
        draw_categories(state, 9, 0, panel_height, left_width);
        draw_trend(state, 9, left_width + 1, panel_height, columns - left_width - 1);
        int recent_y = 9 + panel_height + 1;
        int recent_height = content_bottom - recent_y;
        if (recent_height >= 4) draw_recent(state, recent_y, 0, recent_height, columns);
    } else {
        int category_height = min_int(7, max_int(4, rows - 15));
        draw_categories(state, 9, 0, category_height, columns);
        int recent_y = 9 + category_height;
        int recent_height = content_bottom - recent_y;
        if (recent_height >= 4) draw_recent(state, recent_y, 0, recent_height, columns);
    }
}

static void draw_activity(const UiState *state, int rows, int columns) {
    char title[220];
    const char *filter = state->kind_filter == CMNY_EXPENSE ? "expenses" :
                         state->kind_filter == CMNY_INCOME ? "income" : "all";
    size_t range_start = state->activity_total == 0 ? 0 : state->activity_offset + 1;
    size_t range_end = state->activity_offset + state->transaction_count;
    const CmnyAccount *filtered_account = account_by_id(state, state->account_filter_id);
    const char *account_name = filtered_account != NULL ? filtered_account->name : "all";
    if (state->search[0] != '\0') {
        (void)snprintf(title, sizeof(title), "ACTIVITY  %zu-%zu/%zu  all months  type:%s  account:%s  search:%s",
                       range_start, range_end, state->activity_total, filter, account_name, state->search);
    } else {
        (void)snprintf(title, sizeof(title), "ACTIVITY  %zu-%zu/%zu  type:%s  account:%s",
                       range_start, range_end, state->activity_total, filter, account_name);
    }
    draw_box(3, 0, rows - 4, columns, title);
    if (rows > 9) {
        (void)attron(A_BOLD | A_DIM);
        put_clipped(4, 2, columns - 4,
                    columns < 70 ? "    DATE        CATEGORY                 AMOUNT"
                                 : (columns < 84 ? "    DATE        CATEGORY         NOTE                         AMOUNT"
                                                 : "    DATE        ACCOUNT        CATEGORY       PAYEE / DETAILS       AMOUNT"));
        (void)attroff(A_BOLD | A_DIM);
        draw_transaction_rows(state, state->transactions, state->transaction_count,
                              5, 1, rows - 7, columns - 2, true);
    }
}

static void draw_plan(const UiState *state, int rows, int columns) {
    if (columns >= 78) {
        int left = columns / 2;
        draw_budgets(state, 3, 0, rows - 4, left);
        draw_recurring(state, 3, left + 1, rows - 4, columns - left - 1);
    } else {
        int available = rows - 4;
        int first = max_int(5, available / 2);
        draw_budgets(state, 3, 0, first, columns);
        if (available - first >= 3) {
            draw_recurring(state, 3 + first, 0, available - first, columns);
        }
    }
}

static void draw_insights(const UiState *state, int rows, int columns) {
    char comparison[128];
    int64_t delta = state->summary.expense_cents - state->previous.expense_cents;
    bool has_spending = false;
    for (size_t i = 0; i < CMNY_TREND_MONTHS; i++) {
        if (state->trend[i].expense_cents > 0) has_spending = true;
    }
    if (!has_spending) {
        (void)snprintf(comparison, sizeof(comparison),
                       "No spending to analyze yet. Press %c to record an expense.",
                       state->bindings[ACTION_ADD]);
    } else if (state->previous.expense_cents == 0) {
        (void)snprintf(comparison, sizeof(comparison), "Previous month: no spending baseline");
    } else {
        double percentage = (double)delta * 100.0 / (double)state->previous.expense_cents;
        (void)snprintf(comparison, sizeof(comparison), "Spending vs previous month: %+.1f%%", percentage);
    }
    (void)attron(A_BOLD | attr_color(delta <= 0 ? COLOR_INCOME : COLOR_WARNING));
    put_clipped(3, 2, columns - 4, comparison);
    (void)attroff(A_BOLD | attr_color(delta <= 0 ? COLOR_INCOME : COLOR_WARNING));
    if (columns >= 78) {
        int left = columns / 2;
        draw_categories(state, 5, 0, rows - 6, left);
        draw_trend(state, 5, left + 1, rows - 6, columns - left - 1);
    } else {
        int available = rows - 6;
        int first = max_int(4, available / 2);
        draw_categories(state, 5, 0, first, columns);
        if (available - first >= 3) draw_trend(state, 5 + first, 0, available - first, columns);
    }
}

static void settings_item_text(const UiState *state, size_t item,
                               const char **label, char *value, size_t value_size) {
    if (item == SETTINGS_THEME) {
        *label = "Theme";
        (void)snprintf(value, value_size, "%s", cmny_theme_name(state->theme));
    } else if (item == SETTINGS_START_SCREEN) {
        *label = "Start screen";
        (void)snprintf(value, value_size, "%s", screen_label(state->start_screen));
    } else if (item == SETTINGS_MOUSE) {
        *label = "Mouse";
        if (state->mouse_mode == UI_MOUSE_OFF) {
            (void)snprintf(value, value_size, "off");
        } else {
            (void)snprintf(value, value_size, "%s (%s)", mouse_mode_name(state->mouse_mode),
                           state->mouse_active ? "active" : "unavailable");
        }
    } else if (item >= SETTINGS_BINDING_FIRST && item < SETTINGS_ACCOUNTS) {
        UiAction action = (UiAction)(item - SETTINGS_BINDING_FIRST);
        *label = binding_definitions[action].label;
        (void)snprintf(value, value_size, "[ %c ]", state->bindings[action]);
    } else if (item == SETTINGS_ACCOUNTS) {
        *label = "Accounts";
        (void)snprintf(value, value_size, "%zu total  /  Enter", state->account_count);
    } else if (item == SETTINGS_RECONCILE) {
        *label = "Reconcile statements";
        (void)snprintf(value, value_size, "Enter");
    } else if (item == SETTINGS_INTEGRITY) {
        *label = "Check ledger integrity";
        (void)snprintf(value, value_size, "Enter");
    } else if (item == SETTINGS_TUTORIAL) {
        *label = "Keyboard tutorial";
        (void)snprintf(value, value_size, "Replay");
    } else if (item == SETTINGS_BACKUP) {
        *label = "Create backup now";
        (void)snprintf(value, value_size, "Enter");
    } else {
        *label = "Reset keybindings";
        (void)snprintf(value, value_size, "Enter");
    }
}

static void draw_manage(const UiState *state, int rows, int columns) {
    draw_box(3, 0, rows - 4, columns, "MANAGE  /  SETTINGS & DATA");
    (void)attron(state->options->demo ? attr_color(COLOR_WARNING)
                                      : (A_DIM | attr_color(COLOR_MUTED)));
    const char *manage_note = state->options->demo
        ? "Demo settings and entries disappear when you quit."
        : (state->summary.transaction_count == 0
            ? "No entries this month; start with the defaults or customize them below."
            : "Changes are saved automatically for this ledger.");
    put_clipped(4, 2, columns - 4, manage_note);
    (void)attroff(state->options->demo ? attr_color(COLOR_WARNING)
                                       : (A_DIM | attr_color(COLOR_MUTED)));

    int available = max_int(1, rows - 7);
    size_t first = 0;
    if (state->settings_selected >= (size_t)available) {
        first = state->settings_selected - (size_t)available + 1;
    }
    for (int row = 0; row < available; row++) {
        size_t item = first + (size_t)row;
        if (item >= SETTINGS_ITEM_COUNT) break;
        const char *label = "";
        char value[64];
        settings_item_text(state, item, &label, value, sizeof(value));
        int y = 5 + row;
        bool selected = item == state->settings_selected;
        if (selected) {
            (void)attron(selection_attr());
            (void)mvhline(y, 1, ' ', columns - 2);
        }
        char line[180];
        (void)snprintf(line, sizeof(line), "%-26s  %s", label, value);
        put_clipped(y, 3, columns - 6, line);
        if (selected) (void)attroff(selection_attr());
    }
}

static void draw_resize(int rows, int columns) {
    const char *line1 = "CMNY needs a little more room.";
    char line2[80];
    (void)snprintf(line2, sizeof(line2), "Current %dx%d  /  minimum 40x14", columns, rows);
    int y = rows / 2;
    put_clipped(y - 1, max_int(0, (columns - (int)strlen(line1)) / 2), columns, line1);
    put_clipped(y + 1, max_int(0, (columns - (int)strlen(line2)) / 2), columns, line2);
    put_clipped(rows - 1, 0, columns, "q quit");
}

static void draw(const UiState *state) {
    int rows, columns;
    getmaxyx(stdscr, rows, columns);
    (void)erase();
    if (rows < 14 || columns < 40) {
        draw_resize(rows, columns);
    } else {
        draw_header(state, columns);
        if (state->screen == SCREEN_OVERVIEW) draw_overview(state, rows, columns);
        else if (state->screen == SCREEN_ACTIVITY) draw_activity(state, rows, columns);
        else if (state->screen == SCREEN_PLAN) draw_plan(state, rows, columns);
        else if (state->screen == SCREEN_INSIGHTS) draw_insights(state, rows, columns);
        else draw_manage(state, rows, columns);
        draw_footer(state, rows, columns);
    }
    (void)wnoutrefresh(stdscr);
    (void)doupdate();
}

static bool input_modal(const char *title, const char *label, char *buffer, size_t capacity) {
    if (capacity < 2) return false;
    (void)timeout(-1);
    size_t length = strlen(buffer);
    if (length >= capacity) length = capacity - 1;
    buffer[length] = '\0';
    for (;;) {
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        int width = min_int(74, columns - 4);
        if (rows < 10 || width < 30) {
            (void)curs_set(0);
            (void)erase();
            draw_resize(rows, columns);
            put_clipped(rows - 1, 0, columns, "Resize terminal or Esc cancel");
            (void)refresh();
            int small_ch = getch();
            if (interrupted || small_ch == 27) {
                (void)timeout(250);
                return false;
            }
            continue;
        }
        (void)curs_set(1);
        int y = rows / 2 - 3;
        int x = (columns - width) / 2;
        (void)erase();
        draw_box(y, x, 7, width, title);
        put_clipped(y + 2, x + 2, width - 4, label);
        (void)attron(A_REVERSE);
        (void)mvhline(y + 3, x + 2, ' ', width - 4);
        put_clipped(y + 3, x + 2, width - 4, buffer);
        (void)attroff(A_REVERSE);
        (void)attron(A_DIM);
        put_clipped(y + 5, x + 2, width - 4, "Enter accept  Esc cancel  Backspace erase  Ctrl+U clear");
        (void)attroff(A_DIM);
        int cursor_x = x + 2 + min_int((int)length, width - 5);
        (void)move(y + 3, cursor_x);
        (void)refresh();
        int ch = getch();
        if (interrupted) {
            (void)curs_set(0);
            (void)timeout(250);
            return false;
        }
        if (ch == 27) {
            (void)curs_set(0);
            (void)timeout(250);
            return false;
        }
        if (ch == KEY_RESIZE) continue;
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            (void)curs_set(0);
            (void)timeout(250);
            return true;
        }
        if (ch == 21) {
            length = 0;
            buffer[0] = '\0';
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (length > 0) {
                length--;
                buffer[length] = '\0';
            }
        } else if (ch >= 32 && ch <= 126 && length + 1 < capacity) {
            buffer[length++] = (char)ch;
            buffer[length] = '\0';
        }
    }
}

static bool keybinding_modal(const UiState *state, UiAction action, char *result) {
    (void)timeout(-1);
    char warning[128] = {0};
    for (;;) {
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        int width = min_int(70, columns - 4);
        int height = 8;
        int y = max_int(0, (rows - height) / 2);
        int x = max_int(0, (columns - width) / 2);
        (void)erase();
        if (rows < 10 || width < 34) {
            draw_resize(rows, columns);
            put_clipped(rows - 1, 0, columns, "Resize terminal or Esc cancel");
        } else {
            draw_box(y, x, height, width, "CHANGE KEYBINDING");
            char line[160];
            (void)snprintf(line, sizeof(line), "%s  (currently %c)",
                           binding_definitions[action].label, state->bindings[action]);
            put_clipped(y + 2, x + 2, width - 4, line);
            put_clipped(y + 3, x + 2, width - 4,
                        "Press one letter or /. Uppercase is treated as lowercase.");
            (void)attron(A_DIM);
            put_clipped(y + 5, x + 2, width - 4,
                        "q is reserved for Quit. Esc or Backspace cancels.");
            (void)attroff(A_DIM);
            if (warning[0] != '\0') {
                (void)attron(A_BOLD | attr_color(COLOR_WARNING));
                put_clipped(y + 6, x + 2, width - 4, warning);
                (void)attroff(A_BOLD | attr_color(COLOR_WARNING));
            }
        }
        (void)refresh();
        int ch = getch();
        if (interrupted || ch == 27 || ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            (void)timeout(250);
            return false;
        }
        if (ch == KEY_RESIZE) continue;
        int key = normalize_binding_key(ch);
        if (key == 0) {
            (void)snprintf(warning, sizeof(warning),
                           "That key is reserved. Choose a-z (except q) or /.");
            continue;
        }
        UiAction conflict;
        if (binding_conflicts(state, action, key, &conflict)) {
            (void)snprintf(warning, sizeof(warning), "%c is already used by %s.",
                           key, binding_definitions[conflict].label);
            continue;
        }
        *result = (char)key;
        (void)timeout(250);
        return true;
    }
}

static bool choose_kind(CmnyKind *kind) {
    (void)timeout(-1);
    for (;;) {
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        int width = min_int(58, columns - 4);
        if (rows < 10 || width < 30) {
            (void)erase();
            draw_resize(rows, columns);
            put_clipped(rows - 1, 0, columns, "Resize terminal or Esc cancel");
            (void)refresh();
            int small_ch = getch();
            if (interrupted || small_ch == 27) { (void)timeout(250); return false; }
            continue;
        }
        int y = rows / 2 - 3;
        int x = (columns - width) / 2;
        (void)erase();
        draw_box(y, x, 7, width, "TRANSACTION TYPE");
        put_clipped(y + 2, x + 3, width - 6, "[1] Expense       [2] Income");
        put_clipped(y + 3, x + 3, width - 6,
                    *kind == CMNY_INCOME ? "Current: Income" : "Current: Expense");
        (void)attron(A_DIM);
        put_clipped(y + 5, x + 3, width - 6, "Enter keep  Esc cancel");
        (void)attroff(A_DIM);
        (void)refresh();
        int ch = getch();
        if (interrupted) { (void)timeout(250); return false; }
        if (ch == KEY_RESIZE) continue;
        if (ch == 27) { (void)timeout(250); return false; }
        if (ch == '1') { *kind = CMNY_EXPENSE; (void)timeout(250); return true; }
        if (ch == '2') { *kind = CMNY_INCOME; (void)timeout(250); return true; }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) { (void)timeout(250); return true; }
    }
}

static bool confirm_modal(const char *title, const char *question) {
    (void)timeout(-1);
    for (;;) {
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        int width = min_int(70, columns - 4);
        int y = rows / 2 - 3;
        int x = (columns - width) / 2;
        (void)erase();
        if (rows < 10 || width < 30) {
            draw_resize(rows, columns);
            put_clipped(rows - 1, 0, columns, "Resize terminal or Esc cancel");
        } else {
            draw_box(y, x, 7, width, title);
            put_clipped(y + 2, x + 2, width - 4, question);
            (void)attron(A_BOLD | attr_color(COLOR_WARNING));
            put_clipped(y + 4, x + 2, width - 4, "Press y to confirm; any other key cancels.");
            (void)attroff(A_BOLD | attr_color(COLOR_WARNING));
        }
        (void)refresh();
        int ch = getch();
        if (interrupted) { (void)timeout(250); return false; }
        if (ch == KEY_RESIZE) continue;
        (void)timeout(250);
        return ch == 'y' || ch == 'Y';
    }
}

static void message_modal(const char *title, const char *const *lines, size_t count) {
    (void)timeout(-1);
    for (;;) {
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        int width = min_int(78, columns - 4);
        int height = min_int(rows - 2, (int)count + 4);
        int y = max_int(0, (rows - height) / 2);
        int x = max_int(0, (columns - width) / 2);
        (void)erase();
        if (rows < 8 || width < 30) {
            draw_resize(rows, columns);
        } else {
            draw_box(y, x, height, width, title);
            size_t shown = count < (size_t)(height - 3) ? count : (size_t)(height - 3);
            for (size_t i = 0; i < shown; i++) {
                put_clipped(y + 1 + (int)i, x + 2, width - 4, lines[i]);
            }
            (void)attron(A_DIM);
            put_clipped(y + height - 2, x + 2, width - 4, "Esc or Enter to go back");
            (void)attroff(A_DIM);
        }
        (void)refresh();
        int ch = getch();
        if (interrupted || ch == 27 || ch == KEY_BACKSPACE || ch == 127 || ch == 8 ||
            ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            (void)timeout(250);
            return;
        }
    }
}

static void show_help(const UiState *state) {
    char lines[17][160];
    (void)snprintf(lines[0], sizeof(lines[0]), "NAVIGATION");
    (void)snprintf(lines[1], sizeof(lines[1]),
                   "  1-5              Overview / Activity / Plan / Insights / Manage");
    (void)snprintf(lines[2], sizeof(lines[2]),
                   "  Tab / Shift-Tab  Next / previous space");
    (void)snprintf(lines[3], sizeof(lines[3]),
                   "  Esc / Backspace  Back one level / cancel");
    (void)snprintf(lines[4], sizeof(lines[4]),
                   "  Arrows / Enter   Move / open selected item");
    (void)snprintf(lines[5], sizeof(lines[5]),
                   "  [ / ]            Previous / next month");
    (void)snprintf(lines[6], sizeof(lines[6]), "MONEY");
    (void)snprintf(lines[7], sizeof(lines[7]),
                   "  %c / %c / %c / %c        Add / edit / delete / undo",
                   state->bindings[ACTION_ADD], state->bindings[ACTION_EDIT],
                   state->bindings[ACTION_DELETE], state->bindings[ACTION_UNDO]);
    (void)snprintf(lines[8], sizeof(lines[8]),
                   "  %c / %c / %c / %c        Search / type / account / clear",
                   state->bindings[ACTION_SEARCH], state->bindings[ACTION_FILTER],
                   state->bindings[ACTION_ACCOUNT_FILTER], state->bindings[ACTION_CLEAR]);
    (void)snprintf(lines[9], sizeof(lines[9]),
                   "  %c transfer  %c accounts  %c reconcile; %c undo latest change",
                   state->bindings[ACTION_TRANSFER], state->bindings[ACTION_ACCOUNTS],
                   state->bindings[ACTION_RECONCILE], state->bindings[ACTION_UNDO]);
    (void)snprintf(lines[10], sizeof(lines[10]), "PLANNING");
    (void)snprintf(lines[11], sizeof(lines[11]),
                   "  %c / %c / %c            Budget / recurring / current month",
                   state->bindings[ACTION_BUDGET], state->bindings[ACTION_RECURRING],
                   state->bindings[ACTION_TODAY]);
    (void)snprintf(lines[12], sizeof(lines[12]), "APPLICATION");
    (void)snprintf(lines[13], sizeof(lines[13]),
                   "  %c / : / ? / q        Manage / commands / key guide / quit",
                   state->bindings[ACTION_SETTINGS]);
    (void)snprintf(lines[14], sizeof(lines[14]), "MOUSE");
    if (state->mouse_mode == UI_MOUSE_OFF) {
        (void)snprintf(lines[15], sizeof(lines[15]),
                       "  Mouse is off; every action remains available from the keyboard");
    } else {
        (void)snprintf(lines[15], sizeof(lines[15]),
                       "  Mouse %s (%s): click selects; double-click opens",
                       mouse_mode_name(state->mouse_mode),
                       state->mouse_active ? "active" : "unavailable");
    }
    (void)snprintf(lines[16], sizeof(lines[16]),
                   "  Shift-drag selects text. Replay the tutorial from Manage.");

    size_t offset = 0;
    const size_t count = sizeof(lines) / sizeof(lines[0]);
    (void)timeout(-1);
    for (;;) {
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        int width = min_int(86, columns - 2);
        int height = min_int(rows - 2, (int)count + 4);
        int y = max_int(0, (rows - height) / 2);
        int x = max_int(0, (columns - width) / 2);
        (void)erase();
        if (rows < 8 || width < 30) {
            draw_resize(rows, columns);
            put_clipped(rows - 1, 0, columns, "Resize terminal or Esc close key guide");
        } else {
            int visible = max_int(1, height - 3);
            size_t maximum = count > (size_t)visible ? count - (size_t)visible : 0;
            if (offset > maximum) offset = maximum;
            draw_box(y, x, height, width, "KEYBINDINGS  /  ?");
            for (int row = 0; row < visible; row++) {
                size_t index = offset + (size_t)row;
                if (index >= count) break;
                bool heading = index == 0 || index == 6 || index == 10 ||
                               index == 12 || index == 14;
                if (heading) (void)attron(A_BOLD | attr_color(COLOR_BRAND));
                put_clipped(y + 1 + row, x + 2, width - 4, lines[index]);
                if (heading) (void)attroff(A_BOLD | attr_color(COLOR_BRAND));
            }
            char footer[96];
            if (maximum > 0) {
                size_t end = offset + (size_t)visible;
                if (end > count) end = count;
                (void)snprintf(footer, sizeof(footer),
                               "Up/Down scroll  %zu-%zu/%zu  Esc close",
                               offset + 1, end, count);
            } else {
                (void)snprintf(footer, sizeof(footer), "Esc or Enter close");
            }
            (void)attron(A_DIM);
            put_clipped(y + height - 2, x + 2, width - 4, footer);
            (void)attroff(A_DIM);
        }
        (void)refresh();
        int ch = getch();
        if (interrupted || ch == 27 || ch == KEY_BACKSPACE || ch == 127 || ch == 8 ||
            ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == '?') {
            (void)timeout(250);
            return;
        }
        if (ch == KEY_RESIZE) continue;
        int visible = max_int(1, height - 3);
        size_t maximum = count > (size_t)visible ? count - (size_t)visible : 0;
        if (ch == KEY_UP && offset > 0) offset--;
        else if (ch == KEY_DOWN && offset < maximum) offset++;
        else if (ch == KEY_HOME) offset = 0;
        else if (ch == KEY_END) offset = maximum;
        else if (ch == KEY_NPAGE) {
            size_t next = offset + (size_t)visible;
            offset = next < maximum ? next : maximum;
        } else if (ch == KEY_PPAGE) {
            offset = offset > (size_t)visible ? offset - (size_t)visible : 0;
        }
    }
}

static bool choose_account(UiState *state, const char *title, int64_t excluded_id,
                           bool allow_all, bool auto_single, int64_t *account_id) {
    size_t choices[UI_ACCOUNT_LIMIT];
    size_t count = 0;
    for (size_t i = 0; i < state->account_count; i++) {
        if (!state->accounts[i].archived && state->accounts[i].id != excluded_id) {
            choices[count++] = i;
        }
    }
    if (!allow_all && count == 0) {
        statusf(state, "No active account is available.");
        return false;
    }
    if (!allow_all && auto_single && count == 1) {
        *account_id = state->accounts[choices[0]].id;
        return true;
    }
    size_t selected = 0;
    size_t total = count + (allow_all ? 1U : 0U);
    (void)timeout(-1);
    for (;;) {
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        int height = min_int(max_int(8, (int)total + 4), rows - 2);
        int width = min_int(72, columns - 4);
        if (rows < 10 || width < 34) {
            (void)erase();
            draw_resize(rows, columns);
        } else {
            int y = (rows - height) / 2;
            int x = (columns - width) / 2;
            (void)erase();
            draw_box(y, x, height, width, title);
            int visible = height - 3;
            size_t first = selected >= (size_t)visible ? selected - (size_t)visible + 1 : 0;
            for (int row = 0; row < visible; row++) {
                size_t item = first + (size_t)row;
                if (item >= total) break;
                char line[180];
                if (allow_all && item == 0) {
                    (void)snprintf(line, sizeof(line), "All accounts");
                } else {
                    size_t choice = choices[item - (allow_all ? 1U : 0U)];
                    const CmnyAccount *account = &state->accounts[choice];
                    char balance[64];
                    cmny_money_format(account->balance_minor, balance, sizeof(balance));
                    (void)snprintf(line, sizeof(line), "%-30s %12s %s",
                                   account->name, balance, account->institution);
                }
                if (item == selected) {
                    (void)attron(selection_attr());
                    (void)mvhline(y + 1 + row, x + 1, ' ', width - 2);
                }
                put_clipped(y + 1 + row, x + 2, width - 4, line);
                if (item == selected) (void)attroff(selection_attr());
            }
            (void)attron(A_DIM);
            put_clipped(y + height - 2, x + 2, width - 4, "Up/Down choose  Enter accept  Esc cancel");
            (void)attroff(A_DIM);
        }
        (void)refresh();
        int ch = getch();
        if (interrupted || ch == 27 || ch == KEY_BACKSPACE || ch == 127) {
            (void)timeout(250);
            return false;
        }
        if (ch == KEY_RESIZE) continue;
        if (ch == KEY_UP && selected > 0) selected--;
        else if (ch == KEY_DOWN && selected + 1 < total) selected++;
        else if (ch == KEY_HOME) selected = 0;
        else if (ch == KEY_END) selected = total - 1;
        else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (allow_all && selected == 0) *account_id = 0;
            else *account_id = state->accounts[choices[selected - (allow_all ? 1U : 0U)]].id;
            (void)timeout(250);
            return true;
        }
    }
}

static void edit_transaction(UiState *state, const CmnyTransaction *existing) {
    CmnyTransaction draft = {0};
    bool editing = existing != NULL;
    int64_t account_id = 0;
    int64_t expected_revision = 0;
    char payee[CMNY_PAYEE_MAX + 1] = {0};
    CmnyLedgerEntry original = {0};
    if (editing) {
        draft = *existing;
        char err[256] = {0};
        if (!cmny_ledger_entry_get(state->db, existing->id, &original, err, sizeof(err))) {
            statusf(state, "Cannot edit entry: %s", err);
            return;
        }
        if (original.type != CMNY_ENTRY_NORMAL || original.posting_count != 1 ||
            original.allocation_count != 1) {
            cmny_ledger_entry_destroy(&original);
            statusf(state, "Only single-category income and expense entries can be edited here.");
            return;
        }
        account_id = original.postings[0].account_id;
        expected_revision = original.revision;
        (void)snprintf(payee, sizeof(payee), "%s", original.payee);
        cmny_ledger_entry_destroy(&original);
    } else {
        draft.kind = CMNY_EXPENSE;
        cmny_today(draft.occurred_on);
        if (!choose_account(state, "ACCOUNT FOR ENTRY", 0, false, true, &account_id)) return;
    }
edit_fields:
    ;
    if (!choose_kind(&draft.kind)) return;

    char amount[CMNY_AMOUNT_EXPRESSION_MAX + 1] = {0};
    if (draft.amount_cents > 0) cmny_money_format_plain(draft.amount_cents, amount, sizeof(amount));
    bool amount_invalid = false;
    for (;;) {
        if (!input_modal(editing ? "EDIT TRANSACTION" : "NEW TRANSACTION",
                         amount_invalid ? "Invalid amount or calculation; the result must be positive"
                                        : "Amount or calculation (e.g. 12.50 + 4.20)",
                         amount, sizeof(amount))) return;
        if (cmny_amount_expression_parse((const unsigned char *)amount, strlen(amount),
                                         &draft.amount_cents) &&
            draft.amount_cents <= 9000000000000000LL) break;
        amount_invalid = true;
    }

    char date_input[CMNY_DATE_EXPRESSION_MAX + 1];
    (void)snprintf(date_input, sizeof(date_input), "%s", draft.occurred_on);
    char base_date[11];
    cmny_today(base_date);
    bool date_invalid = false;
    for (;;) {
        if (!input_modal("TRANSACTION DATE",
                         date_invalid ? "Invalid date expression; try today, -3d, or YYYY-MM-DD"
                                      : "Date (YYYY-MM-DD, today, yesterday, or -3d)",
                         date_input, sizeof(date_input))) return;
        char parsed_date[11];
        if (cmny_date_expression_parse((const unsigned char *)date_input, strlen(date_input),
                                       base_date, parsed_date)) {
            (void)snprintf(draft.occurred_on, sizeof(draft.occurred_on), "%s", parsed_date);
            break;
        }
        date_invalid = true;
    }
    if (draft.category[0] == '\0') {
        const char *key = draft.kind == CMNY_INCOME ? "last_income_category" : "last_expense_category";
        if (!cmny_db_setting_get(state->db, key, draft.category, sizeof(draft.category))) {
            (void)snprintf(draft.category, sizeof(draft.category), "%s",
                           draft.kind == CMNY_INCOME ? "Salary" : "Food");
        }
    }
    bool category_invalid = false;
    for (;;) {
        if (!input_modal("CATEGORY",
                         category_invalid ? "Category needs at least one visible ASCII character"
                                          : "Category (1-32 printable ASCII characters)", draft.category,
                         sizeof(draft.category))) return;
        if (has_visible_text(draft.category)) break;
        category_invalid = true;
    }
    if (!input_modal("PAYEE", "Optional payee (printable ASCII)", payee, sizeof(payee))) return;
    if (!input_modal("NOTE", "Optional note (printable ASCII)", draft.note, sizeof(draft.note))) return;

retry_save:
    ;
    char err[256] = {0};
    CmnyCategory category;
    if (!cmny_category_find(state->db, draft.category, &category, err, sizeof(err))) {
        int mask = draft.kind == CMNY_INCOME ? CMNY_CATEGORY_INCOME : CMNY_CATEGORY_EXPENSE;
        if (!cmny_category_create(state->db, draft.category, mask, 0, &category.id,
                                  err, sizeof(err))) {
            char detail[300];
            (void)snprintf(detail, sizeof(detail), "Category error: %s", err);
            const char *lines[] = {detail, "Your completed draft is still in memory."};
            message_modal("SAVE FAILED", lines, 2);
            goto edit_fields;
        }
    }
    int64_t signed_amount = draft.kind == CMNY_EXPENSE ? -draft.amount_cents : draft.amount_cents;
    CmnySplitDraft split = {.category_id = category.id, .amount_minor = signed_amount, .note = ""};
    CmnyNormalEntryDraft ledger_draft = {
        .account_id = account_id,
        .amount_minor = signed_amount,
        .split_count = 1,
        .occurred_on = draft.occurred_on,
        .payee = payee,
        .note = draft.note,
        .splits = &split
    };
    bool ok = editing
        ? cmny_entry_update_normal(state->db, draft.id, expected_revision, &ledger_draft,
                                   err, sizeof(err))
        : cmny_entry_create_normal(state->db, &ledger_draft, NULL, err, sizeof(err));
    if (!ok) {
        char detail[300];
        (void)snprintf(detail, sizeof(detail), "Database error: %s", err);
        const char *lines[] = {detail, "Your completed draft is still in memory."};
        message_modal("SAVE FAILED", lines, sizeof(lines) / sizeof(lines[0]));
        if (interrupted) return;
        if (confirm_modal("SAVE FAILED", "Press y to retry now; any other key returns to the form.")) {
            goto retry_save;
        }
        goto edit_fields;
    }
    memcpy(state->month, draft.occurred_on, 7);
    state->month[7] = '\0';
    state->activity_offset = 0;
    const char *category_key = draft.kind == CMNY_INCOME
        ? "last_income_category" : "last_expense_category";
    (void)cmny_db_setting_set(state->db, category_key, draft.category, err, sizeof(err));
    statusf(state, editing ? "Transaction updated." : "Transaction saved.");
    refresh_state(state);
    statusf(state, editing ? "Transaction updated." : "Transaction saved.");
}

static void create_transfer(UiState *state) {
    int64_t from_id = 0;
    int64_t to_id = 0;
    if (!choose_account(state, "TRANSFER FROM", 0, false, false, &from_id) ||
        !choose_account(state, "TRANSFER TO", from_id, false, false, &to_id)) return;
    char amount[CMNY_AMOUNT_EXPRESSION_MAX + 1] = {0};
    int64_t amount_minor = 0;
    bool invalid = false;
    for (;;) {
        if (!input_modal("NEW TRANSFER",
                         invalid ? "Invalid calculation; transfer amount must be positive"
                                 : "Amount or calculation (e.g. 125.00)",
                         amount, sizeof(amount))) return;
        if (cmny_amount_expression_parse((const unsigned char *)amount, strlen(amount),
                                         &amount_minor)) break;
        invalid = true;
    }
    char occurred_on[CMNY_DATE_EXPRESSION_MAX + 1];
    char base_date[11];
    cmny_today(base_date);
    (void)snprintf(occurred_on, sizeof(occurred_on), "%s", base_date);
    for (;;) {
        if (!input_modal("TRANSFER DATE", "Date (YYYY-MM-DD, today, yesterday, or -3d)",
                         occurred_on, sizeof(occurred_on))) return;
        char parsed[11];
        if (cmny_date_expression_parse((const unsigned char *)occurred_on, strlen(occurred_on),
                                       base_date, parsed)) {
            (void)snprintf(occurred_on, sizeof(occurred_on), "%s", parsed);
            break;
        }
    }
    char payee[CMNY_PAYEE_MAX + 1] = {0};
    char note[CMNY_LEDGER_NOTE_MAX + 1] = {0};
    if (!input_modal("TRANSFER PAYEE", "Optional payee or institution", payee, sizeof(payee)) ||
        !input_modal("TRANSFER NOTE", "Optional note", note, sizeof(note))) return;
    CmnyTransferDraft draft = {
        .from_account_id = from_id,
        .to_account_id = to_id,
        .amount_minor = amount_minor,
        .occurred_on = occurred_on,
        .payee = payee,
        .note = note
    };
    char err[256] = {0};
    if (!cmny_transfer_create(state->db, &draft, NULL, err, sizeof(err))) {
        statusf(state, "Transfer failed: %s", err);
        return;
    }
    memcpy(state->month, occurred_on, 7);
    state->month[7] = '\0';
    state->activity_offset = 0;
    refresh_state(state);
    statusf(state, "Transfer saved. It does not count as spending.");
}

static void show_detail(const UiState *state, const CmnyTransaction *tx) {
    const UiEntryMeta *meta = NULL;
    for (size_t i = 0; i < state->transaction_count; i++) {
        if (state->transactions[i].id == tx->id) {
            meta = &state->transaction_meta[i];
            break;
        }
    }
    char amount[96];
    char id[64], kind[64], date[64], account[160], payee[160], category[96], clear[96], note[180], value[80];
    format_value(state, tx->amount_cents, value, sizeof(value));
    (void)snprintf(id, sizeof(id), "ID        %lld", (long long)tx->id);
    const char *type = meta != NULL && meta->entry_type == CMNY_ENTRY_TRANSFER ? "Transfer" :
                       (meta != NULL && meta->entry_type == CMNY_ENTRY_ADJUSTMENT ? "Balance adjustment" :
                        (tx->kind == CMNY_INCOME ? "Income" : "Expense"));
    (void)snprintf(kind, sizeof(kind), "Type      %s", type);
    (void)snprintf(amount, sizeof(amount), "Amount    %s", value);
    (void)snprintf(date, sizeof(date), "Date      %s", tx->occurred_on);
    const char *from_account = meta != NULL ? meta->account : "-";
    const char *to_account = meta != NULL ? meta->counterparty : "";
    if (meta != NULL && meta->entry_type == CMNY_ENTRY_TRANSFER && !meta->transfer_outgoing) {
        from_account = meta->counterparty;
        to_account = meta->account;
    }
    (void)snprintf(account, sizeof(account), "Account   %s%s%s", from_account,
                   to_account[0] != '\0' ? " -> " : "", to_account);
    (void)snprintf(payee, sizeof(payee), "Payee     %s",
                   meta != NULL && meta->payee[0] != '\0' ? meta->payee : "-");
    (void)snprintf(category, sizeof(category), "Category  %s", tx->category);
    const char *clear_label = meta != NULL && meta->clear_state == CMNY_POSTING_RECONCILED
        ? "reconciled" : (meta != NULL && meta->clear_state == CMNY_POSTING_CLEARED
                          ? "cleared" : "uncleared");
    (void)snprintf(clear, sizeof(clear), "Status    %s", clear_label);
    (void)snprintf(note, sizeof(note), "Note      %s", tx->note[0] != '\0' ? tx->note : "-");
    const char *lines[] = {id, kind, amount, date, account, payee, category, clear, note};
    message_modal("TRANSACTION DETAIL", lines, sizeof(lines) / sizeof(lines[0]));
}

static const char *account_type_name(CmnyAccountType type) {
    static const char *names[] = {"", "Cash", "Checking", "Savings", "Credit",
                                  "Loan", "Investment", "Other"};
    return type >= CMNY_ACCOUNT_CASH && type <= CMNY_ACCOUNT_OTHER ? names[type] : "Other";
}

static bool choose_account_type(CmnyAccountType *type) {
    size_t selected = *type >= CMNY_ACCOUNT_CASH && *type <= CMNY_ACCOUNT_OTHER
        ? (size_t)(*type - CMNY_ACCOUNT_CASH) : 0;
    (void)timeout(-1);
    for (;;) {
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        int width = min_int(48, columns - 4);
        int height = 11;
        (void)erase();
        if (rows < 13 || width < 30) {
            draw_resize(rows, columns);
        } else {
            int y = (rows - height) / 2;
            int x = (columns - width) / 2;
            draw_box(y, x, height, width, "ACCOUNT TYPE");
            for (size_t i = 0; i < 7; i++) {
                if (i == selected) {
                    (void)attron(selection_attr());
                    (void)mvhline(y + 1 + (int)i, x + 1, ' ', width - 2);
                }
                put_clipped(y + 1 + (int)i, x + 3, width - 6,
                            account_type_name((CmnyAccountType)(i + 1)));
                if (i == selected) (void)attroff(selection_attr());
            }
            (void)attron(A_DIM);
            put_clipped(y + 9, x + 2, width - 4, "Up/Down choose  Enter accept  Esc cancel");
            (void)attroff(A_DIM);
        }
        (void)refresh();
        int ch = getch();
        if (interrupted || ch == 27 || ch == 127 || ch == KEY_BACKSPACE) {
            (void)timeout(250);
            return false;
        }
        if (ch == KEY_UP && selected > 0) selected--;
        else if (ch == KEY_DOWN && selected < 6) selected++;
        else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            *type = (CmnyAccountType)(selected + 1);
            (void)timeout(250);
            return true;
        }
    }
}

static bool parse_signed_amount(const char *text, int64_t *amount) {
    while (isspace((unsigned char)*text)) text++;
    bool negative = *text == '-';
    bool positive = *text == '+';
    if (negative || positive) text++;
    while (isspace((unsigned char)*text)) text++;
    if (zero_amount(text)) {
        *amount = 0;
        return true;
    }
    int64_t magnitude = 0;
    if (!cmny_amount_expression_parse((const unsigned char *)text, strlen(text), &magnitude)) {
        return false;
    }
    *amount = negative ? -magnitude : magnitude;
    return true;
}

static void account_manager(UiState *state) {
    size_t selected = 0;
    char notice[180] = {0};
    (void)timeout(-1);
    for (;;) {
        char err[256] = {0};
        if (!cmny_account_list(state->db, true, state->accounts, UI_ACCOUNT_LIMIT,
                               &state->account_count, err, sizeof(err))) {
            statusf(state, "Accounts failed: %s", err);
            (void)timeout(250);
            return;
        }
        if (state->account_count > 0 && selected >= state->account_count) {
            selected = state->account_count - 1;
        }
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        (void)erase();
        if (rows < 14 || columns < 40) {
            draw_resize(rows, columns);
        } else {
            draw_box(1, 1, rows - 2, columns - 2, "ACCOUNTS");
            put_clipped(2, 3, columns - 6,
                        columns < 76 ? "NAME / BALANCE"
                                     : "NAME                    TYPE        INSTITUTION             BALANCE");
            int visible = rows - 7;
            size_t first = selected >= (size_t)visible ? selected - (size_t)visible + 1 : 0;
            for (int row = 0; row < visible; row++) {
                size_t item = first + (size_t)row;
                if (item >= state->account_count) break;
                const CmnyAccount *account = &state->accounts[item];
                char balance[64], line[240];
                cmny_money_format(account->balance_minor, balance, sizeof(balance));
                if (columns < 76) {
                    (void)snprintf(line, sizeof(line), "%s%s  %s",
                                   account->archived ? "[archived] " : "", account->name, balance);
                } else {
                    (void)snprintf(line, sizeof(line), "%-23s %-11s %-22s %12s%s",
                                   account->name, account_type_name(account->type), account->institution,
                                   balance, account->archived ? "  archived" : "");
                }
                if (item == selected) {
                    (void)attron(selection_attr());
                    (void)mvhline(3 + row, 2, ' ', columns - 4);
                }
                put_clipped(3 + row, 3, columns - 6, line);
                if (item == selected) (void)attroff(selection_attr());
            }
            if (notice[0] != '\0') {
                (void)attron(A_BOLD | attr_color(COLOR_WARNING));
                put_clipped(rows - 4, 3, columns - 6, notice);
                (void)attroff(A_BOLD | attr_color(COLOR_WARNING));
            }
            (void)attron(A_DIM);
            put_clipped(rows - 3, 3, columns - 6,
                        "a create  e edit  x archive/restore  r reconcile  Esc close");
            (void)attroff(A_DIM);
        }
        (void)refresh();
        int ch = getch();
        if (interrupted || ch == 27 || ch == 127 || ch == KEY_BACKSPACE) break;
        if (ch == KEY_RESIZE) continue;
        if (ch == KEY_UP && selected > 0) selected--;
        else if (ch == KEY_DOWN && selected + 1 < state->account_count) selected++;
        else if (ch == KEY_HOME) selected = 0;
        else if (ch == KEY_END && state->account_count > 0) selected = state->account_count - 1;
        else if (ch == 'a') {
            char name[CMNY_ACCOUNT_NAME_MAX + 1] = {0};
            char institution[CMNY_INSTITUTION_MAX + 1] = {0};
            char opening[CMNY_AMOUNT_EXPRESSION_MAX + 2] = "0";
            char date[11];
            cmny_today(date);
            CmnyAccountType type = CMNY_ACCOUNT_CHECKING;
            if (!input_modal("NEW ACCOUNT", "Account name", name, sizeof(name)) ||
                !choose_account_type(&type) ||
                !input_modal("NEW ACCOUNT", "Optional institution", institution, sizeof(institution)) ||
                !input_modal("OPENING BALANCE ADJUSTMENT", "Signed balance (0 for none)",
                             opening, sizeof(opening))) continue;
            int64_t balance = 0;
            if (!parse_signed_amount(opening, &balance)) {
                (void)snprintf(notice, sizeof(notice), "Invalid opening balance.");
                continue;
            }
            if (balance != 0 &&
                !input_modal("BALANCE DATE", "Opening balance date (YYYY-MM-DD)", date, sizeof(date))) continue;
            if (cmny_account_create_with_opening(state->db, name, type, institution, balance,
                                                 balance != 0 ? date : NULL, NULL,
                                                 err, sizeof(err))) {
                (void)snprintf(notice, sizeof(notice), "Account created with explicit opening balance.");
            } else {
                (void)snprintf(notice, sizeof(notice), "Create failed: %.130s", err);
            }
        } else if (ch == 'e' && state->account_count > 0) {
            CmnyAccount account = state->accounts[selected];
            if (!input_modal("EDIT ACCOUNT", "Account name", account.name, sizeof(account.name)) ||
                !choose_account_type(&account.type) ||
                !input_modal("EDIT ACCOUNT", "Optional institution", account.institution,
                             sizeof(account.institution))) continue;
            if (cmny_account_update(state->db, account.id, account.revision, account.name,
                                    account.type, account.institution, err, sizeof(err))) {
                (void)snprintf(notice, sizeof(notice), "Account updated.");
            } else {
                (void)snprintf(notice, sizeof(notice), "Update failed: %.130s", err);
            }
        } else if (ch == 'x' && state->account_count > 0) {
            CmnyAccount account = state->accounts[selected];
            size_t active = 0;
            for (size_t i = 0; i < state->account_count; i++) if (!state->accounts[i].archived) active++;
            if (!account.archived && active == 1) {
                (void)snprintf(notice, sizeof(notice), "Keep at least one active account.");
            } else if (confirm_modal(account.archived ? "RESTORE ACCOUNT" : "ARCHIVE ACCOUNT",
                                     account.archived ? "Restore this account?" :
                                                        "Archive this account? Existing history stays intact.")) {
                if (cmny_account_set_archived(state->db, account.id, account.revision,
                                              !account.archived, err, sizeof(err))) {
                    (void)snprintf(notice, sizeof(notice), "%s.",
                                   account.archived ? "Account restored" : "Account archived");
                    if (!account.archived && state->account_filter_id == account.id) {
                        state->account_filter_id = 0;
                    }
                } else {
                    (void)snprintf(notice, sizeof(notice), "Archive failed: %.130s", err);
                }
            }
        } else if (ch == 'r') {
            reconciliation_center(state);
            (void)snprintf(notice, sizeof(notice), "Returned from reconciliation.");
        }
    }
    (void)timeout(250);
    refresh_state(state);
}

static void reconciliation_session(UiState *state, int64_t session_id) {
    size_t selected = 0;
    char notice[180] = {0};
    (void)timeout(-1);
    for (;;) {
        char err[256] = {0};
        CmnyReconcileSession session;
        CmnyReconcilePosting postings[CMNY_RECONCILE_LIST_LIMIT];
        size_t posting_count = 0;
        if (!cmny_reconcile_get(state->db, session_id, &session, err, sizeof(err)) ||
            !cmny_reconcile_postings(state->db, session_id, 0, postings,
                                     CMNY_RECONCILE_LIST_LIMIT, &posting_count,
                                     err, sizeof(err))) {
            statusf(state, "Reconciliation failed: %s", err);
            break;
        }
        if (posting_count > 0 && selected >= posting_count) selected = posting_count - 1;
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        (void)erase();
        if (rows < 14 || columns < 40) {
            draw_resize(rows, columns);
        } else {
            const CmnyAccount *account = account_by_id(state, session.account_id);
            char title[160];
            (void)snprintf(title, sizeof(title), "RECONCILE  %s  /  statement %s",
                           account != NULL ? account->name : "Account", session.statement_on);
            draw_box(1, 1, rows - 2, columns - 2, title);
            char statement[48], cleared[48], discrepancy[48], balances[220];
            cmny_money_format(session.statement_balance_minor, statement, sizeof(statement));
            cmny_money_format(session.cleared_balance_minor, cleared, sizeof(cleared));
            cmny_money_format(session.discrepancy_minor, discrepancy, sizeof(discrepancy));
            (void)snprintf(balances, sizeof(balances),
                           "Statement %s    Cleared %s    Discrepancy %s",
                           statement, cleared, discrepancy);
            (void)attron(A_BOLD | attr_color(session.discrepancy_minor == 0
                                              ? COLOR_INCOME : COLOR_WARNING));
            put_clipped(2, 3, columns - 6, balances);
            (void)attroff(A_BOLD | attr_color(session.discrepancy_minor == 0
                                               ? COLOR_INCOME : COLOR_WARNING));
            put_clipped(3, 3, columns - 6,
                        "MARK DATE        PAYEE / ENTRY                              AMOUNT");
            int visible = rows - 8;
            size_t first = selected >= (size_t)visible ? selected - (size_t)visible + 1 : 0;
            for (int row = 0; row < visible; row++) {
                size_t item = first + (size_t)row;
                if (item >= posting_count) break;
                CmnyReconcilePosting *posting = &postings[item];
                char amount[64], line[220];
                cmny_money_format(posting->amount_minor, amount, sizeof(amount));
                const char *marker = posting->clear_state == CMNY_POSTING_RECONCILED ? "[R]" :
                                     (posting->selected ? "[x]" : "[ ]");
                (void)snprintf(line, sizeof(line), "%s  %s  %-38s %12s",
                               marker, posting->occurred_on,
                               posting->payee[0] != '\0' ? posting->payee : "(no payee)", amount);
                if (item == selected) {
                    (void)attron(selection_attr());
                    (void)mvhline(4 + row, 2, ' ', columns - 4);
                }
                put_clipped(4 + row, 3, columns - 6, line);
                if (item == selected) (void)attroff(selection_attr());
            }
            if (posting_count == 0) {
                (void)attron(A_DIM);
                put_clipped(5, 3, columns - 6, "No eligible postings through the statement date.");
                (void)attroff(A_DIM);
            }
            if (notice[0] != '\0') {
                (void)attron(A_BOLD | attr_color(COLOR_WARNING));
                put_clipped(rows - 4, 3, columns - 6, notice);
                (void)attroff(A_BOLD | attr_color(COLOR_WARNING));
            }
            (void)attron(A_DIM);
            put_clipped(rows - 3, 3, columns - 6,
                        "Space/Enter toggle cleared  f finalize  x cancel  Esc save & close");
            (void)attroff(A_DIM);
        }
        (void)refresh();
        int ch = getch();
        if (interrupted || ch == 27 || ch == 127 || ch == KEY_BACKSPACE) {
            (void)snprintf(state->status, sizeof(state->status),
                           "Reconciliation left open; resume it any time.");
            break;
        }
        if (ch == KEY_RESIZE) continue;
        if (ch == KEY_UP && selected > 0) selected--;
        else if (ch == KEY_DOWN && selected + 1 < posting_count) selected++;
        else if ((ch == ' ' || ch == '\n' || ch == '\r' || ch == KEY_ENTER) &&
                 posting_count > 0) {
            CmnyReconcilePosting *posting = &postings[selected];
            int64_t new_revision = 0;
            if (cmny_reconcile_set_cleared(state->db, session.id, session.revision,
                                           posting->posting_id, !posting->selected,
                                           &new_revision, err, sizeof(err))) {
                (void)snprintf(notice, sizeof(notice), "%s posting.",
                               posting->selected ? "Uncleared" : "Cleared");
            } else {
                (void)snprintf(notice, sizeof(notice), "Toggle failed: %.140s", err);
            }
        } else if (ch == 'f') {
            if (session.discrepancy_minor != 0) {
                (void)snprintf(notice, sizeof(notice),
                               "Cannot finalize: discrepancy must be exactly zero.");
            } else if (confirm_modal("FINALIZE RECONCILIATION",
                                     "Finalize? Selected postings become permanently reconciled.")) {
                if (cmny_reconcile_finalize(state->db, session.id, session.revision,
                                            err, sizeof(err))) {
                    refresh_state(state);
                    statusf(state, "Reconciliation finalized at zero discrepancy.");
                    break;
                }
                (void)snprintf(notice, sizeof(notice), "Finalize failed: %.135s", err);
            }
        } else if (ch == 'x' && confirm_modal("CANCEL RECONCILIATION",
                                              "Cancel and restore every prior cleared state?")) {
            if (cmny_reconcile_cancel(state->db, session.id, session.revision,
                                      err, sizeof(err))) {
                refresh_state(state);
                statusf(state, "Reconciliation cancelled; prior clear states restored.");
                break;
            }
            (void)snprintf(notice, sizeof(notice), "Cancel failed: %.140s", err);
        }
    }
    (void)timeout(250);
}

static void reconciliation_center(UiState *state) {
    size_t selected = 0;
    char notice[180] = {0};
    (void)timeout(-1);
    for (;;) {
        char err[256] = {0};
        CmnyReconcileSession sessions[CMNY_RECONCILE_LIST_LIMIT];
        size_t count = 0;
        if (!cmny_reconcile_list(state->db, false, 0, sessions,
                                 CMNY_RECONCILE_LIST_LIMIT, &count, err, sizeof(err))) {
            statusf(state, "Reconciliation failed: %s", err);
            break;
        }
        if (count > 0 && selected >= count) selected = count - 1;
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        (void)erase();
        if (rows < 14 || columns < 40) {
            draw_resize(rows, columns);
        } else {
            draw_box(1, 1, rows - 2, columns - 2, "RECONCILIATIONS");
            put_clipped(2, 3, columns - 6,
                        "OPEN SESSION                         STATEMENT      DISCREPANCY");
            int visible = rows - 7;
            for (int row = 0; row < visible && (size_t)row < count; row++) {
                const CmnyReconcileSession *session = &sessions[row];
                const CmnyAccount *account = account_by_id(state, session->account_id);
                char balance[64], line[220];
                cmny_money_format(session->discrepancy_minor, balance, sizeof(balance));
                (void)snprintf(line, sizeof(line), "%-35s %s     %12s",
                               account != NULL ? account->name : "Account",
                               session->statement_on, balance);
                if ((size_t)row == selected) {
                    (void)attron(selection_attr());
                    (void)mvhline(3 + row, 2, ' ', columns - 4);
                }
                put_clipped(3 + row, 3, columns - 6, line);
                if ((size_t)row == selected) (void)attroff(selection_attr());
            }
            if (count == 0) {
                (void)attron(A_DIM);
                put_clipped(4, 3, columns - 6,
                            "No open session. Start from a statement balance and date.");
                (void)attroff(A_DIM);
            }
            if (notice[0] != '\0') put_clipped(rows - 4, 3, columns - 6, notice);
            (void)attron(A_DIM);
            put_clipped(rows - 3, 3, columns - 6,
                        "a start  Enter resume  Esc close   Sessions survive restart");
            (void)attroff(A_DIM);
        }
        (void)refresh();
        int ch = getch();
        if (interrupted || ch == 27 || ch == 127 || ch == KEY_BACKSPACE) break;
        if (ch == KEY_RESIZE) continue;
        if (ch == KEY_UP && selected > 0) selected--;
        else if (ch == KEY_DOWN && selected + 1 < count) selected++;
        else if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && count > 0) {
            reconciliation_session(state, sessions[selected].id);
        } else if (ch == 'a') {
            int64_t account_id = 0;
            if (!choose_account(state, "ACCOUNT TO RECONCILE", 0, false, false, &account_id)) continue;
            char base_date[11], date_input[CMNY_DATE_EXPRESSION_MAX + 1];
            cmny_today(base_date);
            (void)snprintf(date_input, sizeof(date_input), "%s", base_date);
            if (!input_modal("STATEMENT DATE", "Statement end date", date_input,
                             sizeof(date_input))) continue;
            char statement_on[11];
            if (!cmny_date_expression_parse((const unsigned char *)date_input, strlen(date_input),
                                            base_date, statement_on)) {
                (void)snprintf(notice, sizeof(notice), "Invalid statement date.");
                continue;
            }
            char balance_input[CMNY_AMOUNT_EXPRESSION_MAX + 2] = "0";
            if (!input_modal("STATEMENT BALANCE", "Signed ending balance",
                             balance_input, sizeof(balance_input))) continue;
            int64_t statement_balance = 0;
            if (!parse_signed_amount(balance_input, &statement_balance)) {
                (void)snprintf(notice, sizeof(notice), "Invalid statement balance.");
                continue;
            }
            int64_t session_id = 0;
            if (cmny_reconcile_start(state->db, account_id, statement_on, statement_balance,
                                     &session_id, err, sizeof(err))) {
                reconciliation_session(state, session_id);
            } else {
                (void)snprintf(notice, sizeof(notice), "Start failed: %.140s", err);
            }
        }
    }
    (void)timeout(250);
    refresh_state(state);
}

static bool zero_amount(const char *text) {
    while (isspace((unsigned char)*text)) text++;
    bool saw_zero = false;
    bool saw_separator = false;
    while (*text != '\0' && !isspace((unsigned char)*text)) {
        if (*text == '0') saw_zero = true;
        else if ((*text == '.' || *text == ',') && !saw_separator) saw_separator = true;
        else return false;
        text++;
    }
    while (isspace((unsigned char)*text)) text++;
    return saw_zero && *text == '\0';
}

static void set_budget(UiState *state) {
    char category[CMNY_CATEGORY_MAX + 1] = {0};
    if (state->screen == SCREEN_ACTIVITY && state->transaction_count > 0) {
        (void)snprintf(category, sizeof(category), "%s",
                       state->transactions[state->selected].category);
    } else if (state->budget_count > 0) {
        (void)snprintf(category, sizeof(category), "%s", state->budgets[0].category);
    } else {
        (void)cmny_db_setting_get(state->db, "last_expense_category", category, sizeof(category));
    }
    if (!input_modal("MONTHLY BUDGET", "Category", category, sizeof(category)) ||
        !cmny_text_valid(category, CMNY_CATEGORY_MAX, false)) return;

    char amount[64] = {0};
    for (size_t i = 0; i < state->budget_count; i++) {
        if (strcmp(category, state->budgets[i].category) == 0) {
            cmny_money_format_plain(state->budgets[i].limit_cents, amount, sizeof(amount));
            break;
        }
    }
    int64_t limit = 0;
    for (;;) {
        if (!input_modal("MONTHLY BUDGET", "Monthly limit; enter 0 to remove", amount,
                         sizeof(amount))) return;
        if (zero_amount(amount)) break;
        if (cmny_money_parse(amount, &limit) && limit <= 9000000000000000LL) break;
    }
    char err[256] = {0};
    if (!cmny_db_budget_set(state->db, state->month, category, limit, err, sizeof(err))) {
        statusf(state, "Budget failed: %s", err);
        return;
    }
    refresh_state(state);
    statusf(state, limit == 0 ? "Budget removed for %s." : "Budget saved for %s.", category);
}

static void recurring_manager(UiState *state) {
    size_t selected = 0;
    char notice[180] = {0};
    bool can_save_selected = state->screen == SCREEN_ACTIVITY && state->transaction_count > 0;
    (void)timeout(-1);
    for (;;) {
        CmnyRecurring items[CMNY_RECURRING_LIMIT];
        size_t count = 0;
        char err[256] = {0};
        if (!cmny_db_recurring_list(state->db, items, CMNY_RECURRING_LIMIT,
                                    &count, err, sizeof(err))) {
            statusf(state, "Recurring error: %s", err);
            (void)timeout(250);
            return;
        }
        if (count == 0) selected = 0;
        else if (selected >= count) selected = count - 1;

        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        int width = min_int(86, columns - 4);
        int visible = max_int(1, min_int((int)count, rows - 9));
        int height = max_int(9, visible + 5);
        int y = max_int(0, (rows - height) / 2);
        int x = max_int(0, (columns - width) / 2);
        (void)erase();
        if (rows < 11 || width < 44) {
            draw_resize(rows, columns);
            put_clipped(rows - 1, 0, columns, "Resize terminal or Esc go back");
        } else {
            draw_box(y, x, height, width, "RECURRING ENTRIES");
            size_t first = 0;
            if (selected >= (size_t)visible) first = selected - (size_t)visible + 1;
            if (count == 0) {
                put_clipped(y + 2, x + 2, width - 4, "No recurring entries yet.");
                put_clipped(y + 3, x + 2, width - 4,
                            can_save_selected
                                ? "Press the Add key below to save the selected activity."
                                : "Go back to Activity, select an entry, then reopen this manager.");
            } else {
                for (int row = 0; row < visible; row++) {
                    size_t index = first + (size_t)row;
                    if (index >= count) break;
                    char amount[64], line[200];
                    cmny_money_format_plain(items[index].amount_cents, amount, sizeof(amount));
                    (void)snprintf(line, sizeof(line), "%-8s  %-20s  %12s  day %d",
                                   items[index].kind == CMNY_INCOME ? "Income" : "Expense",
                                   items[index].category, amount, items[index].day_of_month);
                    if (index == selected) {
                        (void)attron(selection_attr());
                        (void)mvhline(y + 2 + row, x + 1, ' ', width - 2);
                    }
                    put_clipped(y + 2 + row, x + 2, width - 4, line);
                    if (index == selected) (void)attroff(selection_attr());
                }
            }
            char hints[180];
            (void)snprintf(hints, sizeof(hints),
                           "Up/Down move  Enter add to month  %c save selected  %c delete  Esc back",
                           state->bindings[ACTION_ADD], state->bindings[ACTION_DELETE]);
            (void)attron(A_DIM);
            put_clipped(y + height - 2, x + 2, width - 4,
                        notice[0] != '\0' ? notice : hints);
            (void)attroff(A_DIM);
        }
        (void)refresh();
        (void)timeout(-1);
        int ch = getch();
        if (interrupted || ch == 27 || ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            (void)timeout(250);
            return;
        }
        if (ch == KEY_RESIZE) continue;
        notice[0] = '\0';
        if (ch == KEY_UP && selected > 0) {
            selected--;
        } else if (ch == KEY_DOWN && selected + 1 < count) {
            selected++;
        } else if (action_pressed(state, ACTION_ADD, ch)) {
            if (!can_save_selected) {
                (void)snprintf(notice, sizeof(notice),
                               "Go back to Activity and select an entry first.");
            } else if (cmny_db_recurring_add(state->db,
                                              &state->transactions[state->selected],
                                              err, sizeof(err))) {
                (void)snprintf(notice, sizeof(notice), "Recurring entry saved.");
            } else {
                (void)snprintf(notice, sizeof(notice), "Save failed: %s", err);
            }
        } else if (action_pressed(state, ACTION_DELETE, ch) && count > 0) {
            char question[160];
            (void)snprintf(question, sizeof(question), "Delete recurring entry %s?",
                           items[selected].category);
            if (confirm_modal("DELETE RECURRING", question)) {
                if (cmny_db_recurring_delete(state->db, items[selected].id,
                                             err, sizeof(err))) {
                    (void)snprintf(notice, sizeof(notice), "Recurring entry deleted.");
                } else {
                    (void)snprintf(notice, sizeof(notice), "Delete failed: %s", err);
                }
            }
        } else if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && count > 0) {
            CmnyTransaction tx = {0};
            tx.kind = items[selected].kind;
            tx.amount_cents = items[selected].amount_cents;
            (void)snprintf(tx.category, sizeof(tx.category), "%s", items[selected].category);
            (void)snprintf(tx.note, sizeof(tx.note), "%s", items[selected].note);
            if (!cmny_date_for_month_day(state->month, items[selected].day_of_month,
                                         tx.occurred_on)) {
                (void)snprintf(notice, sizeof(notice), "Could not create a date for this month.");
            } else if (!cmny_db_add(state->db, &tx, NULL, err, sizeof(err))) {
                (void)snprintf(notice, sizeof(notice), "Add failed: %s", err);
            } else {
                refresh_state(state);
                (void)snprintf(notice, sizeof(notice), "%s added on %s.",
                               tx.category, tx.occurred_on);
            }
        }
    }
}

static void create_backup(UiState *state) {
    if (state->options->demo) {
        statusf(state, "Demo data does not need a backup.");
        return;
    }
    char path[4200];
    int written = snprintf(path, sizeof(path), "%s.backup-%lld", state->db_path,
                           (long long)time(NULL));
    if (written < 0 || (size_t)written >= sizeof(path)) {
        statusf(state, "Backup path is too long.");
        return;
    }
    char err[256] = {0};
    if (cmny_db_backup(state->db, path, err, sizeof(err))) {
        statusf(state, "Backup created: %s", path);
    } else {
        statusf(state, "Backup failed: %s", err);
    }
}

static void check_integrity(UiState *state) {
    char err[256] = {0};
    if (cmny_db_check(state->db, err, sizeof(err))) {
        statusf(state, "Ledger integrity check passed.");
    } else {
        statusf(state, "Integrity check failed: %s", err);
    }
}

static bool save_preference(UiState *state, const char *key, const char *value) {
    char err[256] = {0};
    if (cmny_db_setting_set(state->db, key, value, err, sizeof(err))) return true;
    statusf(state, "Could not save setting: %s", err);
    return false;
}

static void close_tutorial(UiState *state, bool completed) {
    state->tutorial_pending = false;
    if (!state->options->demo && !save_preference(state, "tutorial_seen", "1")) return;
    statusf(state, completed
        ? "Tutorial complete. Press ? for the key guide or replay it from Manage."
        : "Tutorial skipped. Replay it any time from Manage.");
}

static void show_tutorial(UiState *state) {
    char pages[5][4][160];
    const char *titles[] = {
        "FIVE SPACES", "ADD MONEY", "SEARCH", "SAFETY", "DEMO"
    };
    (void)snprintf(pages[0][0], sizeof(pages[0][0]),
                   "Five spaces keep the workflow predictable.");
    (void)snprintf(pages[0][1], sizeof(pages[0][1]),
                   "1 Overview   2 Activity   3 Plan");
    (void)snprintf(pages[0][2], sizeof(pages[0][2]),
                   "4 Insights   5 Manage");
    (void)snprintf(pages[0][3], sizeof(pages[0][3]),
                   "Tab / Shift-Tab moves; Esc goes back.");
    (void)snprintf(pages[1][0], sizeof(pages[1][0]),
                   "%c adds an expense or income.", state->bindings[ACTION_ADD]);
    (void)snprintf(pages[1][1], sizeof(pages[1][1]),
                   "Enter amount, date, category, and note.");
    (void)snprintf(pages[1][2], sizeof(pages[1][2]),
                   "Activity: arrows select; Enter opens.");
    (void)snprintf(pages[1][3], sizeof(pages[1][3]),
                   "%c edits, %c deletes, %c restores.", state->bindings[ACTION_EDIT],
                   state->bindings[ACTION_DELETE], state->bindings[ACTION_UNDO]);
    (void)snprintf(pages[2][0], sizeof(pages[2][0]),
                   "%c searches; %c filters; %c clears.", state->bindings[ACTION_SEARCH],
                   state->bindings[ACTION_FILTER], state->bindings[ACTION_CLEAR]);
    (void)snprintf(pages[2][1], sizeof(pages[2][1]),
                   ": opens the searchable command palette.");
    (void)snprintf(pages[2][2], sizeof(pages[2][2]),
                   "? opens the categorized key guide.");
    (void)snprintf(pages[2][3], sizeof(pages[2][3]),
                   "[ and ] change month; %c returns today.", state->bindings[ACTION_TODAY]);
    (void)snprintf(pages[3][0], sizeof(pages[3][0]),
                   "Changes save automatically to this ledger.");
    (void)snprintf(pages[3][1], sizeof(pages[3][1]),
                   "Deletes always ask for confirmation.");
    (void)snprintf(pages[3][2], sizeof(pages[3][2]),
                   "Manage offers integrity checks and backups.");
    (void)snprintf(pages[3][3], sizeof(pages[3][3]),
                   "A backup creates a separate recovery copy.");
    (void)snprintf(pages[4][0], sizeof(pages[4][0]),
                   "--demo is a disposable sandbox.");
    (void)snprintf(pages[4][1], sizeof(pages[4][1]),
                   "Demo entries and settings vanish on exit.");
    (void)snprintf(pages[4][2], sizeof(pages[4][2]),
                   "Explore freely without touching your ledger.");
    (void)snprintf(pages[4][3], sizeof(pages[4][3]),
                   "Replay this tutorial any time from Manage.");

    size_t page = 0;
    (void)timeout(-1);
    (void)curs_set(0);
    for (;;) {
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        int width = min_int(78, columns - 2);
        int height = min_int(10, rows - 2);
        int y = max_int(0, (rows - height) / 2);
        int x = max_int(0, (columns - width) / 2);
        (void)erase();
        if (rows < 12 || width < 36) {
            draw_resize(rows, columns);
            put_clipped(rows - 1, 0, columns, "Resize, s skip tutorial, or q quit");
        } else {
            char title[96];
            (void)snprintf(title, sizeof(title), "GETTING STARTED  %zu/5  /  %s",
                           page + 1, titles[page]);
            draw_box(y, x, height, width, title);
            for (int line = 0; line < 4; line++) {
                put_clipped(y + 2 + line, x + 2, width - 4, pages[page][line]);
            }
            const char *footer = page + 1 == 5
                ? "Left back  Enter finish  s/Esc skip  q quit"
                : "Left back  Enter/Right next  s/Esc skip  q quit";
            (void)attron(A_DIM);
            put_clipped(y + height - 2, x + 2, width - 4, footer);
            (void)attroff(A_DIM);
        }
        (void)refresh();
        int ch = getch();
        if (interrupted) {
            (void)timeout(250);
            return;
        }
        if (ch == KEY_RESIZE) continue;
        if (ch == 'q' || ch == 'Q') {
            state->running = false;
            (void)timeout(250);
            return;
        }
        if (ch == 's' || ch == 'S' || ch == 27 || ch == KEY_BACKSPACE ||
            ch == 127 || ch == 8) {
            close_tutorial(state, false);
            (void)timeout(250);
            return;
        }
        if (ch == KEY_LEFT && page > 0) {
            page--;
        } else if (ch == KEY_RIGHT || ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (page + 1 == 5) {
                close_tutorial(state, true);
                (void)timeout(250);
                return;
            }
            page++;
        }
    }
}

static void change_theme(UiState *state, int direction) {
    int next_index = ((int)state->theme + direction + (int)CMNY_THEME_COUNT) %
                     (int)CMNY_THEME_COUNT;
    CmnyTheme next = (CmnyTheme)next_index;
    if (colors_active && !apply_theme(next, theme_background)) {
        statusf(state, "This terminal could not activate the %s theme.", cmny_theme_name(next));
        return;
    }
    if (save_preference(state, "theme", cmny_theme_name(next))) {
        state->theme = next;
        statusf(state, "Theme saved: %s.", cmny_theme_name(next));
    }
}

static void change_start_screen(UiState *state, int direction) {
    int count = SCREEN_COUNT;
    int next = ((int)state->start_screen + direction + count) % count;
    Screen screen = (Screen)next;
    if (save_preference(state, "start_screen", screen_name(screen))) {
        state->start_screen = screen;
        statusf(state, "Start screen saved: %s.", screen_label(screen));
    }
}

static void change_mouse_mode(UiState *state, int direction) {
    int next_index = ((int)state->mouse_mode + direction + (int)UI_MOUSE_MODE_COUNT) %
                     (int)UI_MOUSE_MODE_COUNT;
    UiMouseMode next = (UiMouseMode)next_index;
    if (!save_preference(state, "mouse", mouse_mode_name(next))) return;
    state->mouse_mode = next;
    bool active = configure_mouse(state);
    if (next == UI_MOUSE_OFF) {
        statusf(state, "Mouse input is off; keyboard controls remain available.");
    } else if (active) {
        statusf(state, "Mouse %s and active. Shift-drag selects terminal text.",
                mouse_mode_name(next));
    } else {
        statusf(state, "Mouse %s saved, but this terminal did not provide click support.",
                mouse_mode_name(next));
    }
}

static void reset_keybindings(UiState *state) {
    if (!confirm_modal("RESET KEYBINDINGS", "Restore every keybinding to its default?")) return;
    for (int i = 0; i < ACTION_COUNT; i++) {
        char value[2] = {binding_definitions[i].default_key, '\0'};
        if (!save_preference(state, binding_definitions[i].setting, value)) return;
        state->bindings[i] = binding_definitions[i].default_key;
    }
    statusf(state, "Keybindings restored to defaults.");
}

static void handle_settings(UiState *state, int ch) {
    if (ch == KEY_UP) {
        if (state->settings_selected > 0) state->settings_selected--;
        state->status[0] = '\0';
        return;
    }
    if (ch == KEY_DOWN) {
        if (state->settings_selected + 1 < SETTINGS_ITEM_COUNT) state->settings_selected++;
        state->status[0] = '\0';
        return;
    }
    if (ch == KEY_HOME) {
        state->settings_selected = 0;
        state->status[0] = '\0';
        return;
    }
    if (ch == KEY_END) {
        state->settings_selected = SETTINGS_ITEM_COUNT - 1;
        state->status[0] = '\0';
        return;
    }
    bool enter = ch == '\n' || ch == '\r' || ch == KEY_ENTER;
    if (!enter && ch != KEY_LEFT && ch != KEY_RIGHT) return;
    int direction = ch == KEY_LEFT ? -1 : 1;
    if (state->settings_selected == SETTINGS_THEME) {
        change_theme(state, direction);
    } else if (state->settings_selected == SETTINGS_START_SCREEN) {
        change_start_screen(state, direction);
    } else if (state->settings_selected == SETTINGS_MOUSE) {
        change_mouse_mode(state, direction);
    } else if (state->settings_selected >= SETTINGS_BINDING_FIRST &&
               state->settings_selected < SETTINGS_ACCOUNTS) {
        if (!enter) return;
        UiAction action = (UiAction)(state->settings_selected - SETTINGS_BINDING_FIRST);
        char key;
        if (keybinding_modal(state, action, &key)) {
            char value[2] = {key, '\0'};
            if (save_preference(state, binding_definitions[action].setting, value)) {
                state->bindings[action] = key;
                statusf(state, "%s key saved as %c.", binding_definitions[action].label, key);
            }
        }
    } else if (state->settings_selected == SETTINGS_ACCOUNTS) {
        if (enter) account_manager(state);
    } else if (state->settings_selected == SETTINGS_RECONCILE) {
        if (enter) reconciliation_center(state);
    } else if (state->settings_selected == SETTINGS_INTEGRITY) {
        if (enter) check_integrity(state);
    } else if (state->settings_selected == SETTINGS_TUTORIAL) {
        if (enter) show_tutorial(state);
    } else if (state->settings_selected == SETTINGS_BACKUP) {
        if (enter) create_backup(state);
    } else if (enter) {
        reset_keybindings(state);
    }
}

static void undo_delete(UiState *state) {
    if (!state->can_undo) {
        statusf(state, "Nothing to undo.");
        return;
    }
    char err[256] = {0};
    int64_t action_id = 0;
    int64_t new_revision = 0;
    if (!cmny_history_undo_latest(state->db, &action_id, &new_revision, err, sizeof(err))) {
        statusf(state, "Undo failed: %s", err);
        return;
    }
    refresh_state(state);
    statusf(state, "Undid latest ledger change (history #%lld).", (long long)action_id);
}

static void handle_activity(UiState *state, int ch) {
    if (ch == KEY_UP) {
        if (state->selected > 0) {
            state->selected--;
        } else if (state->activity_offset > 0) {
            state->activity_offset = state->activity_offset >= CMNY_TX_LIMIT
                                         ? state->activity_offset - CMNY_TX_LIMIT : 0;
            refresh_state(state);
            if (state->transaction_count > 0) state->selected = state->transaction_count - 1;
        }
    } else if (ch == KEY_DOWN) {
        if (state->selected + 1 < state->transaction_count) {
            state->selected++;
        } else if (state->activity_offset + state->transaction_count < state->activity_total) {
            state->activity_offset += state->transaction_count;
            state->selected = 0;
            refresh_state(state);
        }
    } else if (ch == KEY_HOME) {
        state->activity_offset = 0;
        state->selected = 0;
        refresh_state(state);
    } else if (ch == KEY_END && state->activity_total > 0) {
        state->activity_offset = ((state->activity_total - 1) / CMNY_TX_LIMIT) * CMNY_TX_LIMIT;
        refresh_state(state);
        if (state->transaction_count > 0) state->selected = state->transaction_count - 1;
    } else if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && state->transaction_count > 0) {
        show_detail(state, &state->transactions[state->selected]);
    } else if (action_pressed(state, ACTION_EDIT, ch) && state->transaction_count > 0) {
        CmnyTransaction selected = state->transactions[state->selected];
        edit_transaction(state, &selected);
    } else if (action_pressed(state, ACTION_DELETE, ch) && state->transaction_count > 0) {
        CmnyTransaction selected = state->transactions[state->selected];
        UiEntryMeta meta = state->transaction_meta[state->selected];
        char question[160];
        (void)snprintf(question, sizeof(question), "Delete %s on %s?", selected.category, selected.occurred_on);
        if (confirm_modal("DELETE TRANSACTION", question)) {
            char err[256] = {0};
            if (cmny_entry_delete(state->db, selected.id, meta.revision, err, sizeof(err))) {
                refresh_state(state);
                statusf(state, "Entry deleted. Press %c to undo the latest change.",
                        state->bindings[ACTION_UNDO]);
            } else {
                statusf(state, "Delete failed: %s", err);
            }
        }
    }
}

static void replace_screen(UiState *state, Screen screen) {
    state->screen = screen;
    state->status[0] = '\0';
}

static void navigate_to(UiState *state, Screen screen) {
    if (screen < SCREEN_OVERVIEW || screen >= SCREEN_COUNT || screen == state->screen) return;
    if (state->navigation_depth == NAV_HISTORY_LIMIT) {
        memmove(state->navigation_history, state->navigation_history + 1,
                (NAV_HISTORY_LIMIT - 1) * sizeof(state->navigation_history[0]));
        state->navigation_depth--;
    }
    state->navigation_history[state->navigation_depth++] = state->screen;
    replace_screen(state, screen);
}

static void navigate_back(UiState *state) {
    if (state->navigation_depth > 0) {
        replace_screen(state, state->navigation_history[--state->navigation_depth]);
    } else if (state->screen != SCREEN_OVERVIEW) {
        replace_screen(state, SCREEN_OVERVIEW);
    } else {
        state->status[0] = '\0';
    }
}

static void search_activity(UiState *state) {
    char search[sizeof(state->search)];
    (void)snprintf(search, sizeof(search), "%s", state->search);
    if (!input_modal("SEARCH", "Search category or note across every month", search,
                     sizeof(search))) return;
    (void)snprintf(state->search, sizeof(state->search), "%s", search);
    state->selected = 0;
    state->activity_offset = 0;
    navigate_to(state, SCREEN_ACTIVITY);
    refresh_state(state);
}

static void cycle_filter(UiState *state) {
    state->kind_filter = state->kind_filter == 0 ? CMNY_EXPENSE :
                         state->kind_filter == CMNY_EXPENSE ? CMNY_INCOME : 0;
    state->selected = 0;
    state->activity_offset = 0;
    navigate_to(state, SCREEN_ACTIVITY);
    refresh_state(state);
}

static void filter_account(UiState *state) {
    int64_t account_id = state->account_filter_id;
    if (!choose_account(state, "FILTER ACTIVITY BY ACCOUNT", 0, true, false, &account_id)) return;
    state->account_filter_id = account_id;
    state->selected = 0;
    state->activity_offset = 0;
    navigate_to(state, SCREEN_ACTIVITY);
    refresh_state(state);
    const CmnyAccount *account = account_by_id(state, account_id);
    if (account != NULL) statusf(state, "Activity filtered to account: %s.", account->name);
    else statusf(state, "Showing all accounts.");
}

static void clear_activity_filter(UiState *state) {
    state->search[0] = '\0';
    state->kind_filter = 0;
    state->account_filter_id = 0;
    state->selected = 0;
    state->activity_offset = 0;
    navigate_to(state, SCREEN_ACTIVITY);
    refresh_state(state);
}

static void shift_month(UiState *state, int direction) {
    char shifted[8];
    if (!cmny_month_shift(state->month, direction, shifted)) return;
    (void)snprintf(state->month, sizeof(state->month), "%s", shifted);
    state->selected = 0;
    state->activity_offset = 0;
    refresh_state(state);
}

static const char *palette_label(PaletteCommand command) {
    static const char *labels[COMMAND_COUNT] = {
        [COMMAND_OVERVIEW] = "Go to Overview",
        [COMMAND_ACTIVITY] = "Go to Activity",
        [COMMAND_PLAN] = "Go to Plan",
        [COMMAND_INSIGHTS] = "Go to Insights / reports",
        [COMMAND_MANAGE] = "Go to Manage / settings",
        [COMMAND_BACK] = "Back one level",
        [COMMAND_ADD] = "Add transaction",
        [COMMAND_VIEW] = "View selected transaction",
        [COMMAND_EDIT] = "Edit selected transaction",
        [COMMAND_DELETE] = "Delete selected transaction",
        [COMMAND_UNDO] = "Undo latest ledger change",
        [COMMAND_SEARCH] = "Search activity",
        [COMMAND_FILTER] = "Change activity filter",
        [COMMAND_CLEAR] = "Clear activity search and filter",
        [COMMAND_BUDGET] = "Set monthly budget",
        [COMMAND_RECURRING] = "Open recurring entries",
        [COMMAND_TRANSFER] = "Create account transfer",
        [COMMAND_ACCOUNT_FILTER] = "Filter activity by account",
        [COMMAND_ACCOUNTS] = "Manage accounts",
        [COMMAND_RECONCILE] = "Reconcile a statement",
        [COMMAND_TODAY] = "Jump to current month",
        [COMMAND_PREVIOUS_MONTH] = "Previous month",
        [COMMAND_NEXT_MONTH] = "Next month",
        [COMMAND_INTEGRITY] = "Check ledger integrity",
        [COMMAND_BACKUP] = "Create backup now",
        [COMMAND_THEME_SETTINGS] = "Open theme setting",
        [COMMAND_START_SETTINGS] = "Open start-screen setting",
        [COMMAND_RESET_KEYS] = "Reset keybindings",
        [COMMAND_TUTORIAL] = "Replay keyboard tutorial",
        [COMMAND_HELP] = "Open keybinding guide",
        [COMMAND_QUIT] = "Quit CMNY"
    };
    return command >= 0 && command < COMMAND_COUNT ? labels[command] : "Unknown command";
}

static const char *palette_keywords(PaletteCommand command) {
    static const char *keywords[COMMAND_COUNT] = {
        [COMMAND_OVERVIEW] = "home dashboard",
        [COMMAND_ACTIVITY] = "entries list transactions",
        [COMMAND_PLAN] = "planning budgets recurring",
        [COMMAND_INSIGHTS] = "reports stats trends categories",
        [COMMAND_MANAGE] = "settings setup data",
        [COMMAND_BACK] = "escape previous",
        [COMMAND_ADD] = "new expense income",
        [COMMAND_VIEW] = "detail open",
        [COMMAND_EDIT] = "change",
        [COMMAND_DELETE] = "remove",
        [COMMAND_UNDO] = "restore",
        [COMMAND_SEARCH] = "find",
        [COMMAND_FILTER] = "income expenses",
        [COMMAND_CLEAR] = "reset filter",
        [COMMAND_BUDGET] = "plan limit",
        [COMMAND_RECURRING] = "repeat template",
        [COMMAND_TRANSFER] = "move money accounts",
        [COMMAND_ACCOUNT_FILTER] = "account context narrow",
        [COMMAND_ACCOUNTS] = "cash checking savings credit archive balance",
        [COMMAND_RECONCILE] = "statement cleared discrepancy",
        [COMMAND_TODAY] = "now",
        [COMMAND_PREVIOUS_MONTH] = "period back",
        [COMMAND_NEXT_MONTH] = "period forward",
        [COMMAND_INTEGRITY] = "check health database",
        [COMMAND_BACKUP] = "data safety copy",
        [COMMAND_THEME_SETTINGS] = "appearance color",
        [COMMAND_START_SETTINGS] = "startup default",
        [COMMAND_RESET_KEYS] = "controls shortcuts",
        [COMMAND_TUTORIAL] = "onboarding welcome learn getting started",
        [COMMAND_HELP] = "keys tutorial",
        [COMMAND_QUIT] = "exit"
    };
    return command >= 0 && command < COMMAND_COUNT ? keywords[command] : "";
}

static void palette_shortcut(const UiState *state, PaletteCommand command,
                             char *out, size_t out_size) {
    const char *fixed = "";
    switch (command) {
        case COMMAND_OVERVIEW: fixed = "1"; break;
        case COMMAND_ACTIVITY: fixed = "2"; break;
        case COMMAND_PLAN: fixed = "3"; break;
        case COMMAND_INSIGHTS: fixed = "4"; break;
        case COMMAND_MANAGE: fixed = "5"; break;
        case COMMAND_BACK: fixed = "Esc"; break;
        case COMMAND_VIEW: fixed = "Enter"; break;
        case COMMAND_PREVIOUS_MONTH: fixed = "["; break;
        case COMMAND_NEXT_MONTH: fixed = "]"; break;
        case COMMAND_HELP: fixed = "?"; break;
        case COMMAND_QUIT: fixed = "q"; break;
        default: break;
    }
    if (*fixed != '\0') {
        (void)snprintf(out, out_size, "%s", fixed);
        return;
    }
    UiAction action = ACTION_COUNT;
    switch (command) {
        case COMMAND_ADD: action = ACTION_ADD; break;
        case COMMAND_EDIT: action = ACTION_EDIT; break;
        case COMMAND_DELETE: action = ACTION_DELETE; break;
        case COMMAND_UNDO: action = ACTION_UNDO; break;
        case COMMAND_SEARCH: action = ACTION_SEARCH; break;
        case COMMAND_FILTER: action = ACTION_FILTER; break;
        case COMMAND_CLEAR: action = ACTION_CLEAR; break;
        case COMMAND_BUDGET: action = ACTION_BUDGET; break;
        case COMMAND_RECURRING: action = ACTION_RECURRING; break;
        case COMMAND_TRANSFER: action = ACTION_TRANSFER; break;
        case COMMAND_ACCOUNT_FILTER: action = ACTION_ACCOUNT_FILTER; break;
        case COMMAND_ACCOUNTS: action = ACTION_ACCOUNTS; break;
        case COMMAND_RECONCILE: action = ACTION_RECONCILE; break;
        case COMMAND_TODAY: action = ACTION_TODAY; break;
        default: break;
    }
    if (action != ACTION_COUNT) (void)snprintf(out, out_size, "%c", state->bindings[action]);
    else if (out_size > 0) out[0] = '\0';
}

static const char *palette_disabled_reason(const UiState *state, PaletteCommand command) {
    if ((command == COMMAND_VIEW || command == COMMAND_EDIT || command == COMMAND_DELETE) &&
        (state->screen != SCREEN_ACTIVITY || state->transaction_count == 0)) {
        return "Open Activity and select a transaction first.";
    }
    if (command == COMMAND_UNDO && !state->can_undo) return "Nothing to undo.";
    if (command == COMMAND_CLEAR && state->search[0] == '\0' && state->kind_filter == 0 &&
        state->account_filter_id == 0) {
        return "No activity search or filter is active.";
    }
    if (command == COMMAND_BACKUP && state->options->demo) {
        return "Demo data is disposable and cannot be backed up.";
    }
    if (command == COMMAND_BACK && state->navigation_depth == 0 &&
        state->screen == SCREEN_OVERVIEW) {
        return "Already at the top level.";
    }
    return NULL;
}

static bool contains_case_insensitive(const char *text, const char *query) {
    if (query[0] == '\0') return true;
    for (const char *start = text; *start != '\0'; start++) {
        const char *left = start;
        const char *right = query;
        while (*left != '\0' && *right != '\0' &&
               tolower((unsigned char)*left) == tolower((unsigned char)*right)) {
            left++;
            right++;
        }
        if (*right == '\0') return true;
    }
    return false;
}

static size_t palette_matches(const char *query, PaletteCommand out[COMMAND_COUNT]) {
    size_t count = 0;
    for (int command = 0; command < COMMAND_COUNT; command++) {
        PaletteCommand item = (PaletteCommand)command;
        if (contains_case_insensitive(palette_label(item), query) ||
            contains_case_insensitive(palette_keywords(item), query)) {
            out[count++] = item;
        }
    }
    return count;
}

static bool navigation_target_at(int columns, int mouse_x, Screen current, Screen *target) {
    if (target == NULL) return false;
    if (columns < 58) {
        char compact[80];
        (void)snprintf(compact, sizeof(compact), "< [%d/%d] %s >  Tab screens",
                       (int)current + 1, (int)SCREEN_COUNT, screen_label(current));
        const char *next = strchr(compact, '>');
        int next_x = next != NULL ? 2 + (int)(next - compact) : columns - 3;
        if (mouse_x >= 1 && mouse_x <= 4) {
            *target = (Screen)(((int)current + SCREEN_COUNT - 1) % SCREEN_COUNT);
            return true;
        }
        if (mouse_x >= next_x - 1 && mouse_x <= next_x + 2) {
            *target = (Screen)(((int)current + 1) % SCREEN_COUNT);
            return true;
        }
        if (mouse_x > 4 && mouse_x < next_x - 1) {
            *target = current;
            return true;
        }
        return false;
    }

    const char *wide_tabs[] = {
        "[1] Overview", "[2] Activity", "[3] Plan", "[4] Insights", "[5] Manage"
    };
    const char *compact_tabs[] = {"1 Home", "2 Activity", "3 Plan", "4 Insights", "5 Manage"};
    const char *const *tabs = columns < 96 ? compact_tabs : wide_tabs;
    int tab_x = 2;
    for (int i = 0; i < SCREEN_COUNT; i++) {
        int end = tab_x + (int)strlen(tabs[i]);
        if (mouse_x >= tab_x && mouse_x < end) {
            *target = (Screen)i;
            return true;
        }
        tab_x = end + (columns < 96 ? 2 : 3);
    }
    return false;
}

#if defined(KEY_MOUSE) && defined(BUTTON1_CLICKED)
static bool mouse_is_click(mmask_t state) {
    return (state & BUTTON1_CLICKED) != 0
#ifdef BUTTON1_DOUBLE_CLICKED
        || (state & BUTTON1_DOUBLE_CLICKED) != 0
#endif
        ;
}

static bool mouse_is_double_click(mmask_t state) {
#ifdef BUTTON1_DOUBLE_CLICKED
    return (state & BUTTON1_DOUBLE_CLICKED) != 0;
#else
    (void)state;
    return false;
#endif
}

static bool mouse_wheel_up(mmask_t state) {
#ifdef BUTTON4_PRESSED
    return (state & BUTTON4_PRESSED) != 0;
#else
    (void)state;
    return false;
#endif
}

static bool mouse_wheel_down(mmask_t state) {
#ifdef BUTTON5_PRESSED
    return (state & BUTTON5_PRESSED) != 0;
#else
    (void)state;
    return false;
#endif
}
#endif

static void execute_palette_command(UiState *state, PaletteCommand command) {
    switch (command) {
        case COMMAND_OVERVIEW: navigate_to(state, SCREEN_OVERVIEW); break;
        case COMMAND_ACTIVITY: navigate_to(state, SCREEN_ACTIVITY); break;
        case COMMAND_PLAN: navigate_to(state, SCREEN_PLAN); break;
        case COMMAND_INSIGHTS: navigate_to(state, SCREEN_INSIGHTS); break;
        case COMMAND_MANAGE: navigate_to(state, SCREEN_MANAGE); break;
        case COMMAND_BACK: navigate_back(state); break;
        case COMMAND_ADD: edit_transaction(state, NULL); break;
        case COMMAND_VIEW:
            show_detail(state, &state->transactions[state->selected]);
            break;
        case COMMAND_EDIT: {
            CmnyTransaction selected = state->transactions[state->selected];
            edit_transaction(state, &selected);
            break;
        }
        case COMMAND_DELETE:
            handle_activity(state, state->bindings[ACTION_DELETE]);
            break;
        case COMMAND_UNDO: undo_delete(state); break;
        case COMMAND_SEARCH: search_activity(state); break;
        case COMMAND_FILTER: cycle_filter(state); break;
        case COMMAND_CLEAR: clear_activity_filter(state); break;
        case COMMAND_BUDGET: set_budget(state); break;
        case COMMAND_RECURRING:
            recurring_manager(state);
            refresh_state(state);
            break;
        case COMMAND_TRANSFER: create_transfer(state); break;
        case COMMAND_ACCOUNT_FILTER: filter_account(state); break;
        case COMMAND_ACCOUNTS: account_manager(state); break;
        case COMMAND_RECONCILE: reconciliation_center(state); break;
        case COMMAND_TODAY:
            cmny_current_month(state->month);
            state->selected = 0;
            state->activity_offset = 0;
            refresh_state(state);
            break;
        case COMMAND_PREVIOUS_MONTH: shift_month(state, -1); break;
        case COMMAND_NEXT_MONTH: shift_month(state, 1); break;
        case COMMAND_INTEGRITY: check_integrity(state); break;
        case COMMAND_BACKUP: create_backup(state); break;
        case COMMAND_THEME_SETTINGS:
            navigate_to(state, SCREEN_MANAGE);
            state->settings_selected = SETTINGS_THEME;
            break;
        case COMMAND_START_SETTINGS:
            navigate_to(state, SCREEN_MANAGE);
            state->settings_selected = SETTINGS_START_SCREEN;
            break;
        case COMMAND_RESET_KEYS: reset_keybindings(state); break;
        case COMMAND_TUTORIAL: show_tutorial(state); break;
        case COMMAND_HELP: show_help(state); break;
        case COMMAND_QUIT: state->running = false; break;
        case COMMAND_COUNT: break;
    }
}

static void command_palette(UiState *state) {
    char query[64] = {0};
    size_t query_length = 0;
    size_t selected = 0;
    (void)timeout(-1);
    (void)curs_set(1);
    for (;;) {
        int rows, columns;
        getmaxyx(stdscr, rows, columns);
        if (rows < 14 || columns < 40) {
            (void)curs_set(0);
            (void)erase();
            draw_resize(rows, columns);
            put_clipped(rows - 1, 0, columns, "Resize terminal or Esc cancel commands");
            (void)refresh();
            int small_ch = getch();
            if (interrupted || small_ch == 27) {
                (void)timeout(250);
                return;
            }
            continue;
        }

        PaletteCommand matches[COMMAND_COUNT];
        size_t match_count = palette_matches(query, matches);
        if (match_count == 0) selected = 0;
        else if (selected >= match_count) selected = match_count - 1;

        int width = columns < 84 ? columns - 2 : 80;
        int height = rows < 22 ? rows - 2 : 20;
        int x = (columns - width) / 2;
        int y = (rows - height) / 2;
        int list_height = max_int(1, height - 5);
        size_t first = 0;
        if (selected >= (size_t)list_height) first = selected - (size_t)list_height + 1;

        (void)erase();
        draw_box(y, x, height, width, "COMMAND PALETTE");
        (void)attron(A_REVERSE);
        (void)mvhline(y + 1, x + 2, ' ', width - 4);
        char search_line[80];
        (void)snprintf(search_line, sizeof(search_line), ": %s", query);
        put_clipped(y + 1, x + 2, width - 4, search_line);
        (void)attroff(A_REVERSE);

        if (match_count == 0) {
            (void)attron(A_DIM);
            put_clipped(y + 3, x + 2, width - 4, "No commands match. Backspace edits; Esc cancels.");
            (void)attroff(A_DIM);
        } else {
            for (int row = 0; row < list_height; row++) {
                size_t match_index = first + (size_t)row;
                if (match_index >= match_count) break;
                PaletteCommand command = matches[match_index];
                const char *reason = palette_disabled_reason(state, command);
                char shortcut[16];
                palette_shortcut(state, command, shortcut, sizeof(shortcut));
                int draw_y = y + 3 + row;
                bool active = match_index == selected;
                if (active) {
                    (void)attron(selection_attr());
                    (void)mvhline(draw_y, x + 1, ' ', width - 2);
                } else if (reason != NULL) {
                    (void)attron(A_DIM | attr_color(COLOR_MUTED));
                }
                char line[180];
                (void)snprintf(line, sizeof(line), "%s%s", reason != NULL ? "- " : "  ",
                               palette_label(command));
                put_clipped(draw_y, x + 2, width - 4, line);
                if (shortcut[0] != '\0') {
                    int shortcut_x = x + width - (int)strlen(shortcut) - 3;
                    if (shortcut_x > x + 12) {
                        put_clipped(draw_y, shortcut_x, (int)strlen(shortcut), shortcut);
                    }
                }
                if (active) (void)attroff(selection_attr());
                else if (reason != NULL) (void)attroff(A_DIM | attr_color(COLOR_MUTED));
            }
        }

        (void)attron(A_DIM);
        const char *footer = "Type to search  Up/Down move  Enter run  Esc cancel";
        if (match_count > 0) {
            const char *reason = palette_disabled_reason(state, matches[selected]);
            if (reason != NULL) footer = reason;
        }
        put_clipped(y + height - 2, x + 2, width - 4, footer);
        (void)attroff(A_DIM);
        int cursor_x = x + 4 + min_int((int)query_length, width - 7);
        (void)move(y + 1, cursor_x);
        (void)refresh();

        int ch = getch();
        if (interrupted || ch == 27) {
            (void)curs_set(0);
            (void)timeout(250);
            return;
        }
        if (ch == KEY_RESIZE) continue;
#if defined(KEY_MOUSE) && defined(BUTTON1_CLICKED)
        if (ch == KEY_MOUSE && state->mouse_active) {
            MEVENT event;
            if (getmouse(&event) != OK) continue;
            if (mouse_wheel_up(event.bstate) && selected > 0) {
                selected--;
            } else if (mouse_wheel_down(event.bstate) && selected + 1 < match_count) {
                selected++;
            } else if (mouse_is_click(event.bstate) &&
                       event.x >= x + 1 && event.x < x + width - 1 &&
                       event.y >= y + 3 && event.y < y + 3 + list_height) {
                size_t clicked = first + (size_t)(event.y - (y + 3));
                if (clicked < match_count) {
                    selected = clicked;
                    PaletteCommand command = matches[selected];
                    if (mouse_is_double_click(event.bstate) &&
                        palette_disabled_reason(state, command) == NULL) {
                        (void)curs_set(0);
                        (void)timeout(250);
                        execute_palette_command(state, command);
                        return;
                    }
                }
            }
            continue;
        }
#endif
        if (ch == KEY_UP && selected > 0) {
            selected--;
        } else if ((ch == KEY_DOWN || ch == '\t') && selected + 1 < match_count) {
            selected++;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (query_length > 0) query[--query_length] = '\0';
            else {
                (void)curs_set(0);
                (void)timeout(250);
                return;
            }
            selected = 0;
        } else if (ch == 21) {
            query_length = 0;
            query[0] = '\0';
            selected = 0;
        } else if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && match_count > 0) {
            PaletteCommand command = matches[selected];
            if (palette_disabled_reason(state, command) == NULL) {
                (void)curs_set(0);
                (void)timeout(250);
                execute_palette_command(state, command);
                return;
            }
        } else if (ch >= 32 && ch <= 126 && query_length + 1 < sizeof(query)) {
            query[query_length++] = (char)ch;
            query[query_length] = '\0';
            selected = 0;
        }
    }
}

#if defined(KEY_MOUSE) && defined(BUTTON1_CLICKED)
static bool activity_index_at(const UiState *state, int rows, int columns,
                              int mouse_x, int mouse_y, size_t *index) {
    if (state->transaction_count == 0 || rows <= 9 || mouse_x < 1 || mouse_x >= columns - 1 ||
        mouse_y < 5 || mouse_y >= rows - 2 || index == NULL) {
        return false;
    }
    int width = columns - 2;
    int item_height = width < 70 ? 2 : 1;
    int rows_available = max_int(0, (rows - 7) / item_height);
    if (rows_available == 0) return false;
    size_t first = 0;
    if (state->selected >= (size_t)rows_available) {
        first = state->selected - (size_t)rows_available + 1;
    }
    int row = (mouse_y - 5) / item_height;
    if (row < 0 || row >= rows_available) return false;
    size_t clicked = first + (size_t)row;
    if (clicked >= state->transaction_count) return false;
    *index = clicked;
    return true;
}

static bool manage_index_at(const UiState *state, int rows, int columns,
                            int mouse_x, int mouse_y, size_t *index) {
    if (mouse_x < 1 || mouse_x >= columns - 1 || mouse_y < 5 || mouse_y >= rows - 2 ||
        index == NULL) {
        return false;
    }
    int available = max_int(1, rows - 7);
    size_t first = 0;
    if (state->settings_selected >= (size_t)available) {
        first = state->settings_selected - (size_t)available + 1;
    }
    size_t clicked = first + (size_t)(mouse_y - 5);
    if (clicked >= SETTINGS_ITEM_COUNT) return false;
    *index = clicked;
    return true;
}

static void handle_mouse(UiState *state) {
    if (!state->mouse_active) return;
    MEVENT event;
    if (getmouse(&event) != OK) return;
    int rows, columns;
    getmaxyx(stdscr, rows, columns);
    if (rows < 14 || columns < 40 || event.x < 0 || event.x >= columns ||
        event.y < 0 || event.y >= rows) {
        return;
    }

    if (mouse_is_click(event.bstate) && event.y == 1) {
        Screen target;
        if (navigation_target_at(columns, event.x, state->screen, &target)) {
            navigate_to(state, target);
        }
        return;
    }

    if (mouse_wheel_up(event.bstate)) {
        if (state->screen == SCREEN_ACTIVITY) handle_activity(state, KEY_UP);
        else if (state->screen == SCREEN_MANAGE) handle_settings(state, KEY_UP);
        return;
    }
    if (mouse_wheel_down(event.bstate)) {
        if (state->screen == SCREEN_ACTIVITY) handle_activity(state, KEY_DOWN);
        else if (state->screen == SCREEN_MANAGE) handle_settings(state, KEY_DOWN);
        return;
    }
    if (!mouse_is_click(event.bstate)) return;

    if (state->screen == SCREEN_ACTIVITY) {
        size_t index;
        if (activity_index_at(state, rows, columns, event.x, event.y, &index)) {
            state->selected = index;
            state->status[0] = '\0';
            if (mouse_is_double_click(event.bstate)) {
                show_detail(state, &state->transactions[state->selected]);
            }
        }
    } else if (state->screen == SCREEN_MANAGE) {
        size_t index;
        if (manage_index_at(state, rows, columns, event.x, event.y, &index)) {
            state->settings_selected = index;
            state->status[0] = '\0';
            if (mouse_is_double_click(event.bstate)) handle_settings(state, '\n');
        }
    }
}
#endif

static void handle_key(UiState *state, int ch) {
#if defined(KEY_MOUSE) && defined(BUTTON1_CLICKED)
    if (ch == KEY_MOUSE) {
        handle_mouse(state);
        return;
    }
#endif
    if (ch == 'q' || ch == 'Q') {
        state->running = false;
    } else if (ch == 27 || ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        navigate_back(state);
    } else if (ch == '1') {
        navigate_to(state, SCREEN_OVERVIEW);
    } else if (ch == '2') {
        navigate_to(state, SCREEN_ACTIVITY);
    } else if (ch == '3') {
        navigate_to(state, SCREEN_PLAN);
    } else if (ch == '4') {
        navigate_to(state, SCREEN_INSIGHTS);
    } else if (ch == '5') {
        navigate_to(state, SCREEN_MANAGE);
    } else if (ch == '\t') {
        navigate_to(state, (Screen)(((int)state->screen + 1) % SCREEN_COUNT));
#ifdef KEY_BTAB
    } else if (ch == KEY_BTAB) {
        navigate_to(state,
                    (Screen)(((int)state->screen + SCREEN_COUNT - 1) % SCREEN_COUNT));
#endif
    } else if (ch == ':') {
        command_palette(state);
    } else if (ch == '?') {
        show_help(state);
    } else if (action_pressed(state, ACTION_TRANSFER, ch)) {
        create_transfer(state);
    } else if (action_pressed(state, ACTION_ACCOUNT_FILTER, ch)) {
        filter_account(state);
    } else if (action_pressed(state, ACTION_ACCOUNTS, ch)) {
        account_manager(state);
    } else if (action_pressed(state, ACTION_RECONCILE, ch)) {
        reconciliation_center(state);
    } else if (state->screen == SCREEN_MANAGE) {
        handle_settings(state, ch);
    } else if (action_pressed(state, ACTION_SETTINGS, ch)) {
        navigate_to(state, SCREEN_MANAGE);
    } else if (action_pressed(state, ACTION_ADD, ch)) {
        edit_transaction(state, NULL);
    } else if (action_pressed(state, ACTION_UNDO, ch)) {
        undo_delete(state);
    } else if (action_pressed(state, ACTION_SEARCH, ch)) {
        search_activity(state);
    } else if (action_pressed(state, ACTION_FILTER, ch)) {
        cycle_filter(state);
    } else if (action_pressed(state, ACTION_CLEAR, ch)) {
        clear_activity_filter(state);
    } else if (action_pressed(state, ACTION_BUDGET, ch)) {
        set_budget(state);
    } else if (action_pressed(state, ACTION_RECURRING, ch)) {
        recurring_manager(state);
        refresh_state(state);
    } else if (action_pressed(state, ACTION_TODAY, ch)) {
        cmny_current_month(state->month);
        state->selected = 0;
        state->activity_offset = 0;
        refresh_state(state);
    } else if (ch == '[' || ch == KEY_LEFT) {
        shift_month(state, -1);
    } else if (ch == ']' || ch == KEY_RIGHT) {
        shift_month(state, 1);
    } else if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && state->screen == SCREEN_OVERVIEW) {
        navigate_to(state, SCREEN_ACTIVITY);
    } else if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && state->screen == SCREEN_INSIGHTS) {
        navigate_to(state, SCREEN_ACTIVITY);
    } else if (state->screen == SCREEN_ACTIVITY) {
        handle_activity(state, ch);
    }
}

int cmny_ui_run(CmnyDb *db, const CmnyOptions *options, const char *db_path) {
    UiState state = {0};
    state.db = db;
    state.options = options;
    state.db_path = db_path;
    state.start_screen = SCREEN_OVERVIEW;
    state.theme = options->theme;
    state.mouse_mode = UI_MOUSE_AUTO;
    load_bindings(&state);
    char preference[32] = {0};
    if (!options->theme_explicit &&
        cmny_db_setting_get(db, "theme", preference, sizeof(preference))) {
        (void)cmny_theme_parse(preference, &state.theme);
    }
    if (cmny_db_setting_get(db, "start_screen", preference, sizeof(preference))) {
        (void)screen_parse(preference, &state.start_screen);
    } else if (cmny_db_setting_get(db, "screen", preference, sizeof(preference)) &&
               screen_parse(preference, &state.start_screen)) {
        char err[256] = {0};
        (void)cmny_db_setting_set(db, "start_screen", screen_name(state.start_screen),
                                  err, sizeof(err));
    }
    if (cmny_db_setting_get(db, "mouse", preference, sizeof(preference))) {
        (void)mouse_mode_parse(preference, &state.mouse_mode);
    }
    state.tutorial_pending = !options->demo &&
        (!cmny_db_setting_get(db, "tutorial_seen", preference, sizeof(preference)) ||
         strcmp(preference, "1") != 0);
    state.screen = state.start_screen;
    if (state.screen != SCREEN_OVERVIEW) {
        state.navigation_history[state.navigation_depth++] = SCREEN_OVERVIEW;
    }
    state.running = true;
    if (options->demo) {
        (void)snprintf(state.month, sizeof(state.month), "2026-07");
    } else {
        cmny_current_month(state.month);
    }
    refresh_state(&state);

#ifdef _WIN32
    void (*old_int)(int) = signal(SIGINT, handle_signal);
    void (*old_term)(int) = signal(SIGTERM, handle_signal);
#else
    struct sigaction action = {0};
    struct sigaction old_int = {0};
    struct sigaction old_term = {0};
    action.sa_handler = handle_signal;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, &old_int);
    (void)sigaction(SIGTERM, &action, &old_term);
#endif

    if (initscr() == NULL) {
#ifdef _WIN32
        (void)signal(SIGINT, old_int);
        (void)signal(SIGTERM, old_term);
#else
        (void)sigaction(SIGINT, &old_int, NULL);
        (void)sigaction(SIGTERM, &old_term, NULL);
#endif
        (void)fprintf(stderr, "cmny: cannot initialize the terminal\n");
        return 1;
    }
    (void)cbreak();
    (void)noecho();
    (void)keypad(stdscr, true);
    (void)configure_mouse(&state);
#ifdef NCURSES_VERSION
    (void)set_escdelay(25);
#endif
    (void)curs_set(0);
    (void)timeout(250);
    if (!options->no_color && has_colors() && start_color() == OK) {
        theme_background = use_default_colors() == OK ? (short)-1 : COLOR_BLACK;
        colors_active = apply_theme(state.theme, theme_background);
    }
    if (state.tutorial_pending) show_tutorial(&state);

    while (state.running && !interrupted) {
        draw(&state);
        int ch = getch();
        if (ch != ERR && ch != KEY_RESIZE) handle_key(&state, ch);
    }
#if defined(KEY_MOUSE) && defined(BUTTON1_CLICKED)
    if (state.mouse_active) {
        mmask_t old_mask = 0;
        (void)mousemask(0, &old_mask);
    }
#endif
    (void)endwin();
    colors_active = false;
#ifdef _WIN32
    (void)signal(SIGINT, old_int);
    (void)signal(SIGTERM, old_term);
#else
    (void)sigaction(SIGINT, &old_int, NULL);
    (void)sigaction(SIGTERM, &old_term, NULL);
#endif
    return 0;
}
