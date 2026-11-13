#include <string.h>

#include "sheet.h"
#include "test.h"

static td_sheet_t *make_sheet(void)
{
    td_sheet_t *s = td_sheet_new("test", "/tmp/test.tsv");

    if (!s)
        return NULL;
    td_sheet_add_col(s, "name", TD_TYPE_STR, 8);
    td_sheet_add_col(s, "qty", TD_TYPE_INT, 6);
    td_sheet_add_col(s, "ratio", TD_TYPE_FLOAT, -10);
    return s;
}

static void test_sheet_phase1_nav_and_cols(void)
{
    td_sheet_t *s = td_sheet_new("nav", "");

    TD_CHECK(s != NULL);
    if (!s)
        return;

    td_sheet_add_col(s, "k", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP);
    td_sheet_add_col(s, "n", TD_TYPE_INT, -TD_AUTO_WIDTH_CAP);
    td_sheet_add_col(s, "note", TD_TYPE_ANY, -TD_AUTO_WIDTH_CAP);

    for (int i = 0; i < 6; i++)
        TD_CHECK_INT(td_sheet_add_row(s), (size_t)i);

    /* Values in col 1 (int): [10, 10, 20, NULL, 20, 30], with raw ANY on row 1. */
    td_sheet_set_cell(s, 0, 1, td_val_parse(td_slice_from_cstr("10"), TD_TYPE_INT));
    td_sheet_set_cell(s, 1, 1, td_val_parse(td_slice_from_cstr("010"), TD_TYPE_ANY));
    td_sheet_set_cell(s, 2, 1, td_val_parse(td_slice_from_cstr("20"), TD_TYPE_INT));
    /* row 3 stays TD_VAL_NULL */
    td_sheet_set_cell(s, 4, 1, td_val_parse(td_slice_from_cstr(""), TD_TYPE_ANY));
    td_sheet_set_cell(s, 5, 1, td_val_parse(td_slice_from_cstr("30"), TD_TYPE_INT));

    /* Value jumps (> and <). */
    s->cursor_col = 1;
    s->cursor_row = 0;
    TD_CHECK(td_sheet_jump_value(s, 1));
    TD_CHECK_INT(s->cursor_row, 2); /* skipped "010" coerced to 10 */
    TD_CHECK(td_sheet_jump_value(s, 1));
    TD_CHECK_INT(s->cursor_row, 3); /* NULL is distinct from 20 */
    TD_CHECK(td_sheet_jump_value(s, 1));
    TD_CHECK_INT(s->cursor_row, 5); /* row 4 is also blank/NULL */
    TD_CHECK(!td_sheet_jump_value(s, 1));
    TD_CHECK_INT(s->cursor_row, 5);
    TD_CHECK(td_sheet_jump_value(s, -1));
    TD_CHECK_INT(s->cursor_row, 4);

    /* Blank jumps (} and {). */
    s->cursor_row = 0;
    TD_CHECK(td_sheet_jump_null(s, 1));
    TD_CHECK_INT(s->cursor_row, 3);
    TD_CHECK(td_sheet_jump_null(s, 1));
    TD_CHECK_INT(s->cursor_row, 4);
    TD_CHECK(!td_sheet_jump_null(s, 1));
    TD_CHECK(td_sheet_jump_null(s, -1));
    TD_CHECK_INT(s->cursor_row, 3);
    TD_CHECK(!td_sheet_jump_null(s, -1));

    /* Column reordering skipping hidden middle column. */
    td_sheet_hide_col(s, 1);
    s->cursor_col = 0;
    TD_CHECK(td_sheet_move_col(s, 0, 1));
    TD_CHECK_INT(s->cursor_col, 2);
    TD_CHECK_STR(s->cols[2].name, "k");
    TD_CHECK_STR(s->cols[0].name, "note");
    TD_CHECK(!td_sheet_move_col(s, 2, 1)); /* already rightmost visible */
    TD_CHECK(td_sheet_move_col(s, 2, -1));
    TD_CHECK_INT(s->cursor_col, 0);
    TD_CHECK_STR(s->cols[0].name, "k");

    /* Moving a regular column does not cross a pinned key column boundary. */
    s->cols[0].is_key = true;
    TD_CHECK(!td_sheet_move_col(s, 2, -1));
    TD_CHECK(!td_sheet_move_col(s, 0, 1));
    s->cols[0].is_key = false;

    /* Unhide all, set_col_width, autosize (including >40 toggle), autosize_all. */
    td_sheet_unhide_all_cols(s);
    TD_CHECK_INT(s->cols[1].width, -TD_AUTO_WIDTH_CAP);

    TD_CHECK(td_sheet_set_col_width(s, 0, 14));
    TD_CHECK_INT(td_sheet_col_width(s, 0), 14);
    TD_CHECK(td_sheet_set_col_width(s, 0, 0));
    TD_CHECK_INT(s->cols[0].width, 0);
    TD_CHECK(!td_sheet_set_col_width(s, 0, -1));

    const char *long_txt =
        "01234567890123456789012345678901234567890123456789"; /* 50 chars */
    td_sheet_set_cell(s, 0, 2, td_val_parse(td_slice_from_cstr(long_txt), TD_TYPE_STR));
    TD_CHECK_INT(td_sheet_col_width(s, 2), TD_AUTO_WIDTH_CAP);
    TD_CHECK_INT(s->cols[2].cached_width, TD_AUTO_WIDTH_CAP);

    /* Mutating cell invalidates cached_width. */
    td_sheet_set_cell(s, 1, 2, td_val_parse(td_slice_from_cstr("hi"), TD_TYPE_STR));
    TD_CHECK_INT(s->cols[2].cached_width, 0);

    /* Underscore autosizes uncapped, then toggles back to -TD_AUTO_WIDTH_CAP. */
    td_sheet_autosize_col(s, 2);
    TD_CHECK_INT(s->cols[2].width, 50);
    td_sheet_autosize_col(s, 2);
    TD_CHECK_INT(s->cols[2].width, -TD_AUTO_WIDTH_CAP);

    /* g_ autosizes visible columns while leaving hidden col 0 hidden. */
    td_sheet_autosize_all_cols(s);
    TD_CHECK_INT(s->cols[0].width, 0);
    TD_CHECK(s->cols[1].width > 0);
    TD_CHECK_INT(s->cols[2].width, 50);

    td_sheet_free(s);
}

void test_sheet(void)
{
    td_sheet_t *s = make_sheet();
    td_stack_t st = { 0 };
    char buf[64];

    TD_CHECK(s != NULL);
    if (!s)
        return;
    TD_CHECK_STR(s->name, "test");
    TD_CHECK_STR(s->source_path, "/tmp/test.tsv");
    TD_CHECK_INT(s->ncols, 3);

    TD_CHECK_INT(td_sheet_add_row(s), 0);
    TD_CHECK_INT(td_sheet_add_row(s), 1);
    TD_CHECK_INT(td_sheet_add_row(s), 2);
    TD_CHECK_INT(s->nrows, 3);
    TD_CHECK_INT(s->row_order[2], 2);

    TD_CHECK(td_sheet_set_cell(s, 0, 0,
                               td_val_parse(td_slice_from_cstr("Alice"),
                                            TD_TYPE_STR)));
    TD_CHECK(td_sheet_set_cell(s, 1, 1,
                               td_val_parse(td_slice_from_cstr("7"),
                                            TD_TYPE_INT)));
    TD_CHECK(!td_sheet_set_cell(s, 9, 0, TD_VAL_NULL));
    TD_CHECK_INT(td_sheet_get_cell(s, 1, 1).as.i, 7);
    TD_CHECK_INT(td_sheet_get_cell(s, 9, 0).type, TD_TYPE_NULL);

    /* Row indirection does not move cell payloads. */
    size_t tmp = s->row_order[0];
    s->row_order[0] = s->row_order[2];
    s->row_order[2] = tmp;
    TD_CHECK_INT(s->row_order[0], 2);
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[0], 1).as.i, 0);

    TD_CHECK(!td_sheet_is_selected(s, 0));
    td_sheet_select_row(s, 0);
    TD_CHECK(td_sheet_is_selected(s, 0));
    TD_CHECK_INT(s->nselected, 1);
    td_sheet_select_row(s, 0);
    TD_CHECK_INT(s->nselected, 1);
    td_sheet_toggle_row(s, 0);
    TD_CHECK(!td_sheet_is_selected(s, 0));
    TD_CHECK_INT(s->nselected, 0);
    td_sheet_select_all(s);
    TD_CHECK_INT(s->nselected, 3);
    td_sheet_toggle_all(s);
    TD_CHECK_INT(s->nselected, 0);
    td_sheet_select_all(s);
    td_sheet_unselect_all(s);
    TD_CHECK_INT(s->nselected, 0);

    TD_CHECK_STR(td_type_sigil(s->cols[0].type), "~");
    td_sheet_header(buf, sizeof(buf), &s->cols[0]);
    TD_CHECK_STR(buf, "~ name");
    td_sheet_header(buf, sizeof(buf), &s->cols[1]);
    TD_CHECK_STR(buf, "# qty");

    td_sheet_rename_col(s, 0, "person");
    TD_CHECK_STR(s->cols[0].name, "person");

    TD_CHECK(td_sheet_set_col_type(s, 1, TD_TYPE_STR));
    TD_CHECK_INT(s->cols[1].type, TD_TYPE_STR);
    td_val_format(td_sheet_get_cell(s, 1, 1), s->cols[1].type, buf,
                  sizeof(buf));
    TD_CHECK_STR(buf, "7");

    TD_CHECK(td_sheet_set_col_type(s, 1, TD_TYPE_INT));
    TD_CHECK_INT(td_sheet_get_cell(s, 1, 1).as.i, 7);

    s->cols[1].width = -5;
    TD_CHECK(td_sheet_col_width(s, 1) > 0);
    TD_CHECK(td_sheet_col_width(s, 1) <= 5);
    td_sheet_autosize_col(s, 1);
    TD_CHECK(td_sheet_col_width(s, 1) > 0);

    s->cursor_col = 1;
    td_sheet_hide_col(s, 1);
    TD_CHECK_INT(s->cols[1].width, 0);
    TD_CHECK(s->cursor_col != 1);
    TD_CHECK_INT(td_sheet_first_visible_col(s), 0);
    TD_CHECK_INT(td_sheet_last_visible_col(s), 2);
    TD_CHECK_INT(td_sheet_next_visible_col(s, 0, 1), 2);
    TD_CHECK_INT(td_sheet_next_visible_col(s, 2, -1), 0);

    td_sheet_toggle_key_col(s, 0);
    TD_CHECK(s->cols[0].is_key);
    td_sheet_toggle_key_col(s, 0);
    TD_CHECK(!s->cols[0].is_key);

    /* Vertical viewport math. */
    for (int i = 0; i < 10; i++)
        TD_CHECK_INT(td_sheet_add_row(s), (size_t)(3 + i));
    s->cursor_row = 9;
    td_sheet_ensure_cursor_visible(s, 3, 80);
    TD_CHECK_INT(s->top_row, 7);
    s->cursor_row = 0;
    td_sheet_ensure_cursor_visible(s, 3, 80);
    TD_CHECK_INT(s->top_row, 0);

    /* Horizontal viewport math: the cursor must fit, scrolling off the left. */
    for (size_t c = 0; c < s->ncols; c++)
        s->cols[c].width = 5;
    s->cursor_col = 2;
    td_sheet_ensure_cursor_visible(s, 3, 12);
    TD_CHECK_INT(s->left_col, 1);
    s->cursor_col = 2;
    td_sheet_ensure_cursor_visible(s, 3, 6);
    TD_CHECK_INT(s->left_col, 2);

    TD_CHECK(td_stack_push(&st, s));
    TD_CHECK_INT(st.len, 1);
    TD_CHECK(td_stack_active(&st) == s);
    td_sheet_t *second = td_sheet_new("second", "");
    TD_CHECK(second != NULL);
    TD_CHECK(td_stack_push(&st, second));
    TD_CHECK(td_stack_active(&st) == second);
    TD_CHECK(td_stack_pop(&st) == second);
    td_sheet_free(second);
    TD_CHECK(td_stack_active(&st) == s);
    td_stack_free(&st);

    test_sheet_phase1_nav_and_cols();
}
