#include "val.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

td_slice_t td_slice_from_cstr(const char *s)
{
    td_slice_t out = { NULL, 0 };

    if (s) {
        size_t n = strlen(s);

        out.data = s;
        out.len = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
    }
    return out;
}

td_slice_t td_slice_trim(td_slice_t s)
{
    size_t begin = 0;
    size_t end = s.len;

    if (!s.data)
        return (td_slice_t){ NULL, 0 };

    while (begin < end && isspace((unsigned char)s.data[begin]))
        begin++;
    while (end > begin && isspace((unsigned char)s.data[end - 1]))
        end--;
    return (td_slice_t){ s.data + begin, (uint32_t)(end - begin) };
}

bool td_slice_eq(td_slice_t a, td_slice_t b)
{
    if (a.len != b.len)
        return false;
    if (a.len == 0)
        return true;
    if (!a.data || !b.data)
        return false;
    return memcmp(a.data, b.data, a.len) == 0;
}

int td_slice_cmp(td_slice_t a, td_slice_t b)
{
    size_t n = a.len < b.len ? a.len : b.len;
    int rc = 0;

    if (n && a.data && b.data)
        rc = memcmp(a.data, b.data, n);
    if (rc != 0)
        return rc < 0 ? -1 : 1;
    if (a.len == b.len)
        return 0;
    return a.len < b.len ? -1 : 1;
}

const char *td_type_sigil(td_type_t t)
{
    switch (t) {
    case TD_TYPE_ANY:      return " ";
    case TD_TYPE_STR:      return "~";
    case TD_TYPE_INT:      return "#";
    case TD_TYPE_FLOAT:    return "%";
    case TD_TYPE_CURRENCY: return "$";
    case TD_TYPE_DATE:     return "@";
    case TD_TYPE_NULL:     return "";
    case TD_TYPE_ERR:      return "!";
    }
    return "";
}

const char *td_type_name(td_type_t t)
{
    switch (t) {
    case TD_TYPE_ANY:      return "any";
    case TD_TYPE_STR:      return "str";
    case TD_TYPE_INT:      return "int";
    case TD_TYPE_FLOAT:    return "float";
    case TD_TYPE_CURRENCY: return "currency";
    case TD_TYPE_DATE:     return "date";
    case TD_TYPE_NULL:     return "null";
    case TD_TYPE_ERR:      return "error";
    }
    return "unknown";
}

bool td_type_is_numeric(td_type_t t)
{
    return t == TD_TYPE_INT || t == TD_TYPE_FLOAT ||
           t == TD_TYPE_CURRENCY || t == TD_TYPE_DATE;
}

/* Copy src into buf, always NUL-terminating when cap > 0. Returns the number
 * of bytes the complete value needs, excluding the NUL. */
static size_t td_copy_out(char *buf, size_t cap, const char *src, size_t len)
{
    if (cap > 0) {
        size_t n = len < cap - 1 ? len : cap - 1;

        if (n)
            memcpy(buf, src, n);
        buf[n] = '\0';
    }
    return len;
}

static bool td_parse_int64(td_slice_t raw, int64_t *out)
{
    td_slice_t s = td_slice_trim(raw);
    char tmp[64];
    char *end = NULL;
    long long v;

    if (s.len == 0 || s.len >= sizeof(tmp))
        return false;
    memcpy(tmp, s.data, s.len);
    tmp[s.len] = '\0';

    errno = 0;
    v = strtoll(tmp, &end, 10);
    if (errno == ERANGE || end == tmp || *end != '\0')
        return false;
    *out = (int64_t)v;
    return true;
}

static bool td_parse_double(td_slice_t raw, bool currency, double *out)
{
    td_slice_t s = td_slice_trim(raw);
    char tmp[128];
    size_t n = 0;
    bool negative = false;
    char *end = NULL;
    double v;

    if (s.len == 0)
        return false;

    if (currency) {
        /* Allow $1,234.50 and accounting-style (1,234.50). */
        if (s.len >= 2 && s.data[0] == '(' && s.data[s.len - 1] == ')') {
            negative = true;
            s.data++;
            s.len -= 2;
            s = td_slice_trim(s);
        }
        while (s.len && (s.data[0] == '$' || s.data[0] == ' '))
            s.data++, s.len--;
        while (s.len && (s.data[s.len - 1] == '$' || s.data[s.len - 1] == ' '))
            s.len--;
    }

    for (uint32_t i = 0; i < s.len; i++) {
        if (s.data[i] == ',' && currency)
            continue;
        if (n + 1 >= sizeof(tmp))
            return false;
        tmp[n++] = s.data[i];
    }
    tmp[n] = '\0';
    if (n == 0)
        return false;

    errno = 0;
    v = strtod(tmp, &end);
    if (errno == ERANGE || end == tmp || *end != '\0' || !isfinite(v))
        return false;
    *out = negative ? -v : v;
    return true;
}

/* Days since 1970-01-01 for a proleptic Gregorian date. */
static int64_t td_days_from_civil(int64_t y, unsigned m, unsigned d)
{
    int64_t era;
    unsigned yoe, mp, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    mp = m > 2 ? m - 3 : m + 9;
    doy = (153u * mp + 2u) / 5u + d - 1u;
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static bool td_read_digits(const char **pp, const char *end, int count, int *out)
{
    const char *p = *pp;
    int v = 0;

    if (end - p < count)
        return false;
    for (int i = 0; i < count; i++) {
        if (!isdigit((unsigned char)p[i]))
            return false;
        v = v * 10 + (p[i] - '0');
    }
    *pp = p + count;
    *out = v;
    return true;
}

static bool td_parse_date(td_slice_t raw, int64_t *out)
{
    td_slice_t s = td_slice_trim(raw);
    const char *p;
    const char *end;
    int year, month, day, hour = 0, minute = 0, second = 0;
    int offset = 0;
    int64_t epoch;

    if (s.len == 0)
        return false;

    /* Bare integer: already epoch seconds. */
    if (td_parse_int64(s, out))
        return true;

    p = s.data;
    end = s.data + s.len;

    if (!td_read_digits(&p, end, 4, &year))
        return false;
    if (p >= end || *p != '-')
        return false;
    p++;
    if (!td_read_digits(&p, end, 2, &month))
        return false;
    if (p >= end || *p != '-')
        return false;
    p++;
    if (!td_read_digits(&p, end, 2, &day))
        return false;

    if (month < 1 || month > 12 || day < 1 || day > 31)
        return false;

    if (p < end && (*p == 'T' || *p == ' ')) {
        p++;
        if (!td_read_digits(&p, end, 2, &hour))
            return false;
        if (p >= end || *p != ':')
            return false;
        p++;
        if (!td_read_digits(&p, end, 2, &minute))
            return false;
        if (p < end && *p == ':') {
            p++;
            if (!td_read_digits(&p, end, 2, &second))
                return false;
            if (p < end && *p == '.') {
                p++;
                while (p < end && isdigit((unsigned char)*p))
                    p++;
            }
        }

        if (p < end && (*p == 'Z' || *p == 'z')) {
            p++;
        } else if (p < end && (*p == '+' || *p == '-')) {
            int sign = *p == '-' ? -1 : 1;
            int oh, om = 0;

            p++;
            if (!td_read_digits(&p, end, 2, &oh))
                return false;
            if (p < end && *p == ':')
                p++;
            if (p < end) {
                if (!td_read_digits(&p, end, 2, &om))
                    return false;
            }
            offset = sign * (oh * 3600 + om * 60);
        }
    }

    if (p != end)
        return false;
    if (hour > 23 || minute > 59 || second > 60)
        return false;

    epoch = td_days_from_civil(year, (unsigned)month, (unsigned)day) * 86400;
    epoch += hour * 3600 + minute * 60 + second - offset;
    *out = epoch;
    return true;
}

td_val_t td_val_parse(td_slice_t raw, td_type_t target_type)
{
    td_val_t v = TD_VAL_NULL;

    if (!raw.data || raw.len == 0)
        return TD_VAL_NULL;

    switch (target_type) {
    case TD_TYPE_NULL:
        return TD_VAL_NULL;
    case TD_TYPE_ERR:
        return TD_VAL_ERR;
    case TD_TYPE_ANY:
        v.type = TD_TYPE_ANY;
        v.as.s = raw;
        return v;
    case TD_TYPE_STR:
        v.type = TD_TYPE_STR;
        v.as.s = raw;
        return v;
    case TD_TYPE_INT:
        if (td_parse_int64(raw, &v.as.i)) {
            v.type = TD_TYPE_INT;
            return v;
        }
        return TD_VAL_ERR;
    case TD_TYPE_FLOAT:
        if (td_parse_double(raw, false, &v.as.f)) {
            v.type = TD_TYPE_FLOAT;
            return v;
        }
        return TD_VAL_ERR;
    case TD_TYPE_CURRENCY:
        if (td_parse_double(raw, true, &v.as.f)) {
            v.type = TD_TYPE_CURRENCY;
            return v;
        }
        return TD_VAL_ERR;
    case TD_TYPE_DATE:
        if (td_parse_date(raw, &v.as.i)) {
            v.type = TD_TYPE_DATE;
            return v;
        }
        return TD_VAL_ERR;
    }
    return TD_VAL_ERR;
}

static size_t td_format_date(int64_t epoch, char *buf, size_t cap)
{
    struct tm tm;
    time_t t = (time_t)epoch;
    char tmp[40];

    if (!gmtime_r(&t, &tm))
        return td_copy_out(buf, cap, "#DATE", 5);

    if (tm.tm_hour == 0 && tm.tm_min == 0 && tm.tm_sec == 0) {
        int n = snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return td_copy_out(buf, cap, tmp, n > 0 ? (size_t)n : 0);
    }

    int n = snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d %02d:%02d:%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
    return td_copy_out(buf, cap, tmp, n > 0 ? (size_t)n : 0);
}

size_t td_val_format(td_val_t v, td_type_t col_type, char *buf, size_t cap)
{
    char tmp[64];
    int n = 0;

    if (cap > 0)
        buf[0] = '\0';

    if (v.type == TD_TYPE_ANY && col_type != TD_TYPE_ANY) {
        v = td_val_parse(v.as.s, col_type);
    }

    switch (v.type) {
    case TD_TYPE_NULL:
        return 0;
    case TD_TYPE_ANY:
    case TD_TYPE_STR:
        if (!v.as.s.data)
            return 0;
        return td_copy_out(buf, cap, v.as.s.data, v.as.s.len);
    case TD_TYPE_INT:
        n = snprintf(tmp, sizeof(tmp), "%" PRId64, v.as.i);
        break;
    case TD_TYPE_FLOAT:
        n = snprintf(tmp, sizeof(tmp), "%.15g", v.as.f);
        break;
    case TD_TYPE_CURRENCY:
        n = snprintf(tmp, sizeof(tmp), "%.2f", v.as.f);
        break;
    case TD_TYPE_DATE:
        return td_format_date(v.as.i, buf, cap);
    case TD_TYPE_ERR:
        if (v.as.s.data && v.as.s.len)
            return td_copy_out(buf, cap, v.as.s.data, v.as.s.len);
        return td_copy_out(buf, cap, "#ERR", 4);
    }

    if (n < 0)
        return 0;
    return td_copy_out(buf, cap, tmp, (size_t)n);
}

static int td_type_rank(td_type_t t)
{
    switch (t) {
    case TD_TYPE_NULL:     return 0;
    case TD_TYPE_INT:
    case TD_TYPE_FLOAT:
    case TD_TYPE_CURRENCY:
    case TD_TYPE_DATE:     return 1;
    case TD_TYPE_ANY:
    case TD_TYPE_STR:      return 2;
    case TD_TYPE_ERR:      return 3;
    }
    return 4;
}

static long double td_val_number(td_val_t v)
{
    switch (v.type) {
    case TD_TYPE_INT:
    case TD_TYPE_DATE:
        return (long double)v.as.i;
    case TD_TYPE_FLOAT:
    case TD_TYPE_CURRENCY:
        return (long double)v.as.f;
    default:
        return 0.0L;
    }
}

int td_val_cmp(td_val_t a, td_val_t b)
{
    int ra = td_type_rank(a.type);
    int rb = td_type_rank(b.type);

    if (ra != rb)
        return ra < rb ? -1 : 1;

    switch (a.type) {
    case TD_TYPE_NULL:
        return 0;
    case TD_TYPE_INT:
    case TD_TYPE_DATE:
        if (b.type == TD_TYPE_INT || b.type == TD_TYPE_DATE)
            return a.as.i == b.as.i ? 0 : (a.as.i < b.as.i ? -1 : 1);
        break;
    case TD_TYPE_FLOAT:
    case TD_TYPE_CURRENCY:
        if (td_type_is_numeric(b.type))
            break;
        break;
    case TD_TYPE_ANY:
    case TD_TYPE_STR:
        return td_slice_cmp(a.as.s, b.as.s);
    case TD_TYPE_ERR:
        return td_slice_cmp(a.as.s, b.as.s);
    }

    long double na = td_val_number(a);
    long double nb = td_val_number(b);

    if (na == nb)
        return 0;
    return na < nb ? -1 : 1;
}
