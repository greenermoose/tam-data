#include "sheet.h"

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "utf8.h"

#define TD_SHEET_ROW_CHUNK 16
#define TD_SHEET_COL_CHUNK 8
#define TD_WIDTH_SAMPLE_ROWS 1000
#define TD_WIDTH_VIEWPORT_ROWS 128

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
    free(s->last_search);
    if (s->mmap_base && s->mmap_len > 0)
        munmap(s->mmap_base, s->mmap_len);
    free(s->buf_base);
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
    size_t phys;

    if (!s || !td_sheet_grow_rows(s, s->phys_nrows + 1))
        return SIZE_MAX;

    phys = s->phys_nrows;
    for (size_t c = 0; c < s->ncols; c++) {
        s->cols[c].cells[phys] = TD_VAL_NULL;
        s->cols[c].cached_width = 0;
    }
    s->row_order[s->nrows] = phys;
    s->phys_nrows++;
    s->nrows++;
    return phys;
}

td_val_t td_sheet_get_cell(const td_sheet_t *s, size_t row, size_t col)
{
    if (!s || row >= s->phys_nrows || col >= s->ncols)
        return TD_VAL_NULL;
    return s->cols[col].cells[row];
}

bool td_sheet_set_cell(td_sheet_t *s, size_t row, size_t col, td_val_t v)
{
    if (!s || row >= s->phys_nrows || col >= s->ncols)
        return false;
    s->cols[col].cells[row] = v;
    s->cols[col].cached_width = 0;
    return true;
}

static bool td_row_valid(const td_sheet_t *s, size_t row)
{
    return s && row < s->phys_nrows && s->sel_bits;
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
    if (!s || row >= s->phys_nrows || !s->sel_bits)
        return false;
    return (s->sel_bits[row / 64] >> (row % 64)) & 1u;
}

void td_sheet_select_all(td_sheet_t *s)
{
    if (!s)
        return;
    for (size_t vr = 0; vr < s->nrows; vr++)
        td_sheet_select_row(s, s->row_order ? s->row_order[vr] : vr);
}

void td_sheet_unselect_all(td_sheet_t *s)
{
    if (!s)
        return;
    for (size_t vr = 0; vr < s->nrows; vr++)
        td_sheet_unselect_row(s, s->row_order ? s->row_order[vr] : vr);
}

void td_sheet_toggle_all(td_sheet_t *s)
{
    if (!s)
        return;
    for (size_t vr = 0; vr < s->nrows; vr++)
        td_sheet_toggle_row(s, s->row_order ? s->row_order[vr] : vr);
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

static int td_measure_cell_width(td_val_t v, td_type_t type)
{
    char buf[512];
    size_t n = td_val_format(v, type, buf, sizeof(buf));

    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    buf[n] = '\0';
    return (int)td_utf8_width(buf);
}

static int td_compute_col_width(const td_sheet_t *s, size_t col, int cap)
{
    const td_col_t *c = &s->cols[col];
    int width = td_header_width(c);
    size_t sample = s->nrows < TD_WIDTH_SAMPLE_ROWS
                        ? s->nrows
                        : TD_WIDTH_SAMPLE_ROWS;

    for (size_t r = 0; r < sample; r++) {
        size_t phys = s->row_order ? s->row_order[r] : r;
        int w = phys < s->phys_nrows
                    ? td_measure_cell_width(c->cells[phys], c->type)
                    : 0;

        if (w > width)
            width = w;
        if (cap > 0 && width >= cap)
            return cap;
    }

    if (s->nrows > TD_WIDTH_SAMPLE_ROWS && s->row_order) {
        size_t start = s->top_row < s->nrows ? s->top_row : 0;
        size_t end = start + TD_WIDTH_VIEWPORT_ROWS;

        if (end > s->nrows)
            end = s->nrows;
        for (size_t vr = start; vr < end; vr++) {
            size_t phys = s->row_order[vr];

            if (phys < s->phys_nrows) {
                int w = td_measure_cell_width(c->cells[phys], c->type);

                if (w > width)
                    width = w;
                if (cap > 0 && width >= cap)
                    return cap;
            }
        }
    }

    if (width < TD_AUTO_WIDTH_MIN)
        width = TD_AUTO_WIDTH_MIN;
    if (cap > 0 && width > cap)
        width = cap;
    return width;
}

int td_sheet_col_width(const td_sheet_t *s, size_t col)
{
    td_col_t *c;
    int cap;

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
    if (c->cached_width > 0)
        return c->cached_width > cap ? cap : c->cached_width;

    c->cached_width = td_compute_col_width(s, col, cap);
    return c->cached_width;
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

    for (size_t r = 0; r < s->phys_nrows; r++)
        s->cols[col].cells[r] = td_cell_as_type(s, s->cols[col].cells[r],
                                                type);
    s->cols[col].type = type;
    s->cols[col].cached_width = 0;
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
    s->cols[col].cached_width = 0;
    return true;
}

void td_sheet_hide_col(td_sheet_t *s, size_t col)
{
    if (!s || col >= s->ncols)
        return;
    s->cols[col].width = 0;
    s->cols[col].cached_width = 0;
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
    int exact;

    if (!s || col >= s->ncols)
        return;
    exact = td_compute_col_width(s, col, 0);
    if (s->cols[col].width > 0 && s->cols[col].width == exact &&
        exact > TD_AUTO_WIDTH_CAP) {
        s->cols[col].width = -TD_AUTO_WIDTH_CAP;
    } else {
        s->cols[col].width = exact;
    }
    s->cols[col].cached_width = 0;
}

void td_sheet_autosize_all_cols(td_sheet_t *s)
{
    if (!s)
        return;
    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].width != 0)
            td_sheet_autosize_col(s, c);
    }
}

bool td_sheet_set_col_width(td_sheet_t *s, size_t col, int width)
{
    if (!s || col >= s->ncols || width < 0)
        return false;
    if (width == 0) {
        td_sheet_hide_col(s, col);
        return true;
    }
    s->cols[col].width = width;
    s->cols[col].cached_width = 0;
    return true;
}

void td_sheet_unhide_all_cols(td_sheet_t *s)
{
    if (!s)
        return;
    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].width == 0) {
            s->cols[c].width = -TD_AUTO_WIDTH_CAP;
            s->cols[c].cached_width = 0;
        }
    }
}

bool td_sheet_move_col(td_sheet_t *s, size_t col, int dir)
{
    size_t target = SIZE_MAX;
    td_col_t tmp;
    bool key;

    if (!s || col >= s->ncols || dir == 0 || s->cols[col].width == 0)
        return false;

    key = s->cols[col].is_key;
    if (dir < 0) {
        for (size_t c = col; c-- > 0;) {
            if (s->cols[c].width != 0 && s->cols[c].is_key == key) {
                target = c;
                break;
            }
        }
    } else {
        for (size_t c = col + 1; c < s->ncols; c++) {
            if (s->cols[c].width != 0 && s->cols[c].is_key == key) {
                target = c;
                break;
            }
        }
    }
    if (target == SIZE_MAX)
        return false;

    tmp = s->cols[col];
    s->cols[col] = s->cols[target];
    s->cols[target] = tmp;

    if (s->cursor_col == col)
        s->cursor_col = target;
    else if (s->cursor_col == target)
        s->cursor_col = col;
    return true;
}

static td_val_t td_sheet_eval_vis_cell(const td_sheet_t *s, size_t vis_row,
                                       size_t col)
{
    size_t phys = s->row_order[vis_row];
    td_val_t v = td_sheet_get_cell(s, phys, col);
    td_type_t type = s->cols[col].type;

    if (v.type == TD_TYPE_ANY && type != TD_TYPE_ANY)
        v = td_val_parse(v.as.s, type);
    return v;
}

static bool td_val_is_blank(td_val_t v)
{
    if (v.type == TD_TYPE_NULL)
        return true;
    if (v.type == TD_TYPE_ANY || v.type == TD_TYPE_STR)
        return !v.as.s.data || v.as.s.len == 0;
    return false;
}

bool td_sheet_jump_value(td_sheet_t *s, int dir)
{
    td_val_t cur;

    if (!s || s->nrows == 0 || s->ncols == 0 || dir == 0 ||
        s->cursor_row >= s->nrows || s->cursor_col >= s->ncols)
        return false;

    cur = td_sheet_eval_vis_cell(s, s->cursor_row, s->cursor_col);
    if (dir > 0) {
        for (size_t r = s->cursor_row + 1; r < s->nrows; r++) {
            if (td_val_cmp(td_sheet_eval_vis_cell(s, r, s->cursor_col), cur) != 0) {
                s->cursor_row = r;
                return true;
            }
        }
    } else {
        for (size_t r = s->cursor_row; r-- > 0;) {
            if (td_val_cmp(td_sheet_eval_vis_cell(s, r, s->cursor_col), cur) != 0) {
                s->cursor_row = r;
                return true;
            }
        }
    }
    return false;
}

bool td_sheet_jump_null(td_sheet_t *s, int dir)
{
    if (!s || s->nrows == 0 || s->ncols == 0 || dir == 0 ||
        s->cursor_row >= s->nrows || s->cursor_col >= s->ncols)
        return false;

    if (dir > 0) {
        for (size_t r = s->cursor_row + 1; r < s->nrows; r++) {
            if (td_val_is_blank(td_sheet_eval_vis_cell(s, r, s->cursor_col))) {
                s->cursor_row = r;
                return true;
            }
        }
    } else {
        for (size_t r = s->cursor_row; r-- > 0;) {
            if (td_val_is_blank(td_sheet_eval_vis_cell(s, r, s->cursor_col))) {
                s->cursor_row = r;
                return true;
            }
        }
    }
    return false;
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
    } else {
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
    }

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

static void td_set_err(char *err, size_t err_cap, const char *msg)
{
    if (err && err_cap > 0)
        snprintf(err, err_cap, "%s", msg ? msg : "");
}

static void td_invalidate_auto_widths(td_sheet_t *s)
{
    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].width < 0)
            s->cols[c].cached_width = 0;
    }
}

static bool td_compile_regex(regex_t *re, const char *pattern,
                             char *err, size_t err_cap)
{
    int rc;

    if (!pattern) {
        td_set_err(err, err_cap, "missing pattern");
        return false;
    }
    rc = regcomp(re, pattern, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        if (err && err_cap > 0)
            regerror(rc, re, err, err_cap);
        return false;
    }
    return true;
}

static bool td_cell_matches_regex(const td_sheet_t *s, size_t phys,
                                  size_t col, const regex_t *re)
{
    char stack_buf[512];
    td_val_t v = td_sheet_get_cell(s, phys, col);
    td_type_t type = s->cols[col].type;
    size_t n = td_val_format(v, type, stack_buf, sizeof(stack_buf));

    if (n < sizeof(stack_buf))
        return regexec(re, stack_buf, 0, NULL, 0) == 0;

    char *dyn = malloc(n + 1);
    bool matched;

    if (!dyn)
        return false;
    (void)td_val_format(v, type, dyn, n + 1);
    matched = regexec(re, dyn, 0, NULL, 0) == 0;
    free(dyn);
    return matched;
}

bool td_sheet_select_regex(td_sheet_t *s, const char *pattern, bool all_cols,
                           bool select, size_t *matched_out, char *err,
                           size_t err_cap)
{
    regex_t re;
    size_t matched = 0;

    if (matched_out)
        *matched_out = 0;
    if (err && err_cap > 0)
        err[0] = '\0';
    if (!s) {
        td_set_err(err, err_cap, "no active sheet");
        return false;
    }
    if (!td_compile_regex(&re, pattern, err, err_cap))
        return false;

    if (s->nrows > 0 && s->ncols > 0 &&
        (all_cols || s->cursor_col < s->ncols)) {
        for (size_t vr = 0; vr < s->nrows; vr++) {
            size_t phys = s->row_order ? s->row_order[vr] : vr;
            bool row_hit = false;

            if (!all_cols) {
                row_hit = td_cell_matches_regex(s, phys, s->cursor_col, &re);
            } else {
                for (size_t c = 0; c < s->ncols; c++) {
                    if (s->cols[c].width == 0)
                        continue;
                    if (td_cell_matches_regex(s, phys, c, &re)) {
                        row_hit = true;
                        break;
                    }
                }
            }
            if (row_hit) {
                matched++;
                if (select)
                    td_sheet_select_row(s, phys);
                else
                    td_sheet_unselect_row(s, phys);
            }
        }
    }

    regfree(&re);
    if (matched_out)
        *matched_out = matched;
    return true;
}

static bool td_sheet_search_scan(td_sheet_t *s, const regex_t *re, int dir,
                                 bool all_cols)
{
    size_t start;

    if (!s || s->nrows == 0 || s->ncols == 0)
        return false;
    if (!all_cols && s->cursor_col >= s->ncols)
        return false;

    start = s->cursor_row < s->nrows ? s->cursor_row : 0;
    for (size_t step = 1; step <= s->nrows; step++) {
        size_t offset = step % s->nrows;
        size_t vr = dir >= 0
                        ? (start + offset) % s->nrows
                        : (start + s->nrows - offset) % s->nrows;
        size_t phys = s->row_order ? s->row_order[vr] : vr;

        if (!all_cols) {
            if (td_cell_matches_regex(s, phys, s->cursor_col, re)) {
                s->cursor_row = vr;
                return true;
            }
            continue;
        }

        if (s->cursor_col < s->ncols && s->cols[s->cursor_col].width != 0 &&
            td_cell_matches_regex(s, phys, s->cursor_col, re)) {
            s->cursor_row = vr;
            return true;
        }
        for (size_t c = 0; c < s->ncols; c++) {
            if (s->cols[c].width == 0 || c == s->cursor_col)
                continue;
            if (td_cell_matches_regex(s, phys, c, re)) {
                s->cursor_row = vr;
                s->cursor_col = c;
                return true;
            }
        }
    }
    return false;
}

bool td_sheet_search(td_sheet_t *s, const char *pattern, int dir, bool all_cols,
                     char *err, size_t err_cap)
{
    regex_t re;
    const char *pat;
    char *saved;
    bool hit;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!s) {
        td_set_err(err, err_cap, "no active sheet");
        return false;
    }

    if (pattern && pattern[0]) {
        pat = pattern;
    } else if (s->last_search && s->last_search[0]) {
        pat = s->last_search;
    } else {
        td_set_err(err, err_cap, "no previous search");
        return false;
    }

    if (!td_compile_regex(&re, pat, err, err_cap))
        return false;

    if (pat != s->last_search) {
        size_t len = strlen(pat);

        saved = malloc(len + 1);
        if (!saved) {
            regfree(&re);
            td_set_err(err, err_cap, "out of memory");
            return false;
        }
        memcpy(saved, pat, len + 1);
        free(s->last_search);
        s->last_search = saved;
    }
    s->search_dir = dir >= 0 ? 1 : -1;
    s->search_all_cols = all_cols;

    hit = td_sheet_search_scan(s, &re, s->search_dir, all_cols);
    regfree(&re);
    if (!hit && err && err_cap > 0)
        snprintf(err, err_cap, "not found: %s", s->last_search);
    return hit;
}

bool td_sheet_search_next(td_sheet_t *s, int dir_mult, char *err,
                          size_t err_cap)
{
    regex_t re;
    int eff_dir;
    bool hit;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!s || !s->last_search || !s->last_search[0]) {
        td_set_err(err, err_cap, "no previous search");
        return false;
    }
    if (!td_compile_regex(&re, s->last_search, err, err_cap))
        return false;

    eff_dir = (s->search_dir >= 0 ? 1 : -1) * (dir_mult >= 0 ? 1 : -1);
    hit = td_sheet_search_scan(s, &re, eff_dir, s->search_all_cols);
    regfree(&re);
    if (!hit && err && err_cap > 0)
        snprintf(err, err_cap, "not found: %s", s->last_search);
    return hit;
}

static void td_clamp_cursor_after_delete(td_sheet_t *s)
{
    if (s->nrows == 0) {
        s->cursor_row = 0;
        s->top_row = 0;
    } else if (s->cursor_row >= s->nrows) {
        s->cursor_row = s->nrows - 1;
    }
}

bool td_sheet_delete_row(td_sheet_t *s, size_t vis_row)
{
    size_t phys;

    if (!s || vis_row >= s->nrows || !s->row_order)
        return false;

    phys = s->row_order[vis_row];
    td_sheet_unselect_row(s, phys);
    if (vis_row + 1 < s->nrows)
        memmove(&s->row_order[vis_row], &s->row_order[vis_row + 1],
                (s->nrows - vis_row - 1) * sizeof(s->row_order[0]));
    s->nrows--;
    td_clamp_cursor_after_delete(s);
    td_invalidate_auto_widths(s);
    return true;
}

size_t td_sheet_delete_selected(td_sheet_t *s)
{
    size_t write = 0;
    size_t deleted = 0;

    if (!s || s->nrows == 0 || s->nselected == 0 || !s->row_order)
        return 0;

    for (size_t vr = 0; vr < s->nrows; vr++) {
        size_t phys = s->row_order[vr];

        if (td_sheet_is_selected(s, phys)) {
            td_sheet_unselect_row(s, phys);
            deleted++;
        } else {
            s->row_order[write++] = phys;
        }
    }
    s->nrows = write;
    if (deleted > 0) {
        td_clamp_cursor_after_delete(s);
        td_invalidate_auto_widths(s);
    }
    return deleted;
}

static td_val_t td_clone_val_to_arena(td_arena_t *arena, td_val_t v, bool *ok)
{
    *ok = true;
    if ((v.type == TD_TYPE_ANY || v.type == TD_TYPE_STR ||
         v.type == TD_TYPE_ERR) && v.as.s.data) {
        char *copy = td_arena_strndup(arena, v.as.s.data, v.as.s.len);

        if (!copy) {
            *ok = false;
            return TD_VAL_ERR;
        }
        v.as.s.data = copy;
    }
    return v;
}

td_sheet_t *td_sheet_dup_selected(const td_sheet_t *s, const char *suffix)
{
    const char *base;
    const char *sfx;
    size_t name_len;
    char *name_buf;
    td_sheet_t *dup;

    if (!s || s->nselected == 0)
        return NULL;

    base = s->name ? s->name : "";
    sfx = suffix ? suffix : "[selected]";
    name_len = strlen(base) + strlen(sfx) + 1;
    name_buf = malloc(name_len);
    if (!name_buf)
        return NULL;
    snprintf(name_buf, name_len, "%s%s", base, sfx);

    dup = td_sheet_new(name_buf, s->source_path);
    free(name_buf);
    if (!dup)
        return NULL;

    for (size_t c = 0; c < s->ncols; c++) {
        size_t idx = td_sheet_add_col(dup, s->cols[c].name, s->cols[c].type,
                                      s->cols[c].width);
        if (idx == SIZE_MAX) {
            td_sheet_free(dup);
            return NULL;
        }
        dup->cols[idx].is_key = s->cols[c].is_key;
    }

    for (size_t vr = 0; vr < s->nrows; vr++) {
        size_t phys = s->row_order ? s->row_order[vr] : vr;
        size_t dst_row;

        if (!td_sheet_is_selected(s, phys))
            continue;
        dst_row = td_sheet_add_row(dup);
        if (dst_row == SIZE_MAX) {
            td_sheet_free(dup);
            return NULL;
        }
        for (size_t c = 0; c < s->ncols; c++) {
            bool ok = true;
            td_val_t v = td_clone_val_to_arena(&dup->arena,
                                               td_sheet_get_cell(s, phys, c),
                                               &ok);
            if (!ok || !td_sheet_set_cell(dup, dst_row, c, v)) {
                td_sheet_free(dup);
                return NULL;
            }
        }
    }

    if (dup->ncols > 0 && s->cursor_col < dup->ncols)
        dup->cursor_col = s->cursor_col;
    return dup;
}

typedef struct {
    const td_sheet_t *sheet;
    const size_t *cols;
    size_t ncols;
    bool desc;
} td_sort_ctx_t;

static td_val_t td_sheet_eval_phys_cell(const td_sheet_t *s, size_t phys,
                                        size_t col)
{
    td_val_t v = td_sheet_get_cell(s, phys, col);
    td_type_t type = s->cols[col].type;

    if (v.type == TD_TYPE_ANY && type != TD_TYPE_ANY)
        v = td_val_parse(v.as.s, type);
    return v;
}

static int td_sort_cmp(const td_sort_ctx_t *ctx, size_t phys_a, size_t phys_b)
{
    const td_sheet_t *s = ctx->sheet;

    for (size_t k = 0; k < ctx->ncols; k++) {
        size_t col = ctx->cols[k];
        td_val_t va;
        td_val_t vb;
        int cmp;

        if (col >= s->ncols)
            continue;
        va = td_sheet_eval_phys_cell(s, phys_a, col);
        vb = td_sheet_eval_phys_cell(s, phys_b, col);
        cmp = td_val_cmp(va, vb);
        if (cmp != 0)
            return ctx->desc ? -cmp : cmp;
    }
    return 0;
}

static void td_merge_runs(size_t *dst, const size_t *src, size_t left,
                          size_t mid, size_t right, const td_sort_ctx_t *ctx)
{
    size_t i = left;
    size_t j = mid;
    size_t k = left;

    while (i < mid && j < right) {
        if (td_sort_cmp(ctx, src[i], src[j]) <= 0)
            dst[k++] = src[i++];
        else
            dst[k++] = src[j++];
    }
    while (i < mid)
        dst[k++] = src[i++];
    while (j < right)
        dst[k++] = src[j++];
}

bool td_sheet_sort_cols(td_sheet_t *s, const size_t *cols, size_t ncols,
                        bool desc)
{
    td_sort_ctx_t ctx;
    size_t *scratch;

    if (!s || !cols || ncols == 0 || s->ncols == 0)
        return false;
    for (size_t i = 0; i < ncols; i++) {
        if (cols[i] >= s->ncols)
            return false;
    }
    if (s->nrows <= 1 || !s->row_order)
        return true;

    scratch = malloc(s->nrows * sizeof(*scratch));
    if (!scratch)
        return false;

    ctx.sheet = s;
    ctx.cols = cols;
    ctx.ncols = ncols;
    ctx.desc = desc;

    for (size_t width = 1; width < s->nrows; width *= 2) {
        for (size_t left = 0; left < s->nrows; left += 2 * width) {
            size_t mid = left + width;
            size_t right = left + 2 * width;

            if (mid > s->nrows)
                mid = s->nrows;
            if (right > s->nrows)
                right = s->nrows;
            td_merge_runs(scratch, s->row_order, left, mid, right, &ctx);
        }
        memcpy(s->row_order, scratch, s->nrows * sizeof(*scratch));
    }

    free(scratch);
    td_invalidate_auto_widths(s);
    return true;
}

bool td_sheet_sort_col(td_sheet_t *s, size_t col, bool desc)
{
    return td_sheet_sort_cols(s, &col, 1, desc);
}

bool td_sheet_sort_keys(td_sheet_t *s, bool desc)
{
    size_t small[32];
    size_t *keys = small;
    size_t nkeys = 0;
    bool ok;

    if (!s || s->ncols == 0)
        return false;

    if (s->ncols > sizeof(small) / sizeof(small[0])) {
        keys = malloc(s->ncols * sizeof(*keys));
        if (!keys)
            return false;
    }

    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].is_key && s->cols[c].width != 0)
            keys[nkeys++] = c;
    }

    if (nkeys == 0) {
        if (s->cursor_col < s->ncols)
            keys[nkeys++] = s->cursor_col;
    }

    ok = nkeys > 0 && td_sheet_sort_cols(s, keys, nkeys, desc);
    if (keys != small)
        free(keys);
    return ok;
}
