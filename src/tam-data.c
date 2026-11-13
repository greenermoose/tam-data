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

#include "agg.h"
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

static void td_render_footer(td_term_t *t, const td_sheet_t *s, int start_row,
                             int footer_rows)
{
    for (int f = 0; f < footer_rows; f++) {
        int y = start_row + f;
        int bx = 0;

        for (size_t pass = 0; pass < 2; pass++) {
            size_t start = pass == 0 ? 0 : s->left_col;

            for (size_t c = start; c < s->ncols && bx < t->cols; c++) {
                const td_col_t *col = &s->cols[c];
                int width;
                char text[512];
                int pad = 0;

                if (pass == 0 && !col->is_key)
                    continue;
                if (pass == 1 && col->is_key)
                    continue;
                width = td_sheet_col_width(s, c);
                if (width <= 0)
                    continue;

                if (f < (int)col->naggs) {
                    td_agg_kind_t ak = (td_agg_kind_t)col->aggs[f];
                    td_type_t rt = td_agg_result_type(ak, col->type);
                    td_val_t v = col->agg_vals[f];
                    char vbuf[256];

                    if (v.type == TD_TYPE_NULL)
                        snprintf(vbuf, sizeof(vbuf), "-");
                    else
                        (void)td_val_format(v, rt, vbuf, sizeof(vbuf));
                    snprintf(text, sizeof(text), "%s=%s",
                             td_agg_name(ak), vbuf);
                    if (td_type_is_numeric(rt) || ak == TD_AGG_COUNT ||
                        ak == TD_AGG_DISTINCT || ak == TD_AGG_NULLS) {
                        int tw = (int)td_utf8_width(text);

                        if (tw < width)
                            pad = width - tw;
                    }
                    td_term_put_str(t, y, bx + pad, width - pad, text,
                                    TD_COLOR_DEFAULT, TD_COLOR_DEFAULT,
                                    TD_ATTR_BOLD);
                }
                bx += width;
                if (bx < t->cols) {
                    td_draw_separator(t, y, bx, col->is_key);
                    bx++;
                }
            }
        }
    }
}

void td_view_render(td_term_t *t, td_stack_t *stack,
                    const td_view_state_t *state)
{
    td_sheet_t *s = td_stack_active(stack);
    int header_row = 0;
    int footer_rows;
    int body_rows;
    int x;

    if (!t || !t->back)
        return;

    td_term_clear_back(t);
    td_render_status(t, stack, state);
    if (!s || t->rows < 2)
        return;

    td_sheet_refresh_col_aggs(s);
    footer_rows = td_sheet_footer_rows(s, t->rows);
    body_rows = t->rows - 2 - footer_rows;
    if (body_rows < 0)
        body_rows = 0;
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

    if (footer_rows > 0)
        td_render_footer(t, s, t->rows - 1 - footer_rows, footer_rows);
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

static void td_set_message(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;

    if (!buf || cap == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
}

static bool td_parse_int_range(const char *text, long min_v, long max_v,
                               long *out)
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
    if (*end != '\0' || v < min_v || v > max_v)
        return false;
    *out = v;
    return true;
}

static bool td_exec_fail(td_exec_ctx_t *ctx, char *err, size_t err_cap,
                         const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (err && err_cap > 0)
        snprintf(err, err_cap, "%s", buf);
    if (ctx && ctx->msg && ctx->msg_cap > 0)
        snprintf(ctx->msg, ctx->msg_cap, "%s", buf);
    return false;
}

static bool td_push_derived_sheet(td_exec_ctx_t *ctx, td_sheet_t *derived,
                                  const char *fallback_err, char *err,
                                  size_t err_cap, const char *noun)
{
    if (!derived)
        return td_exec_fail(ctx, err, err_cap, "%s",
                            fallback_err && fallback_err[0]
                                ? fallback_err
                                : "operation failed");
    if (!td_stack_push(ctx->stack, derived)) {
        td_sheet_free(derived);
        return td_exec_fail(ctx, err, err_cap, "out of memory");
    }
    td_set_message(ctx->msg, ctx->msg_cap, "%s (%zu %s)",
                   noun ? noun : derived->name, derived->nrows,
                   derived->nrows == 1 ? "row" : "rows");
    return true;
}

bool td_exec_action(td_cmd_t cmd, const char *input_arg, td_exec_ctx_t *ctx,
                    char *err, size_t err_cap)
{
    td_sheet_t *s;
    size_t orig_row;
    size_t orig_col;
    int footer_rows;
    int body_rows;
    size_t next;
    bool ok = true;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!ctx || !ctx->stack)
        return td_exec_fail(ctx, err, err_cap, "missing execution context");

    s = td_stack_active(ctx->stack);
    if (!s)
        return td_exec_fail(ctx, err, err_cap, "no active sheet");

    orig_row = s->cursor_row;
    orig_col = s->cursor_col;
    footer_rows = td_sheet_footer_rows(s, ctx->term ? ctx->term->rows : 24);
    body_rows = (ctx->term && ctx->term->rows > 2 + footer_rows)
                    ? (ctx->term->rows - 2 - footer_rows)
                    : 22;
    if (body_rows < 1)
        body_rows = 1;

    switch (cmd) {
    case TD_CMD_NONE:
        return true;
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
            td_set_message(ctx->msg, ctx->msg_cap, "no different value below");
        break;
    case TD_CMD_PREV_VALUE:
        if (!td_sheet_jump_value(s, -1))
            td_set_message(ctx->msg, ctx->msg_cap, "no different value above");
        break;
    case TD_CMD_NEXT_NULL:
        if (!td_sheet_jump_null(s, 1))
            td_set_message(ctx->msg, ctx->msg_cap, "no blank cell below");
        break;
    case TD_CMD_PREV_NULL:
        if (!td_sheet_jump_null(s, -1))
            td_set_message(ctx->msg, ctx->msg_cap, "no blank cell above");
        break;
    case TD_CMD_MOVE_COL_LEFT:
        if (!td_sheet_move_col(s, s->cursor_col, -1))
            td_set_message(ctx->msg, ctx->msg_cap, "already at leftmost column");
        break;
    case TD_CMD_MOVE_COL_RIGHT:
        if (!td_sheet_move_col(s, s->cursor_col, 1))
            td_set_message(ctx->msg, ctx->msg_cap,
                           "already at rightmost column");
        break;
    case TD_CMD_HIDE_COL:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column to hide");
        td_sheet_hide_col(s, s->cursor_col);
        td_set_message(ctx->msg, ctx->msg_cap, "column hidden");
        break;
    case TD_CMD_TOGGLE_KEY_COL:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column");
        td_sheet_toggle_key_col(s, s->cursor_col);
        td_set_message(ctx->msg, ctx->msg_cap, "%s key column",
                       s->cols[s->cursor_col].is_key ? "pinned" : "unpinned");
        break;
    case TD_CMD_AUTOSIZE_COL:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column");
        td_sheet_autosize_col(s, s->cursor_col);
        td_set_message(ctx->msg, ctx->msg_cap, "column width %d",
                       td_sheet_col_width(s, s->cursor_col));
        break;
    case TD_CMD_AUTOSIZE_ALL_COLS:
        td_sheet_autosize_all_cols(s);
        td_set_message(ctx->msg, ctx->msg_cap, "autosized all columns");
        break;
    case TD_CMD_SET_COL_WIDTH: {
        long width = 0;

        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column");
        if (!td_parse_int_range(input_arg, 0, 1000, &width))
            return td_exec_fail(ctx, err, err_cap, "invalid width: %s",
                                input_arg ? input_arg : "");
        if (td_sheet_set_col_width(s, s->cursor_col, (int)width)) {
            if (width == 0)
                td_set_message(ctx->msg, ctx->msg_cap, "column hidden");
            else
                td_set_message(ctx->msg, ctx->msg_cap, "column width %ld",
                               width);
        }
        break;
    }
    case TD_CMD_UNHIDE_ALL_COLS:
        td_sheet_unhide_all_cols(s);
        td_set_message(ctx->msg, ctx->msg_cap, "unhid all columns");
        break;
    case TD_CMD_RENAME_COL:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column");
        if (!td_sheet_rename_col(s, s->cursor_col, input_arg ? input_arg : ""))
            return td_exec_fail(ctx, err, err_cap, "rename failed");
        td_set_message(ctx->msg, ctx->msg_cap, "renamed column to %s",
                       input_arg ? input_arg : "");
        break;
    case TD_CMD_SAVE_SHEET: {
        char serr[256];

        if (!input_arg || !input_arg[0])
            return td_exec_fail(ctx, err, err_cap,
                                "save cancelled: empty path");
        if (!td_csv_save_sheet(s, input_arg, '\0', serr, sizeof(serr)))
            return td_exec_fail(ctx, err, err_cap, "save failed: %s", serr);
        td_set_message(ctx->msg, ctx->msg_cap, "saved %zu rows to %s",
                       s->nrows, input_arg);
        break;
    }
    case TD_CMD_TYPE_ANY:
    case TD_CMD_TYPE_STR:
    case TD_CMD_TYPE_INT:
    case TD_CMD_TYPE_FLOAT:
    case TD_CMD_TYPE_CURRENCY:
    case TD_CMD_TYPE_DATE: {
        td_type_t t = TD_TYPE_ANY;
        const char *label = "any";

        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column");
        if (cmd == TD_CMD_TYPE_STR) {
            t = TD_TYPE_STR;
            label = "string";
        } else if (cmd == TD_CMD_TYPE_INT) {
            t = TD_TYPE_INT;
            label = "int";
        } else if (cmd == TD_CMD_TYPE_FLOAT) {
            t = TD_TYPE_FLOAT;
            label = "float";
        } else if (cmd == TD_CMD_TYPE_CURRENCY) {
            t = TD_TYPE_CURRENCY;
            label = "currency";
        } else if (cmd == TD_CMD_TYPE_DATE) {
            t = TD_TYPE_DATE;
            label = "date";
        }
        td_sheet_set_col_type(s, s->cursor_col, t);
        td_set_message(ctx->msg, ctx->msg_cap, "column type %s", label);
        break;
    }
    case TD_CMD_POP_SHEET: {
        td_sheet_t *popped;

        (void)td_cmdlog_record(ctx->cmdlog, s, orig_row, orig_col, cmd,
                               input_arg, NULL);
        popped = td_stack_pop(ctx->stack);
        td_sheet_free(popped);
        if (ctx->stack->len == 0) {
            if (ctx->quit)
                *ctx->quit = true;
        } else {
            td_set_message(ctx->msg, ctx->msg_cap, "sheet closed");
        }
        return true;
    }
    case TD_CMD_QUIT_ALL:
        if (ctx->quit)
            *ctx->quit = true;
        break;
    case TD_CMD_SELECT_ROW:
        if (s->nrows > 0 && s->cursor_row < s->nrows)
            td_sheet_select_row(s, s->row_order[s->cursor_row]);
        break;
    case TD_CMD_UNSELECT_ROW:
        if (s->nrows > 0 && s->cursor_row < s->nrows)
            td_sheet_unselect_row(s, s->row_order[s->cursor_row]);
        break;
    case TD_CMD_TOGGLE_ROW:
        if (s->nrows > 0 && s->cursor_row < s->nrows)
            td_sheet_toggle_row(s, s->row_order[s->cursor_row]);
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
    case TD_CMD_SELECT_REGEX:
    case TD_CMD_UNSELECT_REGEX:
    case TD_CMD_SELECT_ALL_REGEX:
    case TD_CMD_UNSELECT_ALL_REGEX: {
        bool all_cols = cmd == TD_CMD_SELECT_ALL_REGEX ||
                        cmd == TD_CMD_UNSELECT_ALL_REGEX;
        bool select = cmd == TD_CMD_SELECT_REGEX ||
                      cmd == TD_CMD_SELECT_ALL_REGEX;
        size_t matched = 0;
        char rerr[256];

        if (!input_arg || !input_arg[0])
            return td_exec_fail(ctx, err, err_cap, "empty regex");
        if (!td_sheet_select_regex(s, input_arg, all_cols, select, &matched,
                                   rerr, sizeof(rerr)))
            return td_exec_fail(ctx, err, err_cap, "invalid regex: %s", rerr);
        td_set_message(ctx->msg, ctx->msg_cap, "%s %zu %s",
                       select ? "selected" : "unselected", matched,
                       matched == 1 ? "row" : "rows");
        break;
    }
    case TD_CMD_SEARCH_FWD:
    case TD_CMD_SEARCH_BWD:
    case TD_CMD_SEARCH_ALL_FWD:
    case TD_CMD_SEARCH_ALL_BWD: {
        int dir = (cmd == TD_CMD_SEARCH_FWD || cmd == TD_CMD_SEARCH_ALL_FWD)
                      ? 1
                      : -1;
        bool all_cols = cmd == TD_CMD_SEARCH_ALL_FWD ||
                        cmd == TD_CMD_SEARCH_ALL_BWD;
        char serr[256];

        if (!td_sheet_search(s, input_arg, dir, all_cols, serr, sizeof(serr)))
            return td_exec_fail(ctx, err, err_cap, "%s", serr);
        break;
    }
    case TD_CMD_SEARCH_NEXT:
    case TD_CMD_SEARCH_PREV: {
        char serr[256];
        int dir_mult = cmd == TD_CMD_SEARCH_NEXT ? 1 : -1;

        if (!td_sheet_search_next(s, dir_mult, serr, sizeof(serr)))
            return td_exec_fail(ctx, err, err_cap, "%s", serr);
        break;
    }
    case TD_CMD_DUP_SELECTED: {
        td_sheet_t *dup;

        if (s->nselected == 0)
            return td_exec_fail(ctx, err, err_cap, "no rows selected");
        dup = td_sheet_dup_selected(s, "[selected]");
        if (!dup || !td_stack_push(ctx->stack, dup)) {
            td_sheet_free(dup);
            return td_exec_fail(ctx, err, err_cap,
                                "duplicate failed: out of memory");
        }
        td_set_message(ctx->msg, ctx->msg_cap, "pushed %zu selected %s",
                       dup->nrows, dup->nrows == 1 ? "row" : "rows");
        break;
    }
    case TD_CMD_DELETE_ROW:
        if (s->nrows == 0 || s->cursor_row >= s->nrows)
            return td_exec_fail(ctx, err, err_cap, "no rows to delete");
        if (ctx->clip)
            (void)td_clipboard_yank_row(ctx->clip, s, s->cursor_row);
        if (td_sheet_delete_row(s, s->cursor_row))
            td_set_message(ctx->msg, ctx->msg_cap, "deleted 1 row");
        break;
    case TD_CMD_DELETE_SELECTED: {
        size_t n;

        if (s->nselected == 0)
            return td_exec_fail(ctx, err, err_cap, "no rows selected");
        if (ctx->clip)
            (void)td_clipboard_yank_selected(ctx->clip, s);
        n = td_sheet_delete_selected(s);
        td_set_message(ctx->msg, ctx->msg_cap, "deleted %zu selected %s", n,
                       n == 1 ? "row" : "rows");
        break;
    }
    case TD_CMD_SORT_ASC:
    case TD_CMD_SORT_DESC: {
        bool desc = cmd == TD_CMD_SORT_DESC;

        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column");
        if (td_sheet_sort_col(s, s->cursor_col, desc)) {
            const char *cname = s->cols[s->cursor_col].name
                                    ? s->cols[s->cursor_col].name
                                    : "";

            td_set_message(ctx->msg, ctx->msg_cap, "sorted %s by %s",
                           desc ? "descending" : "ascending", cname);
        }
        break;
    }
    case TD_CMD_SORT_KEYS_ASC:
    case TD_CMD_SORT_KEYS_DESC: {
        bool desc = cmd == TD_CMD_SORT_KEYS_DESC;
        bool has_keys = false;

        if (s->ncols == 0)
            return td_exec_fail(ctx, err, err_cap, "no column");
        for (size_t c = 0; c < s->ncols; c++) {
            if (s->cols[c].is_key && s->cols[c].width != 0) {
                has_keys = true;
                break;
            }
        }
        if (td_sheet_sort_keys(s, desc)) {
            if (has_keys) {
                td_set_message(ctx->msg, ctx->msg_cap,
                               "sorted %s by key columns",
                               desc ? "descending" : "ascending");
            } else {
                const char *cname = s->cols[s->cursor_col].name
                                        ? s->cols[s->cursor_col].name
                                        : "";

                td_set_message(ctx->msg, ctx->msg_cap, "sorted %s by %s",
                               desc ? "descending" : "ascending", cname);
            }
        }
        break;
    }
    case TD_CMD_EDIT_CELL:
        if (s->nrows == 0 || s->ncols == 0 ||
            s->cursor_row >= s->nrows || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no cell to edit");
        if (!td_sheet_edit_cell(s, s->cursor_row, s->cursor_col,
                                input_arg ? input_arg : ""))
            return td_exec_fail(ctx, err, err_cap, "edit failed");
        break;
    case TD_CMD_EDIT_SELECTED: {
        size_t n;

        if (s->nselected == 0)
            return td_exec_fail(ctx, err, err_cap, "no rows selected");
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column");
        n = td_sheet_edit_selected(s, s->cursor_col,
                                   input_arg ? input_arg : "");
        td_set_message(ctx->msg, ctx->msg_cap, "updated %zu selected %s", n,
                       n == 1 ? "row" : "rows");
        break;
    }
    case TD_CMD_CLEAR_CELL:
        if (s->nrows == 0 || s->ncols == 0 ||
            s->cursor_row >= s->nrows || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no cell to clear");
        if (td_sheet_clear_cell(s, s->cursor_row, s->cursor_col))
            td_set_message(ctx->msg, ctx->msg_cap, "cleared cell");
        break;
    case TD_CMD_CLEAR_SELECTED: {
        size_t n;

        if (s->nselected == 0)
            return td_exec_fail(ctx, err, err_cap, "no rows selected");
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column");
        n = td_sheet_clear_selected(s, s->cursor_col);
        td_set_message(ctx->msg, ctx->msg_cap, "cleared %zu selected %s", n,
                       n == 1 ? "cell" : "cells");
        break;
    }
    case TD_CMD_ADD_ROW: {
        size_t vis_pos = s->nrows == 0 ? 0 : (s->cursor_row + 1);

        if (!td_sheet_insert_rows(s, vis_pos, 1))
            return td_exec_fail(ctx, err, err_cap, "add row failed");
        td_set_message(ctx->msg, ctx->msg_cap, "added 1 row");
        break;
    }
    case TD_CMD_ADD_ROWS: {
        long count = 0;
        size_t vis_pos = s->nrows == 0 ? 0 : (s->cursor_row + 1);

        if (!td_parse_int_range(input_arg, 1, 100000, &count))
            return td_exec_fail(ctx, err, err_cap, "invalid row count: %s",
                                input_arg ? input_arg : "");
        if (!td_sheet_insert_rows(s, vis_pos, (size_t)count))
            return td_exec_fail(ctx, err, err_cap, "add rows failed");
        td_set_message(ctx->msg, ctx->msg_cap, "added %ld %s", count,
                       count == 1 ? "row" : "rows");
        break;
    }
    case TD_CMD_ADD_COL: {
        size_t idx = td_sheet_append_blank_col(s);

        if (idx == SIZE_MAX)
            return td_exec_fail(ctx, err, err_cap, "add column failed");
        td_set_message(ctx->msg, ctx->msg_cap, "added column %s",
                       s->cols[idx].name);
        break;
    }
    case TD_CMD_DUP_ROW:
        if (s->nrows == 0 || s->cursor_row >= s->nrows)
            return td_exec_fail(ctx, err, err_cap, "no row to duplicate");
        if (!td_sheet_dup_row(s, s->cursor_row))
            return td_exec_fail(ctx, err, err_cap, "duplicate row failed");
        td_set_message(ctx->msg, ctx->msg_cap, "duplicated row");
        break;
    case TD_CMD_DUP_COL:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column to duplicate");
        if (!td_sheet_dup_col(s, s->cursor_col))
            return td_exec_fail(ctx, err, err_cap, "duplicate column failed");
        td_set_message(ctx->msg, ctx->msg_cap, "duplicated column");
        break;
    case TD_CMD_YANK_ROW:
        if (s->nrows == 0 || s->cursor_row >= s->nrows)
            return td_exec_fail(ctx, err, err_cap, "no row to yank");
        if (!ctx->clip ||
            !td_clipboard_yank_row(ctx->clip, s, s->cursor_row))
            return td_exec_fail(ctx, err, err_cap, "yank failed");
        td_set_message(ctx->msg, ctx->msg_cap, "yanked 1 row");
        break;
    case TD_CMD_YANK_SELECTED: {
        size_t n;

        if (s->nselected == 0)
            return td_exec_fail(ctx, err, err_cap, "no rows selected");
        n = ctx->clip ? td_clipboard_yank_selected(ctx->clip, s) : 0;
        if (n == 0)
            return td_exec_fail(ctx, err, err_cap, "yank failed");
        td_set_message(ctx->msg, ctx->msg_cap, "yanked %zu selected %s", n,
                       n == 1 ? "row" : "rows");
        break;
    }
    case TD_CMD_YANK_CELL:
        if (s->nrows == 0 || s->ncols == 0 ||
            s->cursor_row >= s->nrows || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no cell to yank");
        if (!ctx->clip ||
            !td_clipboard_yank_cell(ctx->clip, s, s->cursor_row, s->cursor_col))
            return td_exec_fail(ctx, err, err_cap, "yank failed");
        td_set_message(ctx->msg, ctx->msg_cap, "yanked cell");
        break;
    case TD_CMD_YANK_SELECTED_CELLS: {
        size_t n;

        if (s->nselected == 0)
            return td_exec_fail(ctx, err, err_cap, "no rows selected");
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column");
        n = ctx->clip ? td_clipboard_yank_selected_cells(ctx->clip, s,
                                                         s->cursor_col)
                      : 0;
        if (n == 0)
            return td_exec_fail(ctx, err, err_cap, "yank failed");
        td_set_message(ctx->msg, ctx->msg_cap, "yanked %zu %s", n,
                       n == 1 ? "cell" : "cells");
        break;
    }
    case TD_CMD_PASTE_AFTER:
    case TD_CMD_PASTE_BEFORE:
    case TD_CMD_PASTE_CELL: {
        bool after = cmd != TD_CMD_PASTE_BEFORE;
        bool force_cell = cmd == TD_CMD_PASTE_CELL;
        size_t pasted = 0;
        char perr[256];

        if (!td_sheet_paste(s, ctx->clip, after, force_cell, &pasted,
                            perr, sizeof(perr)))
            return td_exec_fail(ctx, err, err_cap, "%s", perr);
        td_set_message(ctx->msg, ctx->msg_cap, "pasted %zu %s", pasted,
                       pasted == 1 ? "item" : "items");
        break;
    }
    case TD_CMD_UNDO: {
        char desc[64];

        if (!td_sheet_undo(s, desc, sizeof(desc)))
            return td_exec_fail(ctx, err, err_cap, "nothing to undo");
        td_set_message(ctx->msg, ctx->msg_cap, "undid: %s", desc);
        break;
    }
    case TD_CMD_REDO: {
        char desc[64];

        if (!td_sheet_redo(s, desc, sizeof(desc)))
            return td_exec_fail(ctx, err, err_cap, "nothing to redo");
        td_set_message(ctx->msg, ctx->msg_cap, "redid: %s", desc);
        break;
    }
    case TD_CMD_SHOW_CMDLOG: {
        td_sheet_t *cl = td_cmdlog_to_sheet(ctx->cmdlog);

        if (!cl)
            return td_exec_fail(ctx, err, err_cap,
                                "cmdlog failed: out of memory");
        if (s->name && strcmp(s->name, "cmdlog") == 0)
            td_sheet_free(td_stack_pop(ctx->stack));
        if (!td_stack_push(ctx->stack, cl)) {
            td_sheet_free(cl);
            return td_exec_fail(ctx, err, err_cap,
                                "cmdlog failed: out of memory");
        }
        td_set_message(ctx->msg, ctx->msg_cap, "command log (%zu %s)",
                       cl->nrows, cl->nrows == 1 ? "entry" : "entries");
        return true;
    }
    case TD_CMD_FREQ_COL: {
        char ferr[256];
        td_sheet_t *freq;

        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column");
        freq = td_agg_freq_sheet(s, &s->cursor_col, 1, ferr, sizeof(ferr));
        ok = td_push_derived_sheet(ctx, freq, ferr, err, err_cap,
                                   "frequency table");
        break;
    }
    case TD_CMD_FREQ_KEYS: {
        char ferr[256];
        size_t *keys = NULL;
        size_t nkeys = 0;
        td_sheet_t *freq;

        if (s->ncols == 0)
            return td_exec_fail(ctx, err, err_cap, "no column");
        keys = malloc(s->ncols * sizeof(*keys));
        if (!keys)
            return td_exec_fail(ctx, err, err_cap, "out of memory");
        for (size_t c = 0; c < s->ncols; c++) {
            if (s->cols[c].is_key && s->cols[c].width != 0)
                keys[nkeys++] = c;
        }
        if (nkeys == 0 && s->cursor_col < s->ncols)
            keys[nkeys++] = s->cursor_col;
        freq = td_agg_freq_sheet(s, keys, nkeys, ferr, sizeof(ferr));
        free(keys);
        ok = td_push_derived_sheet(ctx, freq, ferr, err, err_cap,
                                   "frequency table");
        break;
    }
    case TD_CMD_AGGREGATOR: {
        char msg_buf[256] = "";
        char aerr[256] = "";

        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column");
        if (!td_sheet_toggle_agg(s, s->cursor_col, input_arg, msg_buf,
                                 sizeof(msg_buf), aerr, sizeof(aerr)))
            return td_exec_fail(ctx, err, err_cap, "%s", aerr);
        td_set_message(ctx->msg, ctx->msg_cap, "%s", msg_buf);
        break;
    }
    case TD_CMD_AGGREGATOR_ONE: {
        td_agg_kind_t ak;
        td_val_t v;
        td_type_t rt;
        char vbuf[256];
        const char *cname;

        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column");
        if (!td_agg_parse(input_arg, &ak))
            return td_exec_fail(ctx, err, err_cap, "unknown aggregator: %s",
                                input_arg ? input_arg : "");
        v = td_agg_compute_rows(s, s->cursor_col, NULL, 0, ak);
        rt = td_agg_result_type(ak, s->cols[s->cursor_col].type);
        cname = s->cols[s->cursor_col].name ? s->cols[s->cursor_col].name : "";
        if (v.type == TD_TYPE_NULL && td_agg_is_numeric_only(ak) &&
            !td_type_is_numeric(s->cols[s->cursor_col].type)) {
            td_set_message(ctx->msg, ctx->msg_cap,
                           "%s %s = (untyped: set # / %% / $)",
                           cname, td_agg_name(ak));
        } else if (v.type == TD_TYPE_NULL) {
            td_set_message(ctx->msg, ctx->msg_cap, "%s %s = (null)",
                           cname, td_agg_name(ak));
        } else {
            (void)td_val_format(v, rt, vbuf, sizeof(vbuf));
            td_set_message(ctx->msg, ctx->msg_cap, "%s %s = %s",
                           cname, td_agg_name(ak), vbuf);
        }
        break;
    }
    case TD_CMD_DESCRIBE: {
        char derr[256];
        td_sheet_t *desc = td_agg_describe_sheet(s, derr, sizeof(derr));

        ok = td_push_derived_sheet(ctx, desc, derr, err, err_cap,
                                   "describe sheet");
        break;
    }
    case TD_CMD_PIVOT: {
        char perr[256];
        td_sheet_t *piv;

        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return td_exec_fail(ctx, err, err_cap, "no column");
        piv = td_agg_pivot_sheet(s, s->cursor_col, perr, sizeof(perr));
        ok = td_push_derived_sheet(ctx, piv, perr, err, err_cap,
                                   "pivot sheet");
        break;
    }
    case TD_CMD_MELT: {
        char merr[256];
        td_sheet_t *melt = td_agg_melt_sheet(s, merr, sizeof(merr));

        ok = td_push_derived_sheet(ctx, melt, merr, err, err_cap,
                                   "melt sheet");
        break;
    }
    case TD_CMD_JOIN: {
        td_join_kind_t jk;
        char jerr[256];
        td_sheet_t *joined;

        if (ctx->stack->len < 2)
            return td_exec_fail(ctx, err, err_cap,
                                "need at least 2 sheets on stack to join");
        if (!td_join_parse(input_arg, &jk))
            return td_exec_fail(
                ctx, err, err_cap,
                "invalid join type: %s (use inner/outer/full/diff/append/extend)",
                input_arg ? input_arg : "");
        joined = td_agg_join_sheets(ctx->stack->sheets[ctx->stack->len - 2],
                                    ctx->stack->sheets[ctx->stack->len - 1],
                                    jk, jerr, sizeof(jerr));
        ok = td_push_derived_sheet(ctx, joined, jerr, err, err_cap,
                                   "joined sheet");
        break;
    }
    case TD_CMD_DIVE_ROW: {
        char derr[256];
        td_sheet_t *child = td_agg_dive_row(s, ctx->stack, derr, sizeof(derr));

        ok = td_push_derived_sheet(ctx, child, derr, err, err_cap,
                                   "filtered rows");
        break;
    }
    case TD_CMD_REDRAW:
        if (ctx->term)
            ctx->term->force_full = true;
        return true;
    }

    if (ok && ctx->cmdlog)
        (void)td_cmdlog_record(ctx->cmdlog, s, orig_row, orig_col, cmd,
                               input_arg, NULL);
    return ok;
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
        "  -p, --play FILE       replay commands from a .vd / .tsv script\n"
        "  -b, --batch           exit after -p replay without opening the TUI\n"
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
        "  | \\ g| g\\             select / unselect rows by regex (col / all)\n"
        "  / ? g/ g? n N         regex search (fwd/bwd, col/all, next/prev)\n"
        "  \"                     duplicate selected rows into new sheet\n"
        "  e / ge                edit cell / set selected cells in column\n"
        "  zd / gzd              clear cell / clear selected cells\n"
        "  a / ga / za           add blank row / N rows / blank column\n"
        "  g\" / z\"               duplicate current row / column in-place\n"
        "  d / gd                delete current row / selected rows (yanks)\n"
        "  y / gy / zy / gzy     yank row / selected rows / cell / selected cells\n"
        "  p / P / zp            paste after / before / into cell(s)\n"
        "  U / R                 undo / redo last action on active sheet\n"
        "  D                     open Command Log sheet (cmdlog)\n"
        "  F / gF / Enter        frequency table (col / keys) / dive into bin\n"
        "  + / z+                attach persistent / calculate one-off aggregator\n"
        "  I                     open Describe statistical summary sheet\n"
        "  W / M                 pivot by key & cursor col / melt wide table\n"
        "  &                     join top two sheets (inner/outer/full/diff/append/extend)\n"
        "  [ ] / g[ g]           sort asc / desc (current col / key cols)\n"
        "  !                     pin or unpin the current column as a key\n"
        "  - / gv                hide current column / unhide all columns\n"
        "  _ / g_ / z_           autosize column / all columns / set width\n"
        "  ^                     rename the current column\n"
        "  # %% $ @ ~ z~          set column type (int, float, currency,\n"
        "                        date, string, any)\n"
        "  Ctrl+S                save sheet atomically to CSV/TSV/.vd\n"
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

static void td_start_edit_cell_prompt(td_prompt_t *prompt, const td_sheet_t *s)
{
    char stack_buf[512];
    td_val_t v;
    td_type_t type;
    size_t n;

    if (!s || s->nrows == 0 || s->ncols == 0 ||
        s->cursor_row >= s->nrows || s->cursor_col >= s->ncols)
        return;

    v = td_sheet_get_cell(s, s->row_order[s->cursor_row], s->cursor_col);
    type = s->cols[s->cursor_col].type;
    n = td_val_format(v, type, stack_buf, sizeof(stack_buf));
    if (n < sizeof(stack_buf)) {
        td_prompt_start_kind(prompt, TD_PROMPT_EDIT_CELL, "edit cell:",
                             stack_buf);
        return;
    }

    char *dyn = malloc(n + 1);

    if (!dyn) {
        td_prompt_start_kind(prompt, TD_PROMPT_EDIT_CELL, "edit cell:", "");
        return;
    }
    (void)td_val_format(v, type, dyn, n + 1);
    td_prompt_start_kind(prompt, TD_PROMPT_EDIT_CELL, "edit cell:", dyn);
    free(dyn);
}

static void td_handle_command(td_cmd_t cmd, td_exec_ctx_t *ctx,
                              td_prompt_t *prompt)
{
    td_sheet_t *s = td_stack_active(ctx->stack);

    if (!s)
        return;

    switch (cmd) {
    case TD_CMD_NONE:
        return;
    case TD_CMD_SET_COL_WIDTH: {
        char wbuf[32];

        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return;
        snprintf(wbuf, sizeof(wbuf), "%d",
                 td_sheet_col_width(s, s->cursor_col));
        td_prompt_start_kind(prompt, TD_PROMPT_COL_WIDTH, "column width:",
                             wbuf);
        return;
    }
    case TD_CMD_RENAME_COL:
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return;
        td_prompt_start_kind(prompt, TD_PROMPT_RENAME_COL, "rename column:",
                             s->cols[s->cursor_col].name);
        return;
    case TD_CMD_SAVE_SHEET: {
        const char *def = (s->source_path && s->source_path[0] &&
                           strcmp(s->source_path, "(built-in)") != 0)
                              ? s->source_path
                              : "output.csv";

        td_prompt_start_kind(prompt, TD_PROMPT_SAVE_SHEET, "save to:", def);
        return;
    }
    case TD_CMD_SELECT_REGEX:
        td_prompt_start_kind(prompt, TD_PROMPT_SELECT_REGEX,
                             "select regex:", "");
        return;
    case TD_CMD_UNSELECT_REGEX:
        td_prompt_start_kind(prompt, TD_PROMPT_UNSELECT_REGEX,
                             "unselect regex:", "");
        return;
    case TD_CMD_SELECT_ALL_REGEX:
        td_prompt_start_kind(prompt, TD_PROMPT_SELECT_ALL_REGEX,
                             "select all regex:", "");
        return;
    case TD_CMD_UNSELECT_ALL_REGEX:
        td_prompt_start_kind(prompt, TD_PROMPT_UNSELECT_ALL_REGEX,
                             "unselect all regex:", "");
        return;
    case TD_CMD_SEARCH_FWD:
        td_prompt_start_kind(prompt, TD_PROMPT_SEARCH_FWD, "/", "");
        return;
    case TD_CMD_SEARCH_BWD:
        td_prompt_start_kind(prompt, TD_PROMPT_SEARCH_BWD, "?", "");
        return;
    case TD_CMD_SEARCH_ALL_FWD:
        td_prompt_start_kind(prompt, TD_PROMPT_SEARCH_ALL_FWD, "g/", "");
        return;
    case TD_CMD_SEARCH_ALL_BWD:
        td_prompt_start_kind(prompt, TD_PROMPT_SEARCH_ALL_BWD, "g?", "");
        return;
    case TD_CMD_EDIT_CELL:
        if (s->nrows == 0 || s->ncols == 0 ||
            s->cursor_row >= s->nrows || s->cursor_col >= s->ncols) {
            td_set_message(ctx->msg, ctx->msg_cap, "no cell to edit");
            return;
        }
        td_start_edit_cell_prompt(prompt, s);
        return;
    case TD_CMD_EDIT_SELECTED:
        if (s->nselected == 0) {
            td_set_message(ctx->msg, ctx->msg_cap, "no rows selected");
            return;
        }
        if (s->ncols == 0 || s->cursor_col >= s->ncols)
            return;
        td_prompt_start_kind(prompt, TD_PROMPT_EDIT_SELECTED,
                             "set selected to:", "");
        return;
    case TD_CMD_ADD_ROWS:
        td_prompt_start_kind(prompt, TD_PROMPT_ADD_ROWS, "add rows:", "1");
        return;
    case TD_CMD_AGGREGATOR:
        if (s->ncols == 0 || s->cursor_col >= s->ncols) {
            td_set_message(ctx->msg, ctx->msg_cap, "no column");
            return;
        }
        td_prompt_start_kind(prompt, TD_PROMPT_AGGREGATOR,
                             "add aggregator:", "");
        return;
    case TD_CMD_AGGREGATOR_ONE:
        if (s->ncols == 0 || s->cursor_col >= s->ncols) {
            td_set_message(ctx->msg, ctx->msg_cap, "no column");
            return;
        }
        td_prompt_start_kind(prompt, TD_PROMPT_AGGREGATOR_ONE,
                             "calc aggregator:", "");
        return;
    case TD_CMD_JOIN:
        if (ctx->stack->len < 2) {
            td_set_message(ctx->msg, ctx->msg_cap,
                           "need at least 2 sheets on stack to join");
            return;
        }
        td_prompt_start_kind(prompt, TD_PROMPT_JOIN,
                             "join (inner/outer/full/diff/append/extend):",
                             "inner");
        return;
    default:
        (void)td_exec_action(cmd, NULL, ctx, NULL, 0);
        return;
    }
}

static td_cmd_t td_prompt_to_cmd(td_prompt_kind_t kind)
{
    switch (kind) {
    case TD_PROMPT_RENAME_COL:         return TD_CMD_RENAME_COL;
    case TD_PROMPT_COL_WIDTH:          return TD_CMD_SET_COL_WIDTH;
    case TD_PROMPT_SAVE_SHEET:         return TD_CMD_SAVE_SHEET;
    case TD_PROMPT_SELECT_REGEX:       return TD_CMD_SELECT_REGEX;
    case TD_PROMPT_UNSELECT_REGEX:     return TD_CMD_UNSELECT_REGEX;
    case TD_PROMPT_SELECT_ALL_REGEX:   return TD_CMD_SELECT_ALL_REGEX;
    case TD_PROMPT_UNSELECT_ALL_REGEX: return TD_CMD_UNSELECT_ALL_REGEX;
    case TD_PROMPT_SEARCH_FWD:         return TD_CMD_SEARCH_FWD;
    case TD_PROMPT_SEARCH_BWD:         return TD_CMD_SEARCH_BWD;
    case TD_PROMPT_SEARCH_ALL_FWD:     return TD_CMD_SEARCH_ALL_FWD;
    case TD_PROMPT_SEARCH_ALL_BWD:     return TD_CMD_SEARCH_ALL_BWD;
    case TD_PROMPT_EDIT_CELL:          return TD_CMD_EDIT_CELL;
    case TD_PROMPT_EDIT_SELECTED:      return TD_CMD_EDIT_SELECTED;
    case TD_PROMPT_ADD_ROWS:           return TD_CMD_ADD_ROWS;
    case TD_PROMPT_AGGREGATOR:         return TD_CMD_AGGREGATOR;
    case TD_PROMPT_AGGREGATOR_ONE:     return TD_CMD_AGGREGATOR_ONE;
    case TD_PROMPT_JOIN:               return TD_CMD_JOIN;
    default:                           return TD_CMD_NONE;
    }
}

static void td_handle_prompt_result(td_prompt_result_t pr, td_prompt_t *prompt,
                                    td_exec_ctx_t *ctx)
{
    td_prompt_kind_t kind = td_prompt_kind(prompt);

    if (pr == TD_PROMPT_ACCEPT) {
        const char *text = td_prompt_text(prompt);
        td_cmd_t cmd = td_prompt_to_cmd(kind);

        if (cmd != TD_CMD_NONE)
            (void)td_exec_action(cmd, text, ctx, NULL, 0);
        td_prompt_stop(prompt);
    } else if (pr == TD_PROMPT_CANCEL) {
        const char *what = "cancelled";

        if (kind == TD_PROMPT_RENAME_COL)
            what = "rename cancelled";
        else if (kind == TD_PROMPT_COL_WIDTH)
            what = "width cancelled";
        else if (kind == TD_PROMPT_SAVE_SHEET)
            what = "save cancelled";
        else if (kind == TD_PROMPT_SEARCH_FWD ||
                 kind == TD_PROMPT_SEARCH_BWD ||
                 kind == TD_PROMPT_SEARCH_ALL_FWD ||
                 kind == TD_PROMPT_SEARCH_ALL_BWD)
            what = "search cancelled";
        else if (kind == TD_PROMPT_SELECT_REGEX ||
                 kind == TD_PROMPT_UNSELECT_REGEX ||
                 kind == TD_PROMPT_SELECT_ALL_REGEX ||
                 kind == TD_PROMPT_UNSELECT_ALL_REGEX)
            what = "selection cancelled";
        else if (kind == TD_PROMPT_EDIT_CELL ||
                 kind == TD_PROMPT_EDIT_SELECTED)
            what = "edit cancelled";
        else if (kind == TD_PROMPT_ADD_ROWS)
            what = "add rows cancelled";
        else if (kind == TD_PROMPT_AGGREGATOR ||
                 kind == TD_PROMPT_AGGREGATOR_ONE)
            what = "aggregator cancelled";
        else if (kind == TD_PROMPT_JOIN)
            what = "join cancelled";
        td_prompt_stop(prompt);
        td_set_message(ctx->msg, ctx->msg_cap, "%s", what);
    }
}

static bool td_replay_exec_cb(td_cmd_t cmd, const char *input, void *ctx,
                              char *err, size_t err_cap)
{
    return td_exec_action(cmd, input, (td_exec_ctx_t *)ctx, err, err_cap);
}

int main(int argc, char **argv)
{
    bool demo = false;
    bool batch = false;
    bool stop_flags = false;
    const char *play_path = NULL;
    td_csv_opts_t csv_opts;
    const char **files = NULL;
    size_t nfiles = 0;
    td_stack_t stack = { 0 };
    td_clipboard_t clip;
    td_cmdlog_t cmdlog;
    td_term_t term;
    td_key_reader_t reader;
    td_input_t input;
    td_prompt_t prompt;
    td_view_state_t view;
    td_exec_ctx_t exec_ctx;
    char msg[512] = "";
    char err[256] = "";
    bool quit = false;
    int rc = 0;

    td_csv_opts_default(&csv_opts);
    td_clipboard_init(&clip);
    td_cmdlog_init(&cmdlog);
    if (argc > 1) {
        files = calloc((size_t)argc, sizeof(*files));
        if (!files) {
            fprintf(stderr, "tam-data: out of memory\n");
            td_clipboard_free(&clip);
            td_cmdlog_free(&cmdlog);
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
                td_clipboard_free(&clip);
                td_cmdlog_free(&cmdlog);
                return 0;
            }
            if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
                td_usage(stdout);
                free(files);
                td_clipboard_free(&clip);
                td_cmdlog_free(&cmdlog);
                return 0;
            }
            if (strcmp(arg, "--demo") == 0) {
                demo = true;
                continue;
            }
            if (strcmp(arg, "--batch") == 0 || strcmp(arg, "-b") == 0) {
                batch = true;
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
            if (strcmp(arg, "-p") == 0 || strcmp(arg, "--play") == 0) {
                if (i + 1 >= argc || !argv[i + 1][0]) {
                    fprintf(stderr, "tam-data: %s requires a script path\n",
                            arg);
                    free(files);
                    td_clipboard_free(&clip);
                    td_cmdlog_free(&cmdlog);
                    return 2;
                }
                play_path = argv[++i];
                continue;
            }
            if (strncmp(arg, "--play=", 7) == 0) {
                if (!arg[7]) {
                    fprintf(stderr, "tam-data: --play requires a script path\n");
                    free(files);
                    td_clipboard_free(&clip);
                    td_cmdlog_free(&cmdlog);
                    return 2;
                }
                play_path = arg + 7;
                continue;
            }
            if (strcmp(arg, "-d") == 0 || strcmp(arg, "--delimiter") == 0) {
                if (i + 1 >= argc ||
                    !td_parse_delimiter_arg(argv[i + 1], &csv_opts.delimiter)) {
                    fprintf(stderr, "tam-data: %s requires a single-character delimiter\n",
                            arg);
                    free(files);
                    td_clipboard_free(&clip);
                    td_cmdlog_free(&cmdlog);
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
                    td_clipboard_free(&clip);
                    td_cmdlog_free(&cmdlog);
                    return 2;
                }
                continue;
            }
            if (strncmp(arg, "-d", 2) == 0 && arg[2] != '\0') {
                if (!td_parse_delimiter_arg(arg + 2, &csv_opts.delimiter)) {
                    fprintf(stderr, "tam-data: invalid delimiter: %s\n",
                            arg + 2);
                    free(files);
                    td_clipboard_free(&clip);
                    td_cmdlog_free(&cmdlog);
                    return 2;
                }
                continue;
            }
            fprintf(stderr, "tam-data: unknown argument: %s\n", arg);
            td_usage(stderr);
            free(files);
            td_clipboard_free(&clip);
            td_cmdlog_free(&cmdlog);
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
                td_clipboard_free(&clip);
                td_cmdlog_free(&cmdlog);
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
            td_clipboard_free(&clip);
            td_cmdlog_free(&cmdlog);
            return 1;
        }
    } else {
        td_sheet_t *sheet = td_demo_sheet();

        if (!sheet || !td_stack_push(&stack, sheet)) {
            fprintf(stderr, "tam-data: cannot create demo sheet\n");
            td_sheet_free(sheet);
            free(files);
            td_clipboard_free(&clip);
            td_cmdlog_free(&cmdlog);
            return 1;
        }
    }
    free(files);

    exec_ctx.stack = &stack;
    exec_ctx.clip = &clip;
    exec_ctx.cmdlog = &cmdlog;
    exec_ctx.term = NULL;
    exec_ctx.msg = msg;
    exec_ctx.msg_cap = sizeof(msg);
    exec_ctx.quit = &quit;

    if (play_path) {
        if (!td_cmdlog_replay_file(play_path, &stack, td_replay_exec_cb,
                                   &exec_ctx, &quit, err, sizeof(err))) {
            fprintf(stderr, "tam-data: replay failed: %s\n",
                    err[0] ? err : "unknown error");
            td_stack_free(&stack);
            td_clipboard_free(&clip);
            td_cmdlog_free(&cmdlog);
            return 1;
        }
    }

    if (batch || quit || stack.len == 0) {
        td_stack_free(&stack);
        td_clipboard_free(&clip);
        td_cmdlog_free(&cmdlog);
        return 0;
    }

    if (td_term_open(&term, getenv("TERM"), td_locale()) != 0) {
        fprintf(stderr, "tam-data: cannot open controlling terminal\n");
        td_stack_free(&stack);
        td_clipboard_free(&clip);
        td_cmdlog_free(&cmdlog);
        return 1;
    }
    exec_ctx.term = &term;

    td_key_reader_fd(&reader, term.fd_in);
    td_input_init(&input);
    td_prompt_init(&prompt);
    memset(&view, 0, sizeof(view));

    while (!quit) {
        td_sheet_t *active = td_stack_active(&stack);
        int footer_rows = td_sheet_footer_rows(active, term.rows);
        int body_rows = term.rows - 2 - footer_rows;

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

            td_handle_prompt_result(pr, &prompt, &exec_ctx);
            continue;
        }

        td_cmd_t cmd = td_input_feed(&input, key);

        td_handle_command(cmd, &exec_ctx, &prompt);
    }

    td_term_close(&term);
    td_prompt_free(&prompt);
    td_stack_free(&stack);
    td_clipboard_free(&clip);
    td_cmdlog_free(&cmdlog);
    return rc;
}

#endif /* TD_TAM_DATA_NO_MAIN */
