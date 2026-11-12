#ifndef TD_TAM_DATA_H
#define TD_TAM_DATA_H

#include <stdbool.h>
#include <stddef.h>

#include "sheet.h"
#include "term.h"

typedef struct {
    const char *message;
    const char *pending;
    bool prompt_active;
    const char *prompt_label;
    const char *prompt_text;
    size_t prompt_cursor;
} td_view_state_t;

void td_view_render(td_term_t *term, td_stack_t *stack,
                    const td_view_state_t *state);
td_sheet_t *td_demo_sheet(void);

#endif /* TD_TAM_DATA_H */
