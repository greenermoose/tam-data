#include <math.h>
#include <stdio.h>
#include <string.h>

#include "expr.h"
#include "sheet.h"
#include "test.h"

static td_sheet_t *make_expr_sheet(void)
{
    td_sheet_t *s = td_sheet_new("expr_test", "");

    if (!s)
        return NULL;
    td_sheet_add_col(s, "item", TD_TYPE_STR, -20);
    td_sheet_add_col(s, "qty", TD_TYPE_INT, -10);
    td_sheet_add_col(s, "unit price", TD_TYPE_CURRENCY, -10);
    td_sheet_add_col(s, "when", TD_TYPE_DATE, -16);
    td_sheet_add_col(s, "raw_num", TD_TYPE_ANY, -10);

    td_sheet_add_row(s);
    td_sheet_add_row(s);
    td_sheet_add_row(s);

    td_sheet_set_cell(s, 0, 0,
                      td_val_parse(td_slice_from_cstr("  Widget Alpha  "),
                                   TD_TYPE_STR));
    td_sheet_set_cell(s, 0, 1,
                      td_val_parse(td_slice_from_cstr("4"), TD_TYPE_INT));
    td_sheet_set_cell(s, 0, 2,
                      td_val_parse(td_slice_from_cstr("$12.50"),
                                   TD_TYPE_CURRENCY));
    td_sheet_set_cell(s, 0, 3,
                      td_val_parse(td_slice_from_cstr("2026-03-15"),
                                   TD_TYPE_DATE));
    td_sheet_set_cell(s, 0, 4,
                      td_val_parse(td_slice_from_cstr("7"), TD_TYPE_ANY));

    td_sheet_set_cell(s, 1, 0,
                      td_val_parse(td_slice_from_cstr("Gadget"), TD_TYPE_STR));
    td_sheet_set_cell(s, 1, 1,
                      td_val_parse(td_slice_from_cstr("0"), TD_TYPE_INT));
    td_sheet_set_cell(s, 1, 2,
                      td_val_parse(td_slice_from_cstr("5.00"),
                                   TD_TYPE_CURRENCY));
    td_sheet_set_cell(s, 1, 3,
                      td_val_parse(td_slice_from_cstr("2026-03-10"),
                                   TD_TYPE_DATE));
    td_sheet_set_cell(s, 1, 4, TD_VAL_NULL);

    td_sheet_set_cell(s, 2, 0,
                      td_val_parse(td_slice_from_cstr("café"), TD_TYPE_STR));
    td_sheet_set_cell(s, 2, 1,
                      td_val_parse(td_slice_from_cstr("-9"), TD_TYPE_INT));
    td_sheet_set_cell(s, 2, 2, TD_VAL_NULL);
    td_sheet_set_cell(s, 2, 3,
                      td_val_parse(td_slice_from_cstr("2025-12-31"),
                                   TD_TYPE_DATE));
    td_sheet_set_cell(s, 2, 4,
                      td_val_parse(td_slice_from_cstr("2.5"), TD_TYPE_ANY));
    return s;
}

static td_val_t eval_once(const td_sheet_t *s, size_t row, const char *src,
                          td_arena_t *scratch)
{
    char err[256] = "";
    td_expr_t *e = td_expr_compile(s, src, err, sizeof(err));
    td_val_t v;

    TD_CHECK(e != NULL);
    if (!e)
        return TD_VAL_ERR;
    td_arena_reset(scratch);
    v = td_expr_eval(e, s, row, scratch, err, sizeof(err));
    td_expr_free(e);
    return v;
}

void test_expr(void)
{
    td_sheet_t *s = make_expr_sheet();
    td_arena_t scratch;
    char err[256];
    char buf[128];

    TD_CHECK(s != NULL);
    if (!s)
        return;

    td_arena_init(&scratch, 0);

    /* Column references: bare name, braced ${...}, and 0-based $N. */
    td_val_t v = eval_once(s, 0, "qty * ${unit price}", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_CURRENCY);
    TD_CHECK(fabs(v.as.f - 50.0) < 1e-9);

    v = eval_once(s, 0, "$1 + $4", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 11);

    /* Precedence, right-associative **, division returning float, unary. */
    v = eval_once(s, 0, "2 + 3 * 4 ** 2", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 50);

    v = eval_once(s, 0, "2 ** 3 ** 2", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 512);

    v = eval_once(s, 0, "5 / 2", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_FLOAT);
    TD_CHECK(fabs(v.as.f - 2.5) < 1e-9);

    v = eval_once(s, 0, "17 % 5", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 2);

    v = eval_once(s, 0, "-qty + +3", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, -1);

    /* Relational, equality, logical short-circuiting, ternary, NULLs. */
    v = eval_once(s, 0, "qty == 4 && ${unit price} > 10", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 1);

    v = eval_once(s, 1, "qty != 0 && (10 / qty) > 1", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 0);

    v = eval_once(s, 1, "qty == 0 || (10 / qty) > 1", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 1);

    v = eval_once(s, 1, "raw_num == null ? 'missing' : 'present'", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_STR);
    td_val_format(v, TD_TYPE_STR, buf, sizeof(buf));
    TD_CHECK_STR(buf, "missing");

    v = eval_once(s, 2, "${unit price} + 10", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_NULL);

    v = eval_once(s, 1, "10 / qty", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_ERR);

    /* String functions (including UTF-8 len/substr and regex match). */
    v = eval_once(s, 0, "upper(trim(item))", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_STR);
    td_val_format(v, TD_TYPE_STR, buf, sizeof(buf));
    TD_CHECK_STR(buf, "WIDGET ALPHA");

    v = eval_once(s, 0, "lower(replace(trim(item), 'Alpha', 'Beta'))", &scratch);
    td_val_format(v, TD_TYPE_STR, buf, sizeof(buf));
    TD_CHECK_STR(buf, "widget beta");

    v = eval_once(s, 2, "len(item)", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 4);

    v = eval_once(s, 2, "substr(item, 1, 3)", &scratch);
    td_val_format(v, TD_TYPE_STR, buf, sizeof(buf));
    TD_CHECK_STR(buf, "afé");

    v = eval_once(s, 0, "substr(trim(item), -5)", &scratch);
    td_val_format(v, TD_TYPE_STR, buf, sizeof(buf));
    TD_CHECK_STR(buf, "Alpha");

    v = eval_once(s, 0, "contains(item, 'Alpha') && !contains(item, 'Zeta')",
                  &scratch);
    TD_CHECK_INT(v.as.i, 1);

    v = eval_once(s, 0, "match(trim(item), '^Widget [A-Z][a-z]+$')", &scratch);
    TD_CHECK_INT(v.as.i, 1);

    v = eval_once(s, 1, "item + ' #' + qty", &scratch);
    td_val_format(v, TD_TYPE_STR, buf, sizeof(buf));
    TD_CHECK_STR(buf, "Gadget #0");

    /* Math functions. */
    v = eval_once(s, 2, "abs(qty) + sqrt(9) + floor(raw_num) + ceil(raw_num)",
                  &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_FLOAT);
    TD_CHECK(fabs(v.as.f - 17.0) < 1e-9);

    v = eval_once(s, 0, "round(3.14159, 2)", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_FLOAT);
    TD_CHECK(fabs(v.as.f - 3.14) < 1e-9);

    v = eval_once(s, 0, "round(3.6)", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 4);

    v = eval_once(s, 0, "min(qty, raw_num, 2, null)", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 2);

    v = eval_once(s, 0, "max(qty, raw_num, 2)", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_ANY);
    TD_CHECK_INT(td_val_parse(v.as.s, TD_TYPE_INT).as.i, 7);

    v = eval_once(s, 0, "log(1)", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_FLOAT);
    TD_CHECK(fabs(v.as.f - 0.0) < 1e-9);

    v = eval_once(s, 2, "log(qty)", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_ERR);

    v = eval_once(s, 2, "sqrt(qty)", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_ERR);

    /* Date functions. */
    v = eval_once(s, 0, "year(when) * 10000 + month(when) * 100 + day(when)",
                  &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 20260315);

    v = eval_once(s, 0, "datediff(when, '2026-03-10')", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_FLOAT);
    TD_CHECK(fabs(v.as.f - 5.0) < 1e-9);

    v = eval_once(s, 0, "datediff(when, '2026-03-14', 'hours')", &scratch);
    TD_CHECK_INT(v.type, TD_TYPE_FLOAT);
    TD_CHECK(fabs(v.as.f - 24.0) < 1e-9);

    /* Compile-time error handling. */
    TD_CHECK(td_expr_compile(s, "", err, sizeof(err)) == NULL);
    TD_CHECK(td_expr_compile(s, "no_such_col + 1", err, sizeof(err)) == NULL);
    TD_CHECK(td_expr_compile(s, "$99", err, sizeof(err)) == NULL);
    TD_CHECK(td_expr_compile(s, "${missing}", err, sizeof(err)) == NULL);
    TD_CHECK(td_expr_compile(s, "bogus(1)", err, sizeof(err)) == NULL);
    TD_CHECK(td_expr_compile(s, "abs(1, 2)", err, sizeof(err)) == NULL);
    TD_CHECK(td_expr_compile(s, "match(item, '[unclosed')", err, sizeof(err)) ==
             NULL);
    TD_CHECK(td_expr_compile(s, "(1 + 2", err, sizeof(err)) == NULL);
    TD_CHECK(td_expr_compile(s, "1 + 2)", err, sizeof(err)) == NULL);

    td_arena_free(&scratch);
    td_sheet_free(s);
}
