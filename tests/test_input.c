#include <string.h>

#include "input.h"
#include "test.h"

static td_key_t key_char(uint32_t ch)
{
    td_key_t k = { TD_KEY_CHAR, ch };

    return k;
}

void test_input(void)
{
    static const char stream[] =
        "a\xC3\xA9\x1b[A\x1b[[A\x1bOP\x1b[5~\x1b[3~\x1b[1;2A";
    td_key_reader_t r;
    td_key_t k;
    td_input_t in;
    td_prompt_t p;

    td_key_reader_mem(&r, stream, sizeof(stream) - 1);
    k = td_key_read(&r);
    TD_CHECK_INT(k.kind, TD_KEY_CHAR);
    TD_CHECK_INT(k.ch, 'a');
    k = td_key_read(&r);
    TD_CHECK_INT(k.kind, TD_KEY_CHAR);
    TD_CHECK_INT(k.ch, 0xE9);
    k = td_key_read(&r);
    TD_CHECK_INT(k.kind, TD_KEY_UP);
    k = td_key_read(&r);
    TD_CHECK_INT(k.kind, TD_KEY_F1);
    k = td_key_read(&r);
    TD_CHECK_INT(k.kind, TD_KEY_F1);
    k = td_key_read(&r);
    TD_CHECK_INT(k.kind, TD_KEY_PGUP);
    k = td_key_read(&r);
    TD_CHECK_INT(k.kind, TD_KEY_DELETE);
    k = td_key_read(&r);
    TD_CHECK_INT(k.kind, TD_KEY_UP);
    k = td_key_read(&r);
    TD_CHECK_INT(k.kind, TD_KEY_NONE);

    td_key_reader_mem(&r, "\x1b", 1);
    TD_CHECK_INT(td_key_read(&r).kind, TD_KEY_ESC);

    td_key_reader_mem(&r, "\x1bx", 2);
    TD_CHECK_INT(td_key_read(&r).kind, TD_KEY_ESC);
    k = td_key_read(&r);
    TD_CHECK_INT(k.kind, TD_KEY_CHAR);
    TD_CHECK_INT(k.ch, 'x');

    td_key_reader_mem(&r, "\x1b[6~", 4);
    TD_CHECK_INT(td_key_read(&r).kind, TD_KEY_PGDN);

    td_input_init(&in);
    TD_CHECK_STR(td_input_pending(&in), "");
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_STR(td_input_pending(&in), "g");
    TD_CHECK_INT(td_input_feed(&in, key_char('h')), TD_CMD_FIRST_COL);
    TD_CHECK_STR(td_input_pending(&in), "");

    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_FIRST_ROW);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('j')), TD_CMD_LAST_ROW);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('k')), TD_CMD_FIRST_ROW);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('l')), TD_CMD_LAST_COL);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('q')), TD_CMD_QUIT_ALL);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('s')), TD_CMD_SELECT_ALL);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('u')), TD_CMD_UNSELECT_ALL);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('t')), TD_CMD_TOGGLE_ALL);

    TD_CHECK_INT(td_input_feed(&in, key_char('z')), TD_CMD_NONE);
    TD_CHECK_STR(td_input_pending(&in), "z");
    TD_CHECK_INT(td_input_feed(&in, key_char('h')), TD_CMD_NONE);
    TD_CHECK_STR(td_input_pending(&in), "");
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('z')), TD_CMD_NONE);
    TD_CHECK_STR(td_input_pending(&in), "gz");
    TD_CHECK_INT(td_input_feed(&in, key_char('h')), TD_CMD_NONE);
    TD_CHECK_STR(td_input_pending(&in), "");

    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, (td_key_t){ TD_KEY_ESC, 0 }), TD_CMD_NONE);
    TD_CHECK_STR(td_input_pending(&in), "");

    TD_CHECK_INT(td_input_feed(&in, key_char('h')), TD_CMD_LEFT);
    TD_CHECK_INT(td_input_feed(&in, key_char('j')), TD_CMD_DOWN);
    TD_CHECK_INT(td_input_feed(&in, key_char('k')), TD_CMD_UP);
    TD_CHECK_INT(td_input_feed(&in, key_char('l')), TD_CMD_RIGHT);
    TD_CHECK_INT(td_input_feed(&in, key_char('G')), TD_CMD_LAST_ROW);
    TD_CHECK_INT(td_input_feed(&in, key_char('q')), TD_CMD_POP_SHEET);
    TD_CHECK_INT(td_input_feed(&in, key_char('#')), TD_CMD_TYPE_INT);
    TD_CHECK_INT(td_input_feed(&in, key_char('%')), TD_CMD_TYPE_FLOAT);
    TD_CHECK_INT(td_input_feed(&in, key_char('$')), TD_CMD_TYPE_CURRENCY);
    TD_CHECK_INT(td_input_feed(&in, key_char('@')), TD_CMD_TYPE_DATE);
    TD_CHECK_INT(td_input_feed(&in, key_char('~')), TD_CMD_TYPE_STR);
    TD_CHECK_INT(td_input_feed(&in, key_char('!')), TD_CMD_TOGGLE_KEY_COL);
    TD_CHECK_INT(td_input_feed(&in, key_char('-')), TD_CMD_HIDE_COL);
    TD_CHECK_INT(td_input_feed(&in, key_char('_')), TD_CMD_AUTOSIZE_COL);
    TD_CHECK_INT(td_input_feed(&in, key_char('^')), TD_CMD_RENAME_COL);
    TD_CHECK_INT(td_input_feed(&in, key_char('s')), TD_CMD_SELECT_ROW);
    TD_CHECK_INT(td_input_feed(&in, key_char('u')), TD_CMD_UNSELECT_ROW);
    TD_CHECK_INT(td_input_feed(&in, key_char('t')), TD_CMD_TOGGLE_ROW);
    TD_CHECK_INT(td_input_feed(&in, key_char(6)), TD_CMD_PAGE_DOWN);
    TD_CHECK_INT(td_input_feed(&in, key_char(2)), TD_CMD_PAGE_UP);
    TD_CHECK_INT(td_input_feed(&in, key_char(12)), TD_CMD_REDRAW);
    TD_CHECK_INT(td_input_feed(&in, key_char('x')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, (td_key_t){ TD_KEY_PGUP, 0 }),
                 TD_CMD_PAGE_UP);
    TD_CHECK_INT(td_input_feed(&in, (td_key_t){ TD_KEY_PGDN, 0 }),
                 TD_CMD_PAGE_DOWN);

    /* Phase 1 single-key and prefix chords. */
    TD_CHECK_INT(td_input_feed(&in, key_char('<')), TD_CMD_PREV_VALUE);
    TD_CHECK_INT(td_input_feed(&in, key_char('>')), TD_CMD_NEXT_VALUE);
    TD_CHECK_INT(td_input_feed(&in, key_char('{')), TD_CMD_PREV_NULL);
    TD_CHECK_INT(td_input_feed(&in, key_char('}')), TD_CMD_NEXT_NULL);
    TD_CHECK_INT(td_input_feed(&in, key_char('H')), TD_CMD_MOVE_COL_LEFT);
    TD_CHECK_INT(td_input_feed(&in, key_char('L')), TD_CMD_MOVE_COL_RIGHT);
    TD_CHECK_INT(td_input_feed(&in, key_char(19)), TD_CMD_SAVE_SHEET);

    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('_')), TD_CMD_AUTOSIZE_ALL_COLS);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('v')), TD_CMD_UNHIDE_ALL_COLS);
    TD_CHECK_INT(td_input_feed(&in, key_char('z')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('_')), TD_CMD_SET_COL_WIDTH);
    TD_CHECK_INT(td_input_feed(&in, key_char('z')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('~')), TD_CMD_TYPE_ANY);
    TD_CHECK_STR(td_input_pending(&in), "");

    td_prompt_init(&p);
    td_prompt_start(&p, "rename:", "abc");
    TD_CHECK(p.active);
    TD_CHECK_STR(td_prompt_label(&p), "rename:");
    TD_CHECK_STR(td_prompt_text(&p), "abc");
    TD_CHECK_INT(td_prompt_cursor(&p), 3);
    TD_CHECK_INT(td_prompt_key(&p, key_char('d')), TD_PROMPT_CHANGED);
    TD_CHECK_STR(td_prompt_text(&p), "abcd");
    TD_CHECK_INT(td_prompt_key(&p, (td_key_t){ TD_KEY_LEFT, 0 }),
                 TD_PROMPT_CHANGED);
    TD_CHECK_INT(td_prompt_cursor(&p), 3);
    TD_CHECK_INT(td_prompt_key(&p, (td_key_t){ TD_KEY_BACKSPACE, 0 }),
                 TD_PROMPT_CHANGED);
    TD_CHECK_STR(td_prompt_text(&p), "abd");
    TD_CHECK_INT(td_prompt_key(&p, (td_key_t){ TD_KEY_HOME, 0 }),
                 TD_PROMPT_CHANGED);
    TD_CHECK_INT(td_prompt_cursor(&p), 0);
    TD_CHECK_INT(td_prompt_key(&p, (td_key_t){ TD_KEY_END, 0 }),
                 TD_PROMPT_CHANGED);
    TD_CHECK_INT(td_prompt_cursor(&p), 3);
    TD_CHECK_INT(td_prompt_key(&p, key_char(21)), TD_PROMPT_CHANGED);
    TD_CHECK_STR(td_prompt_text(&p), "");

    const char *words = "hello world";
    for (const char *q = words; *q; q++)
        TD_CHECK_INT(td_prompt_key(&p, key_char((unsigned char)*q)),
                     TD_PROMPT_CHANGED);
    TD_CHECK_STR(td_prompt_text(&p), "hello world");
    TD_CHECK_INT(td_prompt_key(&p, key_char(23)), TD_PROMPT_CHANGED);
    TD_CHECK_STR(td_prompt_text(&p), "hello ");
    TD_CHECK_INT(td_prompt_key(&p, key_char(21)), TD_PROMPT_CHANGED);
    TD_CHECK_STR(td_prompt_text(&p), "");
    TD_CHECK_INT(td_prompt_key(&p, key_char('x')), TD_PROMPT_CHANGED);
    TD_CHECK_INT(td_prompt_key(&p, (td_key_t){ TD_KEY_ENTER, 0 }),
                 TD_PROMPT_ACCEPT);
    TD_CHECK_STR(td_prompt_text(&p), "x");

    td_prompt_start(&p, "rename:", "abc");
    TD_CHECK_INT(td_prompt_key(&p, (td_key_t){ TD_KEY_ESC, 0 }),
                 TD_PROMPT_CANCEL);
    td_prompt_stop(&p);
    TD_CHECK(!p.active);

    td_prompt_start(&p, "edit:", "");
    TD_CHECK_INT(td_prompt_key(&p, key_char(0xE9)), TD_PROMPT_CHANGED);
    TD_CHECK_STR(td_prompt_text(&p), "\xC3\xA9");
    TD_CHECK_INT(td_prompt_cursor(&p), 2);
    TD_CHECK_INT(td_prompt_key(&p, (td_key_t){ TD_KEY_BACKSPACE, 0 }),
                 TD_PROMPT_CHANGED);
    TD_CHECK_STR(td_prompt_text(&p), "");
    TD_CHECK_INT(td_prompt_key(&p, key_char('a')), TD_PROMPT_CHANGED);
    TD_CHECK_INT(td_prompt_key(&p, key_char('b')), TD_PROMPT_CHANGED);
    TD_CHECK_INT(td_prompt_key(&p, key_char('c')), TD_PROMPT_CHANGED);
    TD_CHECK_INT(td_prompt_key(&p, (td_key_t){ TD_KEY_HOME, 0 }),
                 TD_PROMPT_CHANGED);
    TD_CHECK_INT(td_prompt_key(&p, (td_key_t){ TD_KEY_DELETE, 0 }),
                 TD_PROMPT_CHANGED);
    TD_CHECK_STR(td_prompt_text(&p), "bc");
    TD_CHECK_INT(td_prompt_key(&p, key_char(5)), TD_PROMPT_CHANGED);
    TD_CHECK_INT(td_prompt_cursor(&p), 2);
    TD_CHECK_INT(td_prompt_key(&p, key_char(1)), TD_PROMPT_CHANGED);
    TD_CHECK_INT(td_prompt_cursor(&p), 0);
    TD_CHECK_INT(td_prompt_key(&p, key_char(11)), TD_PROMPT_CHANGED);
    TD_CHECK_STR(td_prompt_text(&p), "");

    td_prompt_start_kind(&p, TD_PROMPT_COL_WIDTH, "column width:", "12");
    TD_CHECK_INT(td_prompt_kind(&p), TD_PROMPT_COL_WIDTH);
    TD_CHECK_STR(td_prompt_text(&p), "12");
    td_prompt_start_kind(&p, TD_PROMPT_SAVE_SHEET, "save to:", "out.csv");
    TD_CHECK_INT(td_prompt_kind(&p), TD_PROMPT_SAVE_SHEET);
    td_prompt_stop(&p);
    TD_CHECK_INT(td_prompt_kind(&p), TD_PROMPT_NONE);

    /* Phase 2 chords and prompt kinds. */
    TD_CHECK_INT(td_input_feed(&in, key_char('|')), TD_CMD_SELECT_REGEX);
    TD_CHECK_INT(td_input_feed(&in, key_char('\\')), TD_CMD_UNSELECT_REGEX);
    TD_CHECK_INT(td_input_feed(&in, key_char('/')), TD_CMD_SEARCH_FWD);
    TD_CHECK_INT(td_input_feed(&in, key_char('?')), TD_CMD_SEARCH_BWD);
    TD_CHECK_INT(td_input_feed(&in, key_char('n')), TD_CMD_SEARCH_NEXT);
    TD_CHECK_INT(td_input_feed(&in, key_char('N')), TD_CMD_SEARCH_PREV);
    TD_CHECK_INT(td_input_feed(&in, key_char('"')), TD_CMD_DUP_SELECTED);
    TD_CHECK_INT(td_input_feed(&in, key_char('d')), TD_CMD_DELETE_ROW);
    TD_CHECK_INT(td_input_feed(&in, key_char('[')), TD_CMD_SORT_ASC);
    TD_CHECK_INT(td_input_feed(&in, key_char(']')), TD_CMD_SORT_DESC);

    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('|')), TD_CMD_SELECT_ALL_REGEX);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('\\')), TD_CMD_UNSELECT_ALL_REGEX);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('/')), TD_CMD_SEARCH_ALL_FWD);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('?')), TD_CMD_SEARCH_ALL_BWD);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('d')), TD_CMD_DELETE_SELECTED);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char('[')), TD_CMD_SORT_KEYS_ASC);
    TD_CHECK_INT(td_input_feed(&in, key_char('g')), TD_CMD_NONE);
    TD_CHECK_INT(td_input_feed(&in, key_char(']')), TD_CMD_SORT_KEYS_DESC);

    td_prompt_start_kind(&p, TD_PROMPT_SELECT_REGEX, "select regex:", "");
    TD_CHECK_INT(td_prompt_kind(&p), TD_PROMPT_SELECT_REGEX);
    td_prompt_start_kind(&p, TD_PROMPT_UNSELECT_REGEX, "unselect regex:", "");
    TD_CHECK_INT(td_prompt_kind(&p), TD_PROMPT_UNSELECT_REGEX);
    td_prompt_start_kind(&p, TD_PROMPT_SELECT_ALL_REGEX, "select all regex:", "");
    TD_CHECK_INT(td_prompt_kind(&p), TD_PROMPT_SELECT_ALL_REGEX);
    td_prompt_start_kind(&p, TD_PROMPT_UNSELECT_ALL_REGEX, "unselect all regex:", "");
    TD_CHECK_INT(td_prompt_kind(&p), TD_PROMPT_UNSELECT_ALL_REGEX);
    td_prompt_start_kind(&p, TD_PROMPT_SEARCH_FWD, "/", "");
    TD_CHECK_INT(td_prompt_kind(&p), TD_PROMPT_SEARCH_FWD);
    td_prompt_start_kind(&p, TD_PROMPT_SEARCH_BWD, "?", "");
    TD_CHECK_INT(td_prompt_kind(&p), TD_PROMPT_SEARCH_BWD);
    td_prompt_start_kind(&p, TD_PROMPT_SEARCH_ALL_FWD, "g/", "");
    TD_CHECK_INT(td_prompt_kind(&p), TD_PROMPT_SEARCH_ALL_FWD);
    td_prompt_start_kind(&p, TD_PROMPT_SEARCH_ALL_BWD, "g?", "");
    TD_CHECK_INT(td_prompt_kind(&p), TD_PROMPT_SEARCH_ALL_BWD);
    td_prompt_stop(&p);

    td_prompt_free(&p);
}
