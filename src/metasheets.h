#ifndef TD_METASHEETS_H
#define TD_METASHEETS_H

#include <stdbool.h>
#include <stddef.h>

#include "csv.h"
#include "options.h"
#include "sheet.h"

td_sheet_t *td_meta_sheets_build(const td_stack_t *stack, bool all_sheets);
td_sheet_t *td_meta_cols_build(const td_stack_t *stack, bool all_sheets);
td_sheet_t *td_meta_options_build(td_options_t *opts);
td_sheet_t *td_meta_dir_build(const char *dirpath, char *err, size_t err_cap);
td_sheet_t *td_meta_help_build(void);

bool td_meta_cols_apply_edit(td_sheet_t *cols_sheet, td_stack_t *stack,
                             size_t vis_row, size_t col,
                             char *err, size_t err_cap);
bool td_meta_options_apply_edit(td_sheet_t *opt_sheet, td_options_t *opts,
                                size_t vis_row, size_t col,
                                char *err, size_t err_cap);
bool td_meta_sheets_drop_row(td_sheet_t *sheets_sheet, td_stack_t *stack,
                             size_t vis_row, char *err, size_t err_cap);

td_sheet_t *td_meta_dive_row(const td_sheet_t *s, td_stack_t *stack,
                             const td_csv_opts_t *csv_opts,
                             int *cmd_out, char *err, size_t err_cap);

#endif /* TD_METASHEETS_H */
