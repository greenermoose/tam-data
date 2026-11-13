#ifndef TD_SHEET_H
#define TD_SHEET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "val.h"

#define TD_AUTO_WIDTH_CAP 40
#define TD_AUTO_WIDTH_MIN 3
#define TD_MAX_COL_AGGS 12

typedef struct {
    char *name;
    td_type_t type;
    int width;        /* > 0 fixed, 0 hidden, < 0 auto with cap -width */
    int cached_width; /* > 0 cached auto-width, 0 dirty */
    bool is_key;      /* pinned key column */
    uint8_t aggs[TD_MAX_COL_AGGS]; /* attached td_agg_kind_t list */
    uint8_t naggs;
    bool agg_dirty;
    td_val_t agg_vals[TD_MAX_COL_AGGS];
    td_val_t *cells;  /* row_cap contiguous cells */
} td_col_t;

typedef struct td_undo_entry td_undo_entry_t;

typedef struct {
    td_undo_entry_t *items;
    size_t len;
    size_t cap;
} td_undo_stack_t;

typedef struct {
    char *name;
    char *source_path;
    td_arena_t arena;
    void *mmap_base;     /* optional mmap backing buffer, unmapped on free */
    size_t mmap_len;
    char *buf_base;      /* optional heap backing buffer (stdin/pipes), freed */
    size_t buf_len;

    td_col_t *cols;
    size_t ncols;
    size_t col_cap;

    size_t nrows;
    size_t phys_nrows;   /* allocated physical rows (0 .. phys_nrows - 1) */
    size_t row_cap;

    size_t *row_order;   /* visible index -> physical row index */
    uint64_t *sel_bits;  /* packed selected physical rows */
    size_t sel_words;
    size_t nselected;

    size_t cursor_row;
    size_t cursor_col;
    size_t top_row;
    size_t left_col;

    char *last_search;
    bool search_all_cols;
    int search_dir;      /* +1 forward, -1 backward */

    td_undo_stack_t undo;
    td_undo_stack_t redo;
    bool record_undo;

    uint64_t id;
    uint64_t source_id;
    uint8_t kind;        /* td_sheet_kind_t */
    void *meta;
    void (*meta_free)(void *);
} td_sheet_t;

typedef struct {
    td_sheet_t **sheets;
    size_t len;
    size_t cap;
} td_stack_t;

td_sheet_t *td_sheet_new(const char *name, const char *source_path);
void td_sheet_free(td_sheet_t *s);

size_t td_sheet_add_col(td_sheet_t *s, const char *name, td_type_t type,
                        int width);
size_t td_sheet_add_row(td_sheet_t *s);
td_val_t td_sheet_get_cell(const td_sheet_t *s, size_t row, size_t col);
td_val_t td_sheet_eval_cell(const td_sheet_t *s, size_t phys_row, size_t col);
bool td_sheet_set_cell(td_sheet_t *s, size_t row, size_t col, td_val_t v);

void td_sheet_select_row(td_sheet_t *s, size_t row);
void td_sheet_unselect_row(td_sheet_t *s, size_t row);
void td_sheet_toggle_row(td_sheet_t *s, size_t row);
bool td_sheet_is_selected(const td_sheet_t *s, size_t row);
void td_sheet_select_all(td_sheet_t *s);
void td_sheet_unselect_all(td_sheet_t *s);
void td_sheet_toggle_all(td_sheet_t *s);

size_t td_sheet_next_visible_col(const td_sheet_t *s, size_t col, int dir);
size_t td_sheet_first_visible_col(const td_sheet_t *s);
size_t td_sheet_last_visible_col(const td_sheet_t *s);
int td_sheet_col_width(const td_sheet_t *s, size_t col);
int td_sheet_header(char *buf, size_t cap, const td_col_t *c);

bool td_sheet_set_col_type(td_sheet_t *s, size_t col, td_type_t type);
bool td_sheet_rename_col(td_sheet_t *s, size_t col, const char *name);
void td_sheet_hide_col(td_sheet_t *s, size_t col);
void td_sheet_toggle_key_col(td_sheet_t *s, size_t col);
void td_sheet_autosize_col(td_sheet_t *s, size_t col);
void td_sheet_autosize_all_cols(td_sheet_t *s);
bool td_sheet_set_col_width(td_sheet_t *s, size_t col, int width);
void td_sheet_unhide_all_cols(td_sheet_t *s);
bool td_sheet_move_col(td_sheet_t *s, size_t col, int dir);
bool td_sheet_jump_value(td_sheet_t *s, int dir);
bool td_sheet_jump_null(td_sheet_t *s, int dir);
void td_sheet_ensure_cursor_visible(td_sheet_t *s, int grid_rows,
                                    int grid_cols);

bool td_sheet_select_regex(td_sheet_t *s, const char *pattern, bool all_cols,
                           bool select, size_t *matched_out, char *err,
                           size_t err_cap);
bool td_sheet_search(td_sheet_t *s, const char *pattern, int dir, bool all_cols,
                     char *err, size_t err_cap);
bool td_sheet_search_next(td_sheet_t *s, int dir_mult, char *err,
                          size_t err_cap);
bool td_sheet_delete_row(td_sheet_t *s, size_t vis_row);
size_t td_sheet_delete_selected(td_sheet_t *s);
td_sheet_t *td_sheet_extract_phys_rows(const td_sheet_t *s,
                                         const size_t *phys_rows, size_t nphys,
                                         const char *suffix);
td_sheet_t *td_sheet_dup_selected(const td_sheet_t *s, const char *suffix);
bool td_sheet_set_col_aggs(td_sheet_t *s, size_t col, const uint8_t *aggs,
                           uint8_t naggs, const char *desc);
int td_sheet_footer_rows(const td_sheet_t *s, int term_rows);
bool td_sheet_sort_cols(td_sheet_t *s, const size_t *cols, size_t ncols,
                        bool desc);
bool td_sheet_sort_col(td_sheet_t *s, size_t col, bool desc);
bool td_sheet_sort_keys(td_sheet_t *s, bool desc);

void td_sheet_set_undo_recording(td_sheet_t *s, bool enabled);
bool td_sheet_can_undo(const td_sheet_t *s);
bool td_sheet_can_redo(const td_sheet_t *s);
bool td_sheet_undo(td_sheet_t *s, char *desc_out, size_t desc_cap);
bool td_sheet_redo(td_sheet_t *s, char *desc_out, size_t desc_cap);

bool td_sheet_edit_cell(td_sheet_t *s, size_t vis_row, size_t col,
                        const char *text);
size_t td_sheet_edit_selected(td_sheet_t *s, size_t col, const char *text);
bool td_sheet_clear_cell(td_sheet_t *s, size_t vis_row, size_t col);
size_t td_sheet_clear_selected(td_sheet_t *s, size_t col);
bool td_sheet_insert_rows(td_sheet_t *s, size_t vis_pos, size_t count);
bool td_sheet_insert_rows_populated(td_sheet_t *s, size_t vis_pos,
                                    size_t nrows, size_t ncols,
                                    const td_val_t *cells, const char *desc);
bool td_sheet_apply_cell_batch(td_sheet_t *s, const size_t *vis_rows,
                               const size_t *cols, const td_val_t *vals,
                               size_t count, const char *desc);
size_t td_sheet_append_blank_col(td_sheet_t *s);
bool td_sheet_dup_row(td_sheet_t *s, size_t vis_row);
bool td_sheet_dup_col(td_sheet_t *s, size_t col);
bool td_sheet_add_expr_col(td_sheet_t *s, size_t col_pos,
                           const char *expr_text, char *err, size_t err_cap);
size_t td_sheet_set_selected_expr(td_sheet_t *s, size_t col,
                                  const char *expr_text, char *err,
                                  size_t err_cap);
bool td_sheet_split_col_delim(td_sheet_t *s, size_t col, const char *delim,
                              size_t *added_out, char *err, size_t err_cap);
bool td_sheet_split_col_regex(td_sheet_t *s, size_t col, const char *pattern,
                              size_t *added_out, char *err, size_t err_cap);

bool td_stack_push(td_stack_t *st, td_sheet_t *s);
td_sheet_t *td_stack_pop(td_stack_t *st);
td_sheet_t *td_stack_active(const td_stack_t *st);
td_sheet_t *td_stack_find_by_id(const td_stack_t *st, uint64_t id);
void td_stack_free(td_stack_t *st);

#endif /* TD_SHEET_H */
