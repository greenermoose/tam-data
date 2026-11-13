#include "clipboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void td_yank_entry_free(td_yank_entry_t *e)
{
    if (!e)
        return;
    free(e->cells);
    td_arena_free(&e->arena);
    memset(e, 0, sizeof(*e));
}

static td_val_t td_clip_clone_val(td_arena_t *arena, td_val_t v, bool *ok)
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

static td_yank_entry_t *td_clipboard_alloc_slot(td_clipboard_t *cb)
{
    size_t idx;

    if (!cb)
        return NULL;
    if (cb->count == TD_YANK_RING_CAP) {
        td_yank_entry_free(&cb->entries[cb->head]);
        cb->head = (cb->head + 1) % TD_YANK_RING_CAP;
        cb->count--;
    }
    idx = (cb->head + cb->count) % TD_YANK_RING_CAP;
    memset(&cb->entries[idx], 0, sizeof(cb->entries[idx]));
    td_arena_init(&cb->entries[idx].arena, 0);
    return &cb->entries[idx];
}

void td_clipboard_init(td_clipboard_t *cb)
{
    if (!cb)
        return;
    memset(cb, 0, sizeof(*cb));
}

void td_clipboard_free(td_clipboard_t *cb)
{
    if (!cb)
        return;
    for (size_t i = 0; i < cb->count; i++) {
        size_t idx = (cb->head + i) % TD_YANK_RING_CAP;

        td_yank_entry_free(&cb->entries[idx]);
    }
    memset(cb, 0, sizeof(*cb));
}

const td_yank_entry_t *td_clipboard_top(const td_clipboard_t *cb)
{
    if (!cb || cb->count == 0)
        return NULL;
    return &cb->entries[(cb->head + cb->count - 1) % TD_YANK_RING_CAP];
}

size_t td_clipboard_len(const td_clipboard_t *cb)
{
    return cb ? cb->count : 0;
}

static bool td_clipboard_yank_row_list(td_clipboard_t *cb, const td_sheet_t *s,
                                       const size_t *phys_rows, size_t nrows)
{
    td_yank_entry_t *e;
    size_t total;

    if (!cb || !s || !phys_rows || nrows == 0)
        return false;

    e = td_clipboard_alloc_slot(cb);
    if (!e)
        return false;
    e->kind = TD_YANK_ROWS;
    e->nrows = nrows;
    e->ncols = s->ncols;
    e->ncells = nrows * s->ncols;
    total = e->ncells;

    if (total > 0) {
        e->cells = malloc(total * sizeof(td_val_t));
        if (!e->cells) {
            td_yank_entry_free(e);
            return false;
        }
        for (size_t r = 0; r < nrows; r++) {
            for (size_t c = 0; c < s->ncols; c++) {
                bool ok = true;
                td_val_t v = td_sheet_get_cell(s, phys_rows[r], c);

                e->cells[r * s->ncols + c] =
                    td_clip_clone_val(&e->arena, v, &ok);
                if (!ok) {
                    td_yank_entry_free(e);
                    return false;
                }
            }
        }
    }
    cb->count++;
    return true;
}

bool td_clipboard_yank_row(td_clipboard_t *cb, const td_sheet_t *s,
                           size_t vis_row)
{
    size_t phys;

    if (!cb || !s || vis_row >= s->nrows || !s->row_order)
        return false;
    phys = s->row_order[vis_row];
    return td_clipboard_yank_row_list(cb, s, &phys, 1);
}

size_t td_clipboard_yank_selected(td_clipboard_t *cb, const td_sheet_t *s)
{
    size_t *phys_rows;
    size_t n = 0;
    bool ok;

    if (!cb || !s || s->nrows == 0 || s->nselected == 0 || !s->row_order)
        return 0;

    phys_rows = malloc(s->nrows * sizeof(size_t));
    if (!phys_rows)
        return 0;
    for (size_t vr = 0; vr < s->nrows; vr++) {
        size_t phys = s->row_order[vr];

        if (td_sheet_is_selected(s, phys))
            phys_rows[n++] = phys;
    }
    ok = n > 0 && td_clipboard_yank_row_list(cb, s, phys_rows, n);
    free(phys_rows);
    return ok ? n : 0;
}

static bool td_clipboard_yank_cell_list(td_clipboard_t *cb, const td_sheet_t *s,
                                        const size_t *phys_rows, size_t col,
                                        size_t ncells)
{
    td_yank_entry_t *e;

    if (!cb || !s || !phys_rows || col >= s->ncols || ncells == 0)
        return false;

    e = td_clipboard_alloc_slot(cb);
    if (!e)
        return false;
    e->kind = TD_YANK_CELLS;
    e->nrows = ncells;
    e->ncols = 1;
    e->ncells = ncells;
    e->cells = malloc(ncells * sizeof(td_val_t));
    if (!e->cells) {
        td_yank_entry_free(e);
        return false;
    }

    for (size_t i = 0; i < ncells; i++) {
        bool ok = true;
        td_val_t v = td_sheet_get_cell(s, phys_rows[i], col);

        e->cells[i] = td_clip_clone_val(&e->arena, v, &ok);
        if (!ok) {
            td_yank_entry_free(e);
            return false;
        }
    }
    cb->count++;
    return true;
}

bool td_clipboard_yank_cell(td_clipboard_t *cb, const td_sheet_t *s,
                            size_t vis_row, size_t col)
{
    size_t phys;

    if (!cb || !s || vis_row >= s->nrows || col >= s->ncols || !s->row_order)
        return false;
    phys = s->row_order[vis_row];
    return td_clipboard_yank_cell_list(cb, s, &phys, col, 1);
}

size_t td_clipboard_yank_selected_cells(td_clipboard_t *cb, const td_sheet_t *s,
                                        size_t col)
{
    size_t *phys_rows;
    size_t n = 0;
    bool ok;

    if (!cb || !s || s->nrows == 0 || s->nselected == 0 ||
        col >= s->ncols || !s->row_order)
        return 0;

    phys_rows = malloc(s->nrows * sizeof(size_t));
    if (!phys_rows)
        return 0;
    for (size_t vr = 0; vr < s->nrows; vr++) {
        size_t phys = s->row_order[vr];

        if (td_sheet_is_selected(s, phys))
            phys_rows[n++] = phys;
    }
    ok = n > 0 && td_clipboard_yank_cell_list(cb, s, phys_rows, col, n);
    free(phys_rows);
    return ok ? n : 0;
}

static void td_clip_set_err(char *err, size_t err_cap, const char *msg)
{
    if (err && err_cap > 0)
        snprintf(err, err_cap, "%s", msg ? msg : "");
}

bool td_sheet_paste(td_sheet_t *s, const td_clipboard_t *cb, bool after,
                    bool force_cell, size_t *pasted_out,
                    char *err, size_t err_cap)
{
    const td_yank_entry_t *e = td_clipboard_top(cb);

    if (pasted_out)
        *pasted_out = 0;
    if (err && err_cap > 0)
        err[0] = '\0';
    if (!s) {
        td_clip_set_err(err, err_cap, "no active sheet");
        return false;
    }
    if (!e || e->kind == TD_YANK_NONE) {
        td_clip_set_err(err, err_cap, "clipboard is empty");
        return false;
    }

    if (e->kind == TD_YANK_ROWS && !force_cell) {
        size_t vis_pos = 0;

        if (s->nrows > 0)
            vis_pos = after ? (s->cursor_row + 1) : s->cursor_row;
        if (!td_sheet_insert_rows_populated(s, vis_pos, e->nrows, e->ncols,
                                            e->cells, "paste rows")) {
            td_clip_set_err(err, err_cap, "paste failed: out of memory");
            return false;
        }
        if (pasted_out)
            *pasted_out = e->nrows;
        return true;
    }

    if (s->nrows == 0 || s->ncols == 0 ||
        s->cursor_row >= s->nrows || s->cursor_col >= s->ncols) {
        td_clip_set_err(err, err_cap, "no target cell");
        return false;
    }

    size_t count = e->kind == TD_YANK_CELLS ? e->ncells : e->nrows;
    size_t src_col = (e->kind == TD_YANK_ROWS && s->cursor_col < e->ncols)
                         ? s->cursor_col
                         : 0;
    size_t avail = s->nrows - s->cursor_row;
    size_t n = count < avail ? count : avail;
    size_t *vrows = malloc(n * sizeof(size_t));
    size_t *cols = malloc(n * sizeof(size_t));
    td_val_t *vals = malloc(n * sizeof(td_val_t));
    bool ok;

    if (!vrows || !cols || !vals) {
        free(vrows);
        free(cols);
        free(vals);
        td_clip_set_err(err, err_cap, "out of memory");
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        vrows[i] = s->cursor_row + i;
        cols[i] = s->cursor_col;
        if (e->kind == TD_YANK_CELLS)
            vals[i] = e->cells[i];
        else
            vals[i] = e->ncols > 0 ? e->cells[i * e->ncols + src_col]
                                   : TD_VAL_NULL;
    }
    ok = td_sheet_apply_cell_batch(s, vrows, cols, vals, n, "paste cells");
    free(vrows);
    free(cols);
    free(vals);
    if (!ok) {
        td_clip_set_err(err, err_cap, "paste failed");
        return false;
    }
    if (pasted_out)
        *pasted_out = n;
    return true;
}
