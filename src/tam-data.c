#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "tam-data.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    snprintf(left, sizeof(left), "[%zu/%zu] %s", stack->len, stack->len,
             s ? s->name : "tam-data");
    if (s) {
        const char *pending = state && state->pending ? state->pending : "";

        if (pending[0])
            snprintf(right, sizeof(right), "%s | %zu rows (%zu selected)",
                     pending, s->nrows, s->nselected);
        else
            snprintf(right, sizeof(right), "%zu rows (%zu selected)",
                     s->nrows, s->nselected);
    } else {
        snprintf(right, sizeof(right), "%s", "");
    }

    td_term_fill_rect(t, row, 0, 1, cols, ' ', fg, bg, attr);
    leftw = (int)td_utf8_width(left);
    td_term_put_str(t, row, 0, leftw, left, fg, bg, attr);

    rightw = (int)td_utf8_width(right);
    if (rightw > cols)
        rightw = cols;
    td_term_put_str(t, row, cols - rightw, rightw, right, fg, bg, attr);

    center_x = leftw + 2;
    center_max = cols - rightw - center_x - 1;
    if (center_max <= 0)
        return;

    if (state && state->prompt_active) {
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
            if (text_x + cursorw < cols) {
                td_cell_t *c = &t->back[(size_t)row * (size_t)t->cols +
                                        (size_t)(text_x + cursorw)];

                c->attr |= TD_ATTR_UNDERLINE;
            }
        }
        return;
    }

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

                td_val_format(col->cells[physical], col->type, value,
                              sizeof(value));
                if (selected) {
                    fg = 15;
                    bg = 4;
                }
                if (vr == s->cursor_row)
                    attr |= TD_ATTR_BOLD;
                if (vr == s->cursor_row && c == s->cursor_col)
                    attr |= TD_ATTR_REV;
                td_term_put_str(t, header_row + 1 + i, bx, width, value, fg,
                                bg, attr);
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
        "usage: tam-data [--demo] [--version|-v] [--help|-h]\n"
        "\n"
        "Phase 0 interactive grid. Open the built-in demo sheet and explore:\n"
        "  h j k l / arrows      move the cursor\n"
        "  gh gl gg G            first/last column, first/last row\n"
        "  Ctrl+F / Ctrl+B       page down / page up\n"
        "  s u t                 select / unselect / toggle current row\n"
        "  gs gu gt              select / unselect / toggle all rows\n"
        "  !                     pin or unpin the current column as a key\n"
        "  -                     hide the current column\n"
        "  _                     autosize the current column\n"
        "  ^                     rename the current column\n"
        "  # %% $ @ ~             set column type (int, float, currency,\n"
        "                        date, string)\n"
        "  q                     pop the current sheet (quits the last one)\n"
        "  gq                    quit tam-data\n"
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
    case TD_CMD_HIDE_COL:
        td_sheet_hide_col(s, s->cursor_col);
        td_set_message(msg, msg_cap, "column hidden");
        break;
    case TD_CMD_TOGGLE_KEY_COL:
        td_sheet_toggle_key_col(s, s->cursor_col);
        td_set_message(msg, msg_cap, "%s key column",
                       s->cols[s->cursor_col].is_key ? "pinned" : "unpinned");
        break;
    case TD_CMD_AUTOSIZE_COL:
        td_sheet_autosize_col(s, s->cursor_col);
        td_set_message(msg, msg_cap, "column width %d",
                       td_sheet_col_width(s, s->cursor_col));
        break;
    case TD_CMD_RENAME_COL:
        td_prompt_start(prompt, "rename column:", s->cols[s->cursor_col].name);
        break;
    case TD_CMD_TYPE_ANY:
        td_sheet_set_col_type(s, s->cursor_col, TD_TYPE_ANY);
        td_set_message(msg, msg_cap, "column type any");
        break;
    case TD_CMD_TYPE_STR:
        td_sheet_set_col_type(s, s->cursor_col, TD_TYPE_STR);
        td_set_message(msg, msg_cap, "column type string");
        break;
    case TD_CMD_TYPE_INT:
        td_sheet_set_col_type(s, s->cursor_col, TD_TYPE_INT);
        td_set_message(msg, msg_cap, "column type int");
        break;
    case TD_CMD_TYPE_FLOAT:
        td_sheet_set_col_type(s, s->cursor_col, TD_TYPE_FLOAT);
        td_set_message(msg, msg_cap, "column type float");
        break;
    case TD_CMD_TYPE_CURRENCY:
        td_sheet_set_col_type(s, s->cursor_col, TD_TYPE_CURRENCY);
        td_set_message(msg, msg_cap, "column type currency");
        break;
    case TD_CMD_TYPE_DATE:
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

int main(int argc, char **argv)
{
    bool demo = false;
    td_stack_t stack = { 0 };
    td_term_t term;
    td_key_reader_t reader;
    td_input_t input;
    td_prompt_t prompt;
    td_view_state_t view;
    char msg[512] = "";
    bool quit = false;
    int rc = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            if (td_build_id()[0])
                printf("tam-data %s (%s)\n", td_version(), td_build_id());
            else
                printf("tam-data %s\n", td_version());
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            td_usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--demo") == 0) {
            demo = true;
            continue;
        }
        fprintf(stderr, "tam-data: unknown argument: %s\n", argv[i]);
        td_usage(stderr);
        return 2;
    }
    (void)demo;

    td_sheet_t *sheet = td_demo_sheet();
    if (!sheet || !td_stack_push(&stack, sheet)) {
        fprintf(stderr, "tam-data: cannot create demo sheet\n");
        td_sheet_free(sheet);
        return 1;
    }

    if (td_term_open(&term, getenv("TERM"), td_locale()) != 0) {
        fprintf(stderr, "tam-data: standard input is not a terminal\n");
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

            if (pr == TD_PROMPT_ACCEPT && active) {
                char name[512];

                snprintf(name, sizeof(name), "%s", td_prompt_text(&prompt));
                if (td_sheet_rename_col(active, active->cursor_col, name))
                    td_set_message(msg, sizeof(msg), "renamed column to %s",
                                   name);
                td_prompt_stop(&prompt);
            } else if (pr == TD_PROMPT_CANCEL) {
                td_prompt_stop(&prompt);
                td_set_message(msg, sizeof(msg), "rename cancelled");
            }
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
