#ifndef TD_SHEET_H
#define TD_SHEET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "val.h"

typedef struct {
    char *name;
    td_type_t type;
    int width;       /* > 0 fixed, 0 hidden, < 0 auto with cap -width */
    bool is_key;     /* pinned key column */
    td_val_t *cells; /* row_cap contiguous cells */
} td_col_t;

typedef struct {
    char *name;
    char *source_path;
    td_arena_t arena;

    td_col_t *cols;
    size_t ncols;
    size_t col_cap;

    size_t nrows;
    size_t row_cap;

    size_t *row_order;   /* visible index -> physical row index */
    uint64_t *sel_bits;  /* packed selected physical rows */
    size_t sel_words;
    size_t nselected;

    size_t cursor_row;
    size_t cursor_col;
    size_t top_row;
    size_t left_col;
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
void td_sheet_ensure_cursor_visible(td_sheet_t *s, int grid_rows,
                                    int grid_cols);

bool td_stack_push(td_stack_t *st, td_sheet_t *s);
td_sheet_t *td_stack_pop(td_stack_t *st);
td_sheet_t *td_stack_active(const td_stack_t *st);
void td_stack_free(td_stack_t *st);

#endif /* TD_SHEET_H */
