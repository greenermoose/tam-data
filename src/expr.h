#ifndef TD_EXPR_H
#define TD_EXPR_H

#include <stdbool.h>
#include <stddef.h>

#include "arena.h"
#include "sheet.h"
#include "val.h"

typedef struct td_expr td_expr_t;

td_expr_t *td_expr_compile(const td_sheet_t *s, const char *src,
                           char *err, size_t err_cap);
void td_expr_free(td_expr_t *e);
td_val_t td_expr_eval(const td_expr_t *e, const td_sheet_t *s,
                      size_t phys_row, td_arena_t *scratch,
                      char *err, size_t err_cap);
const char *td_expr_source(const td_expr_t *e);

#endif /* TD_EXPR_H */
