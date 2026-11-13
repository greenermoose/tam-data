#include <stdlib.h>
#include <string.h>

#include "clipboard.h"
#include "sheet.h"
#include "test.h"

static td_sheet_t *make_sample_sheet(const char *name)
{
    td_sheet_t *s = td_sheet_new(name, "");

    if (!s)
        return NULL;
    td_sheet_add_col(s, "name", TD_TYPE_STR, -20);
    td_sheet_add_col(s, "qty", TD_TYPE_INT, -10);
    td_sheet_add_row(s);
    td_sheet_add_row(s);
    td_sheet_set_cell(s, 0, 0,
                      td_val_parse(td_slice_from_cstr("alpha"), TD_TYPE_STR));
    td_sheet_set_cell(s, 0, 1,
                      td_val_parse(td_slice_from_cstr("10"), TD_TYPE_INT));
    td_sheet_set_cell(s, 1, 0,
                      td_val_parse(td_slice_from_cstr("beta"), TD_TYPE_STR));
    td_sheet_set_cell(s, 1, 1,
                      td_val_parse(td_slice_from_cstr("20"), TD_TYPE_INT));
    return s;
}

void test_clipboard(void)
{
    td_clipboard_t cb;
    td_sheet_t *src;
    td_sheet_t *dst;
    char err[128];
    char buf[64];
    size_t pasted = 0;

    td_clipboard_init(&cb);
    TD_CHECK_INT(td_clipboard_len(&cb), 0);
    TD_CHECK(td_clipboard_top(&cb) == NULL);

    src = make_sample_sheet("src");
    dst = make_sample_sheet("dst");
    TD_CHECK(src != NULL);
    TD_CHECK(dst != NULL);
    if (!src || !dst) {
        td_sheet_free(src);
        td_sheet_free(dst);
        td_clipboard_free(&cb);
        return;
    }

    /* Empty clipboard fails cleanly. */
    TD_CHECK(!td_sheet_paste(dst, &cb, true, false, &pasted, err, sizeof(err)));
    TD_CHECK_STR(err, "clipboard is empty");

    /* Yank selected rows from src, free src, then paste into dst. */
    td_sheet_select_all(src);
    TD_CHECK_INT(td_clipboard_yank_selected(&cb, src), 2);
    TD_CHECK_INT(td_clipboard_len(&cb), 1);
    td_sheet_free(src);
    src = NULL;

    dst->cursor_row = 0;
    TD_CHECK(td_sheet_paste(dst, &cb, true, false, &pasted, err, sizeof(err)));
    TD_CHECK_INT(pasted, 2);
    TD_CHECK_INT(dst->nrows, 4);
    td_val_format(td_sheet_get_cell(dst, dst->row_order[1], 0), TD_TYPE_STR,
                  buf, sizeof(buf));
    TD_CHECK_STR(buf, "alpha");
    td_val_format(td_sheet_get_cell(dst, dst->row_order[2], 0), TD_TYPE_STR,
                  buf, sizeof(buf));
    TD_CHECK_STR(buf, "beta");

    /* Undo paste rows. */
    TD_CHECK(td_sheet_undo(dst, err, sizeof(err)));
    TD_CHECK_INT(dst->nrows, 2);

    /* Yank single row and paste before (P) at row 0. */
    TD_CHECK(td_clipboard_yank_row(&cb, dst, 1));
    dst->cursor_row = 0;
    TD_CHECK(td_sheet_paste(dst, &cb, false, false, &pasted, err, sizeof(err)));
    TD_CHECK_INT(pasted, 1);
    TD_CHECK_INT(dst->nrows, 3);
    td_val_format(td_sheet_get_cell(dst, dst->row_order[0], 0), TD_TYPE_STR,
                  buf, sizeof(buf));
    TD_CHECK_STR(buf, "beta");
    TD_CHECK_INT(td_sheet_get_cell(dst, dst->row_order[0], 1).as.i, 20);

    /* Yank cell and paste into cell. */
    TD_CHECK(td_clipboard_yank_cell(&cb, dst, 1, 0));
    dst->cursor_row = 0;
    dst->cursor_col = 0;
    TD_CHECK(td_sheet_paste(dst, &cb, true, false, &pasted, err, sizeof(err)));
    TD_CHECK_INT(pasted, 1);
    td_val_format(td_sheet_get_cell(dst, dst->row_order[0], 0), TD_TYPE_STR,
                  buf, sizeof(buf));
    TD_CHECK_STR(buf, "alpha");

    /* Yank selected cells in col 1 and paste downward. */
    td_sheet_select_row(dst, dst->row_order[1]);
    td_sheet_select_row(dst, dst->row_order[2]);
    TD_CHECK_INT(td_clipboard_yank_selected_cells(&cb, dst, 1), 2);
    dst->cursor_row = 0;
    dst->cursor_col = 1;
    TD_CHECK(td_sheet_paste(dst, &cb, true, false, &pasted, err, sizeof(err)));
    TD_CHECK_INT(pasted, 2);
    TD_CHECK_INT(td_sheet_get_cell(dst, dst->row_order[0], 1).as.i, 10);
    TD_CHECK_INT(td_sheet_get_cell(dst, dst->row_order[1], 1).as.i, 20);

    /* Yank ring capacity eviction (> TD_YANK_RING_CAP). */
    for (int i = 0; i < TD_YANK_RING_CAP + 5; i++)
        TD_CHECK(td_clipboard_yank_cell(&cb, dst, 0, 0));
    TD_CHECK_INT(td_clipboard_len(&cb), TD_YANK_RING_CAP);

    td_sheet_free(dst);
    td_clipboard_free(&cb);
}
