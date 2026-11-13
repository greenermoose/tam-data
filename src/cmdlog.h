#ifndef TD_CMDLOG_H
#define TD_CMDLOG_H

#include <stdbool.h>
#include <stddef.h>

#include "arena.h"
#include "input.h"
#include "sheet.h"

typedef struct {
    td_cmd_t cmd;
    const char *longname;
    const char *keystrokes;
    bool takes_input;
} td_cmd_meta_t;

typedef struct {
    const char *sheet;
    const char *col;
    const char *row;
    const char *longname;
    const char *input;
    const char *keystrokes;
    const char *comment;
} td_cmdlog_entry_t;

typedef struct {
    td_arena_t arena;
    td_cmdlog_entry_t *entries;
    size_t len;
    size_t cap;
} td_cmdlog_t;

typedef bool (*td_cmd_exec_fn)(td_cmd_t cmd, const char *input, void *ctx,
                               char *err, size_t err_cap);

const td_cmd_meta_t *td_cmd_meta(td_cmd_t cmd);
td_cmd_t td_cmd_from_longname(const char *longname);

void td_cmdlog_init(td_cmdlog_t *log);
void td_cmdlog_free(td_cmdlog_t *log);
bool td_cmdlog_record(td_cmdlog_t *log, const td_sheet_t *s,
                      size_t cursor_row, size_t cursor_col, td_cmd_t cmd,
                      const char *input, const char *comment);
td_sheet_t *td_cmdlog_to_sheet(const td_cmdlog_t *log);

bool td_cmdlog_replay_sheet(const td_sheet_t *script, td_stack_t *stack,
                            td_cmd_exec_fn exec_fn, void *ctx,
                            const bool *quit_flag, char *err, size_t err_cap);
bool td_cmdlog_replay_file(const char *path, td_stack_t *stack,
                           td_cmd_exec_fn exec_fn, void *ctx,
                           const bool *quit_flag, char *err, size_t err_cap);

#endif /* TD_CMDLOG_H */
