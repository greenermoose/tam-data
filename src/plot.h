#ifndef TD_PLOT_H
#define TD_PLOT_H

#include <stdbool.h>
#include <stddef.h>

#include "sheet.h"
#include "tam-data.h"
#include "term.h"

#define TD_PLOT_MAX_SERIES 8

typedef enum {
    TD_PLOT_SCATTER = 0,
    TD_PLOT_BAR
} td_plot_kind_t;

typedef struct {
    char *name;
    double *xs;
    double *ys;
    size_t npoints;
    uint8_t color;
} td_plot_series_t;

typedef struct {
    td_plot_kind_t kind;
    char *x_label;
    td_plot_series_t series[TD_PLOT_MAX_SERIES];
    size_t nseries;
    double data_xmin;
    double data_xmax;
    double data_ymin;
    double data_ymax;
    double view_xmin;
    double view_xmax;
    double view_ymin;
    double view_ymax;
} td_plot_meta_t;

td_sheet_t *td_plot_sheet_build(const td_sheet_t *src, bool all_numeric,
                                char *err, size_t err_cap);
void td_plot_pan_zoom(td_sheet_t *s, int dx, int dy, double zoom_factor,
                      bool reset);
void td_plot_render(td_term_t *t, const td_sheet_t *s,
                    const td_view_state_t *state, bool use_braille);

#endif /* TD_PLOT_H */
