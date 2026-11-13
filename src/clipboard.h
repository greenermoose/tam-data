#ifndef TD_CLIPBOARD_H
#define TD_CLIPBOARD_H

#include <stdbool.h>
#include <stddef.h>

#include "arena.h"
#include "sheet.h"
#include "val.h"

#define TD_YANK_RING_CAP 32

typedef enum {
    TD_YANK_NONE = 0,
    TD_YANK_ROWS,
    TD_YANK_CELLS
} td_yank_kind_t;

typedef struct {
    td_yank_kind_t kind;
    td_arena_t arena;
    size_t nrows;
    size_t ncols;
    size_t ncells;
    td_val_t *cells;
} td_yank_entry_t;

typedef struct {
    td_yank_entry_t entries[TD_YANK_RING_CAP];
    size_t head;
    size_t count;
} td_clipboard_t;

void td_clipboard_init(td_clipboard_t *cb);
void td_clipboard_free(td_clipboard_t *cb);

bool td_clipboard_yank_row(td_clipboard_t *cb, const td_sheet_t *s,
                           size_t vis_row);
size_t td_clipboard_yank_selected(td_clipboard_t *cb, const td_sheet_t *s);
bool td_clipboard_yank_cell(td_clipboard_t *cb, const td_sheet_t *s,
                            size_t vis_row, size_t col);
size_t td_clipboard_yank_selected_cells(td_clipboard_t *cb, const td_sheet_t *s,
                                        size_t col);

const td_yank_entry_t *td_clipboard_top(const td_clipboard_t *cb);
size_t td_clipboard_len(const td_clipboard_t *cb);

bool td_sheet_paste(td_sheet_t *s, const td_clipboard_t *cb, bool after,
                    bool force_cell, size_t *pasted_out,
                    char *err, size_t err_cap);

#endif /* TD_CLIPBOARD_H */
