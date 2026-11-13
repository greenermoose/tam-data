#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "tam-data.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csv.h"
#include "input.h"
#include "utf8.h"
#include "version.h"

static uint32_t td_first_cp(const char *s)
{
    const char *p = s;
    const char *end;

    if (!s || !*s)
        return '?';
    end = s + strlen(s);
    return td_utf8_decode(&p, end);
}

static void td_draw_separator(td_term_t *t, int row, int col, bool key)
{
    const char *glyph = key ? t->glyphs.keysep : t->glyphs.vline;

    td_term_put_cell(t, row, col, td_first_cp(glyph), TD_COLOR_DEFAULT,
                     TD_COLOR_DEFAULT, TD_ATTR_DIM);
}

static void td_render_status(td_term_t *t, td_stack_t *stack,
                             const td_view_state_t *state)
{
    int row = t->rows - 1;
    int cols = t->cols;
    td_sheet_t *s = td_stack_active(stack);
    char left[256];
    char right[128];
    char center[512];
    int leftw, rightw;
    int center_x, center_max;
    uint8_t fg = TD_COLOR_DEFAULT;
    uint8_t bg = TD_COLOR_DEFAULT;
    uint8_t attr = TD_ATTR_REV;

    if (row < 0 || cols <= 0)
        return;

    if (s) {
        const char *pending = state && state->pending ? state->pending : "";

        if (s->ncols == 0) {
            snprintf(left, sizeof(left), "[%zu/%zu] %s | r%zu c0",
                     stack->len, stack->len, s->name,
                     s->nrows ? s->cursor_row + 1 : (size_t)0);
        } else {
            size_t col_idx = s->cursor_col < s->ncols ? s->cursor_col : 0;
            size_t r1 = s->nrows ? s->cursor_row + 1 : 0;
            const char *cname = s->cols[col_idx].name
                                    ? s->cols[col_idx].name
                                    : "";

            snprintf(left, sizeof(left), "[%zu/%zu] %s | r%zu c%zu:%s",
                     stack->len, stack->len, s->name, r1, col_idx + 1, cname);
        }

        if (pending[0])
            snprintf(right, sizeof(right), "%s | %zu rows (%zu selected)",
                     pending, s->nrows, s->nselected);
        else
            snprintf(right, sizeof(right), "%zu rows (%zu selected)",
                     s->nrows, s->nselected);
    } else {
        snprintf(left, sizeof(left), "[%zu/%zu] tam-data",
                 stack->len, stack->len);
        snprintf(right, sizeof(right), "%s", "");
    }

    td_term_fill_rect(t, row, 0, 1, cols, ' ', fg, bg, attr);
    rightw = (int)td_utf8_width(right);
    if (rightw > cols)
        rightw = cols;
    leftw = (int)td_utf8_width(left);
    {
        int left_avail = cols - rightw - (rightw > 0 ? 1 : 0);

        if (left_avail > 0 && leftw > 0) {
            int draw_left = leftw < left_avail ? leftw : left_avail;

            td_term_put_str(t, row, 0, draw_left, left, fg, bg, attr);
        }
    }
    td_term_put_str(t, row, cols - rightw, rightw, right, fg, bg, attr);

    center_x = leftw + 2;
    center_max = cols - rightw - center_x - 1;

    if (state && state->prompt_active) {
        if (center_max < 12 && cols - rightw - 1 > 0) {
            center_x = 0;
            center_max = cols - rightw - 1;
            td_term_fill_rect(t, row, 0, 1, center_max, ' ', fg, bg, attr);
        }
        if (center_max <= 0)
            return;
        const char *label = state->prompt_label ? state->prompt_label : "";
        const char *text = state->prompt_text ? state->prompt_text : "";
        int labelw = (int)td_utf8_width(label);
        int text_x = center_x + labelw + 1;
        int text_max = center_max - labelw - 1;
        int cursorw;
        const char *p;
        const char *end;

        td_term_put_str(t, row, center_x, labelw, label, fg, bg, attr);
        if (text_max > 0) {
            td_term_put_str(t, row, text_x, text_max, text, fg, bg, attr);
            p = text;
            end = text + strlen(text);
            cursorw = 0;
            while ((size_t)(p - text) < state->prompt_cursor && p < end) {
                (void)td_utf8_decode(&p, end);
                cursorw++;
            }
            if (text_x + cursorw < center_x + center_max &&
                text_x + cursorw < cols) {
                td_cell_t *c = &t->back[(size_t)row * (size_t)t->cols +
                                        (size_t)(text_x + cursorw)];

                c->attr |= TD_ATTR_UNDERLINE;
            }
        }
        return;
    }

    if (center_max <= 0)
        return;
    if (state && state->message && state->message[0]) {
        snprintf(center, sizeof(center), "%s", state->message);
        td_term_put_str(t, row, center_x, center_max, center, fg, bg, attr);
    }
}

void td_view_render(td_term_t *t, td_stack_t *stack,
                    const td_view_state_t *state)
{
    td_sheet_t *s = td_stack_active(stack);
    int header_row = 0;
    int body_rows;
    int x;

    if (!t || !t->back)
        return;

    td_term_clear_back(t);
    td_render_status(t, stack, state);
    if (!s || t->rows < 2)
        return;

    body_rows = t->rows - 2;
    x = 0;

    /* Header: pinned key columns first, then regular columns from left_col. */
    for (size_t pass = 0; pass < 2; pass++) {
        size_t start = pass == 0 ? 0 : s->left_col;
        size_t end = s->ncols;

        for (size_t c = start; c < end && x < t->cols; c++) {
            td_col_t *col = &s->cols[c];
            int width;
            char header[256];
            uint8_t attr;

            if (pass == 0 && !col->is_key)
                continue;
            if (pass == 1 && col->is_key)
                continue;
            width = td_sheet_col_width(s, c);
            if (width <= 0)
                continue;

            attr = TD_ATTR_BOLD;
            if (c == s->cursor_col)
                attr |= TD_ATTR_REV;
            td_sheet_header(header, sizeof(header), col);
            td_term_put_str(t, header_row, x, width, header,
                            TD_COLOR_DEFAULT, TD_COLOR_DEFAULT, attr);
            x += width;
            if (x < t->cols) {
                td_draw_separator(t, header_row, x, col->is_key);
                x++;
            }
        }
    }
    if (x < t->cols) {
        uint32_t hline = td_first_cp(t->glyphs.hline);

        td_term_fill_rect(t, header_row, x, 1, t->cols - x, hline,
                          TD_COLOR_DEFAULT, TD_COLOR_DEFAULT, TD_ATTR_DIM);
    }

    /* Body rows. */
    for (int i = 0; i < body_rows; i++) {
        size_t vr = s->top_row + (size_t)i;
        size_t physical;
        int bx = 0;
        bool selected;

        if (vr >= s->nrows)
            break;
        physical = s->row_order[vr];
        selected = td_sheet_is_selected(s, physical);

        for (size_t pass = 0; pass < 2; pass++) {
            size_t start = pass == 0 ? 0 : s->left_col;

            for (size_t c = start; c < s->ncols && bx < t->cols; c++) {
                td_col_t *col = &s->cols[c];
                int width;
                char value[512];
                uint8_t fg = TD_COLOR_DEFAULT;
                uint8_t bg = TD_COLOR_DEFAULT;
                uint8_t attr = TD_ATTR_NONE;

                if (pass == 0 && !col->is_key)
                    continue;
                if (pass == 1 && col->is_key)
                    continue;
                width = td_sheet_col_width(s, c);
                if (width <= 0)
                    continue;

                td_val_t cell = col->cells[physical];
                bool is_num_col = col->type == TD_TYPE_INT ||
                                  col->type == TD_TYPE_FLOAT ||
                                  col->type == TD_TYPE_CURRENCY;
                int pad = 0;

                if (cell.type == TD_TYPE_ANY && col->type != TD_TYPE_ANY)
                    cell = td_val_parse(cell.as.s, col->type);
                td_val_format(cell, col->type, value, sizeof(value));

                if (selected) {
                    fg = 15;
                    bg = 4;
                } else if (cell.type == TD_TYPE_ERR) {
                    fg = 1;
                    attr |= TD_ATTR_BOLD;
                }
                if (vr == s->cursor_row)
                    attr |= TD_ATTR_BOLD;
                if (vr == s->cursor_row && c == s->cursor_col)
                    attr |= TD_ATTR_REV;

                if (selected || (vr == s->cursor_row && c == s->cursor_col))
                    td_term_fill_rect(t, header_row + 1 + i, bx, 1, width,
                                      ' ', fg, bg, attr);

                if (is_num_col && cell.type != TD_TYPE_ERR) {
                    int vw = (int)td_utf8_width(value);

                    if (vw < width)
                        pad = width - vw;
                }
                td_term_put_str(t, header_row + 1 + i, bx + pad, width - pad,
                                value, fg, bg, attr);
                bx += width;
                if (bx < t->cols) {
                    td_draw_separator(t, header_row + 1 + i, bx, col->is_key);
                    bx++;
                }
            }
        }
    }
}

td_sheet_t *td_demo_sheet(void)
{
    static const char *const names[] = {
        "Alice", "Bob", "Carol", "Dave", "Eve", "Frank", "Grace", "Heidi"
    };
    static const char *const qtys[] = { "12", "7", "31", "0", "18", "5", "42", "9" };
    static const char *const prices[] = {
        "19.99", "4.50", "129.00", "0.00", "8.75", "56.20", "3.10", "77.77"
    };
    static const char *const dates[] = {
        "2024-01-15", "2024-02-01", "2024-02-29", "2024-03-10",
        "2024-04-01", "2024-05-05", "2024-06-30", "2024-07-04"
    };
    static const char *const ratios[] = {
        "0.25", "1.5", "0.333", "2.0", "0.75", "1.0", "0.125", "3.14159"
    };
    static const char *const notes[] = {
        "first", "second", "", "missing note", "hello, world",
        "quoted \"text\"", "tab\there", "last"
    };
    td_sheet_t *s = td_sheet_new("demo", "(built-in)");
    size_t cols[6];

    if (!s)
        return NULL;
    cols[0] = td_sheet_add_col(s, "name", TD_TYPE_STR, -20);
    cols[1] = td_sheet_add_col(s, "qty", TD_TYPE_INT, -8);
    cols[2] = td_sheet_add_col(s, "price", TD_TYPE_CURRENCY, -12);
    cols[3] = td_sheet_add_col(s, "date", TD_TYPE_DATE, -12);
    cols[4] = td_sheet_add_col(s, "ratio", TD_TYPE_FLOAT, -10);
    cols[5] = td_sheet_add_col(s, "notes", TD_TYPE_ANY, -24);
    if (cols[0] == SIZE_MAX || cols[5] == SIZE_MAX) {
        td_sheet_free(s);
        return NULL;
    }
    s->cols[cols[0]].is_key = true;

    for (size_t r = 0; r < sizeof(names) / sizeof(names[0]); r++) {
        size_t row = td_sheet_add_row(s);

        if (row == SIZE_MAX) {
            td_sheet_free(s);
            return NULL;
        }
        td_sheet_set_cell(s, row, cols[0],
                          td_val_parse(td_slice_from_cstr(names[r]),
                                       TD_TYPE_STR));
        td_sheet_set_cell(s, row, cols[1],
                          td_val_parse(td_slice_from_cstr(qtys[r]),
                                       TD_TYPE_INT));
        td_sheet_set_cell(s, row, cols[2],
                          td_val_parse(td_slice_from_cstr(prices[r]),
                                       TD_TYPE_CURRENCY));
        td_sheet_set_cell(s, row, cols[3],
                          td_val_parse(td_slice_from_cstr(dates[r]),
                                       TD_TYPE_DATE));
        td_sheet_set_cell(s, row, cols[4],
                          td_val_parse(td_slice_from_cstr(ratios[r]),
                                       TD_TYPE_FLOAT));
        td_sheet_set_cell(s, row, cols[5],
                          td_val_parse(td_slice_from_cstr(notes[r]),
                                       TD_TYPE_ANY));
    }
    return s;
}

#ifndef TD_TAM_DATA_NO_MAIN

static void td_usage(FILE *out)
{
    fprintf(out,
        "usage: tam-data [options] [file ...]\n"
        "\n"
        "Options:\n"
        "  -d, --delimiter CHAR  explicit field delimiter (use \\t for tab)\n"
        "      --csv             force comma-delimited CSV\n"
        "      --tsv             force tab-delimited TSV\n"
        "      --no-header       treat first row as data (col1, col2, ...)\n"
        "      --demo            open the built-in sample sheet\n"
        "  -v, --version         print version information\n"
        "  -h, --help            show this help\n"
        "\n"
        "Pass '-' or pipe data on stdin to read from standard input.\n"
        "\n"
        "Keybindings:\n"
        "  h j k l / arrows      move the cursor\n"
        "  gh gl gg G            first/last column, first/last row\n"
        "  Ctrl+F / Ctrl+B       page down / page up\n"
        "  < / >                 previous / next distinct value in column\n"
        "  { / }                 previous / next blank cell in column\n"
        "  H / L                 move column left / right\n"
        "  s u t                 select / unselect / toggle current row\n"
        "  gs gu gt              select / unselect / toggle all rows\n"
        "  !                     pin or unpin the current column as a key\n"
        "  - / gv                hide current column / unhide all columns\n"
        "  _ / g_ / z_           autosize column / all columns / set width\n"
        "  ^                     rename the current column\n"
        "  # %% $ @ ~ z~          set column type (int, float, currency,\n"
        "                        date, string, any)\n"
        "  Ctrl+S                save sheet atomically to CSV/TSV\n"
        "  q / gq                pop sheet / quit tam-data\n"
        "  Ctrl+L                full redraw\n");
}

static const char *td_locale(void)
{
    const char *s = getenv("LC_ALL");

    if (!s || !*s)
        s = getenv("LC_CTYPE");
    if (!s || !*s)
        s = getenv("LANG");
    return s;
}

static void td_set_message(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;

    if (!buf || cap == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
}

static void td_handle_command(td_cmd_t cmd, td_stack_t *stack, td_term_t *term,
                              td_prompt_t *prompt, char *msg, size_t msg_cap,
                              bool *quit)
{
    td_sheet_t *s = td_stack_active(stack);
    int body_rows = term->rows - 2;
    size_t next;

    if (body_rows < 1)
        body_rows = 1;
    if (!s)
        return;

    switch (cmd) {
    case TD_CMD_NONE:
        break;
    case TD_CMD_LEFT:
        next = td_sheet_next_visible_col(s, s->cursor_col, -1);
        if (next != SIZE_MAX)
            s->cursor_col = next;
        break;
    case TD_CMD_RIGHT:
        next = td_sheet_next_visible_col(s, s->cursor_col, 1);
        if (next != SIZE_MAX)
            s->cursor_col = next;
        break;
    case TD_CMD_UP:
        if (s->cursor_row > 0)
            s->cursor_row--;
        break;
    case TD_CMD_DOWN:
        if (s->cursor_row + 1 < s->nrows)
            s->cursor_row++;
        break;
    case TD_CMD_FIRST_COL:
        next = td_sheet_first_visible_col(s);
        if (next != SIZE_MAX)
            s->cursor_col = next;
        break;
    case TD_CMD_LAST_COL:
        next = td_sheet_last_visible_col(s);
        if (next != SIZE_MAX)
            s->cursor_col = next;
        break;
    case TD_CMD_FIRST_ROW:
        s->cursor_row = 0;
        break;
    case TD_CMD_LAST_ROW:
        if (s->nrows)
            s->cursor_row = s->nrows - 1;
        break;
    case TD_CMD_PAGE_DOWN:
        s->cursor_row += (size_t)body_rows;
        if (s->nrows && s->cursor_row >= s->nrows)
            s->cursor_row = s->nrows - 1;
        break;
    case TD_CMD_PAGE_UP:
        if (s->cursor_row > (size_t)body_rows)
            s->cursor_row -= (size_t)body_rows;
        else
            s->cursor_row = 0;
        break;
    case TD_CMD_NEXT_VALUE:
        if (!td_sheet_jump_value(s, 1))
            td_set_message(msg, msg_cap, "no different value below");
        break;
    case TD_CMD_PREV_VALUE:
        if (!td_sheet_jump_value(s, -1))
            td_set_message(msg, msg_cap, "no different value above");
        break;
    case TD_CMD_NEXT_NULL:
        if (!td_sheet_jump_null(s, 1))
            td_set_message(msg, msg_cap, "no blank cell below");
        break;
    case TD_CMD_PREV_NULL:
        if (!td_sheet_jump_null(s, -1))
            td_set_message(msg, msg_cap, "no blank cell above");
        break;
    case TD_CMD_MOVE_COL_LEFT:
        if (!td_sheet_move_col(s, s->cursor_col, -1))
            td_set_message(msg, msg_cap, "already at leftmost column");
        break;
    case TD_CMD_MOVE_COL_RIGHT:
        if (!td_sheet_move_col(s, s->cursor_col, 1))
            td_set_message(msg, msg_cap, "already at rightmost column");
        break;
    case TD_CMD_HIDE_COL:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            break;
        td_sheet_hide_col(s, s->cursor_col);
        td_set_message(msg, msg_cap, "column hidden");
        break;
    case TD_CMD_TOGGLE_KEY_COL:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            break;
        td_sheet_toggle_key_col(s, s->cursor_col);
        td_set_message(msg, msg_cap, "%s key column",
                       s->cols[s->cursor_col].is_key ? "pinned" : "unpinned");
        break;
    case TD_CMD_AUTOSIZE_COL:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            break;
        td_sheet_autosize_col(s, s->cursor_col);
        td_set_message(msg, msg_cap, "column width %d",
                       td_sheet_col_width(s, s->cursor_col));
        break;
    case TD_CMD_AUTOSIZE_ALL_COLS:
        td_sheet_autosize_all_cols(s);
        td_set_message(msg, msg_cap, "autosized all columns");
        break;
    case TD_CMD_SET_COL_WIDTH: {
        char wbuf[32];

        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            break;
        snprintf(wbuf, sizeof(wbuf), "%d",
                 td_sheet_col_width(s, s->cursor_col));
        td_prompt_start_kind(prompt, TD_PROMPT_COL_WIDTH, "column width:",
                             wbuf);
        break;
    }
    case TD_CMD_UNHIDE_ALL_COLS:
        td_sheet_unhide_all_cols(s);
        td_set_message(msg, msg_cap, "unhid all columns");
        break;
    case TD_CMD_RENAME_COL:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            break;
        td_prompt_start_kind(prompt, TD_PROMPT_RENAME_COL, "rename column:",
                             s->cols[s->cursor_col].name);
        break;
    case TD_CMD_SAVE_SHEET: {
        const char *def = (s->source_path && s->source_path[0] &&
                           strcmp(s->source_path, "(built-in)") != 0)
                              ? s->source_path
                              : "output.csv";

        td_prompt_start_kind(prompt, TD_PROMPT_SAVE_SHEET, "save to:", def);
        break;
    }
    case TD_CMD_TYPE_ANY:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            break;
        td_sheet_set_col_type(s, s->cursor_col, TD_TYPE_ANY);
        td_set_message(msg, msg_cap, "column type any");
        break;
    case TD_CMD_TYPE_STR:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            break;
        td_sheet_set_col_type(s, s->cursor_col, TD_TYPE_STR);
        td_set_message(msg, msg_cap, "column type string");
        break;
    case TD_CMD_TYPE_INT:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            break;
        td_sheet_set_col_type(s, s->cursor_col, TD_TYPE_INT);
        td_set_message(msg, msg_cap, "column type int");
        break;
    case TD_CMD_TYPE_FLOAT:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            break;
        td_sheet_set_col_type(s, s->cursor_col, TD_TYPE_FLOAT);
        td_set_message(msg, msg_cap, "column type float");
        break;
    case TD_CMD_TYPE_CURRENCY:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            break;
        td_sheet_set_col_type(s, s->cursor_col, TD_TYPE_CURRENCY);
        td_set_message(msg, msg_cap, "column type currency");
        break;
    case TD_CMD_TYPE_DATE:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            break;
        td_sheet_set_col_type(s, s->cursor_col, TD_TYPE_DATE);
        td_set_message(msg, msg_cap, "column type date");
        break;
    case TD_CMD_POP_SHEET:
        s = td_stack_pop(stack);
        td_sheet_free(s);
        if (stack->len == 0) {
            *quit = true;
        } else {
            td_set_message(msg, msg_cap, "sheet closed");
        }
        break;
    case TD_CMD_QUIT_ALL:
        *quit = true;
        break;
    case TD_CMD_SELECT_ROW:
        td_sheet_select_row(s, s->cursor_row);
        break;
    case TD_CMD_UNSELECT_ROW:
        td_sheet_unselect_row(s, s->cursor_row);
        break;
    case TD_CMD_TOGGLE_ROW:
        td_sheet_toggle_row(s, s->cursor_row);
        break;
    case TD_CMD_SELECT_ALL:
        td_sheet_select_all(s);
        break;
    case TD_CMD_UNSELECT_ALL:
        td_sheet_unselect_all(s);
        break;
    case TD_CMD_TOGGLE_ALL:
        td_sheet_toggle_all(s);
        break;
    case TD_CMD_REDRAW:
        term->force_full = true;
        break;
    }
}

static bool td_parse_delimiter_arg(const char *arg, char *out)
{
    if (!arg || !arg[0])
        return false;
    if (strcmp(arg, "\\t") == 0) {
        *out = '\t';
        return true;
    }
    if (arg[1] == '\0') {
        *out = arg[0];
        return true;
    }
    return false;
}

static bool td_parse_width_text(const char *text, int *out)
{
    char *end = NULL;
    long v;

    if (!text || !text[0])
        return false;
    errno = 0;
    v = strtol(text, &end, 10);
    if (errno == ERANGE || end == text)
        return false;
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0' || v < 0 || v > 1000)
        return false;
    *out = (int)v;
    return true;
}

static void td_handle_prompt_result(td_prompt_result_t pr, td_prompt_t *prompt,
                                    td_sheet_t *active, char *msg,
                                    size_t msg_cap)
{
    td_prompt_kind_t kind = td_prompt_kind(prompt);

    if (pr == TD_PROMPT_ACCEPT && active) {
        const char *text = td_prompt_text(prompt);

        switch (kind) {
        case TD_PROMPT_RENAME_COL:
            if (td_sheet_rename_col(active, active->cursor_col, text))
                td_set_message(msg, msg_cap, "renamed column to %s", text);
            break;
        case TD_PROMPT_COL_WIDTH: {
            int width = 0;

            if (!td_parse_width_text(text, &width)) {
                td_set_message(msg, msg_cap, "invalid width: %s", text);
            } else if (td_sheet_set_col_width(active, active->cursor_col,
                                              width)) {
                if (width == 0)
                    td_set_message(msg, msg_cap, "column hidden");
                else
                    td_set_message(msg, msg_cap, "column width %d", width);
            }
            break;
        }
        case TD_PROMPT_SAVE_SHEET: {
            char err[256];

            if (!text[0]) {
                td_set_message(msg, msg_cap, "save cancelled: empty path");
            } else if (td_csv_save_sheet(active, text, '\0', err,
                                         sizeof(err))) {
                td_set_message(msg, msg_cap, "saved %zu rows to %s",
                               active->nrows, text);
            } else {
                td_set_message(msg, msg_cap, "save failed: %s", err);
            }
            break;
        }
        case TD_PROMPT_NONE:
            break;
        }
        td_prompt_stop(prompt);
    } else if (pr == TD_PROMPT_CANCEL) {
        const char *what = "cancelled";

        if (kind == TD_PROMPT_RENAME_COL)
            what = "rename cancelled";
        else if (kind == TD_PROMPT_COL_WIDTH)
            what = "width cancelled";
        else if (kind == TD_PROMPT_SAVE_SHEET)
            what = "save cancelled";
        td_prompt_stop(prompt);
        td_set_message(msg, msg_cap, "%s", what);
    }
}

int main(int argc, char **argv)
{
    bool demo = false;
    bool stop_flags = false;
    td_csv_opts_t csv_opts;
    const char **files = NULL;
    size_t nfiles = 0;
    td_stack_t stack = { 0 };
    td_term_t term;
    td_key_reader_t reader;
    td_input_t input;
    td_prompt_t prompt;
    td_view_state_t view;
    char msg[512] = "";
    char err[256] = "";
    bool quit = false;
    int rc = 0;

    td_csv_opts_default(&csv_opts);
    if (argc > 1) {
        files = calloc((size_t)argc, sizeof(*files));
        if (!files) {
            fprintf(stderr, "tam-data: out of memory\n");
            return 1;
        }
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!stop_flags && strcmp(arg, "--") == 0) {
            stop_flags = true;
            continue;
        }
        if (!stop_flags && arg[0] == '-' && arg[1] != '\0') {
            if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
                if (td_build_id()[0])
                    printf("tam-data %s (%s)\n", td_version(), td_build_id());
                else
                    printf("tam-data %s\n", td_version());
                free(files);
                return 0;
            }
            if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
                td_usage(stdout);
                free(files);
                return 0;
            }
            if (strcmp(arg, "--demo") == 0) {
                demo = true;
                continue;
            }
            if (strcmp(arg, "--csv") == 0) {
                csv_opts.delimiter = ',';
                csv_opts.strict_tsv = false;
                continue;
            }
            if (strcmp(arg, "--tsv") == 0) {
                csv_opts.delimiter = '\t';
                continue;
            }
            if (strcmp(arg, "--no-header") == 0) {
                csv_opts.has_header = false;
                continue;
            }
            if (strcmp(arg, "-d") == 0 || strcmp(arg, "--delimiter") == 0) {
                if (i + 1 >= argc ||
                    !td_parse_delimiter_arg(argv[i + 1], &csv_opts.delimiter)) {
                    fprintf(stderr, "tam-data: %s requires a single-character delimiter\n",
                            arg);
                    free(files);
                    return 2;
                }
                i++;
                continue;
            }
            if (strncmp(arg, "--delimiter=", 12) == 0) {
                if (!td_parse_delimiter_arg(arg + 12, &csv_opts.delimiter)) {
                    fprintf(stderr, "tam-data: invalid delimiter: %s\n",
                            arg + 12);
                    free(files);
                    return 2;
                }
                continue;
            }
            if (strncmp(arg, "-d", 2) == 0 && arg[2] != '\0') {
                if (!td_parse_delimiter_arg(arg + 2, &csv_opts.delimiter)) {
                    fprintf(stderr, "tam-data: invalid delimiter: %s\n",
                            arg + 2);
                    free(files);
                    return 2;
                }
                continue;
            }
            fprintf(stderr, "tam-data: unknown argument: %s\n", arg);
            td_usage(stderr);
            free(files);
            return 2;
        }
        files[nfiles++] = arg;
    }

    if (nfiles > 0) {
        for (size_t i = 0; i < nfiles; i++) {
            td_sheet_t *sheet;

            if (strcmp(files[i], "-") == 0) {
                sheet = td_csv_load_fd(STDIN_FILENO, "stdin", &csv_opts,
                                       err, sizeof(err));
            } else {
                sheet = td_csv_load_file(files[i], &csv_opts, err, sizeof(err));
            }
            if (!sheet || !td_stack_push(&stack, sheet)) {
                fprintf(stderr, "tam-data: %s\n",
                        err[0] ? err : "failed to load input");
                td_sheet_free(sheet);
                td_stack_free(&stack);
                free(files);
                return 1;
            }
        }
    } else if (!demo && !isatty(STDIN_FILENO)) {
        td_sheet_t *sheet = td_csv_load_fd(STDIN_FILENO, "stdin", &csv_opts,
                                           err, sizeof(err));

        if (!sheet || !td_stack_push(&stack, sheet)) {
            fprintf(stderr, "tam-data: %s\n",
                    err[0] ? err : "failed to load stdin");
            td_sheet_free(sheet);
            free(files);
            return 1;
        }
    } else {
        td_sheet_t *sheet = td_demo_sheet();

        if (!sheet || !td_stack_push(&stack, sheet)) {
            fprintf(stderr, "tam-data: cannot create demo sheet\n");
            td_sheet_free(sheet);
            free(files);
            return 1;
        }
    }
    free(files);

    if (td_term_open(&term, getenv("TERM"), td_locale()) != 0) {
        fprintf(stderr, "tam-data: cannot open controlling terminal\n");
        td_stack_free(&stack);
        return 1;
    }

    td_key_reader_fd(&reader, term.fd_in);
    td_input_init(&input);
    td_prompt_init(&prompt);
    memset(&view, 0, sizeof(view));

    while (!quit) {
        td_sheet_t *active = td_stack_active(&stack);
        int body_rows = term.rows - 2;

        if (body_rows < 1)
            body_rows = 1;
        if (active)
            td_sheet_ensure_cursor_visible(active, body_rows, term.cols);

        view.message = msg;
        view.pending = td_input_pending(&input);
        view.prompt_active = prompt.active;
        view.prompt_label = td_prompt_label(&prompt);
        view.prompt_text = td_prompt_text(&prompt);
        view.prompt_cursor = td_prompt_cursor(&prompt);
        td_view_render(&term, &stack, &view);
        td_term_present(&term);
        msg[0] = '\0';

        td_key_t key = td_key_read(&reader);

        for (;;) {
            td_term_event_t ev = td_term_next_event(&term);

            if (ev == TD_TERM_EVENT_NONE)
                break;
            if (ev == TD_TERM_EVENT_QUIT)
                quit = true;
            else if (ev == TD_TERM_EVENT_SUSPEND)
                (void)raise(SIGSTOP);
            else if (ev == TD_TERM_EVENT_RESIZE ||
                     ev == TD_TERM_EVENT_CONTINUE)
                term.force_full = true;
        }
        if (quit)
            break;
        if (!term.raw_active) {
            td_term_resume(&term);
            term.force_full = true;
        }
        if (key.kind == TD_KEY_NONE)
            continue;

        if (prompt.active) {
            td_prompt_result_t pr = td_prompt_key(&prompt, key);

            active = td_stack_active(&stack);
            td_handle_prompt_result(pr, &prompt, active, msg, sizeof(msg));
            continue;
        }

        td_cmd_t cmd = td_input_feed(&input, key);

        td_handle_command(cmd, &stack, &term, &prompt, msg, sizeof(msg),
                          &quit);
    }

    td_term_close(&term);
    td_prompt_free(&prompt);
    td_stack_free(&stack);
    return rc;
}

#endif /* TD_TAM_DATA_NO_MAIN */
