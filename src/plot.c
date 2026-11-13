#include "plot.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void td_plot_err(char *err, size_t cap, const char *msg)
{
    if (err && cap > 0)
        snprintf(err, cap, "%s", msg ? msg : "");
}

static void td_plot_meta_free(void *ptr)
{
    td_plot_meta_t *m = (td_plot_meta_t *)ptr;

    if (!m)
        return;
    free(m->x_label);
    for (size_t i = 0; i < m->nseries; i++) {
        free(m->series[i].name);
        free(m->series[i].xs);
        free(m->series[i].ys);
    }
    free(m);
}

static bool td_val_as_double(const td_sheet_t *s, size_t phys, size_t col,
                             double *out)
{
    td_val_t v = td_sheet_eval_cell(s, phys, col);
    td_val_t parsed;
    char buf[128];

    if (v.type == TD_TYPE_INT) {
        *out = (double)v.as.i;
        return true;
    }
    if (v.type == TD_TYPE_FLOAT || v.type == TD_TYPE_CURRENCY) {
        if (!isfinite(v.as.f))
            return false;
        *out = v.as.f;
        return true;
    }
    if ((v.type == TD_TYPE_ANY || v.type == TD_TYPE_STR) &&
        v.as.s.data && v.as.s.len > 0) {
        td_type_t hint = s->cols[col].type;

        if (hint == TD_TYPE_STR || hint == TD_TYPE_DATE)
            return false;
        td_val_format(v, hint, buf, sizeof(buf));
        parsed = td_val_parse(td_slice_from_cstr(buf),
                              hint == TD_TYPE_CURRENCY ? TD_TYPE_CURRENCY : TD_TYPE_FLOAT);
        if (parsed.type == TD_TYPE_INT) {
            *out = (double)parsed.as.i;
            return true;
        }
        if ((parsed.type == TD_TYPE_FLOAT || parsed.type == TD_TYPE_CURRENCY) &&
            isfinite(parsed.as.f)) {
            *out = parsed.as.f;
            return true;
        }
    }
    return false;
}

static const uint8_t td_series_colors[TD_PLOT_MAX_SERIES] = {
    2, 4, 3, 6, 5, 1, 7, 2
};

td_sheet_t *td_plot_sheet_build(const td_sheet_t *src, bool all_numeric,
                                char *err, size_t err_cap)
{
    td_plot_meta_t *m;
    size_t x_col = SIZE_MAX;
    bool x_is_numeric = false;
    size_t y_cols[TD_PLOT_MAX_SERIES];
    size_t ny = 0;
    td_sheet_t *s;
    char name_buf[128];

    if (!src || src->nrows == 0 || src->ncols == 0) {
        td_plot_err(err, err_cap, "no data to plot");
        return NULL;
    }

    for (size_t c = 0; c < src->ncols; c++) {
        if (src->cols[c].is_key && td_sheet_col_width(src, c) > 0) {
            x_col = c;
            break;
        }
    }

    if (x_col != SIZE_MAX) {
        double dummy;

        for (size_t r = 0; r < src->nrows; r++) {
            if (td_val_as_double(src, src->row_order[r], x_col, &dummy)) {
                x_is_numeric = true;
                break;
            }
        }
    }

    if (!all_numeric) {
        if (src->cursor_col < src->ncols)
            y_cols[ny++] = src->cursor_col;
    } else {
        for (size_t c = 0; c < src->ncols && ny < TD_PLOT_MAX_SERIES; c++) {
            double dummy;

            if (td_sheet_col_width(src, c) <= 0)
                continue;
            if (c == x_col && x_is_numeric)
                continue;
            for (size_t r = 0; r < src->nrows; r++) {
                if (td_val_as_double(src, src->row_order[r], c, &dummy)) {
                    y_cols[ny++] = c;
                    break;
                }
            }
        }
    }

    if (ny == 0) {
        td_plot_err(err, err_cap, "no numeric columns to plot");
        return NULL;
    }

    m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->kind = (x_col != SIZE_MAX && !x_is_numeric) ? TD_PLOT_BAR : TD_PLOT_SCATTER;
    m->x_label = strdup(x_col != SIZE_MAX ? src->cols[x_col].name : "row");
    if (!m->x_label) {
        free(m);
        return NULL;
    }

    bool have_bounds = false;

    for (size_t si = 0; si < ny; si++) {
        size_t yc = y_cols[si];
        double *xs = calloc(src->nrows, sizeof(double));
        double *ys = calloc(src->nrows, sizeof(double));
        size_t np = 0;

        if (!xs || !ys) {
            free(xs);
            free(ys);
            td_plot_meta_free(m);
            return NULL;
        }

        for (size_t vr = 0; vr < src->nrows; vr++) {
            size_t phys = src->row_order[vr];
            double xv = (double)vr;
            double yv = 0.0;

            if (!td_val_as_double(src, phys, yc, &yv))
                continue;
            if (x_col != SIZE_MAX && x_is_numeric) {
                if (!td_val_as_double(src, phys, x_col, &xv))
                    continue;
            }
            xs[np] = xv;
            ys[np] = yv;
            np++;

            if (!have_bounds) {
                m->data_xmin = m->data_xmax = xv;
                m->data_ymin = m->data_ymax = yv;
                have_bounds = true;
            } else {
                if (xv < m->data_xmin) m->data_xmin = xv;
                if (xv > m->data_xmax) m->data_xmax = xv;
                if (yv < m->data_ymin) m->data_ymin = yv;
                if (yv > m->data_ymax) m->data_ymax = yv;
            }
        }

        if (np == 0) {
            free(xs);
            free(ys);
            continue;
        }
        m->series[m->nseries].name = strdup(src->cols[yc].name);
        m->series[m->nseries].xs = xs;
        m->series[m->nseries].ys = ys;
        m->series[m->nseries].npoints = np;
        m->series[m->nseries].color = td_series_colors[m->nseries % TD_PLOT_MAX_SERIES];
        m->nseries++;
    }

    if (m->nseries == 0 || !have_bounds) {
        td_plot_meta_free(m);
        td_plot_err(err, err_cap, "no numeric values found in selected column(s)");
        return NULL;
    }

    if (m->kind == TD_PLOT_BAR && m->data_ymin > 0.0)
        m->data_ymin = 0.0;
    if (m->data_xmin == m->data_xmax) {
        m->data_xmin -= 1.0;
        m->data_xmax += 1.0;
    }
    if (m->data_ymin == m->data_ymax) {
        m->data_ymin -= 1.0;
        m->data_ymax += 1.0;
    }
    m->view_xmin = m->data_xmin;
    m->view_xmax = m->data_xmax;
    m->view_ymin = m->data_ymin;
    m->view_ymax = m->data_ymax;

    snprintf(name_buf, sizeof(name_buf), "%s_plot",
             src->name ? src->name : "sheet");
    s = td_sheet_new(name_buf, src->source_path);
    if (!s) {
        td_plot_meta_free(m);
        return NULL;
    }
    s->kind = TD_SHEET_PLOT;
    s->source_id = src->id;
    s->meta = m;
    s->meta_free = td_plot_meta_free;
    return s;
}

void td_plot_pan_zoom(td_sheet_t *s, int dx, int dy, double zoom_factor,
                      bool reset)
{
    td_plot_meta_t *m;
    double xr;
    double yr;

    if (!s || s->kind != TD_SHEET_PLOT || !s->meta)
        return;
    m = (td_plot_meta_t *)s->meta;
    if (reset) {
        m->view_xmin = m->data_xmin;
        m->view_xmax = m->data_xmax;
        m->view_ymin = m->data_ymin;
        m->view_ymax = m->data_ymax;
        return;
    }
    xr = m->view_xmax - m->view_xmin;
    yr = m->view_ymax - m->view_ymin;
    if (dx != 0) {
        double shift = xr * 0.1 * (double)dx;
        m->view_xmin += shift;
        m->view_xmax += shift;
    }
    if (dy != 0) {
        double shift = yr * 0.1 * (double)dy;
        m->view_ymin += shift;
        m->view_ymax += shift;
    }
    if (zoom_factor > 0.0 && zoom_factor != 1.0) {
        double cx = (m->view_xmin + m->view_xmax) * 0.5;
        double cy = (m->view_ymin + m->view_ymax) * 0.5;
        double nxr = xr * zoom_factor;
        double nyr = yr * zoom_factor;

        if (nxr > 1e-12 && nyr > 1e-12) {
            m->view_xmin = cx - nxr * 0.5;
            m->view_xmax = cx + nxr * 0.5;
            m->view_ymin = cy - nyr * 0.5;
            m->view_ymax = cy + nyr * 0.5;
        }
    }
}

void td_plot_render(td_term_t *t, const td_sheet_t *s,
                    const td_view_state_t *state, bool use_braille)
{
    const td_plot_meta_t *m;
    int top = 1;
    int left = 11;
    int bottom;
    int plot_w;
    int plot_h;
    bool braille;
    uint8_t *masks = NULL;
    uint8_t *colors = NULL;
    char lbl[32];
    int lx = 0;

    (void)state;
    if (!t || !t->back || !s || s->kind != TD_SHEET_PLOT || !s->meta)
        return;
    m = (const td_plot_meta_t *)s->meta;
    if (t->rows < 6 || t->cols < 20)
        return;

    braille = use_braille && t->profile.utf8;
    bottom = t->rows - 3; /* leave 1 row for X labels + 2 rows for status/mode */
    plot_w = t->cols - left - 1;
    plot_h = bottom - top;
    if (plot_w <= 2 || plot_h <= 2)
        return;

    /* Top legend bar on row 0. */
    snprintf(lbl, sizeof(lbl), "Plot (%s): ", m->x_label);
    td_term_put_str(t, 0, 0, t->cols, lbl,
                    TD_COLOR_DEFAULT, TD_COLOR_DEFAULT, TD_ATTR_BOLD);
    lx = (int)strlen(lbl);
    for (size_t i = 0; i < m->nseries && lx < t->cols; i++) {
        char item[64];
        uint8_t fg = t->profile.colors > 0 ? m->series[i].color : TD_COLOR_DEFAULT;

        snprintf(item, sizeof(item), "%s[%s] ", i == 0 ? "" : "", m->series[i].name);
        td_term_put_str(t, 0, lx, t->cols - lx, item,
                        fg, TD_COLOR_DEFAULT, TD_ATTR_BOLD);
        lx += (int)strlen(item);
    }

    /* Y axis labels and border. */
    snprintf(lbl, sizeof(lbl), "%9.3g", m->view_ymax);
    td_term_put_str(t, top, 0, left - 1, lbl,
                    TD_COLOR_DEFAULT, TD_COLOR_DEFAULT, TD_ATTR_DIM);
    snprintf(lbl, sizeof(lbl), "%9.3g", (m->view_ymin + m->view_ymax) * 0.5);
    td_term_put_str(t, top + plot_h / 2, 0, left - 1, lbl,
                    TD_COLOR_DEFAULT, TD_COLOR_DEFAULT, TD_ATTR_DIM);
    snprintf(lbl, sizeof(lbl), "%9.3g", m->view_ymin);
    td_term_put_str(t, bottom - 1, 0, left - 1, lbl,
                    TD_COLOR_DEFAULT, TD_COLOR_DEFAULT, TD_ATTR_DIM);

    for (int r = top; r < bottom; r++) {
        td_term_put_cell(t, r, left - 1, braille ? 0x2502u : (uint32_t)'|',
                         TD_COLOR_DEFAULT, TD_COLOR_DEFAULT, TD_ATTR_DIM);
    }
    td_term_put_cell(t, bottom, left - 1, braille ? 0x2514u : (uint32_t)'+',
                     TD_COLOR_DEFAULT, TD_COLOR_DEFAULT, TD_ATTR_DIM);
    for (int c = left; c < left + plot_w; c++) {
        td_term_put_cell(t, bottom, c, braille ? 0x2500u : (uint32_t)'-',
                         TD_COLOR_DEFAULT, TD_COLOR_DEFAULT, TD_ATTR_DIM);
    }

    snprintf(lbl, sizeof(lbl), "%.3g", m->view_xmin);
    td_term_put_str(t, bottom, left, 16, lbl,
                    TD_COLOR_DEFAULT, TD_COLOR_DEFAULT, TD_ATTR_DIM);
    snprintf(lbl, sizeof(lbl), "%.3g", m->view_xmax);
    if ((int)strlen(lbl) < plot_w) {
        td_term_put_str(t, bottom, left + plot_w - (int)strlen(lbl),
                        (int)strlen(lbl), lbl,
                        TD_COLOR_DEFAULT, TD_COLOR_DEFAULT, TD_ATTR_DIM);
    }

    masks = calloc((size_t)(plot_w * plot_h), sizeof(uint8_t));
    colors = calloc((size_t)(plot_w * plot_h), sizeof(uint8_t));
    if (!masks || !colors) {
        free(masks);
        free(colors);
        return;
    }

    int px_w = braille ? plot_w * 2 : plot_w;
    int px_h = braille ? plot_h * 4 : plot_h;
    double xr = m->view_xmax - m->view_xmin;
    double yr = m->view_ymax - m->view_ymin;
    static const uint8_t braille_bits[2][4] = {
        { 0x01, 0x02, 0x04, 0x40 },
        { 0x08, 0x10, 0x20, 0x80 }
    };

    for (size_t si = 0; si < m->nseries; si++) {
        const td_plot_series_t *ser = &m->series[si];

        for (size_t i = 0; i < ser->npoints; i++) {
            double nx = (ser->xs[i] - m->view_xmin) / xr;
            double ny = (ser->ys[i] - m->view_ymin) / yr;
            int px = (int)llround(nx * (double)(px_w - 1));
            int py = (px_h - 1) - (int)llround(ny * (double)(px_h - 1));

            if (px < 0 || px >= px_w || py < 0 || py >= px_h)
                continue;

            if (m->kind == TD_PLOT_BAR) {
                int zero_py = (px_h - 1) -
                              (int)llround(((0.0 - m->view_ymin) / yr) * (double)(px_h - 1));
                int y0 = py < zero_py ? py : zero_py;
                int y1 = py < zero_py ? zero_py : py;

                if (y0 < 0) y0 = 0;
                if (y1 >= px_h) y1 = px_h - 1;
                for (int yy = y0; yy <= y1; yy++) {
                    int cx = braille ? (px / 2) : px;
                    int cy = braille ? (yy / 4) : yy;
                    size_t idx = (size_t)cy * (size_t)plot_w + (size_t)cx;

                    if (braille)
                        masks[idx] |= braille_bits[px % 2][yy % 4];
                    else
                        masks[idx]++;
                    colors[idx] = ser->color;
                }
            } else {
                int cx = braille ? (px / 2) : px;
                int cy = braille ? (py / 4) : py;
                size_t idx = (size_t)cy * (size_t)plot_w + (size_t)cx;

                if (braille)
                    masks[idx] |= braille_bits[px % 2][py % 4];
                else
                    masks[idx]++;
                colors[idx] = ser->color;
            }
        }
    }

    for (int cy = 0; cy < plot_h; cy++) {
        for (int cx = 0; cx < plot_w; cx++) {
            size_t idx = (size_t)cy * (size_t)plot_w + (size_t)cx;
            uint8_t mask = masks[idx];
            uint8_t fg;
            uint32_t ch;

            if (mask == 0)
                continue;
            fg = t->profile.colors > 0 ? colors[idx] : TD_COLOR_DEFAULT;
            if (braille) {
                ch = 0x2800u | (uint32_t)mask;
            } else {
                static const char ramp[] = " .:*#";
                uint8_t lvl = mask > 4 ? 4 : mask;

                ch = (uint32_t)ramp[lvl];
            }
            td_term_put_cell(t, top + cy, left + cx, ch, fg,
                             TD_COLOR_DEFAULT, TD_ATTR_NONE);
        }
    }

    free(masks);
    free(colors);
}
