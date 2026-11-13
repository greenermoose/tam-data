#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "clipboard.h"
#include "cmdlog.h"
#include "csv.h"
#include "tam-data.h"
#include "test.h"

static bool replay_cb(td_cmd_t cmd, const char *input, void *ctx,
                      char *err, size_t err_cap)
{
    return td_exec_action(cmd, input, (td_exec_ctx_t *)ctx, err, err_cap);
}

static td_sheet_t *make_replay_target(void)
{
    td_sheet_t *s = td_sheet_new("items", "");

    if (!s)
        return NULL;
    td_sheet_add_col(s, "item", TD_TYPE_STR, -20);
    td_sheet_add_col(s, "qty", TD_TYPE_INT, -10);
    td_sheet_add_row(s);
    td_sheet_add_row(s);
    td_sheet_set_cell(s, 0, 0,
                      td_val_parse(td_slice_from_cstr("bolt"), TD_TYPE_STR));
    td_sheet_set_cell(s, 0, 1,
                      td_val_parse(td_slice_from_cstr("5"), TD_TYPE_INT));
    td_sheet_set_cell(s, 1, 0,
                      td_val_parse(td_slice_from_cstr("nut"), TD_TYPE_STR));
    td_sheet_set_cell(s, 1, 1,
                      td_val_parse(td_slice_from_cstr("12"), TD_TYPE_INT));
    return s;
}

void test_cmdlog(void)
{
    td_cmdlog_t log;
    td_clipboard_t clip;
    td_stack_t st = { 0 };
    td_sheet_t *s;
    td_sheet_t *cl;
    char msg[256] = "";
    char err[256] = "";
    bool quit = false;
    td_exec_ctx_t ctx;
    char tmp_path[128];

    TD_CHECK_INT(td_cmd_from_longname("go-down"), TD_CMD_DOWN);
    TD_CHECK_INT(td_cmd_from_longname("edit-cell"), TD_CMD_EDIT_CELL);
    TD_CHECK_INT(td_cmd_from_longname("paste-after"), TD_CMD_PASTE_AFTER);
    TD_CHECK_INT(td_cmd_from_longname("no-such-cmd"), TD_CMD_NONE);
    TD_CHECK(td_cmd_meta(TD_CMD_UNDO) != NULL);
    TD_CHECK_STR(td_cmd_meta(TD_CMD_UNDO)->longname, "undo");

    td_cmdlog_init(&log);
    td_clipboard_init(&clip);
    s = make_replay_target();
    TD_CHECK(s != NULL);
    if (!s) {
        td_cmdlog_free(&log);
        td_clipboard_free(&clip);
        return;
    }
    TD_CHECK(td_stack_push(&st, s));

    ctx.stack = &st;
    ctx.clip = &clip;
    ctx.cmdlog = &log;
    ctx.term = NULL;
    ctx.msg = msg;
    ctx.msg_cap = sizeof(msg);
    ctx.quit = &quit;

    /* Execute movement + edit + Vim delete + paste + redraw + show-cmdlog. */
    TD_CHECK(td_exec_action(TD_CMD_DOWN, NULL, &ctx, err, sizeof(err)));
    TD_CHECK(td_exec_action(TD_CMD_RIGHT, NULL, &ctx, err, sizeof(err)));
    TD_CHECK(td_exec_action(TD_CMD_EDIT_CELL, "42", &ctx, err, sizeof(err)));
    TD_CHECK(td_exec_action(TD_CMD_DELETE_ROW, NULL, &ctx, err, sizeof(err)));
    TD_CHECK(td_exec_action(TD_CMD_PASTE_BEFORE, NULL, &ctx, err, sizeof(err)));
    TD_CHECK(td_exec_action(TD_CMD_REDRAW, NULL, &ctx, err, sizeof(err)));
    TD_CHECK(td_exec_action(TD_CMD_SHOW_CMDLOG, NULL, &ctx, err, sizeof(err)));

    /* Movement keys are recorded; redraw and show-cmdlog are not. */
    TD_CHECK_INT(log.len, 5);
    TD_CHECK_STR(log.entries[0].longname, "go-down");
    TD_CHECK_STR(log.entries[1].longname, "go-right");
    TD_CHECK_STR(log.entries[2].longname, "edit-cell");
    TD_CHECK_STR(log.entries[2].input, "42");
    TD_CHECK_STR(log.entries[3].longname, "delete-row");
    TD_CHECK_STR(log.entries[4].longname, "paste-before");

    /* Commands run while viewing cmdlog do not append to cmdlog. */
    TD_CHECK_STR(td_stack_active(&st)->name, "cmdlog");
    TD_CHECK(td_exec_action(TD_CMD_DOWN, NULL, &ctx, err, sizeof(err)));
    TD_CHECK_INT(log.len, 5);

    /* Materialize and save as .vd, then replay onto a fresh sheet. */
    cl = td_cmdlog_to_sheet(&log);
    TD_CHECK(cl != NULL);
    TD_CHECK_INT(cl->nrows, 5);
    TD_CHECK_INT(cl->ncols, 7);

    snprintf(tmp_path, sizeof(tmp_path), "/tmp/tam-data-test-%ld.vd",
             (long)getpid());
    TD_CHECK(td_csv_save_sheet(cl, tmp_path, '\0', err, sizeof(err)));
    td_sheet_free(cl);
    td_stack_free(&st);
    td_clipboard_free(&clip);
    td_cmdlog_free(&log);

    td_cmdlog_init(&log);
    td_clipboard_init(&clip);
    s = make_replay_target();
    TD_CHECK(td_stack_push(&st, s));
    quit = false;
    ctx.stack = &st;
    ctx.clip = &clip;
    ctx.cmdlog = &log;

    TD_CHECK(td_cmdlog_replay_file(tmp_path, &st, replay_cb, &ctx, &quit,
                                   err, sizeof(err)));
    remove(tmp_path);

    TD_CHECK_INT(s->nrows, 2);
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[0], 1).as.i, 42);
    TD_CHECK_INT(td_sheet_get_cell(s, s->row_order[1], 1).as.i, 5);
    TD_CHECK_INT(log.len, 5);

    /* Error reporting on unknown command in script sheet. */
    td_sheet_t *bad = td_sheet_new("bad", "");
    td_sheet_add_col(bad, "longname", TD_TYPE_STR, -20);
    td_sheet_add_row(bad);
    td_sheet_set_cell(bad, 0, 0,
                      td_val_parse(td_slice_from_cstr("bogus-cmd"),
                                   TD_TYPE_STR));
    TD_CHECK(!td_cmdlog_replay_sheet(bad, &st, replay_cb, &ctx, &quit,
                                     err, sizeof(err)));
    TD_CHECK(strstr(err, "unknown command") != NULL);
    td_sheet_free(bad);

    td_stack_free(&st);
    td_clipboard_free(&clip);
    td_cmdlog_free(&log);
}
