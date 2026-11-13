#include "test.h"

#include "csv.h"
#include "plot.h"
#include "term.h"

void test_plot(void)
{
    static const char data[] = "x,y,z\n1,10,5\n2,20,15\n3,30,25\n";
    char err[128] = "";
    td_csv_opts_t opts;
    td_sheet_t *src;
    td_sheet_t *plot;
    td_term_t term;
    td_view_state_t vs = { 0 };
    bool found_braille = false;
    bool found_ascii = false;

    td_csv_opts_default(&opts);
    src = td_csv_load_mem("nums", NULL, data, sizeof(data) - 1, &opts,
                          err, sizeof(err));
    TD_CHECK(src != NULL);
    src->cols[0].is_key = true;
    src->cursor_col = 1;

    plot = td_plot_sheet_build(src, false, err, sizeof(err));
    TD_CHECK(plot != NULL);
    TD_CHECK_INT(plot->kind, TD_SHEET_PLOT);

    /* UTF-8 Braille rendering */
    TD_CHECK_INT(td_term_init(&term, 20, 60, "foot", "en_US.UTF-8"), 0);
    td_term_clear_back(&term);
    td_plot_render(&term, plot, &vs, true);
    for (int r = 1; r < 17 && !found_braille; r++) {
        for (int c = 11; c < 59; c++) {
            td_cell_t cell = td_term_back_cell(&term, r, c);

            if (cell.ch >= 0x2800u && cell.ch <= 0x28FFu) {
                found_braille = true;
                break;
            }
        }
    }
    TD_CHECK(found_braille);
    td_term_close(&term);

    /* ASCII fallback rendering on non-UTF-8 Linux VT */
    TD_CHECK_INT(td_term_init(&term, 20, 60, "linux", "C"), 0);
    td_term_clear_back(&term);
    td_plot_render(&term, plot, &vs, true);
    for (int r = 1; r < 17 && !found_ascii; r++) {
        for (int c = 11; c < 59; c++) {
            td_cell_t cell = td_term_back_cell(&term, r, c);

            if (cell.ch == '.' || cell.ch == ':' || cell.ch == '*' ||
                cell.ch == '#') {
                found_ascii = true;
                break;
            }
        }
    }
    TD_CHECK(found_ascii);
    td_term_close(&term);

    td_plot_pan_zoom(plot, 1, 0, 0.5, false);
    td_plot_pan_zoom(plot, 0, 0, 1.0, true);
    td_sheet_free(plot);

    /* Multi-series plot (g.) */
    plot = td_plot_sheet_build(src, true, err, sizeof(err));
    TD_CHECK(plot != NULL);
    TD_CHECK_INT(((td_plot_meta_t *)plot->meta)->nseries, 2);
    td_sheet_free(plot);

    td_sheet_free(src);
}
