#include "cmny.h"

#include <ctype.h>
#include <curses.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    SCREEN_REPORTS
} Screen;

typedef struct {
    short brand;
    short income;
    short expense;
    short muted;
    short selected_foreground;
    short selected_background;
    short warning;
} ThemePalette;

static const ThemePalette theme_palettes[CMNY_THEME_COUNT] = {
    [CMNY_THEME_OCEAN] = {COLOR_CYAN, COLOR_GREEN, COLOR_RED, COLOR_WHITE,
                          COLOR_BLACK, COLOR_CYAN, COLOR_YELLOW},
    [CMNY_THEME_VIOLET] = {COLOR_MAGENTA, COLOR_CYAN, COLOR_RED, COLOR_WHITE,
                           COLOR_WHITE, COLOR_MAGENTA, COLOR_YELLOW},
    [CMNY_THEME_AMBER] = {COLOR_YELLOW, COLOR_GREEN, COLOR_RED, COLOR_WHITE,
                          COLOR_BLACK, COLOR_YELLOW, COLOR_MAGENTA}
};

typedef struct {
    CmnyDb *db;
    const CmnyOptions *options;
    const char *db_path;
    Screen screen;
    CmnyTheme theme;
    char month[8];
    char search[CMNY_NOTE_MAX + 1];
    int kind_filter;
    size_t selected;
    CmnyMonthSummary summary;
    CmnyMonthSummary previous;
    CmnyCategoryTotal categories[CMNY_CATEGORY_LIMIT];
    size_t category_count;
    CmnyMonthTrend trend[CMNY_TREND_MONTHS];
    CmnyTransaction transactions[CMNY_TX_LIMIT];
    size_t transaction_count;
    size_t activity_offset;
    size_t activity_total;
    CmnyTransaction recent[6];
    size_t recent_count;
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

static void statusf(UiState *state, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void statusf(UiState *state, const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(state->status, sizeof(state->status), format, args);
    va_end(args);
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

static void refresh_state(UiState *state) {
    char err[256] = {0};
    char previous_month[8];
    state->status[0] = '\0';
    if (!cmny_month_shift(state->month, -1, previous_month) ||
        !cmny_db_month_summary(state->db, state->month, &state->summary, err, sizeof(err)) ||
        !cmny_db_month_summary(state->db, previous_month, &state->previous, err, sizeof(err)) ||
        !cmny_db_category_totals(state->db, state->month, state->categories,
                                 CMNY_CATEGORY_LIMIT, &state->category_count, err, sizeof(err)) ||
        !cmny_db_trend(state->db, state->month, state->trend, CMNY_TREND_MONTHS,
                       err, sizeof(err)) ||
        !cmny_db_count(state->db, state->month, state->search, state->kind_filter,
                       &state->activity_total, err, sizeof(err))) {
        statusf(state, "Database error: %s", err);
        return;
    }
    if (state->activity_total == 0) {
        state->activity_offset = 0;
    } else if (state->activity_offset >= state->activity_total) {
        state->activity_offset = ((state->activity_total - 1) / CMNY_TX_LIMIT) * CMNY_TX_LIMIT;
    }
    if (!cmny_db_list(state->db, state->month, state->search, state->kind_filter,
                      state->activity_offset, state->transactions, CMNY_TX_LIMIT,
                      &state->transaction_count, err, sizeof(err)) ||
        !cmny_db_list(state->db, state->month, "", 0, 0, state->recent, 6,
                      &state->recent_count, err, sizeof(err))) {
        statusf(state, "Database error: %s", err);
        return;
    }
    if (state->transaction_count == 0) {
        state->selected = 0;
    } else if (state->selected >= state->transaction_count) {
        state->selected = state->transaction_count - 1;
    }
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
        const char *badge = "[ DEMO ]";
        int badge_x = right - (int)strlen(badge) - 2;
        if (badge_x > 34) {
            put_clipped(0, badge_x, (int)strlen(badge), badge);
        }
    }
    if (right > (columns < 70 ? 7 : 34)) {
        put_clipped(0, right, columns - right - 1, month_label);
    }
    (void)attroff(A_REVERSE | A_BOLD | attr_color(COLOR_BRAND));

    const char *tabs[] = {"[1] Overview", "[2] Activity", "[3] Reports"};
    int x = 2;
    for (int i = 0; i < 3; i++) {
        bool active = (int)state->screen == i;
        if (active) (void)attron(A_BOLD | attr_color(COLOR_BRAND));
        put_clipped(1, x, columns - x - 1, tabs[i]);
        if (active) (void)attroff(A_BOLD | attr_color(COLOR_BRAND));
        x += (int)strlen(tabs[i]) + 3;
        if (x >= columns - 4) break;
    }
}

static void draw_footer(const UiState *state, int rows, int columns) {
    (void)attron(A_REVERSE | attr_color(COLOR_MUTED));
    (void)mvhline(rows - 1, 0, ' ', columns);
    if (state->status[0] != '\0') {
        put_clipped(rows - 1, 1, columns - 2, state->status);
    } else {
        put_clipped(rows - 1, 1, columns - 2,
                    "n add  [ ] month  t today  p theme  ? help  q quit");
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

static void draw_transaction_rows(const UiState *state, const CmnyTransaction *items, size_t count,
                                  int y, int x, int height, int width, bool selectable) {
    if (count == 0) {
        (void)attron(A_DIM | attr_color(COLOR_MUTED));
        const char *empty = (state->search[0] != '\0' || state->kind_filter != 0)
                                ? "No matches. Press c to clear filters."
                                : "No activity yet. Press n to add your first entry.";
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
        int row = y + row_index * item_height;
        if (selectable && index == state->selected) {
            (void)attron(selection_attr());
            (void)mvhline(row, x, ' ', width);
            if (compact && row + 1 < y + height) (void)mvhline(row + 1, x, ' ', width);
        }
        put_clipped(row, x + 1, min_int(10, width - 2), tx->occurred_on);
        char amount[64];
        char formatted[52];
        cmny_money_format(tx->amount_cents, formatted, sizeof(formatted));
        (void)snprintf(amount, sizeof(amount), "%c%s", tx->kind == CMNY_INCOME ? '+' : '-', formatted);
        int amount_x = x + width - (int)strlen(amount) - 1;
        if (compact) {
            int category_width = amount_x - (x + 15) - 1;
            if (category_width > 0) put_clipped(row, x + 15, category_width, tx->category);
            if (row + 1 < y + height) {
                put_clipped(row + 1, x + 3, width - 5, tx->note[0] != '\0' ? tx->note : "No note");
            }
        } else {
            put_clipped(row, x + 15, min_int(14, width - 30), tx->category);
            if (width >= 72) put_clipped(row, x + 32, width - 55, tx->note);
        }
        if (!(selectable && index == state->selected)) {
            (void)attron(attr_color(tx->kind == CMNY_INCOME ? COLOR_INCOME : COLOR_EXPENSE));
        }
        if (amount_x > x + 14) put_clipped(row, amount_x, (int)strlen(amount), amount);
        if (!(selectable && index == state->selected)) {
            (void)attroff(attr_color(tx->kind == CMNY_INCOME ? COLOR_INCOME : COLOR_EXPENSE));
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
    if (state->search[0] != '\0') {
        (void)snprintf(title, sizeof(title), "ACTIVITY  %zu-%zu/%zu  filter:%s  search:%s",
                       range_start, range_end, state->activity_total, filter, state->search);
    } else {
        (void)snprintf(title, sizeof(title), "ACTIVITY  %zu-%zu/%zu  filter:%s",
                       range_start, range_end, state->activity_total, filter);
    }
    draw_box(3, 0, rows - 4, columns, title);
    if (rows > 9) {
        (void)attron(A_BOLD | A_DIM);
        put_clipped(4, 2, columns - 4,
                    columns < 70 ? "DATE          CATEGORY                 AMOUNT"
                                 : "DATE          CATEGORY         NOTE                              AMOUNT");
        (void)attroff(A_BOLD | A_DIM);
        draw_transaction_rows(state, state->transactions, state->transaction_count,
                              5, 1, rows - 7, columns - 2, true);
    }
}

static void draw_reports(const UiState *state, int rows, int columns) {
    char comparison[128];
    int64_t delta = state->summary.expense_cents - state->previous.expense_cents;
    if (state->previous.expense_cents == 0) {
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
        int first = max_int(5, available / 2);
        draw_categories(state, 5, 0, first, columns);
        if (available - first >= 5) draw_trend(state, 5 + first, 0, available - first, columns);
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
        else draw_reports(state, rows, columns);
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
            put_clipped(y + height - 2, x + 2, width - 4, "Press any key to close");
            (void)attroff(A_DIM);
        }
        (void)refresh();
        int ch = getch();
        if (interrupted || ch != KEY_RESIZE) {
            (void)timeout(250);
            return;
        }
    }
}

static void show_help(void) {
    static const char *lines[] = {
        "  _____ __  __ _   ___   __",
        " / ____|  \\/  | \\ | |\\ \\ / /",
        "| |    | |\\/| |  \\| |\\ V /",
        "| |____| |  | | |\\  | | |",
        " \\_____|_|  |_|_| \\_| |_|",
        "",
        "1 / 2 / 3      Overview / Activity / Reports",
        "n              Add a transaction",
        "[ / ]          Previous / next month",
        "t              Jump to the current month",
        "Up/Down, j/k   Move through activity",
        "e / d          Edit / delete selected activity",
        "/              Search category and note",
        "f / c          Cycle filter / clear filters",
        "p              Cycle color theme",
        "? / q          Help / quit"
    };
    message_modal("CMNY HELP", lines, sizeof(lines) / sizeof(lines[0]));
}

static void plain_amount(int64_t cents, char *out, size_t out_size) {
    (void)snprintf(out, out_size, "%lld.%02lld",
                   (long long)(cents / 100), (long long)(cents % 100));
}

static void edit_transaction(UiState *state, const CmnyTransaction *existing) {
    CmnyTransaction draft = {0};
    bool editing = existing != NULL;
    if (editing) {
        draft = *existing;
    } else {
        draft.kind = CMNY_EXPENSE;
        cmny_today(draft.occurred_on);
    }
edit_fields:
    ;
    if (!choose_kind(&draft.kind)) return;

    char amount[64] = {0};
    if (draft.amount_cents > 0) plain_amount(draft.amount_cents, amount, sizeof(amount));
    bool amount_invalid = false;
    for (;;) {
        if (!input_modal(editing ? "EDIT TRANSACTION" : "NEW TRANSACTION",
                         amount_invalid ? "Invalid amount: use a positive value such as 12.50"
                                        : "Amount (positive, e.g. 12.50)",
                         amount, sizeof(amount))) return;
        if (cmny_money_parse(amount, &draft.amount_cents) && draft.amount_cents <= 9000000000000000LL) break;
        amount_invalid = true;
    }

    bool date_invalid = false;
    for (;;) {
        if (!input_modal("TRANSACTION DATE",
                         date_invalid ? "Invalid date: enter a real date as YYYY-MM-DD"
                                      : "Date (YYYY-MM-DD)", draft.occurred_on,
                         sizeof(draft.occurred_on))) return;
        if (cmny_date_valid(draft.occurred_on)) break;
        date_invalid = true;
    }
    if (draft.category[0] == '\0') {
        (void)snprintf(draft.category, sizeof(draft.category), "%s",
                       draft.kind == CMNY_INCOME ? "Salary" : "Food");
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
    if (!input_modal("NOTE", "Optional note (printable ASCII)", draft.note, sizeof(draft.note))) return;

retry_save:
    ;
    char err[256] = {0};
    bool ok;
    if (editing) {
        ok = cmny_db_update(state->db, &draft, err, sizeof(err));
    } else {
        ok = cmny_db_add(state->db, &draft, NULL, err, sizeof(err));
    }
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
    statusf(state, editing ? "Transaction updated." : "Transaction saved.");
    refresh_state(state);
    statusf(state, editing ? "Transaction updated." : "Transaction saved.");
}

static void show_detail(const UiState *state, const CmnyTransaction *tx) {
    char amount[96];
    char id[64], kind[64], date[64], category[96], note[180], value[80];
    format_value(state, tx->amount_cents, value, sizeof(value));
    (void)snprintf(id, sizeof(id), "ID        %lld", (long long)tx->id);
    (void)snprintf(kind, sizeof(kind), "Type      %s", tx->kind == CMNY_INCOME ? "Income" : "Expense");
    (void)snprintf(amount, sizeof(amount), "Amount    %s", value);
    (void)snprintf(date, sizeof(date), "Date      %s", tx->occurred_on);
    (void)snprintf(category, sizeof(category), "Category  %s", tx->category);
    (void)snprintf(note, sizeof(note), "Note      %s", tx->note[0] != '\0' ? tx->note : "-");
    const char *lines[] = {id, kind, amount, date, category, note};
    message_modal("TRANSACTION DETAIL", lines, sizeof(lines) / sizeof(lines[0]));
}

static void handle_activity(UiState *state, int ch) {
    if (ch == KEY_UP || ch == 'k') {
        if (state->selected > 0) {
            state->selected--;
        } else if (state->activity_offset > 0) {
            state->activity_offset = state->activity_offset >= CMNY_TX_LIMIT
                                         ? state->activity_offset - CMNY_TX_LIMIT : 0;
            refresh_state(state);
            if (state->transaction_count > 0) state->selected = state->transaction_count - 1;
        }
    } else if (ch == KEY_DOWN || ch == 'j') {
        if (state->selected + 1 < state->transaction_count) {
            state->selected++;
        } else if (state->activity_offset + state->transaction_count < state->activity_total) {
            state->activity_offset += state->transaction_count;
            state->selected = 0;
            refresh_state(state);
        }
    } else if (ch == 'g') {
        state->activity_offset = 0;
        state->selected = 0;
        refresh_state(state);
    } else if (ch == 'G' && state->activity_total > 0) {
        state->activity_offset = ((state->activity_total - 1) / CMNY_TX_LIMIT) * CMNY_TX_LIMIT;
        refresh_state(state);
        if (state->transaction_count > 0) state->selected = state->transaction_count - 1;
    }
    else if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && state->transaction_count > 0) {
        show_detail(state, &state->transactions[state->selected]);
    } else if (ch == 'e' && state->transaction_count > 0) {
        CmnyTransaction selected = state->transactions[state->selected];
        edit_transaction(state, &selected);
    } else if (ch == 'd' && state->transaction_count > 0) {
        CmnyTransaction selected = state->transactions[state->selected];
        char question[160];
        (void)snprintf(question, sizeof(question), "Delete %s on %s?", selected.category, selected.occurred_on);
        if (confirm_modal("DELETE TRANSACTION", question)) {
            char err[256] = {0};
            if (cmny_db_delete(state->db, selected.id, err, sizeof(err))) {
                refresh_state(state);
                statusf(state, "Transaction deleted.");
            } else {
                statusf(state, "Delete failed: %s", err);
            }
        }
    } else if (ch == '/') {
        char search[sizeof(state->search)];
        (void)snprintf(search, sizeof(search), "%s", state->search);
        if (input_modal("SEARCH", "Search category or note", search, sizeof(search))) {
            (void)snprintf(state->search, sizeof(state->search), "%s", search);
            state->selected = 0;
            state->activity_offset = 0;
            refresh_state(state);
        }
    } else if (ch == 'f') {
        state->kind_filter = state->kind_filter == 0 ? CMNY_EXPENSE :
                             state->kind_filter == CMNY_EXPENSE ? CMNY_INCOME : 0;
        state->selected = 0;
        state->activity_offset = 0;
        refresh_state(state);
    } else if (ch == 'c') {
        state->search[0] = '\0';
        state->kind_filter = 0;
        state->selected = 0;
        state->activity_offset = 0;
        refresh_state(state);
    }
}

static void handle_key(UiState *state, int ch) {
    if (ch == 'q' || ch == 'Q') {
        state->running = false;
    } else if (ch == '1') {
        state->screen = SCREEN_OVERVIEW;
    } else if (ch == '2') {
        state->screen = SCREEN_ACTIVITY;
    } else if (ch == '3') {
        state->screen = SCREEN_REPORTS;
    } else if (ch == 'n' || ch == 'a') {
        edit_transaction(state, NULL);
    } else if (ch == '[') {
        char shifted[8];
        if (cmny_month_shift(state->month, -1, shifted)) {
            (void)snprintf(state->month, sizeof(state->month), "%s", shifted);
            state->selected = 0;
            state->activity_offset = 0;
            refresh_state(state);
        }
    } else if (ch == ']') {
        char shifted[8];
        if (cmny_month_shift(state->month, 1, shifted)) {
            (void)snprintf(state->month, sizeof(state->month), "%s", shifted);
            state->selected = 0;
            state->activity_offset = 0;
            refresh_state(state);
        }
    } else if (ch == 't') {
        cmny_current_month(state->month);
        state->selected = 0;
        state->activity_offset = 0;
        refresh_state(state);
    } else if (ch == 'p') {
        if (!colors_active) {
            statusf(state, "Themes are unavailable when terminal colors are disabled.");
        } else {
            CmnyTheme next = (CmnyTheme)(((int)state->theme + 1) % (int)CMNY_THEME_COUNT);
            if (apply_theme(next, theme_background)) {
                state->theme = next;
                statusf(state, "Theme: %s", cmny_theme_name(state->theme));
            } else {
                statusf(state, "This terminal could not activate the next theme.");
            }
        }
    } else if (ch == '?') {
        show_help();
    } else if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && state->screen == SCREEN_OVERVIEW) {
        state->screen = SCREEN_ACTIVITY;
    } else if (state->screen == SCREEN_ACTIVITY) {
        handle_activity(state, ch);
    }
}

int cmny_ui_run(CmnyDb *db, const CmnyOptions *options, const char *db_path) {
    UiState state = {0};
    state.db = db;
    state.options = options;
    state.db_path = db_path;
    state.screen = SCREEN_OVERVIEW;
    state.theme = options->theme;
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
    (void)curs_set(0);
    (void)timeout(250);
    if (!options->no_color && has_colors() && start_color() == OK) {
        theme_background = use_default_colors() == OK ? (short)-1 : COLOR_BLACK;
        colors_active = apply_theme(state.theme, theme_background);
    }

    while (state.running && !interrupted) {
        draw(&state);
        int ch = getch();
        if (ch != ERR && ch != KEY_RESIZE) handle_key(&state, ch);
    }
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
