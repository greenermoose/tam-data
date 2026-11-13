#ifndef TD_AGG_H
#define TD_AGG_H

#include <stdbool.h>
#include <stddef.h>

#include "sheet.h"

typedef enum {
    TD_AGG_COUNT = 0,
    TD_AGG_DISTINCT,
    TD_AGG_NULLS,
    TD_AGG_SUM,
    TD_AGG_MEAN,
    TD_AGG_MEDIAN,
    TD_AGG_MIN,
    TD_AGG_MAX,
    TD_AGG_STDEV,
    TD_AGG_PCT90,
    TD_AGG_PCT95,
    TD_AGG_PCT99,
    TD_AGG_KIND_COUNT
} td_agg_kind_t;

typedef enum {
    TD_SHEET_DATA = 0,
    TD_SHEET_FREQ,
    TD_SHEET_DESCRIBE,
    TD_SHEET_PIVOT
} td_sheet_kind_t;

typedef enum {
    TD_JOIN_INNER = 0,
    TD_JOIN_OUTER,
    TD_JOIN_FULL,
    TD_JOIN_DIFF,
    TD_JOIN_APPEND,
    TD_JOIN_EXTEND
} td_join_kind_t;

bool td_agg_parse(const char *name, td_agg_kind_t *out);
const char *td_agg_name(td_agg_kind_t kind);
td_type_t td_agg_result_type(td_agg_kind_t agg, td_type_t col_type);
bool td_agg_is_numeric_only(td_agg_kind_t agg);

bool td_join_parse(const char *name, td_join_kind_t *out);
const char *td_join_name(td_join_kind_t kind);

td_val_t td_agg_compute_rows(const td_sheet_t *s, size_t col,
                             const size_t *phys_rows, size_t nrows,
                             td_agg_kind_t agg);

bool td_sheet_toggle_agg(td_sheet_t *s, size_t col, const char *spec,
                         char *msg, size_t msg_cap,
                         char *err, size_t err_cap);
void td_sheet_refresh_col_aggs(td_sheet_t *s);

td_sheet_t *td_agg_freq_sheet(const td_sheet_t *s, const size_t *key_cols,
                              size_t nkeys, char *err, size_t err_cap);
td_sheet_t *td_agg_describe_sheet(const td_sheet_t *s, char *err,
                                  size_t err_cap);
td_sheet_t *td_agg_pivot_sheet(const td_sheet_t *s, size_t pivot_col,
                               char *err, size_t err_cap);
td_sheet_t *td_agg_melt_sheet(const td_sheet_t *s, char *err, size_t err_cap);
td_sheet_t *td_agg_join_sheets(const td_sheet_t *left, const td_sheet_t *right,
                               td_join_kind_t kind, char *err, size_t err_cap);
td_sheet_t *td_agg_dive_row(const td_sheet_t *derived, const td_stack_t *stack,
                            char *err, size_t err_cap);

#endif /* TD_AGG_H */
