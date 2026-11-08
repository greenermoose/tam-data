#include "sheet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utf8.h"

#define TD_SHEET_ROW_CHUNK 16
#define TD_SHEET_COL_CHUNK 8
#define TD_AUTO_WIDTH_CAP 40
#define TD_AUTO_WIDTH_MIN 3

td_sheet_t *td_sheet_new(const char *name, const char *source_path)
{
    td_sheet_t *s = calloc(1, sizeof(*s));

    if (!s)
        return NULL;
    td_arena_init(&s->arena, 0);
    s->name = td_arena_strdup(&s->arena, name ? name : "");
    s->source_path = td_arena_strdup(&s->arena, source_path ? source_path : "");
    if (!s->name || !s->source_path) {
        td_sheet_free(s);
        return NULL;
    }
    return s;
}

void td_sheet_free(td_sheet_t *s)
{
    if (!s)
        return;
    for (size_t c = 0; c < s->ncols; c++)
        free(s->cols[c].cells);
    free(s->cols);
    free(s->row_order);
    free(s->sel_bits);
    td_arena_free(&s->arena);
    free(s);
}

static bool td_sheet_grow_rows(td_sheet_t *s, size_t need)
{
    size_t new_cap;
    size_t *order;
    uint64_t *bits;
    size_t words;

    if (need <= s->row_cap)
        return true;

    new_cap = s->row_cap ? s->row_cap : TD_SHEET_ROW_CHUNK;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2)
            return false;
        new_cap *= 2;
    }

    for (size_t c = 0; c < s->ncols; c++) {
        td_val_t *cells = realloc(s->cols[c].cells,
                                  new_cap * sizeof(td_val_t));
        if (!cells)
            return false;
        for (size_t r = s->row_cap; r < new_cap; r++)
            cells[r] = TD_VAL_NULL;
        s->cols[c].cells = cells;
    }

    order = realloc(s->row_order, new_cap * sizeof(size_t));
    if (!order)
        return false;
    s->row_order = order;

    words = (new_cap + 63) / 64;
    bits = realloc(s->sel_bits, words * sizeof(uint64_t));
    if (!bits)
        return false;
    for (size_t w = s->sel_words; w < words; w++)
        bits[w] = 0;
    s->sel_bits = bits;
    s->sel_words = words;
    s->row_cap = new_cap;
    return true;
}

static bool td_sheet_grow_cols(td_sheet_t *s, size_t need)
{
    size_t new_cap;
    td_col_t *cols;

    if (need <= s->col_cap)
        return true;

    new_cap = s->col_cap ? s->col_cap : TD_SHEET_COL_CHUNK;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2)
            return false;
        new_cap *= 2;
    }

    cols = realloc(s->cols, new_cap * sizeof(td_col_t));
    if (!cols)
        return false;
    memset(cols + s->col_cap, 0, (new_cap - s->col_cap) * sizeof(td_col_t));
    s->cols = cols;
    s->col_cap = new_cap;
    return true;
}

size_t td_sheet_add_col(td_sheet_t *s, const char *name, td_type_t type,
                        int width)
{
    td_col_t *c;

    if (!s || !td_sheet_grow_cols(s, s->ncols + 1))
        return SIZE_MAX;

    c = &s->cols[s->ncols];
    memset(c, 0, sizeof(*c));
    c->name = td_arena_strdup(&s->arena, name ? name : "");
    if (!c->name)
        return SIZE_MAX;
    c->type = type;
    c->width = width;
    c->is_key = false;
    c->cells = NULL;

    if (s->row_cap) {
        c->cells = malloc(s->row_cap * sizeof(td_val_t));
        if (!c->cells)
            return SIZE_MAX;
        for (size_t r = 0; r < s->row_cap; r++)
            c->cells[r] = TD_VAL_NULL;
    }

    s->ncols++;
    return s->ncols - 1;
}

size_t td_sheet_add_row(td_sheet_t *s)
{
    size_t row;

    if (!s || !td_sheet_grow_rows(s, s->nrows + 1))
        return SIZE_MAX;

    row = s->nrows;
    for (size_t c = 0; c < s->ncols; c++)
        s->cols[c].cells[row] = TD_VAL_NULL;
    s->row_order[row] = row;
    s->nrows++;
    return row;
}

td_val_t td_sheet_get_cell(const td_sheet_t *s, size_t row, size_t col)
{
    if (!s || row >= s->nrows || col >= s->ncols)
        return TD_VAL_NULL;
    return s->cols[col].cells[row];
}

bool td_sheet_set_cell(td_sheet_t *s, size_t row, size_t col, td_val_t v)
{
    if (!s || row >= s->nrows || col >= s->ncols)
        return false;
    s->cols[col].cells[row] = v;
    return true;
}

static bool td_row_valid(const td_sheet_t *s, size_t row)
{
    return s && row < s->nrows && s->sel_bits;
}

void td_sheet_select_row(td_sheet_t *s, size_t row)
{
    if (!td_row_valid(s, row) || td_sheet_is_selected(s, row))
        return;
    s->sel_bits[row / 64] |= UINT64_C(1) << (row % 64);
    s->nselected++;
}

void td_sheet_unselect_row(td_sheet_t *s, size_t row)
{
    if (!td_row_valid(s, row) || !td_sheet_is_selected(s, row))
        return;
    s->sel_bits[row / 64] &= ~(UINT64_C(1) << (row % 64));
    s->nselected--;
}

void td_sheet_toggle_row(td_sheet_t *s, size_t row)
{
    if (!td_row_valid(s, row))
        return;
    if (td_sheet_is_selected(s, row))
        td_sheet_unselect_row(s, row);
    else
        td_sheet_select_row(s, row);
}

bool td_sheet_is_selected(const td_sheet_t *s, size_t row)
{
    if (!s || row >= s->nrows || !s->sel_bits)
        return false;
    return (s->sel_bits[row / 64] >> (row % 64)) & 1u;
}

void td_sheet_select_all(td_sheet_t *s)
{
    if (!s)
        return;
    for (size_t r = 0; r < s->nrows; r++)
        td_sheet_select_row(s, r);
}

void td_sheet_unselect_all(td_sheet_t *s)
{
    if (!s)
        return;
    for (size_t r = 0; r < s->nrows; r++)
        td_sheet_unselect_row(s, r);
}

void td_sheet_toggle_all(td_sheet_t *s)
{
    if (!s)
        return;
    for (size_t r = 0; r < s->nrows; r++)
        td_sheet_toggle_row(s, r);
}

size_t td_sheet_next_visible_col(const td_sheet_t *s, size_t col, int dir)
{
    if (!s || s->ncols == 0)
        return SIZE_MAX;

    if (dir < 0) {
        while (col > 0) {
            col--;
            if (s->cols[col].width != 0)
                return col;
        }
    } else {
        while (col + 1 < s->ncols) {
            col++;
            if (s->cols[col].width != 0)
                return col;
        }
    }
    return SIZE_MAX;
}

size_t td_sheet_first_visible_col(const td_sheet_t *s)
{
    if (!s)
        return SIZE_MAX;
    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].width != 0)
            return c;
    }
    return SIZE_MAX;
}

size_t td_sheet_last_visible_col(const td_sheet_t *s)
{
    if (!s)
        return SIZE_MAX;
    for (size_t c = s->ncols; c-- > 0;) {
        if (s->cols[c].width != 0)
            return c;
    }
    return SIZE_MAX;
}

int td_sheet_header(char *buf, size_t cap, const td_col_t *c)
{
    const char *sigil;
    int n;

    if (!c)
        return 0;
    sigil = td_type_sigil(c->type);
    if (!sigil || !sigil[0])
        n = snprintf(buf, cap, "%s", c->name ? c->name : "");
    else
        n = snprintf(buf, cap, "%s %s", sigil, c->name ? c->name : "");
    return n < 0 ? 0 : n;
}

static int td_header_width(const td_col_t *c)
{
    char buf[256];
    int n = td_sheet_header(buf, sizeof(buf), c);

    if (n <= 0)
        return 0;
    return (int)td_utf8_width(buf);
}

int td_sheet_col_width(const td_sheet_t *s, size_t col)
{
    td_col_t *c;
    int cap;
    int width;
    char buf[512];

    if (!s || col >= s->ncols)
        return 0;
    c = &s->cols[col];
    if (c->width == 0)
        return 0;
    if (c->width > 0)
        return c->width;

    cap = -c->width;
    if (cap <= 0)
        cap = TD_AUTO_WIDTH_CAP;
    width = td_header_width(c);
    for (size_t r = 0; r < s->nrows; r++) {
        size_t n = td_val_format(c->cells[r], c->type, buf, sizeof(buf));
        int w;

        if (n >= sizeof(buf))
            n = sizeof(buf) - 1;
        w = (int)td_utf8_width(buf);
        if (w > width)
            width = w;
        if (width >= cap)
            return cap;
    }
    if (width < TD_AUTO_WIDTH_MIN)
        width = TD_AUTO_WIDTH_MIN;
    return width > cap ? cap : width;
}

static td_val_t td_cell_as_type(td_sheet_t *s, td_val_t v, td_type_t type)
{
    char buf[256];
    size_t n;

    if (v.type == type)
        return v;
    if (v.type == TD_TYPE_NULL)
        return TD_VAL_NULL;

    if (v.type == TD_TYPE_ANY || v.type == TD_TYPE_STR) {
        if (type == TD_TYPE_ANY || type == TD_TYPE_STR) {
            v.type = type;
            return v;
        }
        return td_val_parse(v.as.s, type);
    }

    n = td_val_format(v, v.type, buf, sizeof(buf));
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    if (type == TD_TYPE_ANY || type == TD_TYPE_STR) {
        char *copy = td_arena_strndup(&s->arena, buf, n);

        if (!copy)
            return TD_VAL_ERR;
        v.type = type;
        v.as.s.data = copy;
        v.as.s.len = (uint32_t)n;
        return v;
    }
    return td_val_parse((td_slice_t){ buf, (uint32_t)n }, type);
}

bool td_sheet_set_col_type(td_sheet_t *s, size_t col, td_type_t type)
{
    if (!s || col >= s->ncols)
        return false;

    for (size_t r = 0; r < s->nrows; r++)
        s->cols[col].cells[r] = td_cell_as_type(s, s->cols[col].cells[r],
                                                type);
    s->cols[col].type = type;
    return true;
}

bool td_sheet_rename_col(td_sheet_t *s, size_t col, const char *name)
{
    char *copy;

    if (!s || col >= s->ncols)
        return false;
    copy = td_arena_strdup(&s->arena, name ? name : "");
    if (!copy)
        return false;
    s->cols[col].name = copy;
    return true;
}

void td_sheet_hide_col(td_sheet_t *s, size_t col)
{
    if (!s || col >= s->ncols)
        return;
    s->cols[col].width = 0;
    if (s->cursor_col == col) {
        size_t next = td_sheet_next_visible_col(s, col, 1);

        if (next == SIZE_MAX)
            next = td_sheet_next_visible_col(s, col, -1);
        if (next != SIZE_MAX)
            s->cursor_col = next;
    }
}

void td_sheet_toggle_key_col(td_sheet_t *s, size_t col)
{
    if (!s || col >= s->ncols)
        return;
    s->cols[col].is_key = !s->cols[col].is_key;
    s->left_col = 0;
}

void td_sheet_autosize_col(td_sheet_t *s, size_t col)
{
    if (!s || col >= s->ncols)
        return;
    if (s->cols[col].width == 0)
        s->cols[col].width = -TD_AUTO_WIDTH_CAP;
    s->cols[col].width = td_sheet_col_width(s, col);
    if (s->cols[col].width <= 0)
        s->cols[col].width = TD_AUTO_WIDTH_MIN;
}

static size_t td_first_regular_col(const td_sheet_t *s)
{
    if (!s)
        return SIZE_MAX;
    for (size_t c = 0; c < s->ncols; c++) {
        if (!s->cols[c].is_key && s->cols[c].width != 0)
            return c;
    }
    return SIZE_MAX;
}

void td_sheet_ensure_cursor_visible(td_sheet_t *s, int grid_rows,
                                    int grid_cols)
{
    int pinned = 0;
    size_t first_regular;

    if (!s)
        return;

    if (s->nrows == 0) {
        s->cursor_row = 0;
        s->top_row = 0;
        return;
    }
    if (s->cursor_row >= s->nrows)
        s->cursor_row = s->nrows - 1;

    if (grid_rows < 1)
        grid_rows = 1;
    if (s->cursor_row < s->top_row)
        s->top_row = s->cursor_row;
    else if (s->cursor_row >= s->top_row + (size_t)grid_rows)
        s->top_row = s->cursor_row - (size_t)grid_rows + 1;
    if (s->nrows >= (size_t)grid_rows &&
        s->top_row + (size_t)grid_rows > s->nrows)
        s->top_row = s->nrows - (size_t)grid_rows;

    if (s->ncols == 0) {
        s->cursor_col = 0;
        s->left_col = 0;
        return;
    }
    if (s->cursor_col >= s->ncols)
        s->cursor_col = s->ncols - 1;
    if (s->cols[s->cursor_col].width == 0) {
        size_t next = td_sheet_next_visible_col(s, s->cursor_col, 1);

        if (next == SIZE_MAX)
            next = td_sheet_next_visible_col(s, s->cursor_col, -1);
        if (next != SIZE_MAX)
            s->cursor_col = next;
    }

    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].is_key && s->cols[c].width != 0)
            pinned += td_sheet_col_width(s, c) + 1;
    }

    first_regular = td_first_regular_col(s);
    if (s->left_col >= s->ncols || s->cols[s->left_col].width == 0 ||
        s->cols[s->left_col].is_key)
        s->left_col = first_regular == SIZE_MAX ? 0 : first_regular;

    if (s->cols[s->cursor_col].is_key || first_regular == SIZE_MAX)
        return;

    if (s->left_col > s->cursor_col ||
        s->cols[s->left_col].is_key ||
        s->cols[s->left_col].width == 0)
        s->left_col = s->cursor_col;

    if (grid_cols < 1)
        grid_cols = 1;

    int x = pinned;
    for (size_t c = s->left_col; c < s->cursor_col; c++) {
        if (!s->cols[c].is_key && s->cols[c].width != 0)
            x += td_sheet_col_width(s, c) + 1;
    }

    while (s->left_col < s->cursor_col &&
           x + td_sheet_col_width(s, s->cursor_col) + 1 > grid_cols) {
        int w = td_sheet_col_width(s, s->left_col);

        if (!s->cols[s->left_col].is_key && w > 0)
            x -= w + 1;
        size_t next = td_sheet_next_visible_col(s, s->left_col, 1);

        if (next == SIZE_MAX || next > s->cursor_col)
            break;
        s->left_col = next;
    }
}

bool td_stack_push(td_stack_t *st, td_sheet_t *s)
{
    if (!st || !s)
        return false;
    if (st->len == st->cap) {
        size_t cap = st->cap ? st->cap * 2 : 8;
        td_sheet_t **sheets = realloc(st->sheets, cap * sizeof(*sheets));

        if (!sheets)
            return false;
        st->sheets = sheets;
        st->cap = cap;
    }
    st->sheets[st->len++] = s;
    return true;
}

td_sheet_t *td_stack_pop(td_stack_t *st)
{
    if (!st || st->len == 0)
        return NULL;
    return st->sheets[--st->len];
}

td_sheet_t *td_stack_active(const td_stack_t *st)
{
    if (!st || st->len == 0)
        return NULL;
    return st->sheets[st->len - 1];
}

void td_stack_free(td_stack_t *st)
{
    if (!st)
        return;
    for (size_t i = 0; i < st->len; i++)
        td_sheet_free(st->sheets[i]);
    free(st->sheets);
    st->sheets = NULL;
    st->len = 0;
    st->cap = 0;
}
