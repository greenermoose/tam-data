#include "sheet.h"

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "expr.h"
#include "utf8.h"

#define TD_SHEET_ROW_CHUNK 16
#define TD_SHEET_COL_CHUNK 8
#define TD_WIDTH_SAMPLE_ROWS 1000
#define TD_WIDTH_VIEWPORT_ROWS 128
#define TD_SPLIT_MAX_COLS 128

typedef enum {
    TD_UNDO_CELLS = 0,
    TD_UNDO_ROWS,
    TD_UNDO_COL_META,
    TD_UNDO_COL_VIS_ALL,
    TD_UNDO_COL_SWAP,
    TD_UNDO_COL_TYPE,
    TD_UNDO_COL_ADD,
    TD_UNDO_COL_AGGS
} td_undo_kind_t;

typedef struct {
    size_t phys_row;
    size_t col;
    td_val_t alt_val;
} td_cell_delta_t;

struct td_undo_entry {
    td_undo_kind_t kind;
    char desc[48];
    size_t alt_cursor_row;
    size_t alt_cursor_col;
    union {
        struct {
            td_cell_delta_t *items;
            size_t count;
        } cells;
        struct {
            size_t alt_nrows;
            size_t *alt_order;
            uint64_t *alt_sel_bits;
            size_t alt_sel_words;
            size_t alt_nselected;
        } rows;
        struct {
            size_t col;
            int alt_width;
            char *alt_name;
            bool alt_is_key;
            size_t alt_left_col;
        } col_meta;
        struct {
            size_t ncols;
            int *alt_widths;
        } col_vis;
        struct {
            size_t col_a;
            size_t col_b;
        } col_swap;
        struct {
            size_t col;
            td_type_t alt_type;
            td_val_t *alt_cells;
            size_t ncells;
        } col_type;
        struct {
            size_t col_idx;
            size_t count;
            size_t saved_row_cap;
            td_col_t *saved_cols;
            bool present;
        } col_add;
        struct {
            size_t col;
            uint8_t alt_aggs[TD_MAX_COL_AGGS];
            uint8_t alt_naggs;
        } col_aggs;
    } u;
};

static void td_undo_entry_free(td_undo_entry_t *e)
{
    if (!e)
        return;
    switch (e->kind) {
    case TD_UNDO_CELLS:
        free(e->u.cells.items);
        break;
    case TD_UNDO_ROWS:
        free(e->u.rows.alt_order);
        free(e->u.rows.alt_sel_bits);
        break;
    case TD_UNDO_COL_META:
    case TD_UNDO_COL_SWAP:
    case TD_UNDO_COL_AGGS:
        break;
    case TD_UNDO_COL_VIS_ALL:
        free(e->u.col_vis.alt_widths);
        break;
    case TD_UNDO_COL_TYPE:
        free(e->u.col_type.alt_cells);
        break;
    case TD_UNDO_COL_ADD:
        if (!e->u.col_add.present && e->u.col_add.saved_cols) {
            for (size_t i = 0; i < e->u.col_add.count; i++)
                free(e->u.col_add.saved_cols[i].cells);
            free(e->u.col_add.saved_cols);
        }
        break;
    }
    memset(e, 0, sizeof(*e));
}

static void td_undo_stack_free(td_undo_stack_t *st)
{
    if (!st)
        return;
    for (size_t i = 0; i < st->len; i++)
        td_undo_entry_free(&st->items[i]);
    free(st->items);
    st->items = NULL;
    st->len = 0;
    st->cap = 0;
}

static bool td_undo_stack_push(td_undo_stack_t *st, const td_undo_entry_t *e)
{
    if (!st || !e)
        return false;
    if (st->len == st->cap) {
        size_t new_cap = st->cap ? st->cap * 2 : 16;
        td_undo_entry_t *items = realloc(st->items, new_cap * sizeof(*items));

        if (!items)
            return false;
        st->items = items;
        st->cap = new_cap;
    }
    st->items[st->len++] = *e;
    return true;
}

static void td_undo_init_entry(td_undo_entry_t *e, const td_sheet_t *s,
                               td_undo_kind_t kind, const char *desc)
{
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->alt_cursor_row = s->cursor_row;
    e->alt_cursor_col = s->cursor_col;
    snprintf(e->desc, sizeof(e->desc), "%s", desc ? desc : "");
}

static bool td_undo_record(td_sheet_t *s, td_undo_entry_t *e)
{
    if (!s || !s->record_undo) {
        td_undo_entry_free(e);
        return true;
    }
    if (!td_undo_stack_push(&s->undo, e)) {
        td_undo_entry_free(e);
        return false;
    }
    td_undo_stack_free(&s->redo);
    return true;
}

td_sheet_t *td_sheet_new(const char *name, const char *source_path)
{
    static uint64_t next_id = 1;
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
    s->record_undo = true;
    s->id = next_id++;
    return s;
}

void td_sheet_free(td_sheet_t *s)
{
    if (!s)
        return;
    if (s->meta_free && s->meta)
        s->meta_free(s->meta);
    td_undo_stack_free(&s->undo);
    td_undo_stack_free(&s->redo);
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
        s->cols[c].agg_dirty = true;
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

td_val_t td_sheet_eval_cell(const td_sheet_t *s, size_t phys_row, size_t col)
{
    td_val_t v = td_sheet_get_cell(s, phys_row, col);
    td_type_t type;

    if (!s || col >= s->ncols)
        return TD_VAL_NULL;
    type = s->cols[col].type;
    if (v.type == TD_TYPE_ANY && type != TD_TYPE_ANY)
        v = td_val_parse(v.as.s, type);
    return v;
}

bool td_sheet_set_cell(td_sheet_t *s, size_t row, size_t col, td_val_t v)
{
    if (!s || row >= s->phys_nrows || col >= s->ncols)
        return false;
    s->cols[col].cells[row] = v;
    s->cols[col].cached_width = 0;
    s->cols[col].agg_dirty = true;
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

static bool td_undo_snapshot_rows(const td_sheet_t *s, td_undo_entry_t *e,
                                  const char *desc)
{
    td_undo_init_entry(e, s, TD_UNDO_ROWS, desc);
    e->u.rows.alt_nrows = s->nrows;
    e->u.rows.alt_nselected = s->nselected;
    e->u.rows.alt_sel_words = s->sel_words;
    if (s->nrows > 0 && s->row_order) {
        e->u.rows.alt_order = malloc(s->nrows * sizeof(size_t));
        if (!e->u.rows.alt_order)
            return false;
        memcpy(e->u.rows.alt_order, s->row_order, s->nrows * sizeof(size_t));
    }
    if (s->sel_words > 0 && s->sel_bits) {
        e->u.rows.alt_sel_bits = malloc(s->sel_words * sizeof(uint64_t));
        if (!e->u.rows.alt_sel_bits) {
            free(e->u.rows.alt_order);
            e->u.rows.alt_order = NULL;
            return false;
        }
        memcpy(e->u.rows.alt_sel_bits, s->sel_bits,
               s->sel_words * sizeof(uint64_t));
    }
    return true;
}

static bool td_sheet_set_col_type_raw(td_sheet_t *s, size_t col, td_type_t type)
{
    for (size_t r = 0; r < s->phys_nrows; r++)
        s->cols[col].cells[r] = td_cell_as_type(s, s->cols[col].cells[r],
                                                type);
    s->cols[col].type = type;
    s->cols[col].cached_width = 0;
    s->cols[col].agg_dirty = true;
    return true;
}

bool td_sheet_set_col_type(td_sheet_t *s, size_t col, td_type_t type)
{
    td_undo_entry_t e;

    if (!s || col >= s->ncols)
        return false;
    if (s->cols[col].type == type)
        return true;

    if (s->record_undo) {
        td_undo_init_entry(&e, s, TD_UNDO_COL_TYPE, "set column type");
        e.u.col_type.col = col;
        e.u.col_type.alt_type = s->cols[col].type;
        e.u.col_type.ncells = s->phys_nrows;
        if (s->phys_nrows > 0) {
            e.u.col_type.alt_cells = malloc(s->phys_nrows * sizeof(td_val_t));
            if (!e.u.col_type.alt_cells)
                return false;
            memcpy(e.u.col_type.alt_cells, s->cols[col].cells,
                   s->phys_nrows * sizeof(td_val_t));
        }
    }

    (void)td_sheet_set_col_type_raw(s, col, type);
    if (s->record_undo)
        (void)td_undo_record(s, &e);
    return true;
}

bool td_sheet_rename_col(td_sheet_t *s, size_t col, const char *name)
{
    char *copy;
    td_undo_entry_t e;

    if (!s || col >= s->ncols)
        return false;
    copy = td_arena_strdup(&s->arena, name ? name : "");
    if (!copy)
        return false;

    if (s->record_undo) {
        td_undo_init_entry(&e, s, TD_UNDO_COL_META, "rename column");
        e.u.col_meta.col = col;
        e.u.col_meta.alt_width = s->cols[col].width;
        e.u.col_meta.alt_name = s->cols[col].name;
        e.u.col_meta.alt_is_key = s->cols[col].is_key;
        e.u.col_meta.alt_left_col = s->left_col;
        (void)td_undo_record(s, &e);
    }

    s->cols[col].name = copy;
    s->cols[col].cached_width = 0;
    return true;
}

void td_sheet_hide_col(td_sheet_t *s, size_t col)
{
    td_undo_entry_t e;

    if (!s || col >= s->ncols)
        return;
    if (s->cols[col].width == 0)
        return;

    if (s->record_undo) {
        td_undo_init_entry(&e, s, TD_UNDO_COL_META, "hide column");
        e.u.col_meta.col = col;
        e.u.col_meta.alt_width = s->cols[col].width;
        e.u.col_meta.alt_name = s->cols[col].name;
        e.u.col_meta.alt_is_key = s->cols[col].is_key;
        e.u.col_meta.alt_left_col = s->left_col;
        (void)td_undo_record(s, &e);
    }

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
    td_undo_entry_t e;

    if (!s || col >= s->ncols)
        return;
    if (s->record_undo) {
        td_undo_init_entry(&e, s, TD_UNDO_COL_META,
                           s->cols[col].is_key ? "unpin key column"
                                               : "pin key column");
        e.u.col_meta.col = col;
        e.u.col_meta.alt_width = s->cols[col].width;
        e.u.col_meta.alt_name = s->cols[col].name;
        e.u.col_meta.alt_is_key = s->cols[col].is_key;
        e.u.col_meta.alt_left_col = s->left_col;
        (void)td_undo_record(s, &e);
    }
    s->cols[col].is_key = !s->cols[col].is_key;
    s->left_col = 0;
}

static int td_sheet_autosize_col_raw(td_sheet_t *s, size_t col)
{
    int exact = td_compute_col_width(s, col, 0);

    if (s->cols[col].width > 0 && s->cols[col].width == exact &&
        exact > TD_AUTO_WIDTH_CAP) {
        s->cols[col].width = -TD_AUTO_WIDTH_CAP;
    } else {
        s->cols[col].width = exact;
    }
    s->cols[col].cached_width = 0;
    return s->cols[col].width;
}

void td_sheet_autosize_col(td_sheet_t *s, size_t col)
{
    int old_w;
    int new_w;
    td_undo_entry_t e;

    if (!s || col >= s->ncols)
        return;
    old_w = s->cols[col].width;
    if (s->record_undo) {
        td_undo_init_entry(&e, s, TD_UNDO_COL_META, "autosize column");
        e.u.col_meta.col = col;
        e.u.col_meta.alt_width = old_w;
        e.u.col_meta.alt_name = s->cols[col].name;
        e.u.col_meta.alt_is_key = s->cols[col].is_key;
        e.u.col_meta.alt_left_col = s->left_col;
    }
    new_w = td_sheet_autosize_col_raw(s, col);
    if (s->record_undo) {
        if (new_w != old_w)
            (void)td_undo_record(s, &e);
        else
            td_undo_entry_free(&e);
    }
}

void td_sheet_autosize_all_cols(td_sheet_t *s)
{
    td_undo_entry_t e;
    bool changed = false;

    if (!s || s->ncols == 0)
        return;
    if (s->record_undo) {
        td_undo_init_entry(&e, s, TD_UNDO_COL_VIS_ALL, "autosize all columns");
        e.u.col_vis.ncols = s->ncols;
        e.u.col_vis.alt_widths = malloc(s->ncols * sizeof(int));
        if (e.u.col_vis.alt_widths) {
            for (size_t c = 0; c < s->ncols; c++)
                e.u.col_vis.alt_widths[c] = s->cols[c].width;
        }
    }
    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].width != 0) {
            int prev = s->cols[c].width;

            if (td_sheet_autosize_col_raw(s, c) != prev)
                changed = true;
        }
    }
    if (s->record_undo) {
        if (changed && e.u.col_vis.alt_widths)
            (void)td_undo_record(s, &e);
        else
            td_undo_entry_free(&e);
    }
}

bool td_sheet_set_col_width(td_sheet_t *s, size_t col, int width)
{
    td_undo_entry_t e;

    if (!s || col >= s->ncols || width < 0)
        return false;
    if (width == 0) {
        td_sheet_hide_col(s, col);
        return true;
    }
    if (s->cols[col].width == width)
        return true;
    if (s->record_undo) {
        td_undo_init_entry(&e, s, TD_UNDO_COL_META, "set column width");
        e.u.col_meta.col = col;
        e.u.col_meta.alt_width = s->cols[col].width;
        e.u.col_meta.alt_name = s->cols[col].name;
        e.u.col_meta.alt_is_key = s->cols[col].is_key;
        e.u.col_meta.alt_left_col = s->left_col;
        (void)td_undo_record(s, &e);
    }
    s->cols[col].width = width;
    s->cols[col].cached_width = 0;
    return true;
}

void td_sheet_unhide_all_cols(td_sheet_t *s)
{
    td_undo_entry_t e;
    bool changed = false;

    if (!s || s->ncols == 0)
        return;
    if (s->record_undo) {
        td_undo_init_entry(&e, s, TD_UNDO_COL_VIS_ALL, "unhide all columns");
        e.u.col_vis.ncols = s->ncols;
        e.u.col_vis.alt_widths = malloc(s->ncols * sizeof(int));
        if (e.u.col_vis.alt_widths) {
            for (size_t c = 0; c < s->ncols; c++)
                e.u.col_vis.alt_widths[c] = s->cols[c].width;
        }
    }
    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].width == 0) {
            s->cols[c].width = -TD_AUTO_WIDTH_CAP;
            s->cols[c].cached_width = 0;
            changed = true;
        }
    }
    if (s->record_undo) {
        if (changed && e.u.col_vis.alt_widths)
            (void)td_undo_record(s, &e);
        else
            td_undo_entry_free(&e);
    }
}

bool td_sheet_move_col(td_sheet_t *s, size_t col, int dir)
{
    size_t target = SIZE_MAX;
    td_col_t tmp;
    bool key;
    td_undo_entry_t e;

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

    if (s->record_undo) {
        td_undo_init_entry(&e, s, TD_UNDO_COL_SWAP, "move column");
        e.u.col_swap.col_a = col;
        e.u.col_swap.col_b = target;
        (void)td_undo_record(s, &e);
    }

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

td_sheet_t *td_stack_find_by_id(const td_stack_t *st, uint64_t id)
{
    if (!st || id == 0)
        return NULL;
    for (size_t i = st->len; i-- > 0;) {
        if (st->sheets[i] && st->sheets[i]->id == id)
            return st->sheets[i];
    }
    return NULL;
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
        s->cols[c].agg_dirty = true;
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
    td_undo_entry_t e;

    if (!s || vis_row >= s->nrows || !s->row_order)
        return false;

    if (s->record_undo && !td_undo_snapshot_rows(s, &e, "delete row"))
        return false;

    phys = s->row_order[vis_row];
    td_sheet_unselect_row(s, phys);
    if (vis_row + 1 < s->nrows)
        memmove(&s->row_order[vis_row], &s->row_order[vis_row + 1],
                (s->nrows - vis_row - 1) * sizeof(s->row_order[0]));
    s->nrows--;
    td_clamp_cursor_after_delete(s);
    td_invalidate_auto_widths(s);
    if (s->record_undo)
        (void)td_undo_record(s, &e);
    return true;
}

size_t td_sheet_delete_selected(td_sheet_t *s)
{
    size_t write = 0;
    size_t deleted = 0;
    td_undo_entry_t e;

    if (!s || s->nrows == 0 || s->nselected == 0 || !s->row_order)
        return 0;

    if (s->record_undo &&
        !td_undo_snapshot_rows(s, &e, "delete selected rows"))
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
        if (s->record_undo)
            (void)td_undo_record(s, &e);
    } else if (s->record_undo) {
        td_undo_entry_free(&e);
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

td_sheet_t *td_sheet_extract_phys_rows(const td_sheet_t *s,
                                         const size_t *phys_rows, size_t nphys,
                                         const char *suffix)
{
    const char *base;
    const char *sfx;
    size_t name_len;
    char *name_buf;
    td_sheet_t *dup;

    if (!s)
        return NULL;

    base = s->name ? s->name : "";
    sfx = suffix ? suffix : "[filtered]";
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

    for (size_t i = 0; i < nphys; i++) {
        size_t phys = phys_rows[i];
        size_t dst_row;

        if (phys >= s->phys_nrows)
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

td_sheet_t *td_sheet_dup_selected(const td_sheet_t *s, const char *suffix)
{
    size_t *phys_list;
    size_t n = 0;
    td_sheet_t *dup;

    if (!s || s->nselected == 0)
        return NULL;

    phys_list = malloc(s->nrows * sizeof(*phys_list));
    if (!phys_list)
        return NULL;
    for (size_t vr = 0; vr < s->nrows; vr++) {
        size_t phys = s->row_order ? s->row_order[vr] : vr;

        if (td_sheet_is_selected(s, phys))
            phys_list[n++] = phys;
    }

    dup = td_sheet_extract_phys_rows(s, phys_list, n,
                                     suffix ? suffix : "[selected]");
    free(phys_list);
    return dup;
}

bool td_sheet_set_col_aggs(td_sheet_t *s, size_t col, const uint8_t *aggs,
                           uint8_t naggs, const char *desc)
{
    td_undo_entry_t e;

    if (!s || col >= s->ncols || naggs > TD_MAX_COL_AGGS)
        return false;
    if (naggs > 0 && !aggs)
        return false;

    if (s->record_undo) {
        td_undo_init_entry(&e, s, TD_UNDO_COL_AGGS,
                           desc ? desc : "set aggregators");
        e.u.col_aggs.col = col;
        e.u.col_aggs.alt_naggs = s->cols[col].naggs;
        memcpy(e.u.col_aggs.alt_aggs, s->cols[col].aggs,
               sizeof(e.u.col_aggs.alt_aggs));
        if (!td_undo_record(s, &e))
            return false;
    }

    memset(s->cols[col].aggs, 0, sizeof(s->cols[col].aggs));
    if (naggs > 0)
        memcpy(s->cols[col].aggs, aggs, naggs * sizeof(aggs[0]));
    s->cols[col].naggs = naggs;
    s->cols[col].agg_dirty = true;
    return true;
}

int td_sheet_footer_rows(const td_sheet_t *s, int term_rows)
{
    int max_aggs = 0;
    int avail;

    if (!s || term_rows < 5)
        return 0;
    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].width != 0 && (int)s->cols[c].naggs > max_aggs)
            max_aggs = (int)s->cols[c].naggs;
    }
    if (max_aggs == 0)
        return 0;
    avail = term_rows - 4; /* header + at least 1 body row + status + hints */
    if (max_aggs > avail)
        max_aggs = avail;
    return max_aggs > 0 ? max_aggs : 0;
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
    td_undo_entry_t e;

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

    if (s->record_undo &&
        !td_undo_snapshot_rows(s, &e,
                               desc ? "sort descending" : "sort ascending")) {
        free(scratch);
        return false;
    }

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
    if (s->record_undo)
        (void)td_undo_record(s, &e);
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

void td_sheet_set_undo_recording(td_sheet_t *s, bool enabled)
{
    if (s)
        s->record_undo = enabled;
}

bool td_sheet_can_undo(const td_sheet_t *s)
{
    return s && s->undo.len > 0;
}

bool td_sheet_can_redo(const td_sheet_t *s)
{
    return s && s->redo.len > 0;
}

static bool td_undo_swap_rows(td_sheet_t *s, td_undo_entry_t *e)
{
    size_t *cur_order = NULL;
    uint64_t *cur_bits = NULL;
    size_t cur_nrows = s->nrows;
    size_t cur_nselected = s->nselected;
    size_t cur_sel_words = s->sel_words;

    if (e->u.rows.alt_nrows > 0 &&
        !td_sheet_grow_rows(s, e->u.rows.alt_nrows))
        return false;

    if (cur_nrows > 0 && s->row_order) {
        cur_order = malloc(cur_nrows * sizeof(size_t));
        if (!cur_order)
            return false;
        memcpy(cur_order, s->row_order, cur_nrows * sizeof(size_t));
    }
    if (cur_sel_words > 0 && s->sel_bits) {
        cur_bits = malloc(cur_sel_words * sizeof(uint64_t));
        if (!cur_bits) {
            free(cur_order);
            return false;
        }
        memcpy(cur_bits, s->sel_bits, cur_sel_words * sizeof(uint64_t));
    }

    if (e->u.rows.alt_nrows > 0 && e->u.rows.alt_order && s->row_order)
        memcpy(s->row_order, e->u.rows.alt_order,
               e->u.rows.alt_nrows * sizeof(size_t));
    if (s->sel_bits && s->sel_words > 0) {
        size_t copy_words = s->sel_words < e->u.rows.alt_sel_words
                                ? s->sel_words
                                : e->u.rows.alt_sel_words;

        memset(s->sel_bits, 0, s->sel_words * sizeof(uint64_t));
        if (copy_words > 0 && e->u.rows.alt_sel_bits)
            memcpy(s->sel_bits, e->u.rows.alt_sel_bits,
                   copy_words * sizeof(uint64_t));
    }

    free(e->u.rows.alt_order);
    free(e->u.rows.alt_sel_bits);
    s->nrows = e->u.rows.alt_nrows;
    s->nselected = e->u.rows.alt_nselected;
    e->u.rows.alt_order = cur_order;
    e->u.rows.alt_nrows = cur_nrows;
    e->u.rows.alt_sel_bits = cur_bits;
    e->u.rows.alt_sel_words = cur_sel_words;
    e->u.rows.alt_nselected = cur_nselected;
    td_invalidate_auto_widths(s);
    return true;
}

static bool td_undo_swap_col_add(td_sheet_t *s, td_undo_entry_t *e)
{
    size_t idx = e->u.col_add.col_idx;
    size_t count = e->u.col_add.count;

    if (count == 0)
        return false;

    if (e->u.col_add.present) {
        td_col_t *saved;

        if (idx >= s->ncols || count > s->ncols - idx)
            return false;
        saved = calloc(count, sizeof(td_col_t));
        if (!saved)
            return false;
        memcpy(saved, &s->cols[idx], count * sizeof(td_col_t));
        if (idx + count < s->ncols)
            memmove(&s->cols[idx], &s->cols[idx + count],
                    (s->ncols - idx - count) * sizeof(s->cols[0]));
        s->ncols -= count;
        e->u.col_add.saved_cols = saved;
        e->u.col_add.saved_row_cap = s->row_cap;
        e->u.col_add.present = false;
        return true;
    }

    if (idx > s->ncols || count > SIZE_MAX - s->ncols ||
        !td_sheet_grow_cols(s, s->ncols + count))
        return false;
    if (s->row_cap > 0 && e->u.col_add.saved_cols) {
        for (size_t k = 0; k < count; k++) {
            td_col_t *sc = &e->u.col_add.saved_cols[k];

            if (s->row_cap > e->u.col_add.saved_row_cap || !sc->cells) {
                td_val_t *cells = realloc(sc->cells,
                                          s->row_cap * sizeof(td_val_t));
                if (!cells)
                    return false;
                for (size_t r = e->u.col_add.saved_row_cap; r < s->row_cap; r++)
                    cells[r] = TD_VAL_NULL;
                sc->cells = cells;
            }
        }
    }
    if (idx < s->ncols)
        memmove(&s->cols[idx + count], &s->cols[idx],
                (s->ncols - idx) * sizeof(s->cols[0]));
    for (size_t k = 0; k < count; k++) {
        s->cols[idx + k] = e->u.col_add.saved_cols[k];
        s->cols[idx + k].cached_width = 0;
        s->cols[idx + k].agg_dirty = true;
    }
    free(e->u.col_add.saved_cols);
    e->u.col_add.saved_cols = NULL;
    s->ncols += count;
    e->u.col_add.present = true;
    return true;
}

static bool td_undo_apply(td_sheet_t *s, td_undo_stack_t *from,
                          td_undo_stack_t *to, char *desc_out, size_t desc_cap)
{
    td_undo_entry_t e;
    size_t cur_cr;
    size_t cur_cc;
    bool ok = true;

    if (desc_out && desc_cap > 0)
        desc_out[0] = '\0';
    if (!s || !from || from->len == 0)
        return false;

    e = from->items[--from->len];
    cur_cr = s->cursor_row;
    cur_cc = s->cursor_col;

    switch (e.kind) {
    case TD_UNDO_CELLS:
        for (size_t i = 0; i < e.u.cells.count; i++) {
            td_cell_delta_t *d = &e.u.cells.items[i];

            if (d->phys_row < s->phys_nrows && d->col < s->ncols) {
                td_val_t tmp = s->cols[d->col].cells[d->phys_row];

                s->cols[d->col].cells[d->phys_row] = d->alt_val;
                d->alt_val = tmp;
                s->cols[d->col].cached_width = 0;
                s->cols[d->col].agg_dirty = true;
            }
        }
        break;
    case TD_UNDO_ROWS:
        ok = td_undo_swap_rows(s, &e);
        break;
    case TD_UNDO_COL_META: {
        size_t c = e.u.col_meta.col;

        if (c < s->ncols) {
            int tmp_w = s->cols[c].width;
            char *tmp_n = s->cols[c].name;
            bool tmp_k = s->cols[c].is_key;
            size_t tmp_l = s->left_col;

            s->cols[c].width = e.u.col_meta.alt_width;
            s->cols[c].name = e.u.col_meta.alt_name;
            s->cols[c].is_key = e.u.col_meta.alt_is_key;
            s->cols[c].cached_width = 0;
            s->left_col = e.u.col_meta.alt_left_col;

            e.u.col_meta.alt_width = tmp_w;
            e.u.col_meta.alt_name = tmp_n;
            e.u.col_meta.alt_is_key = tmp_k;
            e.u.col_meta.alt_left_col = tmp_l;
        }
        break;
    }
    case TD_UNDO_COL_VIS_ALL:
        for (size_t c = 0; c < e.u.col_vis.ncols && c < s->ncols; c++) {
            int tmp_w = s->cols[c].width;

            s->cols[c].width = e.u.col_vis.alt_widths[c];
            s->cols[c].cached_width = 0;
            e.u.col_vis.alt_widths[c] = tmp_w;
        }
        break;
    case TD_UNDO_COL_SWAP: {
        size_t a = e.u.col_swap.col_a;
        size_t b = e.u.col_swap.col_b;

        if (a < s->ncols && b < s->ncols) {
            td_col_t tmp = s->cols[a];

            s->cols[a] = s->cols[b];
            s->cols[b] = tmp;
        }
        break;
    }
    case TD_UNDO_COL_TYPE: {
        size_t c = e.u.col_type.col;

        if (c < s->ncols) {
            td_type_t tmp_t = s->cols[c].type;
            size_t n = e.u.col_type.ncells;

            if (n > s->phys_nrows)
                n = s->phys_nrows;
            for (size_t r = 0; r < n; r++) {
                td_val_t tmp = s->cols[c].cells[r];

                s->cols[c].cells[r] = e.u.col_type.alt_cells[r];
                e.u.col_type.alt_cells[r] = tmp;
            }
            s->cols[c].type = e.u.col_type.alt_type;
            s->cols[c].cached_width = 0;
            s->cols[c].agg_dirty = true;
            e.u.col_type.alt_type = tmp_t;
        }
        break;
    }
    case TD_UNDO_COL_ADD:
        ok = td_undo_swap_col_add(s, &e);
        break;
    case TD_UNDO_COL_AGGS: {
        size_t c = e.u.col_aggs.col;

        if (c < s->ncols) {
            uint8_t tmp_aggs[TD_MAX_COL_AGGS];
            uint8_t tmp_n = s->cols[c].naggs;

            memcpy(tmp_aggs, s->cols[c].aggs, sizeof(tmp_aggs));
            memcpy(s->cols[c].aggs, e.u.col_aggs.alt_aggs,
                   sizeof(s->cols[c].aggs));
            s->cols[c].naggs = e.u.col_aggs.alt_naggs;
            s->cols[c].agg_dirty = true;
            memcpy(e.u.col_aggs.alt_aggs, tmp_aggs,
                   sizeof(e.u.col_aggs.alt_aggs));
            e.u.col_aggs.alt_naggs = tmp_n;
        }
        break;
    }
    }

    if (!ok) {
        if (!td_undo_stack_push(from, &e))
            td_undo_entry_free(&e);
        return false;
    }

    if (desc_out && desc_cap > 0)
        snprintf(desc_out, desc_cap, "%s", e.desc);

    s->cursor_row = e.alt_cursor_row;
    s->cursor_col = e.alt_cursor_col;
    if (s->nrows == 0)
        s->cursor_row = 0;
    else if (s->cursor_row >= s->nrows)
        s->cursor_row = s->nrows - 1;
    if (s->ncols == 0)
        s->cursor_col = 0;
    else if (s->cursor_col >= s->ncols)
        s->cursor_col = s->ncols - 1;

    e.alt_cursor_row = cur_cr;
    e.alt_cursor_col = cur_cc;
    if (!td_undo_stack_push(to, &e))
        td_undo_entry_free(&e);
    return true;
}

bool td_sheet_undo(td_sheet_t *s, char *desc_out, size_t desc_cap)
{
    if (!s)
        return false;
    return td_undo_apply(s, &s->undo, &s->redo, desc_out, desc_cap);
}

bool td_sheet_redo(td_sheet_t *s, char *desc_out, size_t desc_cap)
{
    if (!s)
        return false;
    return td_undo_apply(s, &s->redo, &s->undo, desc_out, desc_cap);
}

static td_val_t td_parse_text_for_col(td_sheet_t *s, size_t col,
                                      const char *text, bool *ok)
{
    td_type_t type = s->cols[col].type;
    char *copy;
    td_val_t v;

    *ok = true;
    if (!text || !text[0])
        return TD_VAL_NULL;

    copy = td_arena_strdup(&s->arena, text);
    if (!copy) {
        *ok = false;
        return TD_VAL_ERR;
    }
    v = td_val_parse(td_slice_from_cstr(copy), type);
    if (v.type == TD_TYPE_ERR)
        v.as.s = td_slice_from_cstr(copy);
    return v;
}

static td_val_t td_adapt_val_for_col(td_sheet_t *s, size_t col, td_val_t src,
                                     bool *ok)
{
    td_type_t target = s->cols[col].type;
    td_val_t cloned;

    *ok = true;
    if (src.type == TD_TYPE_NULL)
        return TD_VAL_NULL;

    cloned = td_clone_val_to_arena(&s->arena, src, ok);
    if (!*ok)
        return TD_VAL_ERR;

    if (target == TD_TYPE_ANY || cloned.type == target)
        return cloned;

    if (cloned.type == TD_TYPE_ERR && cloned.as.s.data) {
        td_val_t parsed = td_val_parse(cloned.as.s, target);

        if (parsed.type == TD_TYPE_ERR)
            parsed.as.s = cloned.as.s;
        return parsed;
    }

    td_val_t conv = td_cell_as_type(s, cloned, target);

    if (conv.type == TD_TYPE_ERR &&
        (cloned.type == TD_TYPE_ANY || cloned.type == TD_TYPE_STR) &&
        cloned.as.s.data)
        conv.as.s = cloned.as.s;
    return conv;
}

bool td_sheet_apply_cell_batch(td_sheet_t *s, const size_t *vis_rows,
                               const size_t *cols, const td_val_t *vals,
                               size_t count, const char *desc)
{
    td_undo_entry_t e;
    size_t applied = 0;

    if (!s || !vis_rows || !cols || !vals || count == 0 || !s->row_order)
        return false;

    if (s->record_undo) {
        td_undo_init_entry(&e, s, TD_UNDO_CELLS, desc ? desc : "edit cells");
        e.u.cells.items = malloc(count * sizeof(td_cell_delta_t));
        if (!e.u.cells.items)
            return false;
    }

    for (size_t i = 0; i < count; i++) {
        size_t vr = vis_rows[i];
        size_t col = cols[i];
        size_t phys;
        bool ok = true;
        td_val_t nv;

        if (vr >= s->nrows || col >= s->ncols)
            continue;
        phys = s->row_order[vr];
        nv = td_adapt_val_for_col(s, col, vals[i], &ok);
        if (!ok) {
            if (s->record_undo)
                td_undo_entry_free(&e);
            return false;
        }
        if (s->record_undo) {
            e.u.cells.items[applied].phys_row = phys;
            e.u.cells.items[applied].col = col;
            e.u.cells.items[applied].alt_val = s->cols[col].cells[phys];
        }
        s->cols[col].cells[phys] = nv;
        s->cols[col].cached_width = 0;
        s->cols[col].agg_dirty = true;
        applied++;
    }

    if (s->record_undo) {
        if (applied > 0) {
            e.u.cells.count = applied;
            (void)td_undo_record(s, &e);
        } else {
            td_undo_entry_free(&e);
        }
    }
    return applied > 0;
}

bool td_sheet_edit_cell(td_sheet_t *s, size_t vis_row, size_t col,
                        const char *text)
{
    bool ok = true;
    td_val_t v;

    if (!s || vis_row >= s->nrows || col >= s->ncols)
        return false;
    v = td_parse_text_for_col(s, col, text, &ok);
    if (!ok)
        return false;
    return td_sheet_apply_cell_batch(s, &vis_row, &col, &v, 1, "edit cell");
}

size_t td_sheet_edit_selected(td_sheet_t *s, size_t col, const char *text)
{
    bool ok = true;
    td_val_t v;
    size_t *vrows;
    size_t *cols;
    td_val_t *vals;
    size_t n = 0;

    if (!s || s->nrows == 0 || s->nselected == 0 || col >= s->ncols ||
        !s->row_order)
        return 0;

    v = td_parse_text_for_col(s, col, text, &ok);
    if (!ok)
        return 0;

    vrows = malloc(s->nrows * sizeof(size_t));
    cols = malloc(s->nrows * sizeof(size_t));
    vals = malloc(s->nrows * sizeof(td_val_t));
    if (!vrows || !cols || !vals) {
        free(vrows);
        free(cols);
        free(vals);
        return 0;
    }

    for (size_t vr = 0; vr < s->nrows; vr++) {
        if (td_sheet_is_selected(s, s->row_order[vr])) {
            vrows[n] = vr;
            cols[n] = col;
            vals[n] = v;
            n++;
        }
    }

    if (n > 0)
        (void)td_sheet_apply_cell_batch(s, vrows, cols, vals, n,
                                        "edit selected");
    free(vrows);
    free(cols);
    free(vals);
    return n;
}

bool td_sheet_clear_cell(td_sheet_t *s, size_t vis_row, size_t col)
{
    td_val_t v = TD_VAL_NULL;

    if (!s || vis_row >= s->nrows || col >= s->ncols)
        return false;
    return td_sheet_apply_cell_batch(s, &vis_row, &col, &v, 1, "clear cell");
}

size_t td_sheet_clear_selected(td_sheet_t *s, size_t col)
{
    if (!s || s->nselected == 0 || col >= s->ncols)
        return 0;
    return td_sheet_edit_selected(s, col, "");
}

bool td_sheet_insert_rows_populated(td_sheet_t *s, size_t vis_pos,
                                    size_t nrows, size_t ncols,
                                    const td_val_t *cells, const char *desc)
{
    td_undo_entry_t e;

    if (!s || nrows == 0)
        return false;
    if (vis_pos > s->nrows)
        vis_pos = s->nrows;
    if (s->phys_nrows > SIZE_MAX - nrows ||
        !td_sheet_grow_rows(s, s->phys_nrows + nrows))
        return false;

    if (s->record_undo &&
        !td_undo_snapshot_rows(s, &e, desc ? desc : "insert rows"))
        return false;

    if (vis_pos < s->nrows)
        memmove(&s->row_order[vis_pos + nrows], &s->row_order[vis_pos],
                (s->nrows - vis_pos) * sizeof(s->row_order[0]));

    for (size_t r = 0; r < nrows; r++) {
        size_t phys = s->phys_nrows++;

        for (size_t c = 0; c < s->ncols; c++) {
            td_val_t v = TD_VAL_NULL;

            if (cells && c < ncols) {
                bool ok = true;

                v = td_adapt_val_for_col(s, c, cells[r * ncols + c], &ok);
                if (!ok)
                    v = TD_VAL_ERR;
            }
            s->cols[c].cells[phys] = v;
            s->cols[c].cached_width = 0;
            s->cols[c].agg_dirty = true;
        }
        s->row_order[vis_pos + r] = phys;
    }

    s->nrows += nrows;
    s->cursor_row = vis_pos;
    if (s->record_undo)
        (void)td_undo_record(s, &e);
    return true;
}

bool td_sheet_insert_rows(td_sheet_t *s, size_t vis_pos, size_t count)
{
    return td_sheet_insert_rows_populated(s, vis_pos, count, 0, NULL,
                                          count == 1 ? "add row" : "add rows");
}

bool td_sheet_dup_row(td_sheet_t *s, size_t vis_row)
{
    td_val_t *row_vals;
    size_t phys;
    bool ok;

    if (!s || vis_row >= s->nrows || !s->row_order)
        return false;
    if (s->ncols == 0)
        return td_sheet_insert_rows_populated(s, vis_row + 1, 1, 0, NULL,
                                              "duplicate row");

    row_vals = malloc(s->ncols * sizeof(td_val_t));
    if (!row_vals)
        return false;
    phys = s->row_order[vis_row];
    for (size_t c = 0; c < s->ncols; c++)
        row_vals[c] = s->cols[c].cells[phys];

    ok = td_sheet_insert_rows_populated(s, vis_row + 1, 1, s->ncols, row_vals,
                                        "duplicate row");
    free(row_vals);
    return ok;
}

static bool td_col_name_exists(const td_sheet_t *s, const char *name)
{
    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].name && strcmp(s->cols[c].name, name) == 0)
            return true;
    }
    return false;
}

static bool td_sheet_insert_cols_batch(td_sheet_t *s, size_t col_pos,
                                       td_col_t *cols_in, size_t count,
                                       const char *desc)
{
    td_undo_entry_t e;

    if (!s || !cols_in || count == 0 || col_pos > s->ncols ||
        count > SIZE_MAX - s->ncols ||
        !td_sheet_grow_cols(s, s->ncols + count))
        return false;

    if (s->record_undo) {
        td_undo_init_entry(&e, s, TD_UNDO_COL_ADD, desc ? desc : "add columns");
        e.u.col_add.col_idx = col_pos;
        e.u.col_add.count = count;
        e.u.col_add.present = true;
    }

    if (col_pos < s->ncols)
        memmove(&s->cols[col_pos + count], &s->cols[col_pos],
                (s->ncols - col_pos) * sizeof(s->cols[0]));
    for (size_t k = 0; k < count; k++)
        s->cols[col_pos + k] = cols_in[k];
    s->ncols += count;
    s->cursor_col = col_pos;

    if (s->record_undo)
        (void)td_undo_record(s, &e);
    return true;
}

static size_t td_sheet_insert_col_at(td_sheet_t *s, size_t col_pos,
                                     const char *name, td_type_t type,
                                     int width, size_t copy_from,
                                     const char *desc)
{
    td_col_t col;

    if (!s || col_pos > s->ncols)
        return SIZE_MAX;

    memset(&col, 0, sizeof(col));
    col.name = td_arena_strdup(&s->arena, name ? name : "");
    if (!col.name)
        return SIZE_MAX;
    col.type = type;
    col.width = width;
    col.is_key = false;

    if (s->row_cap > 0) {
        col.cells = malloc(s->row_cap * sizeof(td_val_t));
        if (!col.cells)
            return SIZE_MAX;
        for (size_t r = 0; r < s->row_cap; r++)
            col.cells[r] = TD_VAL_NULL;
        if (copy_from < s->ncols) {
            for (size_t r = 0; r < s->phys_nrows; r++) {
                bool ok = true;

                col.cells[r] = td_clone_val_to_arena(
                    &s->arena, s->cols[copy_from].cells[r], &ok);
                if (!ok) {
                    free(col.cells);
                    return SIZE_MAX;
                }
            }
        }
    }

    if (!td_sheet_insert_cols_batch(s, col_pos, &col, 1, desc)) {
        free(col.cells);
        return SIZE_MAX;
    }
    return col_pos;
}

size_t td_sheet_append_blank_col(td_sheet_t *s)
{
    char name[64];
    size_t n;

    if (!s)
        return SIZE_MAX;
    n = s->ncols + 1;
    do {
        snprintf(name, sizeof(name), "col%zu", n++);
    } while (td_col_name_exists(s, name));

    return td_sheet_insert_col_at(s, s->ncols, name, TD_TYPE_ANY,
                                  -TD_AUTO_WIDTH_CAP, SIZE_MAX, "add column");
}

bool td_sheet_dup_col(td_sheet_t *s, size_t col)
{
    const char *base;
    size_t len;
    char *name;
    size_t idx;
    int width;

    if (!s || col >= s->ncols)
        return false;
    base = (s->cols[col].name && s->cols[col].name[0]) ? s->cols[col].name
                                                       : "col";
    len = strlen(base) + 16;
    name = malloc(len);
    if (!name)
        return false;
    snprintf(name, len, "%s_copy", base);
    for (size_t suffix = 2; td_col_name_exists(s, name); suffix++)
        snprintf(name, len, "%s_copy%zu", base, suffix);

    width = s->cols[col].width != 0 ? s->cols[col].width : -TD_AUTO_WIDTH_CAP;
    idx = td_sheet_insert_col_at(s, col + 1, name, s->cols[col].type, width,
                                 col, "duplicate column");
    free(name);
    return idx != SIZE_MAX;
}

static void td_sheet_set_err(char *err, size_t err_cap, const char *msg)
{
    if (err && err_cap > 0)
        snprintf(err, err_cap, "%s", msg ? msg : "");
}

static td_type_t td_infer_expr_col_type(td_type_t cur, bool *has_type,
                                        td_type_t val_type)
{
    if (val_type == TD_TYPE_NULL || val_type == TD_TYPE_ERR)
        return cur;
    if (!*has_type) {
        *has_type = true;
        return val_type;
    }
    if (cur == val_type)
        return cur;
    if ((cur == TD_TYPE_INT || cur == TD_TYPE_FLOAT) &&
        (val_type == TD_TYPE_INT || val_type == TD_TYPE_FLOAT))
        return TD_TYPE_FLOAT;
    if ((cur == TD_TYPE_INT || cur == TD_TYPE_FLOAT ||
         cur == TD_TYPE_CURRENCY) &&
        (val_type == TD_TYPE_INT || val_type == TD_TYPE_FLOAT ||
         val_type == TD_TYPE_CURRENCY))
        return TD_TYPE_CURRENCY;
    return TD_TYPE_ANY;
}

bool td_sheet_add_expr_col(td_sheet_t *s, size_t col_pos,
                           const char *expr_text, char *err, size_t err_cap)
{
    td_expr_t *expr;
    td_col_t col;
    td_arena_t scratch;
    td_type_t inferred = TD_TYPE_ANY;
    bool has_type = false;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!s || col_pos > s->ncols) {
        td_sheet_set_err(err, err_cap, "invalid column position");
        return false;
    }
    expr = td_expr_compile(s, expr_text, err, err_cap);
    if (!expr)
        return false;

    memset(&col, 0, sizeof(col));
    col.name = td_arena_strdup(&s->arena, expr_text);
    if (!col.name) {
        td_expr_free(expr);
        td_sheet_set_err(err, err_cap, "out of memory");
        return false;
    }
    col.type = TD_TYPE_ANY;
    col.width = -TD_AUTO_WIDTH_CAP;

    td_arena_init(&scratch, 0);
    if (s->row_cap > 0) {
        col.cells = malloc(s->row_cap * sizeof(td_val_t));
        if (!col.cells) {
            td_arena_free(&scratch);
            td_expr_free(expr);
            td_sheet_set_err(err, err_cap, "out of memory");
            return false;
        }
        for (size_t r = 0; r < s->row_cap; r++)
            col.cells[r] = TD_VAL_NULL;

        for (size_t r = 0; r < s->phys_nrows; r++) {
            char eval_err[128] = "";
            bool ok = true;
            td_val_t v;

            td_arena_reset(&scratch);
            v = td_expr_eval(expr, s, r, &scratch, eval_err, sizeof(eval_err));
            v = td_clone_val_to_arena(&s->arena, v, &ok);
            if (!ok) {
                free(col.cells);
                td_arena_free(&scratch);
                td_expr_free(expr);
                td_sheet_set_err(err, err_cap, "out of memory");
                return false;
            }
            col.cells[r] = v;
            inferred = td_infer_expr_col_type(inferred, &has_type, v.type);
        }
    }
    td_arena_free(&scratch);
    td_expr_free(expr);

    if (has_type) {
        col.type = inferred;
        if (inferred == TD_TYPE_FLOAT || inferred == TD_TYPE_CURRENCY) {
            for (size_t r = 0; r < s->phys_nrows; r++) {
                if (col.cells[r].type == TD_TYPE_INT) {
                    col.cells[r].as.f = (double)col.cells[r].as.i;
                    col.cells[r].type = inferred;
                } else if (col.cells[r].type == TD_TYPE_FLOAT &&
                           inferred == TD_TYPE_CURRENCY) {
                    col.cells[r].type = TD_TYPE_CURRENCY;
                }
            }
        }
    }

    if (!td_sheet_insert_cols_batch(s, col_pos, &col, 1, "add expr column")) {
        free(col.cells);
        td_sheet_set_err(err, err_cap, "out of memory");
        return false;
    }
    return true;
}

size_t td_sheet_set_selected_expr(td_sheet_t *s, size_t col,
                                  const char *expr_text, char *err,
                                  size_t err_cap)
{
    td_expr_t *expr;
    size_t *vrows;
    size_t *cols;
    td_val_t *vals;
    td_arena_t scratch;
    size_t n = 0;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!s || col >= s->ncols) {
        td_sheet_set_err(err, err_cap, "no column");
        return 0;
    }
    if (s->nselected == 0 || s->nrows == 0 || !s->row_order) {
        td_sheet_set_err(err, err_cap, "no rows selected");
        return 0;
    }

    expr = td_expr_compile(s, expr_text, err, err_cap);
    if (!expr)
        return 0;

    vrows = malloc(s->nrows * sizeof(size_t));
    cols = malloc(s->nrows * sizeof(size_t));
    vals = malloc(s->nrows * sizeof(td_val_t));
    if (!vrows || !cols || !vals) {
        free(vrows);
        free(cols);
        free(vals);
        td_expr_free(expr);
        td_sheet_set_err(err, err_cap, "out of memory");
        return 0;
    }

    td_arena_init(&scratch, 0);
    for (size_t vr = 0; vr < s->nrows; vr++) {
        size_t phys = s->row_order[vr];

        if (!td_sheet_is_selected(s, phys))
            continue;
        char eval_err[128] = "";
        bool ok = true;
        td_val_t v;

        td_arena_reset(&scratch);
        v = td_expr_eval(expr, s, phys, &scratch, eval_err, sizeof(eval_err));
        v = td_clone_val_to_arena(&s->arena, v, &ok);
        if (!ok) {
            n = 0;
            td_sheet_set_err(err, err_cap, "out of memory");
            break;
        }
        vrows[n] = vr;
        cols[n] = col;
        vals[n] = v;
        n++;
    }
    td_arena_free(&scratch);
    td_expr_free(expr);

    if (n > 0 && !td_sheet_apply_cell_batch(s, vrows, cols, vals, n,
                                            "set selected expr")) {
        n = 0;
        td_sheet_set_err(err, err_cap, "failed to apply expression");
    }

    free(vrows);
    free(cols);
    free(vals);
    return n;
}

static char *td_split_format_cell(const td_sheet_t *s, size_t phys_row,
                                  size_t col, char *stack_buf,
                                  size_t stack_cap, size_t *len_out,
                                  char **dyn_out)
{
    td_val_t v = td_sheet_get_cell(s, phys_row, col);
    size_t n;

    *dyn_out = NULL;
    if (v.type == TD_TYPE_NULL) {
        stack_buf[0] = '\0';
        *len_out = 0;
        return stack_buf;
    }
    n = td_val_format(v, s->cols[col].type, stack_buf, stack_cap);
    if (n < stack_cap) {
        *len_out = n;
        return stack_buf;
    }
    *dyn_out = malloc(n + 1);
    if (!*dyn_out) {
        *len_out = 0;
        return NULL;
    }
    (void)td_val_format(v, s->cols[col].type, *dyn_out, n + 1);
    *len_out = n;
    return *dyn_out;
}

static bool td_init_split_cols(td_sheet_t *s, size_t src_col, td_col_t *out,
                               size_t count)
{
    const char *base = (s->cols[src_col].name && s->cols[src_col].name[0])
                           ? s->cols[src_col].name
                           : "col";
    size_t name_cap = strlen(base) + 32;
    char *name_buf = malloc(name_cap);

    if (!name_buf)
        return false;

    for (size_t k = 0; k < count; k++) {
        memset(&out[k], 0, sizeof(out[k]));
        snprintf(name_buf, name_cap, "%s_%zu", base, k + 1);
        for (size_t dup = 2; td_col_name_exists(s, name_buf); dup++)
            snprintf(name_buf, name_cap, "%s_%zu_%zu", base, k + 1, dup);
        out[k].name = td_arena_strdup(&s->arena, name_buf);
        if (!out[k].name) {
            free(name_buf);
            for (size_t j = 0; j < k; j++)
                free(out[j].cells);
            return false;
        }
        out[k].type = TD_TYPE_ANY;
        out[k].width = -TD_AUTO_WIDTH_CAP;
        if (s->row_cap > 0) {
            out[k].cells = malloc(s->row_cap * sizeof(td_val_t));
            if (!out[k].cells) {
                free(name_buf);
                for (size_t j = 0; j < k; j++)
                    free(out[j].cells);
                return false;
            }
            for (size_t r = 0; r < s->row_cap; r++)
                out[k].cells[r] = TD_VAL_NULL;
        }
    }
    free(name_buf);
    return true;
}

static void td_free_split_cols(td_col_t *cols, size_t count)
{
    if (!cols)
        return;
    for (size_t k = 0; k < count; k++)
        free(cols[k].cells);
    free(cols);
}

static td_val_t td_make_arena_slice_val(td_arena_t *a, const char *p,
                                        size_t len, bool *ok)
{
    char *copy;
    td_val_t v;

    *ok = true;
    if (!p || len == 0)
        return TD_VAL_NULL;
    copy = td_arena_strndup(a, p, len);
    if (!copy) {
        *ok = false;
        return TD_VAL_ERR;
    }
    v.type = TD_TYPE_ANY;
    v.as.s.data = copy;
    v.as.s.len = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
    return v;
}

bool td_sheet_split_col_delim(td_sheet_t *s, size_t col, const char *delim,
                              size_t *added_out, char *err, size_t err_cap)
{
    const char *sep;
    size_t sep_len;
    size_t max_parts = 1;
    td_col_t *new_cols;

    if (added_out)
        *added_out = 0;
    if (err && err_cap > 0)
        err[0] = '\0';
    if (!s || col >= s->ncols) {
        td_sheet_set_err(err, err_cap, "no column");
        return false;
    }
    if (!delim || !delim[0]) {
        td_sheet_set_err(err, err_cap, "empty delimiter");
        return false;
    }
    sep = strcmp(delim, "\\t") == 0 ? "\t" : delim;
    sep_len = strlen(sep);

    for (size_t r = 0; r < s->phys_nrows; r++) {
        char stack_buf[256];
        char *dyn = NULL;
        size_t len = 0;
        const char *text = td_split_format_cell(s, r, col, stack_buf,
                                                sizeof(stack_buf), &len, &dyn);
        size_t parts = 1;
        size_t pos = 0;

        if (!text) {
            td_sheet_set_err(err, err_cap, "out of memory");
            return false;
        }
        while (pos + sep_len <= len && parts < TD_SPLIT_MAX_COLS) {
            if (memcmp(text + pos, sep, sep_len) == 0) {
                parts++;
                pos += sep_len;
            } else {
                pos++;
            }
        }
        if (parts > max_parts)
            max_parts = parts;
        free(dyn);
    }

    new_cols = calloc(max_parts, sizeof(td_col_t));
    if (!new_cols || !td_init_split_cols(s, col, new_cols, max_parts)) {
        free(new_cols);
        td_sheet_set_err(err, err_cap, "out of memory");
        return false;
    }

    for (size_t r = 0; r < s->phys_nrows; r++) {
        char stack_buf[256];
        char *dyn = NULL;
        size_t len = 0;
        const char *text = td_split_format_cell(s, r, col, stack_buf,
                                                sizeof(stack_buf), &len, &dyn);
        size_t start = 0;
        size_t pos = 0;
        size_t part = 0;

        if (!text) {
            td_free_split_cols(new_cols, max_parts);
            td_sheet_set_err(err, err_cap, "out of memory");
            return false;
        }
        if (len > 0) {
            while (part + 1 < max_parts && pos + sep_len <= len) {
                if (memcmp(text + pos, sep, sep_len) == 0) {
                    bool ok = true;

                    new_cols[part].cells[r] = td_make_arena_slice_val(
                        &s->arena, text + start, pos - start, &ok);
                    if (!ok) {
                        free(dyn);
                        td_free_split_cols(new_cols, max_parts);
                        td_sheet_set_err(err, err_cap, "out of memory");
                        return false;
                    }
                    part++;
                    pos += sep_len;
                    start = pos;
                } else {
                    pos++;
                }
            }
            if (part < max_parts) {
                bool ok = true;

                new_cols[part].cells[r] = td_make_arena_slice_val(
                    &s->arena, text + start, len - start, &ok);
                if (!ok) {
                    free(dyn);
                    td_free_split_cols(new_cols, max_parts);
                    td_sheet_set_err(err, err_cap, "out of memory");
                    return false;
                }
            }
        }
        free(dyn);
    }

    if (!td_sheet_insert_cols_batch(s, col + 1, new_cols, max_parts,
                                    "split column")) {
        td_free_split_cols(new_cols, max_parts);
        td_sheet_set_err(err, err_cap, "out of memory");
        return false;
    }
    free(new_cols);
    if (added_out)
        *added_out = max_parts;
    return true;
}

bool td_sheet_split_col_regex(td_sheet_t *s, size_t col, const char *pattern,
                              size_t *added_out, char *err, size_t err_cap)
{
    regex_t re;
    int rc;
    size_t ngroups;
    td_col_t *new_cols;
    regmatch_t *pmatch;

    if (added_out)
        *added_out = 0;
    if (err && err_cap > 0)
        err[0] = '\0';
    if (!s || col >= s->ncols) {
        td_sheet_set_err(err, err_cap, "no column");
        return false;
    }
    if (!pattern || !pattern[0]) {
        td_sheet_set_err(err, err_cap, "empty regex");
        return false;
    }

    rc = regcomp(&re, pattern, REG_EXTENDED);
    if (rc != 0) {
        if (err && err_cap > 0)
            regerror(rc, &re, err, err_cap);
        return false;
    }
    ngroups = re.re_nsub;
    if (ngroups == 0) {
        regfree(&re);
        td_sheet_set_err(err, err_cap,
                         "regex must contain at least one capture group (...)");
        return false;
    }
    if (ngroups > TD_SPLIT_MAX_COLS) {
        regfree(&re);
        td_sheet_set_err(err, err_cap, "too many capture groups");
        return false;
    }

    new_cols = calloc(ngroups, sizeof(td_col_t));
    pmatch = calloc(ngroups + 1, sizeof(regmatch_t));
    if (!new_cols || !pmatch || !td_init_split_cols(s, col, new_cols, ngroups)) {
        free(pmatch);
        free(new_cols);
        regfree(&re);
        td_sheet_set_err(err, err_cap, "out of memory");
        return false;
    }

    for (size_t r = 0; r < s->phys_nrows; r++) {
        char stack_buf[256];
        char *dyn = NULL;
        size_t len = 0;
        const char *text = td_split_format_cell(s, r, col, stack_buf,
                                                sizeof(stack_buf), &len, &dyn);

        if (!text) {
            free(pmatch);
            td_free_split_cols(new_cols, ngroups);
            regfree(&re);
            td_sheet_set_err(err, err_cap, "out of memory");
            return false;
        }
        if (len > 0 && regexec(&re, text, ngroups + 1, pmatch, 0) == 0) {
            for (size_t g = 0; g < ngroups; g++) {
                regoff_t so = pmatch[g + 1].rm_so;
                regoff_t eo = pmatch[g + 1].rm_eo;

                if (so >= 0 && eo >= so && (size_t)eo <= len) {
                    bool ok = true;

                    new_cols[g].cells[r] = td_make_arena_slice_val(
                        &s->arena, text + so, (size_t)(eo - so), &ok);
                    if (!ok) {
                        free(dyn);
                        free(pmatch);
                        td_free_split_cols(new_cols, ngroups);
                        regfree(&re);
                        td_sheet_set_err(err, err_cap, "out of memory");
                        return false;
                    }
                }
            }
        }
        free(dyn);
    }

    free(pmatch);
    regfree(&re);

    if (!td_sheet_insert_cols_batch(s, col + 1, new_cols, ngroups,
                                    "capture column")) {
        td_free_split_cols(new_cols, ngroups);
        td_sheet_set_err(err, err_cap, "out of memory");
        return false;
    }
    free(new_cols);
    if (added_out)
        *added_out = ngroups;
    return true;
}
