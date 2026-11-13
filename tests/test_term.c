#include <string.h>

#include "sheet.h"
#include "tam-data.h"
#include "term.h"
#include "test.h"

static td_sheet_t *grid_sheet(void)
{
    td_sheet_t *s = td_sheet_new("demo", "(test)");
    size_t c0, c1;

    if (!s)
        return NULL;
    c0 = td_sheet_add_col(s, "name", TD_TYPE_STR, 6);
    c1 = td_sheet_add_col(s, "qty", TD_TYPE_INT, 5);
    s->cols[c0].is_key = true;

    td_sheet_add_row(s);
    td_sheet_add_row(s);
    td_sheet_set_cell(s, 0, c0,
                      td_val_parse(td_slice_from_cstr("Alice"), TD_TYPE_STR));
    td_sheet_set_cell(s, 0, c1,
                      td_val_parse(td_slice_from_cstr("3"), TD_TYPE_INT));
    td_sheet_set_cell(s, 1, c0,
                      td_val_parse(td_slice_from_cstr("Bob"), TD_TYPE_STR));
    td_sheet_set_cell(s, 1, c1,
                      td_val_parse(td_slice_from_cstr("4"), TD_TYPE_INT));
    return s;
}

void test_term(void)
{
    td_term_profile_t p;
    td_glyphs_t g;
    td_term_t t;
    td_cell_t c;
    td_stack_t st = { 0 };
    td_view_state_t view;

    td_term_profile_detect(&p, "linux", "en_US.UTF-8");
    TD_CHECK(!p.alt_screen);
    TD_CHECK(!p.sync_output);
    TD_CHECK_INT(p.colors, 16);
    TD_CHECK(p.utf8);

    td_term_profile_detect(&p, "xterm-256color", "en_US.UTF-8");
    TD_CHECK(p.alt_screen);
    TD_CHECK(p.sync_output);
    TD_CHECK_INT(p.colors, 256);

    td_term_profile_detect(&p, "dumb", "en_US.UTF-8");
    TD_CHECK(!p.utf8);
    TD_CHECK(!p.alt_screen);

    td_term_glyphs(&g, true);
    TD_CHECK_STR(g.vline, "\u2502");
    TD_CHECK_STR(g.ellipsis, "\u2026");
    TD_CHECK_STR(g.keysep, "\u2551");
    td_term_glyphs(&g, false);
    TD_CHECK_STR(g.vline, "|");
    TD_CHECK_STR(g.ellipsis, "~");

    TD_CHECK_INT(td_term_init(&t, 10, 20, "linux", "en_US.UTF-8"), 0);
    TD_CHECK_INT(t.rows, 10);
    TD_CHECK_INT(t.cols, 20);

    td_term_clear_back(&t);
    TD_CHECK_INT(td_term_put_str(&t, 0, 0, 10, "abcdefghijkl",
                                 TD_COLOR_DEFAULT, TD_COLOR_DEFAULT,
                                 TD_ATTR_NONE), 10);
    TD_CHECK_INT(td_term_back_cell(&t, 0, 0).ch, 'a');
    TD_CHECK_INT(td_term_back_cell(&t, 0, 8).ch, 'i');
    TD_CHECK_INT(td_term_back_cell(&t, 0, 9).ch, 0x2026);
    TD_CHECK_INT(td_term_back_cell(&t, 0, 10).ch, ' ');

    TD_CHECK_INT(td_term_put_str(&t, 1, 0, 20, "short",
                                 TD_COLOR_DEFAULT, TD_COLOR_DEFAULT,
                                 TD_ATTR_NONE), 5);
    TD_CHECK_INT(td_term_back_cell(&t, 1, 4).ch, 't');
    TD_CHECK_INT(td_term_back_cell(&t, 1, 5).ch, ' ');

    td_term_fill_rect(&t, 2, 2, 2, 3, '#', TD_COLOR_DEFAULT,
                      TD_COLOR_DEFAULT, TD_ATTR_BOLD);
    TD_CHECK_INT(td_term_back_cell(&t, 2, 2).ch, '#');
    TD_CHECK_INT(td_term_back_cell(&t, 3, 4).ch, '#');
    TD_CHECK_INT(td_term_back_cell(&t, 4, 2).ch, ' ');

    td_term_present(&t);
    TD_CHECK(td_term_frame_len(&t) > 0);
    TD_CHECK_INT(strncmp(td_term_frame(&t), "\x1b[1;1H", 6), 0);
    TD_CHECK(td_term_front_cell(&t, 0, 0).ch ==
             td_term_back_cell(&t, 0, 0).ch);

    /* A no-op present emits nothing in headless mode. */
    td_term_present(&t);
    TD_CHECK_INT(td_term_frame_len(&t), 0);

    td_term_clear_back(&t);
    td_term_put_cell(&t, 0, 0, 'X', 1, 2, TD_ATTR_BOLD);
    td_term_present(&t);
    TD_CHECK(strstr(td_term_frame(&t), "\x1b[0;1;31;42m") != NULL);
    TD_CHECK(strstr(td_term_frame(&t), "\x1b[1;1H") != NULL);
    c = td_term_front_cell(&t, 0, 0);
    TD_CHECK_INT(c.ch, 'X');
    TD_CHECK_INT(c.fg, 1);
    TD_CHECK_INT(c.bg, 2);
    TD_CHECK_INT(c.attr, TD_ATTR_BOLD);

    TD_CHECK_INT(td_term_resize(&t, 5, 5), 0);
    TD_CHECK_INT(t.rows, 5);
    TD_CHECK_INT(t.cols, 5);
    td_term_present(&t);
    TD_CHECK(td_term_frame_len(&t) > 0);
    td_term_close(&t);

    /* ASCII fallback. */
    TD_CHECK_INT(td_term_init(&t, 3, 10, "linux", "C"), 0);
    td_term_put_str(&t, 0, 0, 10, "abcdefghijkl", TD_COLOR_DEFAULT,
                    TD_COLOR_DEFAULT, TD_ATTR_NONE);
    TD_CHECK_INT(td_term_back_cell(&t, 0, 9).ch, '~');
    td_term_close(&t);

    /* Whole-grid rendering. */
    td_sheet_t *s = grid_sheet();
    TD_CHECK(s != NULL);
    if (!s)
        return;
    TD_CHECK(td_stack_push(&st, s));
    TD_CHECK_INT(td_term_init(&t, 10, 80, "linux", "en_US.UTF-8"), 0);
    memset(&view, 0, sizeof(view));
    view.message = "hello";
    view.pending = "g";
    td_view_render(&t, &st, &view);

    TD_CHECK_INT(td_term_back_cell(&t, 0, 0).ch, '~');
    TD_CHECK_INT(td_term_back_cell(&t, 0, 1).ch, ' ');
    TD_CHECK_INT(td_term_back_cell(&t, 0, 2).ch, 'n');
    TD_CHECK_INT(td_term_back_cell(&t, 0, 6).ch, 0x2551);
    TD_CHECK_INT(td_term_back_cell(&t, 0, 7).ch, '#');
    TD_CHECK_INT(td_term_back_cell(&t, 0, 9).ch, 'q');
    TD_CHECK_INT(td_term_back_cell(&t, 1, 0).ch, 'A');
    TD_CHECK_INT(td_term_back_cell(&t, 1, 1).ch, 'l');
    /* Numeric column qty (x=7, width=5) right-aligns "3" at x=11. */
    TD_CHECK_INT(td_term_back_cell(&t, 1, 7).ch, ' ');
    TD_CHECK_INT(td_term_back_cell(&t, 1, 11).ch, '3');
    TD_CHECK_INT(td_term_back_cell(&t, 2, 0).ch, 'B');
    TD_CHECK_INT(td_term_back_cell(&t, 2, 11).ch, '4');

    /* Status bar (row 8): "[1/1] demo | r1 c1:name" (len 23), center "hello" at 25. */
    TD_CHECK_INT(td_term_back_cell(&t, 8, 0).ch, '[');
    TD_CHECK_INT(td_term_back_cell(&t, 8, 1).ch, '1');
    TD_CHECK_INT(td_term_back_cell(&t, 8, 13).ch, 'r');
    TD_CHECK_INT(td_term_back_cell(&t, 8, 14).ch, '1');
    TD_CHECK_INT(td_term_back_cell(&t, 8, 16).ch, 'c');
    TD_CHECK_INT(td_term_back_cell(&t, 8, 17).ch, '1');
    TD_CHECK_INT(td_term_back_cell(&t, 8, 19).ch, 'n');
    TD_CHECK_INT(td_term_back_cell(&t, 8, 25).ch, 'h');

    /* Mode/keybindings bar (row 9): "PREFIX g | q:quit-all ..." */
    TD_CHECK(td_term_back_cell(&t, 9, 0).attr & TD_ATTR_BOLD);
    TD_CHECK_INT(td_term_back_cell(&t, 9, 0).ch, 'P');
    TD_CHECK_INT(td_term_back_cell(&t, 9, 7).ch, 'g');
    TD_CHECK_INT(td_term_back_cell(&t, 9, 9).ch, '|');
    TD_CHECK_INT(td_term_back_cell(&t, 9, 11).ch, 'q');
    TD_CHECK_INT(td_term_back_cell(&t, 9, 12).ch, ':');

    /* Selection styling and cursor-cell reverse video. */
    td_sheet_select_row(s, 0);
    s->cursor_row = 0;
    s->cursor_col = 0;
    td_view_render(&t, &st, &view);
    c = td_term_back_cell(&t, 1, 0);
    TD_CHECK_INT(c.fg, 15);
    TD_CHECK_INT(c.bg, 4);
    TD_CHECK(c.attr & TD_ATTR_REV);
    TD_CHECK(c.attr & TD_ATTR_BOLD);

    /* Cursor column header is highlighted and status bar reflects r2 c2:qty. */
    s->cursor_row = 1;
    s->cursor_col = 1;
    td_sheet_set_cell(s, 1, 1,
                      td_val_parse(td_slice_from_cstr("oops"), TD_TYPE_ANY));
    td_view_render(&t, &st, &view);
    TD_CHECK(td_term_back_cell(&t, 0, 7).attr & TD_ATTR_REV);
    TD_CHECK(!(td_term_back_cell(&t, 0, 0).attr & TD_ATTR_REV));
    TD_CHECK_INT(td_term_back_cell(&t, 8, 14).ch, '2');
    TD_CHECK_INT(td_term_back_cell(&t, 8, 17).ch, '2');
    TD_CHECK_INT(td_term_back_cell(&t, 8, 19).ch, 'q');
    /* Unselected #ERR cell renders red bold "#ERR" left-aligned at x=7. */
    c = td_term_back_cell(&t, 2, 7);
    TD_CHECK_INT(c.ch, '#');
    TD_CHECK_INT(c.fg, 1);
    TD_CHECK(c.attr & TD_ATTR_BOLD);
    TD_CHECK_INT(td_term_back_cell(&t, 2, 8).ch, 'E');
    TD_CHECK_INT(td_term_back_cell(&t, 2, 10).ch, 'R');

    /* Empty sheet coordinates show r0 c0. */
    td_sheet_t *empty = td_sheet_new("empty", "");
    TD_CHECK(empty != NULL);
    if (empty) {
        TD_CHECK(td_stack_push(&st, empty));
        td_view_render(&t, &st, &view);
        TD_CHECK_INT(td_term_back_cell(&t, 8, 1).ch, '2');
        TD_CHECK_INT(td_term_back_cell(&t, 8, 14).ch, 'r');
        TD_CHECK_INT(td_term_back_cell(&t, 8, 15).ch, '0');
        TD_CHECK_INT(td_term_back_cell(&t, 8, 17).ch, 'c');
        TD_CHECK_INT(td_term_back_cell(&t, 8, 18).ch, '0');
        td_sheet_free(td_stack_pop(&st));
    }

    /* Persistent aggregator footer row rendering. */
    uint8_t qty_aggs[2] = { 3 /* TD_AGG_SUM */, 4 /* TD_AGG_MEAN */ };
    td_sheet_set_col_width(s, 1, 10);
    td_sheet_set_cell(s, 1, 1, td_val_parse(td_slice_from_cstr("7"), TD_TYPE_INT));
    TD_CHECK(td_sheet_set_col_aggs(s, 1, qty_aggs, 2, "test aggs"));
    TD_CHECK_INT(td_sheet_footer_rows(s, 4), 0);
    TD_CHECK_INT(td_sheet_footer_rows(s, 5), 1);
    TD_CHECK_INT(td_sheet_footer_rows(s, 10), 2);
    td_view_render(&t, &st, &view);
    /* Row 6 = first footer (sum=10), Row 7 = second footer (mean=5), Row 8 = status, Row 9 = mode/hints. */
    TD_CHECK(td_term_back_cell(&t, 6, 11).attr & TD_ATTR_BOLD);
    TD_CHECK_INT(td_term_back_cell(&t, 6, 11).ch, 's');
    TD_CHECK_INT(td_term_back_cell(&t, 6, 14).ch, '=');
    TD_CHECK_INT(td_term_back_cell(&t, 6, 15).ch, '1');
    TD_CHECK_INT(td_term_back_cell(&t, 6, 16).ch, '0');
    TD_CHECK_INT(td_term_back_cell(&t, 7, 11).ch, 'm');
    TD_CHECK_INT(td_term_back_cell(&t, 7, 15).ch, '=');
    TD_CHECK_INT(td_term_back_cell(&t, 7, 16).ch, '5');

    /* Normal, edit, and prompt mode footer bars. */
    view.pending = "";
    td_view_render(&t, &st, &view);
    TD_CHECK_INT(td_term_back_cell(&t, 9, 0).ch, 'N');
    TD_CHECK_INT(td_term_back_cell(&t, 9, 5).ch, 'L');
    TD_CHECK_INT(td_term_back_cell(&t, 9, 7).ch, '|');
    TD_CHECK_INT(td_term_back_cell(&t, 9, 9).ch, 'h');

    view.prompt_active = true;
    view.prompt_label = "edit cell:";
    view.prompt_text = "abc";
    td_view_render(&t, &st, &view);
    TD_CHECK_INT(td_term_back_cell(&t, 9, 0).ch, 'E');
    TD_CHECK_INT(td_term_back_cell(&t, 9, 3).ch, 'T');
    TD_CHECK_INT(td_term_back_cell(&t, 9, 5).ch, '|');
    TD_CHECK_INT(td_term_back_cell(&t, 9, 7).ch, 'E');

    view.prompt_label = "/";
    td_view_render(&t, &st, &view);
    TD_CHECK_INT(td_term_back_cell(&t, 9, 0).ch, 'P');
    TD_CHECK_INT(td_term_back_cell(&t, 9, 5).ch, 'T');

    td_term_close(&t);
    td_stack_free(&st);
}
