#ifndef TD_TAM_DATA_H
#define TD_TAM_DATA_H

#include <stdbool.h>
#include <stddef.h>

#include "clipboard.h"
#include "cmdlog.h"
#include "csv.h"
#include "options.h"
#include "sheet.h"
#include "term.h"

typedef struct {
    const char *message;
    const char *pending;
    bool prompt_active;
    const char *prompt_label;
    const char *prompt_text;
    size_t prompt_cursor;
    bool palette_active;
    size_t palette_sel;
    bool braille_plot;
} td_view_state_t;

typedef struct {
    td_stack_t *stack;
    td_clipboard_t *clip;
    td_cmdlog_t *cmdlog;
    td_term_t *term;
    td_options_t *opts;
    const td_csv_opts_t *csv_opts;
    char *msg;
    size_t msg_cap;
    bool *quit;
} td_exec_ctx_t;

void td_view_render(td_term_t *term, td_stack_t *stack,
                    const td_view_state_t *state);
td_sheet_t *td_demo_sheet(void);
bool td_exec_action(td_cmd_t cmd, const char *input_arg, td_exec_ctx_t *ctx,
                    char *err, size_t err_cap);

#endif /* TD_TAM_DATA_H */
