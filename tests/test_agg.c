#include "test.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "agg.h"
#include "cmdlog.h"
#include "csv.h"
#include "tam-data.h"

static bool approx(double a, double b, double tol)
{
    return fabs(a - b) <= tol;
}

static td_sheet_t *load_csv_str(const char *name, const char *csv_text)
{
    td_csv_opts_t opts;
    char err[256] = "";

    td_csv_opts_default(&opts);
    opts.delimiter = ',';
    return td_csv_load_mem(name, "", csv_text, strlen(csv_text), &opts,
                           err, sizeof(err));
}

static bool replay_cb(td_cmd_t cmd, const char *input, void *ctx,
                      char *err, size_t err_cap)
{
    return td_exec_action(cmd, input, (td_exec_ctx_t *)ctx, err, err_cap);
}

static void test_aggregators(void)
{
    td_sheet_t *s = load_csv_str(
        "nums",
        "dept,val,note\n"
        "eng,10,a\n"
        "eng,20,b\n"
        "sales,30,a\n"
        "sales,,\n"
        "ops,40,c\n"
        "ops,bad,d\n");
    td_val_t v;
    char msg[256] = "";
    char err[256] = "";

    TD_CHECK(s != NULL);
    TD_CHECK(td_sheet_set_col_type(s, 1, TD_TYPE_INT));

    v = td_agg_compute_rows(s, 1, NULL, 0, TD_AGG_COUNT);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 4);

    v = td_agg_compute_rows(s, 1, NULL, 0, TD_AGG_NULLS);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 1);

    v = td_agg_compute_rows(s, 0, NULL, 0, TD_AGG_DISTINCT);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 3);

    v = td_agg_compute_rows(s, 1, NULL, 0, TD_AGG_SUM);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 100);

    v = td_agg_compute_rows(s, 1, NULL, 0, TD_AGG_MEAN);
    TD_CHECK_INT(v.type, TD_TYPE_FLOAT);
    TD_CHECK(approx(v.as.f, 25.0, 1e-9));

    v = td_agg_compute_rows(s, 1, NULL, 0, TD_AGG_MEDIAN);
    TD_CHECK_INT(v.type, TD_TYPE_FLOAT);
    TD_CHECK(approx(v.as.f, 25.0, 1e-9));

    v = td_agg_compute_rows(s, 1, NULL, 0, TD_AGG_MIN);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 10);

    v = td_agg_compute_rows(s, 1, NULL, 0, TD_AGG_MAX);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 40);

    /* Sample stdev of [10, 20, 30, 40]: sqrt(500 / 3) = 12.909944487... */
    v = td_agg_compute_rows(s, 1, NULL, 0, TD_AGG_STDEV);
    TD_CHECK_INT(v.type, TD_TYPE_FLOAT);
    TD_CHECK(approx(v.as.f, sqrt(500.0 / 3.0), 1e-6));

    /* Quantiles over [10, 20, 30, 40] (n=4, pos = p*3): */
    v = td_agg_compute_rows(s, 1, NULL, 0, TD_AGG_PCT90);
    TD_CHECK(approx(v.as.f, 37.0, 1e-9));
    v = td_agg_compute_rows(s, 1, NULL, 0, TD_AGG_PCT95);
    TD_CHECK(approx(v.as.f, 38.5, 1e-9));
    v = td_agg_compute_rows(s, 1, NULL, 0, TD_AGG_PCT99);
    TD_CHECK(approx(v.as.f, 39.7, 1e-9));

    /* Persistent aggregator toggling, cache refresh, undo/redo. */
    TD_CHECK(td_sheet_toggle_agg(s, 1, "sum mean", msg, sizeof(msg),
                                 err, sizeof(err)));
    TD_CHECK_INT(s->cols[1].naggs, 2);
    TD_CHECK_INT(s->cols[1].agg_vals[0].as.i, 100);
    TD_CHECK(approx(s->cols[1].agg_vals[1].as.f, 25.0, 1e-9));

    /* Edit cell -> agg_dirty -> refresh updates sum. */
    TD_CHECK(td_sheet_edit_cell(s, 0, 1, "50"));
    TD_CHECK(s->cols[1].agg_dirty);
    td_sheet_refresh_col_aggs(s);
    TD_CHECK(!s->cols[1].agg_dirty);
    TD_CHECK_INT(s->cols[1].agg_vals[0].as.i, 140);

    /* Undo cell edit -> refresh restores sum=100. */
    TD_CHECK(td_sheet_undo(s, NULL, 0));
    td_sheet_refresh_col_aggs(s);
    TD_CHECK_INT(s->cols[1].agg_vals[0].as.i, 100);

    /* Toggle single existing aggregator removes it; undo restores it. */
    TD_CHECK(td_sheet_toggle_agg(s, 1, "sum", msg, sizeof(msg),
                                 err, sizeof(err)));
    TD_CHECK_INT(s->cols[1].naggs, 1);
    TD_CHECK_INT(s->cols[1].aggs[0], TD_AGG_MEAN);
    TD_CHECK(td_sheet_undo(s, NULL, 0));
    TD_CHECK_INT(s->cols[1].naggs, 2);

    /* Clear via "none", invalid name fails cleanly. */
    TD_CHECK(!td_sheet_toggle_agg(s, 1, "bogus", msg, sizeof(msg),
                                  err, sizeof(err)));
    TD_CHECK(strstr(err, "unknown aggregator") != NULL);
    TD_CHECK(td_sheet_toggle_agg(s, 1, "none", msg, sizeof(msg),
                                 err, sizeof(err)));
    TD_CHECK_INT(s->cols[1].naggs, 0);

    /* Single-element stdev -> NULL. */
    size_t single_row = 0;
    v = td_agg_compute_rows(s, 1, &single_row, 1, TD_AGG_STDEV);
    TD_CHECK_INT(v.type, TD_TYPE_NULL);

    td_sheet_free(s);
}

static void test_freq_and_dive(void)
{
    td_sheet_t *s = load_csv_str(
        "orders",
        "region,cat,amount\n"
        "west,books,10.00\n"
        "east,music,20.00\n"
        "west,books,30.00\n"
        "west,games,40.00\n"
        "east,music,50.00\n"
        "north,books,60.00\n");
    td_stack_t st = { 0 };
    char msg[256] = "";
    char err[256] = "";
    td_sheet_t *freq;
    td_sheet_t *freq_keys;
    td_sheet_t *dived;

    TD_CHECK(s != NULL);
    TD_CHECK(td_sheet_set_col_type(s, 2, TD_TYPE_CURRENCY));
    TD_CHECK(td_sheet_toggle_agg(s, 2, "sum", msg, sizeof(msg),
                                 err, sizeof(err)));
    TD_CHECK(td_stack_push(&st, s));

    /* Single-column frequency on region (col 0) includes attached amount_sum. */
    size_t k0 = 0;
    freq = td_agg_freq_sheet(s, &k0, 1, err, sizeof(err));
    TD_CHECK(freq != NULL);
    TD_CHECK_INT(freq->nrows, 3);
    TD_CHECK_INT(freq->ncols, 5); /* region, count, percent, histogram, amount_sum */
    TD_CHECK(td_stack_push(&st, freq));

    /* Sorted descending by count: west (3), east (2), north (1). */
    size_t p0 = freq->row_order[0];
    size_t p1 = freq->row_order[1];
    size_t p2 = freq->row_order[2];
    char buf[64];

    (void)td_val_format(td_sheet_get_cell(freq, p0, 0), TD_TYPE_ANY, buf, sizeof(buf));
    TD_CHECK_STR(buf, "west");
    TD_CHECK_INT(td_sheet_get_cell(freq, p0, 1).as.i, 3);
    TD_CHECK(approx(td_sheet_get_cell(freq, p0, 2).as.f, 50.0, 1e-6));
    (void)td_val_format(td_sheet_get_cell(freq, p0, 3), TD_TYPE_STR, buf, sizeof(buf));
    TD_CHECK_INT((long long)strlen(buf), 20);
    TD_CHECK(approx(td_sheet_get_cell(freq, p0, 4).as.f, 80.0, 1e-6));

    (void)td_val_format(td_sheet_get_cell(freq, p1, 0), TD_TYPE_ANY, buf, sizeof(buf));
    TD_CHECK_STR(buf, "east");
    TD_CHECK_INT(td_sheet_get_cell(freq, p1, 1).as.i, 2);

    (void)td_val_format(td_sheet_get_cell(freq, p2, 0), TD_TYPE_ANY, buf, sizeof(buf));
    TD_CHECK_STR(buf, "north");
    TD_CHECK_INT(td_sheet_get_cell(freq, p2, 1).as.i, 1);

    /* Re-sort freq ascending by region; dive on row 0 ("east") still gets 2 east rows! */
    TD_CHECK(td_sheet_sort_col(freq, 0, false));
    freq->cursor_row = 0;
    dived = td_agg_dive_row(freq, &st, err, sizeof(err));
    TD_CHECK(dived != NULL);
    TD_CHECK_INT(dived->nrows, 2);
    (void)td_val_format(td_sheet_get_cell(dived, dived->row_order[0], 0),
                        TD_TYPE_ANY, buf, sizeof(buf));
    TD_CHECK_STR(buf, "east");
    td_sheet_free(dived);

    /* Multi-key frequency on (region, cat). */
    size_t keys[2] = { 0, 1 };
    freq_keys = td_agg_freq_sheet(s, keys, 2, err, sizeof(err));
    TD_CHECK(freq_keys != NULL);
    TD_CHECK_INT(freq_keys->nrows, 4); /* (west,books), (east,music), (west,games), (north,books) */
    td_sheet_free(freq_keys);

    /* Popping source sheet causes clean error when diving from freq. */
    td_sheet_t *top = td_stack_pop(&st); /* freq */
    td_sheet_free(td_stack_pop(&st));    /* s */
    TD_CHECK(td_stack_push(&st, top));
    TD_CHECK(td_agg_dive_row(top, &st, err, sizeof(err)) == NULL);
    TD_CHECK(strstr(err, "no longer open") != NULL);

    td_stack_free(&st);
}

static void test_describe_pivot_melt(void)
{
    td_sheet_t *s = load_csv_str(
        "sales",
        "region,quarter,revenue\n"
        "west,Q1,100\n"
        "west,Q2,200\n"
        "east,Q1,50\n"
        "east,Q2,150\n"
        "west,Q1,50\n");
    td_stack_t st = { 0 };
    char msg[256] = "";
    char err[256] = "";
    td_sheet_t *desc;
    td_sheet_t *dived_freq;
    td_sheet_t *piv;
    td_sheet_t *melt;
    char buf[64];

    TD_CHECK(s != NULL);
    TD_CHECK(td_sheet_set_col_type(s, 2, TD_TYPE_INT));
    TD_CHECK(td_stack_push(&st, s));

    /* Describe sheet (I) and Enter dive into frequency table. */
    desc = td_agg_describe_sheet(s, err, sizeof(err));
    TD_CHECK(desc != NULL);
    TD_CHECK_INT(desc->nrows, 3);
    TD_CHECK_INT(desc->ncols, 12);
    /* Row 2 = revenue: count=5, nulls=0, errors=0, distinct=4, sum=550, mean=110 */
    TD_CHECK_INT(td_sheet_get_cell(desc, 2, 2).as.i, 5);
    TD_CHECK_INT(td_sheet_get_cell(desc, 2, 5).as.i, 4);
    TD_CHECK(approx(td_sheet_get_cell(desc, 2, 8).as.f, 550.0, 1e-9));
    TD_CHECK(approx(td_sheet_get_cell(desc, 2, 9).as.f, 110.0, 1e-9));

    desc->cursor_row = 0;
    dived_freq = td_agg_dive_row(desc, &st, err, sizeof(err));
    TD_CHECK(dived_freq != NULL);
    TD_CHECK_INT(dived_freq->nrows, 2); /* west, east */
    td_sheet_free(dived_freq);
    td_sheet_free(desc);

    /* Pivot fails without pinned key column. */
    TD_CHECK(td_agg_pivot_sheet(s, 1, err, sizeof(err)) == NULL);
    TD_CHECK(strstr(err, "pin key") != NULL);

    /* Pin region (!), attach sum to revenue (+ sum), pivot on quarter (col 1). */
    td_sheet_toggle_key_col(s, 0);
    TD_CHECK(td_sheet_toggle_agg(s, 2, "sum", msg, sizeof(msg),
                                 err, sizeof(err)));
    piv = td_agg_pivot_sheet(s, 1, err, sizeof(err));
    TD_CHECK(piv != NULL);
    TD_CHECK_INT(piv->nrows, 2); /* west, east */
    TD_CHECK_INT(piv->ncols, 3); /* region, Q1, Q2 */
    (void)td_val_format(td_sheet_get_cell(piv, 0, 0), TD_TYPE_ANY, buf, sizeof(buf));
    TD_CHECK_STR(buf, "west");
    TD_CHECK_INT(td_sheet_get_cell(piv, 0, 1).as.i, 150);
    TD_CHECK_INT(td_sheet_get_cell(piv, 0, 2).as.i, 200);
    TD_CHECK_INT(td_sheet_get_cell(piv, 1, 1).as.i, 50);
    TD_CHECK_INT(td_sheet_get_cell(piv, 1, 2).as.i, 150);

    /* Melt the pivoted table back to long form: 2 rows * 2 non-key cols = 4 rows. */
    melt = td_agg_melt_sheet(piv, err, sizeof(err));
    TD_CHECK(melt != NULL);
    TD_CHECK_INT(melt->nrows, 4);
    TD_CHECK_INT(melt->ncols, 3); /* region, variable, value */
    (void)td_val_format(td_sheet_get_cell(melt, 0, 1), TD_TYPE_STR, buf, sizeof(buf));
    TD_CHECK_STR(buf, "Q1");
    TD_CHECK_INT(td_sheet_get_cell(melt, 0, 2).as.i, 150);

    td_sheet_free(melt);
    td_sheet_free(piv);
    td_stack_free(&st);
}

static void test_joins(void)
{
    td_sheet_t *left = load_csv_str(
        "left",
        "id,score,tag\n"
        "1,10,a\n"
        "2,20,b\n"
        "3,30,c\n");
    td_sheet_t *right = load_csv_str(
        "right",
        "id,score,note\n"
        "2,20,same\n"
        "3,99,changed\n"
        "4,40,only_right\n");
    char err[256] = "";
    td_sheet_t *j;

    TD_CHECK(left != NULL && right != NULL);
    TD_CHECK(td_sheet_set_col_type(left, 0, TD_TYPE_INT));
    TD_CHECK(td_sheet_set_col_type(left, 1, TD_TYPE_INT));
    TD_CHECK(td_sheet_set_col_type(right, 0, TD_TYPE_INT));
    TD_CHECK(td_sheet_set_col_type(right, 1, TD_TYPE_INT));

    /* Append does not require keys. */
    j = td_agg_join_sheets(left, right, TD_JOIN_APPEND, err, sizeof(err));
    TD_CHECK(j != NULL);
    TD_CHECK_INT(j->nrows, 6);
    TD_CHECK_INT(j->ncols, 4); /* id, score, tag, note */
    td_sheet_free(j);

    /* Keyed joins fail without pinned keys. */
    TD_CHECK(td_agg_join_sheets(left, right, TD_JOIN_INNER, err, sizeof(err)) == NULL);

    td_sheet_toggle_key_col(left, 0);
    td_sheet_toggle_key_col(right, 0);

    /* Inner: ids 2, 3. */
    j = td_agg_join_sheets(left, right, TD_JOIN_INNER, err, sizeof(err));
    TD_CHECK(j != NULL);
    TD_CHECK_INT(j->nrows, 2);
    TD_CHECK_INT(j->ncols, 5); /* id, score, tag, score_2, note */
    TD_CHECK_INT(td_sheet_get_cell(j, 0, 0).as.i, 2);
    TD_CHECK_INT(td_sheet_get_cell(j, 1, 0).as.i, 3);
    td_sheet_free(j);

    /* Outer (left): ids 1, 2, 3. */
    j = td_agg_join_sheets(left, right, TD_JOIN_OUTER, err, sizeof(err));
    TD_CHECK(j != NULL);
    TD_CHECK_INT(j->nrows, 3);
    TD_CHECK_INT(td_sheet_get_cell(j, 0, 4).type, TD_TYPE_NULL);
    td_sheet_free(j);

    /* Full: ids 1, 2, 3, 4. */
    j = td_agg_join_sheets(left, right, TD_JOIN_FULL, err, sizeof(err));
    TD_CHECK(j != NULL);
    TD_CHECK_INT(j->nrows, 4);
    TD_CHECK_INT(td_sheet_get_cell(j, 3, 0).as.i, 4);
    TD_CHECK_INT(td_sheet_get_cell(j, 3, 1).type, TD_TYPE_NULL);
    td_sheet_free(j);

    /* Diff: id 1 (left only), id 3 (score 30 vs 99), id 4 (right only); id 2 excluded! */
    j = td_agg_join_sheets(left, right, TD_JOIN_DIFF, err, sizeof(err));
    TD_CHECK(j != NULL);
    TD_CHECK_INT(j->nrows, 3);
    TD_CHECK_INT(td_sheet_get_cell(j, 0, 0).as.i, 1);
    TD_CHECK_INT(td_sheet_get_cell(j, 1, 0).as.i, 3);
    TD_CHECK_INT(td_sheet_get_cell(j, 2, 0).as.i, 4);
    td_sheet_free(j);

    /* Extend: left rows (1, 2, 3) with first match from right. */
    j = td_agg_join_sheets(left, right, TD_JOIN_EXTEND, err, sizeof(err));
    TD_CHECK(j != NULL);
    TD_CHECK_INT(j->nrows, 3);
    td_sheet_free(j);

    td_sheet_free(left);
    td_sheet_free(right);
}

static void test_phase4_cmdlog_replay(void)
{
    td_stack_t st = { 0 };
    td_clipboard_t clip;
    td_cmdlog_t log;
    td_exec_ctx_t ctx;
    char msg[256] = "";
    char err[256] = "";
    bool quit = false;
    td_sheet_t *s = td_demo_sheet();
    td_sheet_t *cl;

    TD_CHECK(s != NULL);
    TD_CHECK(td_stack_push(&st, s));
    td_clipboard_init(&clip);
    td_cmdlog_init(&log);
    memset(&ctx, 0, sizeof(ctx));
    ctx.stack = &st;
    ctx.clip = &clip;
    ctx.cmdlog = &log;
    ctx.msg = msg;
    ctx.msg_cap = sizeof(msg);
    ctx.quit = &quit;

    TD_CHECK(td_exec_action(TD_CMD_RIGHT, NULL, &ctx, err, sizeof(err)));
    TD_CHECK(td_exec_action(TD_CMD_AGGREGATOR, "sum", &ctx, err, sizeof(err)));
    TD_CHECK(td_exec_action(TD_CMD_AGGREGATOR_ONE, "mean", &ctx, err, sizeof(err)));
    TD_CHECK(strstr(msg, "mean") != NULL);
    TD_CHECK(td_exec_action(TD_CMD_FREQ_COL, NULL, &ctx, err, sizeof(err)));
    TD_CHECK_STR(td_stack_active(&st)->name, "demo_qty_freq");
    TD_CHECK(td_exec_action(TD_CMD_DIVE_ROW, NULL, &ctx, err, sizeof(err)));
    TD_CHECK_INT(st.len, 3);
    TD_CHECK(td_exec_action(TD_CMD_POP_SHEET, NULL, &ctx, err, sizeof(err)));
    TD_CHECK(td_exec_action(TD_CMD_POP_SHEET, NULL, &ctx, err, sizeof(err)));
    TD_CHECK_INT(st.len, 1);
    TD_CHECK(td_exec_action(TD_CMD_DESCRIBE, NULL, &ctx, err, sizeof(err)));
    TD_CHECK_INT(td_stack_active(&st)->nrows, 6);
    TD_CHECK(td_exec_action(TD_CMD_POP_SHEET, NULL, &ctx, err, sizeof(err)));
    TD_CHECK(td_exec_action(TD_CMD_MELT, NULL, &ctx, err, sizeof(err)));
    TD_CHECK_INT(td_stack_active(&st)->nrows, 40); /* 8 rows * 5 non-key cols */
    TD_CHECK(td_exec_action(TD_CMD_POP_SHEET, NULL, &ctx, err, sizeof(err)));

    cl = td_cmdlog_to_sheet(&log);
    TD_CHECK(cl != NULL);

    td_stack_free(&st);
    td_clipboard_free(&clip);
    td_cmdlog_free(&log);

    /* Replay recorded Phase 4 command log onto fresh demo sheet. */
    td_cmdlog_init(&log);
    td_clipboard_init(&clip);
    s = td_demo_sheet();
    TD_CHECK(td_stack_push(&st, s));
    quit = false;
    ctx.stack = &st;
    ctx.clip = &clip;
    ctx.cmdlog = &log;

    TD_CHECK(td_cmdlog_replay_sheet(cl, &st, replay_cb, &ctx, &quit,
                                    err, sizeof(err)));
    TD_CHECK_INT(st.len, 1);
    TD_CHECK_INT(s->cols[1].naggs, 1);

    td_sheet_free(cl);
    td_stack_free(&st);
    td_clipboard_free(&clip);
    td_cmdlog_free(&log);
}

void test_agg(void)
{
    test_aggregators();
    test_freq_and_dive();
    test_describe_pivot_melt();
    test_joins();
    test_phase4_cmdlog_replay();
}
