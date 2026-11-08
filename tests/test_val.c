#include <string.h>

#include "test.h"
#include "val.h"

static td_val_t parse(const char *s, td_type_t t)
{
    return td_val_parse(td_slice_from_cstr(s), t);
}

void test_val(void)
{
    char buf[64];
    size_t n;
    td_val_t v;

    TD_CHECK_STR(td_type_sigil(TD_TYPE_ANY), " ");
    TD_CHECK_STR(td_type_sigil(TD_TYPE_STR), "~");
    TD_CHECK_STR(td_type_sigil(TD_TYPE_INT), "#");
    TD_CHECK_STR(td_type_sigil(TD_TYPE_FLOAT), "%");
    TD_CHECK_STR(td_type_sigil(TD_TYPE_CURRENCY), "$");
    TD_CHECK_STR(td_type_sigil(TD_TYPE_DATE), "@");
    TD_CHECK_STR(td_type_sigil(TD_TYPE_ERR), "!");
    TD_CHECK_STR(td_type_sigil(TD_TYPE_NULL), "");

    v = parse("42", TD_TYPE_INT);
    TD_CHECK_INT(v.type, TD_TYPE_INT);
    TD_CHECK_INT(v.as.i, 42);
    TD_CHECK_INT(parse("-7", TD_TYPE_INT).as.i, -7);
    TD_CHECK_INT(parse(" 8 ", TD_TYPE_INT).as.i, 8);
    TD_CHECK_INT(parse("", TD_TYPE_INT).type, TD_TYPE_NULL);
    TD_CHECK_INT(parse("4x", TD_TYPE_INT).type, TD_TYPE_ERR);
    TD_CHECK_INT(parse("999999999999999999999", TD_TYPE_INT).type,
                 TD_TYPE_ERR);

    v = parse("1.5", TD_TYPE_FLOAT);
    TD_CHECK_INT(v.type, TD_TYPE_FLOAT);
    TD_CHECK(v.as.f > 1.49 && v.as.f < 1.51);
    TD_CHECK_INT(parse("nope", TD_TYPE_FLOAT).type, TD_TYPE_ERR);
    TD_CHECK_INT(parse("nan", TD_TYPE_FLOAT).type, TD_TYPE_ERR);

    v = parse("$1,234.50", TD_TYPE_CURRENCY);
    TD_CHECK_INT(v.type, TD_TYPE_CURRENCY);
    TD_CHECK(v.as.f > 1234.49 && v.as.f < 1234.51);
    v = parse("(1,234.50)", TD_TYPE_CURRENCY);
    TD_CHECK(v.as.f < -1234.49 && v.as.f > -1234.51);

    v = parse("2024-01-15", TD_TYPE_DATE);
    TD_CHECK_INT(v.type, TD_TYPE_DATE);
    TD_CHECK_INT(v.as.i, 1705276800);
    v = parse("2024-01-15 12:34:56", TD_TYPE_DATE);
    TD_CHECK_INT(v.as.i, 1705322096);
    v = parse("2024-01-15T12:34:56Z", TD_TYPE_DATE);
    TD_CHECK_INT(v.as.i, 1705322096);
    v = parse("2024-01-15T12:34:56+02:00", TD_TYPE_DATE);
    TD_CHECK_INT(v.as.i, 1705322096 - 7200);
    TD_CHECK_INT(parse("2024-13-01", TD_TYPE_DATE).type, TD_TYPE_ERR);
    TD_CHECK_INT(parse("1705276800", TD_TYPE_DATE).as.i, 1705276800);

    v = parse("hello", TD_TYPE_STR);
    TD_CHECK_INT(v.type, TD_TYPE_STR);
    TD_CHECK_INT(v.as.s.len, 5);
    TD_CHECK_INT(memcmp(v.as.s.data, "hello", 5), 0);
    TD_CHECK_INT(parse("hello", TD_TYPE_ANY).type, TD_TYPE_ANY);

    v = parse("42", TD_TYPE_INT);
    n = td_val_format(v, TD_TYPE_INT, buf, sizeof(buf));
    TD_CHECK_INT(n, 2);
    TD_CHECK_STR(buf, "42");

    v = parse("1.5", TD_TYPE_FLOAT);
    td_val_format(v, TD_TYPE_FLOAT, buf, sizeof(buf));
    TD_CHECK_STR(buf, "1.5");

    v = parse("$1,234.5", TD_TYPE_CURRENCY);
    td_val_format(v, TD_TYPE_CURRENCY, buf, sizeof(buf));
    TD_CHECK_STR(buf, "1234.50");

    v = parse("2024-01-15", TD_TYPE_DATE);
    td_val_format(v, TD_TYPE_DATE, buf, sizeof(buf));
    TD_CHECK_STR(buf, "2024-01-15");

    v = parse("2024-01-15 12:34:56", TD_TYPE_DATE);
    td_val_format(v, TD_TYPE_DATE, buf, sizeof(buf));
    TD_CHECK_STR(buf, "2024-01-15 12:34:56");

    TD_CHECK_INT(td_val_format(TD_VAL_NULL, TD_TYPE_INT, buf, sizeof(buf)), 0);
    TD_CHECK_STR(buf, "");

    v = parse("abcdef", TD_TYPE_STR);
    n = td_val_format(v, TD_TYPE_STR, buf, 4);
    TD_CHECK_INT(n, 6);
    TD_CHECK_STR(buf, "abc");

    TD_CHECK_INT(td_val_cmp(parse("1", TD_TYPE_INT),
                            parse("2", TD_TYPE_INT)), -1);
    TD_CHECK_INT(td_val_cmp(parse("2", TD_TYPE_INT),
                            parse("2", TD_TYPE_INT)), 0);
    TD_CHECK_INT(td_val_cmp(parse("1.5", TD_TYPE_FLOAT),
                            parse("1", TD_TYPE_INT)), 1);
    TD_CHECK_INT(td_val_cmp(parse("a", TD_TYPE_STR),
                            parse("b", TD_TYPE_STR)), -1);
    TD_CHECK_INT(td_val_cmp(parse("ab", TD_TYPE_STR),
                            parse("a", TD_TYPE_STR)), 1);
    TD_CHECK_INT(td_val_cmp(TD_VAL_NULL, parse("1", TD_TYPE_INT)), -1);
    TD_CHECK_INT(td_val_cmp(parse("2024-01-15", TD_TYPE_DATE),
                            parse("2024-01-16", TD_TYPE_DATE)), -1);
}
