#include <stdlib.h>
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

static void test_sheet_phase2_select_search_filter_sort(void)
{
    td_sheet_t *s = td_sheet_new("p2", "/tmp/p2.csv");
    char err[128] = "";
    size_t matched = 0;

    TD_CHECK(s != NULL);
    if (!s)
        return;

    /* Put backing buffer on s->buf_base to verify child sheet independence. */
    static const char raw_data[] = "alpha\0beta\0gamma\0alpha\0delta\0";
    s->buf_base = malloc(sizeof(raw_data));
    TD_CHECK(s->buf_base != NULL);
    if (!s->buf_base) {
        td_sheet_free(s);
        return;
    }
    memcpy(s->buf_base, raw_data, sizeof(raw_data));
    s->buf_len = sizeof(raw_data);

    td_sheet_add_col(s, "dept", TD_TYPE_STR, -20);
    td_sheet_add_col(s, "name", TD_TYPE_STR, -20);
    td_sheet_add_col(s, "score", TD_TYPE_INT, -10);
    td_sheet_add_col(s, "secret", TD_TYPE_STR, 0); /* hidden */

    for (int i = 0; i < 5; i++)
        TD_CHECK_INT(td_sheet_add_row(s), (size_t)i);

    /* Rows:
     * 0: eng,   alpha, 20, hidden_hit
     * 1: sales, beta,  10, ok
     * 2: eng,   gamma, NULL, ok
     * 3: eng,   alpha, 5,  ok
     * 4: ops,   delta, 30, ok
     */
    td_sheet_set_cell(s, 0, 0, td_val_parse(td_slice_from_cstr("eng"), TD_TYPE_STR));
    td_sheet_set_cell(s, 0, 1, td_val_parse((td_slice_t){ s->buf_base + 0, 5 }, TD_TYPE_STR));
    td_sheet_set_cell(s, 0, 2, td_val_parse(td_slice_from_cstr("20"), TD_TYPE_INT));
    td_sheet_set_cell(s, 0, 3, td_val_parse(td_slice_from_cstr("hidden_hit"), TD_TYPE_STR));

    td_sheet_set_cell(s, 1, 0, td_val_parse(td_slice_from_cstr("sales"), TD_TYPE_STR));
    td_sheet_set_cell(s, 1, 1, td_val_parse((td_slice_t){ s->buf_base + 6, 4 }, TD_TYPE_STR));
    td_sheet_set_cell(s, 1, 2, td_val_parse(td_slice_from_cstr("10"), TD_TYPE_INT));
    td_sheet_set_cell(s, 1, 3, td_val_parse(td_slice_from_cstr("ok"), TD_TYPE_STR));

    td_sheet_set_cell(s, 2, 0, td_val_parse(td_slice_from_cstr("eng"), TD_TYPE_STR));
    td_sheet_set_cell(s, 2, 1, td_val_parse((td_slice_t){ s->buf_base + 11, 5 }, TD_TYPE_STR));
    /* row 2 col 2 stays NULL */
    td_sheet_set_cell(s, 2, 3, td_val_parse(td_slice_from_cstr("ok"), TD_TYPE_STR));

    td_sheet_set_cell(s, 3, 0, td_val_parse(td_slice_from_cstr("eng"), TD_TYPE_STR));
    td_sheet_set_cell(s, 3, 1, td_val_parse((td_slice_t){ s->buf_base + 17, 5 }, TD_TYPE_STR));
    td_sheet_set_cell(s, 3, 2, td_val_parse(td_slice_from_cstr("5"), TD_TYPE_INT));
    td_sheet_set_cell(s, 3, 3, td_val_parse(td_slice_from_cstr("ok"), TD_TYPE_STR));

    td_sheet_set_cell(s, 4, 0, td_val_parse(td_slice_from_cstr("ops"), TD_TYPE_STR));
    td_sheet_set_cell(s, 4, 1, td_val_parse((td_slice_t){ s->buf_base + 23, 5 }, TD_TYPE_STR));
    td_sheet_set_cell(s, 4, 2, td_val_parse(td_slice_from_cstr("30"), TD_TYPE_INT));
    td_sheet_set_cell(s, 4, 3, td_val_parse(td_slice_from_cstr("ok"), TD_TYPE_STR));

    /* Duplicating with 0 selected returns NULL (UI warns). */
    TD_CHECK(td_sheet_dup_selected(s, "[selected]") == NULL);

    /* Regex selection in current column (| and \). */
    s->cursor_col = 0;
    TD_CHECK(td_sheet_select_regex(s, "^eng$", false, true, &matched, err, sizeof(err)));
    TD_CHECK_INT(matched, 3);
    TD_CHECK_INT(s->nselected, 3);
    s->cursor_col = 1;
    TD_CHECK(td_sheet_select_regex(s, "gamma", false, false, &matched, err, sizeof(err)));
    TD_CHECK_INT(matched, 1);
    TD_CHECK_INT(s->nselected, 2); /* rows 0 and 3 remain selected */
    TD_CHECK(td_sheet_is_selected(s, 0));
    TD_CHECK(!td_sheet_is_selected(s, 2));
    TD_CHECK(td_sheet_is_selected(s, 3));

    /* All-visible-columns regex skips hidden col 3. */
    td_sheet_unselect_all(s);
    TD_CHECK(td_sheet_select_regex(s, "hidden_hit", true, true, &matched, err, sizeof(err)));
    TD_CHECK_INT(matched, 0);
    TD_CHECK_INT(s->nselected, 0);

    /* Invalid regex fails cleanly. */
    TD_CHECK(!td_sheet_select_regex(s, "[unclosed", false, true, &matched, err, sizeof(err)));
    TD_CHECK(err[0] != '\0');

    /* Search forward (/), n, N, wrap-around, and backward (?). */
    s->cursor_row = 0;
    s->cursor_col = 1;
    TD_CHECK(td_sheet_search(s, "^alpha$", 1, false, err, sizeof(err)));
    TD_CHECK_INT(s->cursor_row, 3);
    TD_CHECK(td_sheet_search_next(s, 1, err, sizeof(err)));
    TD_CHECK_INT(s->cursor_row, 0); /* wrapped around */
    TD_CHECK(td_sheet_search_next(s, -1, err, sizeof(err)));
    TD_CHECK_INT(s->cursor_row, 3);
    TD_CHECK(td_sheet_search(s, "^delta$", -1, false, err, sizeof(err)));
    TD_CHECK_INT(s->cursor_row, 4);

    /* Search across all visible columns (g/) jumps cursor_col. */
    s->cursor_row = 0;
    s->cursor_col = 0;
    TD_CHECK(td_sheet_search(s, "^30$", 1, true, err, sizeof(err)));
    TD_CHECK_INT(s->cursor_row, 4);
    TD_CHECK_INT(s->cursor_col, 2);
    /* Empty pattern reuses last_search ("^30$"). */
    s->cursor_row = 0;
    TD_CHECK(td_sheet_search(s, "", 1, true, err, sizeof(err)));
    TD_CHECK_INT(s->cursor_row, 4);
    /* Invalid regex does not clobber last_search ("^30$"). */
    TD_CHECK(!td_sheet_search(s, "[bad", 1, false, err, sizeof(err)));
    s->cursor_row = 0;
    TD_CHECK(td_sheet_search_next(s, 1, err, sizeof(err)));
    TD_CHECK_INT(s->cursor_row, 4);
    /* Hidden column not matched by g/. */
    TD_CHECK(!td_sheet_search(s, "hidden_hit", 1, true, err, sizeof(err)));

    /* Single-column sort: ascending puts NULL first, descending flips NULL to last. */
    TD_CHECK(td_sheet_sort_col(s, 2, false));
    TD_CHECK_INT(s->row_order[0], 2); /* NULL */
    TD_CHECK_INT(s->row_order[1], 3); /* 5 */
    TD_CHECK_INT(s->row_order[2], 1); /* 10 */
    TD_CHECK_INT(s->row_order[3], 0); /* 20 */
    TD_CHECK_INT(s->row_order[4], 4); /* 30 */

    TD_CHECK(td_sheet_sort_col(s, 2, true));
    TD_CHECK_INT(s->row_order[0], 4); /* 30 */
    TD_CHECK_INT(s->row_order[1], 0); /* 20 */
    TD_CHECK_INT(s->row_order[2], 1); /* 10 */
    TD_CHECK_INT(s->row_order[3], 3); /* 5 */
    TD_CHECK_INT(s->row_order[4], 2); /* NULL flipped to last */

    /* Stability check: sorting on dept (col 0) preserves descending score order within "eng". */
    TD_CHECK(td_sheet_sort_col(s, 0, false));
    TD_CHECK_INT(s->row_order[0], 0); /* eng, 20 */
    TD_CHECK_INT(s->row_order[1], 3); /* eng, 5 */
    TD_CHECK_INT(s->row_order[2], 2); /* eng, NULL */
    TD_CHECK_INT(s->row_order[3], 4); /* ops */
    TD_CHECK_INT(s->row_order[4], 1); /* sales */

    /* g[ / g] fallback to cursor_col when no key columns are pinned. */
    s->cursor_col = 1;
    TD_CHECK(td_sheet_sort_keys(s, false));
    TD_CHECK_INT(s->row_order[0], 0); /* alpha (before 3 by stability) */
    TD_CHECK_INT(s->row_order[1], 3); /* alpha */
    TD_CHECK_INT(s->row_order[2], 1); /* beta */
    TD_CHECK_INT(s->row_order[3], 4); /* delta */
    TD_CHECK_INT(s->row_order[4], 2); /* gamma */

    /* Multi-column key sort: pin dept (0) and score (2). */
    s->cols[0].is_key = true;
    s->cols[2].is_key = true;
    TD_CHECK(td_sheet_sort_keys(s, false));
    TD_CHECK_INT(s->row_order[0], 2); /* eng, NULL */
    TD_CHECK_INT(s->row_order[1], 3); /* eng, 5 */
    TD_CHECK_INT(s->row_order[2], 0); /* eng, 20 */
    TD_CHECK_INT(s->row_order[3], 4); /* ops, 30 */
    TD_CHECK_INT(s->row_order[4], 1); /* sales, 10 */

    /* Filtered extraction (") after sort preserves sorted order and survives parent free. */
    s->cursor_col = 0;
    TD_CHECK(td_sheet_select_regex(s, "^eng$", false, true, &matched, err, sizeof(err)));
    TD_CHECK_INT(s->nselected, 3);
    td_sheet_t *dup = td_sheet_dup_selected(s, "[eng]");
    TD_CHECK(dup != NULL);
    if (dup) {
        char buf[64];

        TD_CHECK_STR(dup->name, "p2[eng]");
        TD_CHECK_INT(dup->nrows, 3);
        TD_CHECK_INT(dup->ncols, 4);
        TD_CHECK(dup->cols[0].is_key);

        /* Delete selected rows on parent, then single row, then free parent completely. */
        TD_CHECK_INT(td_sheet_delete_selected(s), 3);
        TD_CHECK_INT(s->nrows, 2);
        TD_CHECK_INT(s->nselected, 0);
        TD_CHECK_INT(s->row_order[0], 4); /* ops */
        TD_CHECK_INT(s->row_order[1], 1); /* sales */

        /* Select all only selects the 2 remaining visible rows, not deleted ones. */
        td_sheet_select_all(s);
        TD_CHECK_INT(s->nselected, 2);
        s->cursor_row = 1;
        TD_CHECK(td_sheet_delete_row(s, 1));
        TD_CHECK_INT(s->nrows, 1);
        TD_CHECK_INT(s->nselected, 1);
        TD_CHECK_INT(s->cursor_row, 0);
        TD_CHECK(td_sheet_delete_row(s, 0));
        TD_CHECK_INT(s->nrows, 0);
        TD_CHECK_INT(s->nselected, 0);

        td_sheet_free(s);
        s = NULL;

        /* Child sheet's deep-copied arena strings remain valid after parent free. */
        td_val_format(td_sheet_get_cell(dup, 0, 1), dup->cols[1].type, buf, sizeof(buf));
        TD_CHECK_STR(buf, "gamma");
        td_val_format(td_sheet_get_cell(dup, 1, 1), dup->cols[1].type, buf, sizeof(buf));
        TD_CHECK_STR(buf, "alpha");
        td_val_format(td_sheet_get_cell(dup, 2, 2), dup->cols[2].type, buf, sizeof(buf));
        TD_CHECK_STR(buf, "20");
        td_sheet_free(dup);
    } else {
        td_sheet_free(s);
    }
}

static void test_sheet_phase3_edit_insert_dup_undo(void)
{
    td_sheet_t *s = td_sheet_new("p3", "");
    char desc[64];
    char buf[64];

    TD_CHECK(s != NULL);
    if (!s)
        return;

    td_sheet_add_col(s, "name", TD_TYPE_STR, -20);
    td_sheet_add_col(s, "qty", TD_TYPE_INT, -10);
    td_sheet_add_row(s);
    td_sheet_add_row(s);
    td_sheet_add_row(s);
    td_sheet_set_cell(s, 0, 0, td_val_parse(td_slice_from_cstr("alice"), TD_TYPE_STR));
    td_sheet_set_cell(s, 1, 0, td_val_parse(td_slice_from_cstr("bob"), TD_TYPE_STR));
    td_sheet_set_cell(s, 2, 0, td_val_parse(td_slice_from_cstr("carol"), TD_TYPE_STR));
    td_sheet_set_cell(s, 0, 1, td_val_parse(td_slice_from_cstr("30"), TD_TYPE_INT));
    td_sheet_set_cell(s, 1, 1, td_val_parse(td_slice_from_cstr("10"), TD_TYPE_INT));
    td_sheet_set_cell(s, 2, 1, td_val_parse(td_slice_from_cstr("20"), TD_TYPE_INT));

    TD_CHECK(!td_sheet_can_undo(s));
    TD_CHECK(!td_sheet_can_redo(s));

    /* Edit cell + invalid parse preserving #ERR raw text + undo/redo. */
    s->cursor_row = 1;
    s->cursor_col = 1;
    TD_CHECK(td_sheet_edit_cell(s, 1, 1, "99"));
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[1], 1).as.i, 99);
    TD_CHECK(td_sheet_can_undo(s));
    TD_CHECK(td_sheet_edit_cell(s, 1, 1, "oops"));
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[1], 1).type, TD_TYPE_ERR);
    td_val_format(td_sheet_get_cell(s, s->row_order[1], 1), TD_TYPE_INT,
                  buf, sizeof(buf));
    TD_CHECK_STR(buf, "oops");
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[1], 1).as.i, 99);
    TD_CHECK(td_sheet_redo(s, desc, sizeof(desc)));
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[1], 1).type, TD_TYPE_ERR);
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[1], 1).as.i, 10);

    /* Bulk edit selected (ge) and clear cell (zd) + single-step undo. */
    td_sheet_select_row(s, s->row_order[0]);
    td_sheet_select_row(s, s->row_order[2]);
    TD_CHECK_INT(td_sheet_edit_selected(s, 1, "55"), 2);
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[0], 1).as.i, 55);
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[2], 1).as.i, 55);
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[0], 1).as.i, 30);
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[2], 1).as.i, 20);

    TD_CHECK(td_sheet_clear_cell(s, 0, 1));
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[0], 1).type, TD_TYPE_NULL);
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[0], 1).as.i, 30);
    td_sheet_unselect_all(s);

    /* Insert blank rows immediately after cursor_row (a / ga) + undo/redo. */
    s->cursor_row = 0;
    TD_CHECK(td_sheet_insert_rows(s, 1, 2));
    TD_CHECK_INT(s->nrows, 5);
    TD_CHECK_INT(s->cursor_row, 1);
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[1], 0).type, TD_TYPE_NULL);
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[3], 1).as.i, 10);
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(s->nrows, 3);
    TD_CHECK_INT(s->cursor_row, 0);
    TD_CHECK(td_sheet_redo(s, desc, sizeof(desc)));
    TD_CHECK_INT(s->nrows, 5);
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));

    /* Duplicate row (g") and duplicate column (z") and append blank col (za). */
    s->cursor_row = 0;
    TD_CHECK(td_sheet_dup_row(s, 0));
    TD_CHECK_INT(s->nrows, 4);
    td_val_format(td_sheet_get_cell(s, s->row_order[1], 0), TD_TYPE_STR,
                  buf, sizeof(buf));
    TD_CHECK_STR(buf, "alice");
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(s->nrows, 3);

    size_t cidx = td_sheet_append_blank_col(s);
    TD_CHECK_INT(cidx, 2);
    TD_CHECK_INT(s->ncols, 3);
    TD_CHECK_STR(s->cols[2].name, "col3");
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(s->ncols, 2);
    TD_CHECK(td_sheet_redo(s, desc, sizeof(desc)));
    TD_CHECK_INT(s->ncols, 3);
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));

    TD_CHECK(td_sheet_dup_col(s, 0));
    TD_CHECK_INT(s->ncols, 3);
    TD_CHECK_STR(s->cols[1].name, "name_copy");
    td_val_format(td_sheet_get_cell(s, s->row_order[1], 1), TD_TYPE_STR,
                  buf, sizeof(buf));
    TD_CHECK_STR(buf, "bob");
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(s->ncols, 2);

    /* Undo sort ([), delete (d), hide (-), rename (^), move (H/L), type (#). */
    TD_CHECK(td_sheet_sort_col(s, 1, false));
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[0], 1).as.i, 10);
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[0], 1).as.i, 30);

    TD_CHECK(td_sheet_delete_row(s, 1));
    TD_CHECK_INT(s->nrows, 2);
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(s->nrows, 3);
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[1], 1).as.i, 10);

    s->cursor_col = 0;
    td_sheet_hide_col(s, 0);
    TD_CHECK_INT(s->cols[0].width, 0);
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK(s->cols[0].width != 0);
    TD_CHECK_INT(s->cursor_col, 0);

    TD_CHECK(td_sheet_rename_col(s, 0, "person"));
    TD_CHECK_STR(s->cols[0].name, "person");
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_STR(s->cols[0].name, "name");

    TD_CHECK(td_sheet_move_col(s, 0, 1));
    TD_CHECK_STR(s->cols[0].name, "qty");
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_STR(s->cols[0].name, "name");

    /* Lossless undo of column type conversion (e.g. non-numeric STR -> INT -> undo). */
    TD_CHECK(td_sheet_set_col_type(s, 0, TD_TYPE_INT));
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[0], 0).type, TD_TYPE_ERR);
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(s->cols[0].type, TD_TYPE_STR);
    td_val_format(td_sheet_get_cell(s, s->row_order[0], 0), TD_TYPE_STR,
                  buf, sizeof(buf));
    TD_CHECK_STR(buf, "alice");

    /* New edit clears redo stack. */
    TD_CHECK(td_sheet_can_redo(s));
    TD_CHECK(td_sheet_edit_cell(s, 0, 1, "123"));
    TD_CHECK(!td_sheet_can_redo(s));

    td_sheet_free(s);
}

static void test_sheet_phase5_expr_and_split(void)
{
    td_sheet_t *s = td_sheet_new("phase5", "");
    char err[256] = "";
    char desc[64] = "";
    char buf[64];
    size_t added = 0;

    TD_CHECK(s != NULL);
    td_sheet_add_col(s, "tag", TD_TYPE_STR, -20);
    td_sheet_add_col(s, "qty", TD_TYPE_INT, -10);
    td_sheet_add_col(s, "price", TD_TYPE_FLOAT, -10);
    td_sheet_add_row(s);
    td_sheet_add_row(s);
    td_sheet_add_row(s);

    td_sheet_set_cell(s, 0, 0,
                      td_val_parse(td_slice_from_cstr("hw:bolt-10"),
                                   TD_TYPE_STR));
    td_sheet_set_cell(s, 0, 1,
                      td_val_parse(td_slice_from_cstr("3"), TD_TYPE_INT));
    td_sheet_set_cell(s, 0, 2,
                      td_val_parse(td_slice_from_cstr("1.5"), TD_TYPE_FLOAT));

    td_sheet_set_cell(s, 1, 0,
                      td_val_parse(td_slice_from_cstr("sw:util"), TD_TYPE_STR));
    td_sheet_set_cell(s, 1, 1,
                      td_val_parse(td_slice_from_cstr("5"), TD_TYPE_INT));
    td_sheet_set_cell(s, 1, 2,
                      td_val_parse(td_slice_from_cstr("4.0"), TD_TYPE_FLOAT));

    td_sheet_set_cell(s, 2, 0,
                      td_val_parse(td_slice_from_cstr("misc"), TD_TYPE_STR));
    td_sheet_set_cell(s, 2, 1,
                      td_val_parse(td_slice_from_cstr("2"), TD_TYPE_INT));
    td_sheet_set_cell(s, 2, 2,
                      td_val_parse(td_slice_from_cstr("2.25"), TD_TYPE_FLOAT));

    /* Computed expression column (=) + type inference + undo/redo. */
    TD_CHECK(td_sheet_add_expr_col(s, 3, "qty * price", err, sizeof(err)));
    TD_CHECK_INT(s->ncols, 4);
    TD_CHECK_STR(s->cols[3].name, "qty * price");
    TD_CHECK_INT(s->cols[3].type, TD_TYPE_FLOAT);
    td_val_format(td_sheet_get_cell(s, 0, 3), TD_TYPE_FLOAT, buf, sizeof(buf));
    TD_CHECK_STR(buf, "4.5");
    td_val_format(td_sheet_get_cell(s, 1, 3), TD_TYPE_FLOAT, buf, sizeof(buf));
    TD_CHECK_STR(buf, "20");

    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(s->ncols, 3);
    TD_CHECK(td_sheet_redo(s, desc, sizeof(desc)));
    TD_CHECK_INT(s->ncols, 4);
    td_val_format(td_sheet_get_cell(s, 2, 3), TD_TYPE_FLOAT, buf, sizeof(buf));
    TD_CHECK_STR(buf, "4.5");

    /* Set selected cells by expression (g=) + undo/redo. */
    TD_CHECK_INT(td_sheet_set_selected_expr(s, 1, "qty + 10", err, sizeof(err)),
                 0);
    td_sheet_select_row(s, 0);
    td_sheet_select_row(s, 2);
    TD_CHECK_INT(td_sheet_set_selected_expr(s, 1, "qty * 10", err, sizeof(err)),
                 2);
    TD_CHECK_INT(td_sheet_get_cell(s, 0, 1).as.i, 30);
    TD_CHECK_INT(td_sheet_get_cell(s, 1, 1).as.i, 5);
    TD_CHECK_INT(td_sheet_get_cell(s, 2, 1).as.i, 20);
    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(td_sheet_get_cell(s, 0, 1).as.i, 3);
    TD_CHECK_INT(td_sheet_get_cell(s, 2, 1).as.i, 2);

    /* Delimiter split (:) + multi-column atomic undo/redo. */
    TD_CHECK(!td_sheet_split_col_delim(s, 0, "", &added, err, sizeof(err)));
    TD_CHECK(td_sheet_split_col_delim(s, 0, ":", &added, err, sizeof(err)));
    TD_CHECK_INT(added, 2);
    TD_CHECK_INT(s->ncols, 6);
    TD_CHECK_STR(s->cols[1].name, "tag_1");
    TD_CHECK_STR(s->cols[2].name, "tag_2");
    td_val_format(td_sheet_get_cell(s, 0, 1), TD_TYPE_ANY, buf, sizeof(buf));
    TD_CHECK_STR(buf, "hw");
    td_val_format(td_sheet_get_cell(s, 0, 2), TD_TYPE_ANY, buf, sizeof(buf));
    TD_CHECK_STR(buf, "bolt-10");
    TD_CHECK_INT(td_sheet_get_cell(s, 2, 2).type, TD_TYPE_NULL);

    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(s->ncols, 4);
    TD_CHECK(td_sheet_redo(s, desc, sizeof(desc)));
    TD_CHECK_INT(s->ncols, 6);
    td_val_format(td_sheet_get_cell(s, 1, 2), TD_TYPE_ANY, buf, sizeof(buf));
    TD_CHECK_STR(buf, "util");

    /* Regex capture group split (;) + error checks + atomic undo/redo. */
    TD_CHECK(!td_sheet_split_col_regex(s, 0, "no_groups", &added,
                                       err, sizeof(err)));
    TD_CHECK(td_sheet_split_col_regex(s, 0, "^([^:]+):([^-]+)-([0-9]+)$",
                                      &added, err, sizeof(err)));
    TD_CHECK_INT(added, 3);
    TD_CHECK_INT(s->ncols, 9);
    td_val_format(td_sheet_get_cell(s, 0, 1), TD_TYPE_ANY, buf, sizeof(buf));
    TD_CHECK_STR(buf, "hw");
    td_val_format(td_sheet_get_cell(s, 0, 2), TD_TYPE_ANY, buf, sizeof(buf));
    TD_CHECK_STR(buf, "bolt");
    td_val_format(td_sheet_get_cell(s, 0, 3), TD_TYPE_ANY, buf, sizeof(buf));
    TD_CHECK_STR(buf, "10");
    TD_CHECK_INT(td_sheet_get_cell(s, 1, 1).type, TD_TYPE_NULL);

    TD_CHECK(td_sheet_undo(s, desc, sizeof(desc)));
    TD_CHECK_INT(s->ncols, 6);

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
    test_sheet_phase2_select_search_filter_sort();
    test_sheet_phase3_edit_insert_dup_undo();
    test_sheet_phase5_expr_and_split();
}
