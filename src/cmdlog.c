#include "cmdlog.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csv.h"

static const td_cmd_meta_t td_cmd_table[] = {
    { TD_CMD_LEFT,                "go-left",             "h",      false },
    { TD_CMD_RIGHT,               "go-right",            "l",      false },
    { TD_CMD_UP,                  "go-up",               "k",      false },
    { TD_CMD_DOWN,                "go-down",             "j",      false },
    { TD_CMD_FIRST_COL,           "go-first-col",        "gh",     false },
    { TD_CMD_LAST_COL,            "go-last-col",         "gl",     false },
    { TD_CMD_FIRST_ROW,           "go-first-row",        "gg",     false },
    { TD_CMD_LAST_ROW,            "go-last-row",         "G",      false },
    { TD_CMD_PAGE_DOWN,           "page-down",           "Ctrl+F", false },
    { TD_CMD_PAGE_UP,             "page-up",             "Ctrl+B", false },
    { TD_CMD_NEXT_VALUE,          "next-value",          ">",      false },
    { TD_CMD_PREV_VALUE,          "prev-value",          "<",      false },
    { TD_CMD_NEXT_NULL,           "next-null",           "}",      false },
    { TD_CMD_PREV_NULL,           "prev-null",           "{",      false },
    { TD_CMD_MOVE_COL_LEFT,       "move-col-left",       "H",      false },
    { TD_CMD_MOVE_COL_RIGHT,      "move-col-right",      "L",      false },
    { TD_CMD_HIDE_COL,            "hide-col",            "-",      false },
    { TD_CMD_TOGGLE_KEY_COL,      "toggle-key-col",      "!",      false },
    { TD_CMD_AUTOSIZE_COL,        "autosize-col",        "_",      false },
    { TD_CMD_AUTOSIZE_ALL_COLS,   "autosize-all-cols",   "g_",     false },
    { TD_CMD_SET_COL_WIDTH,       "set-col-width",       "z_",     true  },
    { TD_CMD_UNHIDE_ALL_COLS,     "unhide-all-cols",     "gv",     false },
    { TD_CMD_RENAME_COL,          "rename-col",          "^",      true  },
    { TD_CMD_SAVE_SHEET,          "save-sheet",          "Ctrl+S", true  },
    { TD_CMD_TYPE_ANY,            "type-any",            "z~",     false },
    { TD_CMD_TYPE_STR,            "type-str",            "~",      false },
    { TD_CMD_TYPE_INT,            "type-int",            "#",      false },
    { TD_CMD_TYPE_FLOAT,          "type-float",          "%",      false },
    { TD_CMD_TYPE_CURRENCY,       "type-currency",       "$",      false },
    { TD_CMD_TYPE_DATE,           "type-date",           "@",      false },
    { TD_CMD_POP_SHEET,           "pop-sheet",           "q",      false },
    { TD_CMD_QUIT_ALL,            "quit-all",            "gq",     false },
    { TD_CMD_SELECT_ROW,          "select-row",          "s",      false },
    { TD_CMD_UNSELECT_ROW,        "unselect-row",        "u",      false },
    { TD_CMD_TOGGLE_ROW,          "toggle-row",          "t",      false },
    { TD_CMD_SELECT_ALL,          "select-all",          "gs",     false },
    { TD_CMD_UNSELECT_ALL,        "unselect-all",        "gu",     false },
    { TD_CMD_TOGGLE_ALL,          "toggle-all",          "gt",     false },
    { TD_CMD_SELECT_REGEX,        "select-regex",        "|",      true  },
    { TD_CMD_UNSELECT_REGEX,      "unselect-regex",      "\\",     true  },
    { TD_CMD_SELECT_ALL_REGEX,    "select-all-regex",    "g|",     true  },
    { TD_CMD_UNSELECT_ALL_REGEX,  "unselect-all-regex",  "g\\",    true  },
    { TD_CMD_SEARCH_FWD,          "search-fwd",          "/",      true  },
    { TD_CMD_SEARCH_BWD,          "search-bwd",          "?",      true  },
    { TD_CMD_SEARCH_ALL_FWD,      "search-all-fwd",      "g/",     true  },
    { TD_CMD_SEARCH_ALL_BWD,      "search-all-bwd",      "g?",     true  },
    { TD_CMD_SEARCH_NEXT,         "search-next",         "n",      false },
    { TD_CMD_SEARCH_PREV,         "search-prev",         "N",      false },
    { TD_CMD_DUP_SELECTED,        "dup-selected",        "\"",     false },
    { TD_CMD_DELETE_ROW,          "delete-row",          "d",      false },
    { TD_CMD_DELETE_SELECTED,     "delete-selected",     "gd",     false },
    { TD_CMD_SORT_ASC,            "sort-asc",            "[",      false },
    { TD_CMD_SORT_DESC,           "sort-desc",           "]",      false },
    { TD_CMD_SORT_KEYS_ASC,       "sort-keys-asc",       "g[",     false },
    { TD_CMD_SORT_KEYS_DESC,      "sort-keys-desc",      "g]",     false },
    { TD_CMD_EDIT_CELL,           "edit-cell",           "e",      true  },
    { TD_CMD_EDIT_SELECTED,       "edit-selected",       "ge",     true  },
    { TD_CMD_CLEAR_CELL,          "clear-cell",          "zd",     false },
    { TD_CMD_CLEAR_SELECTED,      "clear-selected",      "gzd",    false },
    { TD_CMD_ADD_ROW,             "add-row",             "a",      false },
    { TD_CMD_ADD_ROWS,            "add-rows",            "ga",     true  },
    { TD_CMD_ADD_COL,             "add-col",             "za",     false },
    { TD_CMD_DUP_ROW,             "dup-row",             "g\"",    false },
    { TD_CMD_DUP_COL,             "dup-col",             "z\"",    false },
    { TD_CMD_YANK_ROW,            "yank-row",            "y",      false },
    { TD_CMD_YANK_SELECTED,       "yank-selected",       "gy",     false },
    { TD_CMD_YANK_CELL,           "yank-cell",           "zy",     false },
    { TD_CMD_YANK_SELECTED_CELLS, "yank-selected-cells", "gzy",    false },
    { TD_CMD_PASTE_AFTER,         "paste-after",         "p",      false },
    { TD_CMD_PASTE_BEFORE,        "paste-before",        "P",      false },
    { TD_CMD_PASTE_CELL,          "paste-cell",          "zp",     false },
    { TD_CMD_UNDO,                "undo",                "U",      false },
    { TD_CMD_REDO,                "redo",                "R",      false },
    { TD_CMD_SHOW_CMDLOG,         "show-cmdlog",         "D",      false },
    { TD_CMD_REDRAW,              "redraw",              "Ctrl+L", false }
};

const td_cmd_meta_t *td_cmd_meta(td_cmd_t cmd)
{
    size_t n = sizeof(td_cmd_table) / sizeof(td_cmd_table[0]);

    for (size_t i = 0; i < n; i++) {
        if (td_cmd_table[i].cmd == cmd)
            return &td_cmd_table[i];
    }
    return NULL;
}

td_cmd_t td_cmd_from_longname(const char *longname)
{
    size_t n = sizeof(td_cmd_table) / sizeof(td_cmd_table[0]);

    if (!longname || !longname[0])
        return TD_CMD_NONE;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(td_cmd_table[i].longname, longname) == 0)
            return td_cmd_table[i].cmd;
    }
    return TD_CMD_NONE;
}

void td_cmdlog_init(td_cmdlog_t *log)
{
    if (!log)
        return;
    memset(log, 0, sizeof(*log));
    td_arena_init(&log->arena, 0);
}

void td_cmdlog_free(td_cmdlog_t *log)
{
    if (!log)
        return;
    free(log->entries);
    td_arena_free(&log->arena);
    memset(log, 0, sizeof(*log));
}

static bool td_cmdlog_grow(td_cmdlog_t *log, size_t need)
{
    size_t new_cap;
    td_cmdlog_entry_t *items;

    if (need <= log->cap)
        return true;
    new_cap = log->cap ? log->cap * 2 : 32;
    while (new_cap < need)
        new_cap *= 2;
    items = realloc(log->entries, new_cap * sizeof(*items));
    if (!items)
        return false;
    log->entries = items;
    log->cap = new_cap;
    return true;
}

bool td_cmdlog_record(td_cmdlog_t *log, const td_sheet_t *s,
                      size_t cursor_row, size_t cursor_col, td_cmd_t cmd,
                      const char *input, const char *comment)
{
    const td_cmd_meta_t *meta;
    td_cmdlog_entry_t e;
    char row_buf[32];
    const char *col_name = "";

    if (!log || cmd == TD_CMD_NONE || cmd == TD_CMD_SHOW_CMDLOG ||
        cmd == TD_CMD_REDRAW)
        return false;
    if (s && s->name && strcmp(s->name, "cmdlog") == 0)
        return false;

    meta = td_cmd_meta(cmd);
    if (!meta || !td_cmdlog_grow(log, log->len + 1))
        return false;

    if (s && s->nrows > 0 && cursor_row < s->nrows)
        snprintf(row_buf, sizeof(row_buf), "%zu", cursor_row);
    else
        row_buf[0] = '\0';

    if (s && s->ncols > 0 && cursor_col < s->ncols && s->cols[cursor_col].name)
        col_name = s->cols[cursor_col].name;

    e.sheet = td_arena_strdup(&log->arena, (s && s->name) ? s->name : "");
    e.col = td_arena_strdup(&log->arena, col_name);
    e.row = td_arena_strdup(&log->arena, row_buf);
    e.longname = meta->longname;
    e.input = td_arena_strdup(&log->arena, input ? input : "");
    e.keystrokes = meta->keystrokes;
    e.comment = td_arena_strdup(&log->arena, comment ? comment : "");
    if (!e.sheet || !e.col || !e.row || !e.input || !e.comment)
        return false;

    log->entries[log->len++] = e;
    return true;
}

static bool td_cmdlog_set_str_cell(td_sheet_t *s, size_t row, size_t col,
                                   const char *str)
{
    char *copy;
    td_val_t v;

    if (!str || !str[0])
        return true;
    copy = td_arena_strdup(&s->arena, str);
    if (!copy)
        return false;
    v.type = TD_TYPE_STR;
    v.as.s = td_slice_from_cstr(copy);
    return td_sheet_set_cell(s, row, col, v);
}

td_sheet_t *td_cmdlog_to_sheet(const td_cmdlog_t *log)
{
    static const char *const headers[7] = {
        "sheet", "col", "row", "longname", "input", "keystrokes", "comment"
    };
    td_sheet_t *s = td_sheet_new("cmdlog", "cmdlog.vd");

    if (!s)
        return NULL;
    for (size_t c = 0; c < 7; c++) {
        if (td_sheet_add_col(s, headers[c], TD_TYPE_STR,
                             -TD_AUTO_WIDTH_CAP) == SIZE_MAX) {
            td_sheet_free(s);
            return NULL;
        }
    }
    if (!log)
        return s;

    for (size_t i = 0; i < log->len; i++) {
        const td_cmdlog_entry_t *e = &log->entries[i];
        size_t r = td_sheet_add_row(s);

        if (r == SIZE_MAX ||
            !td_cmdlog_set_str_cell(s, r, 0, e->sheet) ||
            !td_cmdlog_set_str_cell(s, r, 1, e->col) ||
            !td_cmdlog_set_str_cell(s, r, 2, e->row) ||
            !td_cmdlog_set_str_cell(s, r, 3, e->longname) ||
            !td_cmdlog_set_str_cell(s, r, 4, e->input) ||
            !td_cmdlog_set_str_cell(s, r, 5, e->keystrokes) ||
            !td_cmdlog_set_str_cell(s, r, 6, e->comment)) {
            td_sheet_free(s);
            return NULL;
        }
    }
    return s;
}

static size_t td_find_col_by_name(const td_sheet_t *s, const char *name)
{
    if (!s || !name)
        return SIZE_MAX;
    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].name && strcmp(s->cols[c].name, name) == 0)
            return c;
    }
    return SIZE_MAX;
}

static char *td_format_script_cell(const td_sheet_t *s, size_t vis_row,
                                   size_t col)
{
    char stack_buf[256];
    td_val_t v;
    size_t n;
    char *out;

    if (col == SIZE_MAX || vis_row >= s->nrows || !s->row_order) {
        out = malloc(1);
        if (out)
            out[0] = '\0';
        return out;
    }
    v = td_sheet_get_cell(s, s->row_order[vis_row], col);
    n = td_val_format(v, s->cols[col].type, stack_buf, sizeof(stack_buf));
    if (n < sizeof(stack_buf)) {
        out = malloc(n + 1);
        if (out)
            memcpy(out, stack_buf, n + 1);
        return out;
    }
    out = malloc(n + 1);
    if (!out)
        return NULL;
    (void)td_val_format(v, s->cols[col].type, out, n + 1);
    return out;
}

static bool td_parse_size_str(const char *s, size_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (!s || !s[0] || s[0] == '-')
        return false;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0')
        return false;
    *out = (size_t)v;
    return true;
}

static bool td_stack_focus_sheet(td_stack_t *st, const char *name)
{
    if (!st || st->len == 0 || !name || !name[0])
        return false;
    for (size_t i = st->len; i-- > 0;) {
        if (st->sheets[i]->name && strcmp(st->sheets[i]->name, name) == 0) {
            if (i != st->len - 1) {
                td_sheet_t *target = st->sheets[i];

                memmove(&st->sheets[i], &st->sheets[i + 1],
                        (st->len - 1 - i) * sizeof(st->sheets[0]));
                st->sheets[st->len - 1] = target;
            }
            return true;
        }
    }
    return false;
}

bool td_cmdlog_replay_sheet(const td_sheet_t *script, td_stack_t *stack,
                            td_cmd_exec_fn exec_fn, void *ctx,
                            const bool *quit_flag, char *err, size_t err_cap)
{
    size_t c_sheet, c_col, c_row, c_longname, c_input;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!script || !stack || !exec_fn) {
        if (err && err_cap > 0)
            snprintf(err, err_cap, "invalid replay arguments");
        return false;
    }

    c_longname = td_find_col_by_name(script, "longname");
    if (c_longname == SIZE_MAX) {
        if (err && err_cap > 0)
            snprintf(err, err_cap, "cmdlog missing 'longname' column");
        return false;
    }
    c_sheet = td_find_col_by_name(script, "sheet");
    c_col = td_find_col_by_name(script, "col");
    c_row = td_find_col_by_name(script, "row");
    c_input = td_find_col_by_name(script, "input");

    for (size_t vr = 0; vr < script->nrows; vr++) {
        char *sheet_s = td_format_script_cell(script, vr, c_sheet);
        char *col_s = td_format_script_cell(script, vr, c_col);
        char *row_s = td_format_script_cell(script, vr, c_row);
        char *long_s = td_format_script_cell(script, vr, c_longname);
        char *input_s = td_format_script_cell(script, vr, c_input);
        bool ok = true;

        if (!sheet_s || !col_s || !row_s || !long_s || !input_s) {
            if (err && err_cap > 0)
                snprintf(err, err_cap, "out of memory replaying script");
            ok = false;
            goto step_done;
        }

        if (!long_s[0] || long_s[0] == '#')
            goto step_done;

        td_cmd_t cmd = td_cmd_from_longname(long_s);

        if (cmd == TD_CMD_NONE) {
            if (err && err_cap > 0)
                snprintf(err, err_cap, "line %zu: unknown command '%s'",
                         vr + 2, long_s);
            ok = false;
            goto step_done;
        }

        if (sheet_s[0] && !td_stack_focus_sheet(stack, sheet_s)) {
            if (err && err_cap > 0)
                snprintf(err, err_cap, "line %zu: unknown sheet '%s'",
                         vr + 2, sheet_s);
            ok = false;
            goto step_done;
        }

        td_sheet_t *active = td_stack_active(stack);

        if (!active) {
            if (err && err_cap > 0)
                snprintf(err, err_cap, "line %zu: no active sheet", vr + 2);
            ok = false;
            goto step_done;
        }

        if (col_s[0]) {
            size_t col_idx = td_find_col_by_name(active, col_s);

            if (col_idx == SIZE_MAX) {
                size_t parsed_col = 0;

                if (td_parse_size_str(col_s, &parsed_col) &&
                    parsed_col < active->ncols)
                    col_idx = parsed_col;
            }
            if (col_idx == SIZE_MAX) {
                if (err && err_cap > 0)
                    snprintf(err, err_cap, "line %zu: unknown column '%s'",
                             vr + 2, col_s);
                ok = false;
                goto step_done;
            }
            active->cursor_col = col_idx;
        }

        if (row_s[0]) {
            size_t row_idx = 0;

            if (!td_parse_size_str(row_s, &row_idx) ||
                (active->nrows > 0 && row_idx >= active->nrows) ||
                (active->nrows == 0 && row_idx > 0)) {
                if (err && err_cap > 0)
                    snprintf(err, err_cap, "line %zu: invalid row '%s'",
                             vr + 2, row_s);
                ok = false;
                goto step_done;
            }
            active->cursor_row = row_idx;
        }

        char step_err[256] = "";

        if (!exec_fn(cmd, input_s, ctx, step_err, sizeof(step_err))) {
            if (err && err_cap > 0)
                snprintf(err, err_cap, "line %zu (%s): %s", vr + 2, long_s,
                         step_err[0] ? step_err : "command failed");
            ok = false;
        }

    step_done:
        free(sheet_s);
        free(col_s);
        free(row_s);
        free(long_s);
        free(input_s);
        if (!ok)
            return false;
        if (quit_flag && *quit_flag)
            break;
    }

    return true;
}

bool td_cmdlog_replay_file(const char *path, td_stack_t *stack,
                           td_cmd_exec_fn exec_fn, void *ctx,
                           const bool *quit_flag, char *err, size_t err_cap)
{
    td_csv_opts_t opts;
    td_sheet_t *script;
    bool ok;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!path || !path[0]) {
        if (err && err_cap > 0)
            snprintf(err, err_cap, "empty replay script path");
        return false;
    }
    td_csv_opts_default(&opts);
    opts.delimiter = '\t';
    opts.has_header = true;
    opts.strict_tsv = true;
    script = td_csv_load_file(path, &opts, err, err_cap);
    if (!script)
        return false;
    ok = td_cmdlog_replay_sheet(script, stack, exec_fn, ctx, quit_flag, err,
                                err_cap);
    td_sheet_free(script);
    return ok;
}
