#include "agg.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TD_HIST_MAX_WIDTH 20

typedef struct {
    td_agg_kind_t kind;
    const char *name;
    const char *alias;
    bool numeric_only;
} td_agg_desc_t;

static const td_agg_desc_t td_agg_table[TD_AGG_KIND_COUNT] = {
    { TD_AGG_COUNT,    "count",    NULL,  false },
    { TD_AGG_DISTINCT, "distinct", NULL,  false },
    { TD_AGG_NULLS,    "nulls",    NULL,  false },
    { TD_AGG_SUM,      "sum",      NULL,  true  },
    { TD_AGG_MEAN,     "mean",     "avg", true  },
    { TD_AGG_MEDIAN,   "median",   NULL,  true  },
    { TD_AGG_MIN,      "min",      NULL,  false },
    { TD_AGG_MAX,      "max",      NULL,  false },
    { TD_AGG_STDEV,    "stdev",    NULL,  true  },
    { TD_AGG_PCT90,    "pct90",    NULL,  true  },
    { TD_AGG_PCT95,    "pct95",    NULL,  true  },
    { TD_AGG_PCT99,    "pct99",    NULL,  true  }
};

static void td_set_err(char *err, size_t err_cap, const char *msg)
{
    if (err && err_cap > 0)
        snprintf(err, err_cap, "%s", msg ? msg : "");
}

static bool td_strieq(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

bool td_agg_parse(const char *name, td_agg_kind_t *out)
{
    if (!name || !name[0])
        return false;
    for (size_t i = 0; i < TD_AGG_KIND_COUNT; i++) {
        if (td_strieq(name, td_agg_table[i].name) ||
            (td_agg_table[i].alias && td_strieq(name, td_agg_table[i].alias))) {
            if (out)
                *out = td_agg_table[i].kind;
            return true;
        }
    }
    return false;
}

const char *td_agg_name(td_agg_kind_t kind)
{
    if ((size_t)kind < TD_AGG_KIND_COUNT)
        return td_agg_table[kind].name;
    return "unknown";
}

bool td_agg_is_numeric_only(td_agg_kind_t agg)
{
    if ((size_t)agg < TD_AGG_KIND_COUNT)
        return td_agg_table[agg].numeric_only;
    return false;
}

td_type_t td_agg_result_type(td_agg_kind_t agg, td_type_t col_type)
{
    switch (agg) {
    case TD_AGG_COUNT:
    case TD_AGG_DISTINCT:
    case TD_AGG_NULLS:
        return TD_TYPE_INT;
    case TD_AGG_SUM:
        if (col_type == TD_TYPE_INT)
            return TD_TYPE_INT;
        if (col_type == TD_TYPE_CURRENCY)
            return TD_TYPE_CURRENCY;
        return TD_TYPE_FLOAT;
    case TD_AGG_MEAN:
    case TD_AGG_MEDIAN:
    case TD_AGG_PCT90:
    case TD_AGG_PCT95:
    case TD_AGG_PCT99:
        if (col_type == TD_TYPE_CURRENCY)
            return TD_TYPE_CURRENCY;
        if (col_type == TD_TYPE_DATE)
            return TD_TYPE_DATE;
        return TD_TYPE_FLOAT;
    case TD_AGG_STDEV:
        return TD_TYPE_FLOAT;
    case TD_AGG_MIN:
    case TD_AGG_MAX:
        return col_type;
    case TD_AGG_KIND_COUNT:
        break;
    }
    return TD_TYPE_ANY;
}

bool td_join_parse(const char *name, td_join_kind_t *out)
{
    static const struct {
        const char *name;
        td_join_kind_t kind;
    } modes[] = {
        { "inner",  TD_JOIN_INNER  },
        { "outer",  TD_JOIN_OUTER  },
        { "full",   TD_JOIN_FULL   },
        { "diff",   TD_JOIN_DIFF   },
        { "append", TD_JOIN_APPEND },
        { "extend", TD_JOIN_EXTEND }
    };

    if (!name || !name[0])
        return false;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (td_strieq(name, modes[i].name)) {
            if (out)
                *out = modes[i].kind;
            return true;
        }
    }
    return false;
}

const char *td_join_name(td_join_kind_t kind)
{
    switch (kind) {
    case TD_JOIN_INNER:  return "inner";
    case TD_JOIN_OUTER:  return "outer";
    case TD_JOIN_FULL:   return "full";
    case TD_JOIN_DIFF:   return "diff";
    case TD_JOIN_APPEND: return "append";
    case TD_JOIN_EXTEND: return "extend";
    }
    return "unknown";
}

/* ---- Shared Arena & Value Cloning Helpers ---- */

static td_val_t td_clone_val(td_arena_t *arena, td_val_t v, bool *ok)
{
    *ok = true;
    if ((v.type == TD_TYPE_ANY || v.type == TD_TYPE_STR ||
         v.type == TD_TYPE_ERR) && v.as.s.data) {
        char *copy = td_arena_strndup(arena, v.as.s.data, v.as.s.len);

        if (!copy) {
            *ok = false;
            return TD_VAL_ERR;
        }
        v.as.s.data = copy;
    }
    return v;
}

static bool td_set_cloned_cell(td_sheet_t *dst, size_t row, size_t col,
                               td_val_t v)
{
    bool ok = true;
    td_val_t cloned = td_clone_val(&dst->arena, v, &ok);

    if (!ok)
        return false;
    return td_sheet_set_cell(dst, row, col, cloned);
}

static bool td_set_str_cell(td_sheet_t *dst, size_t row, size_t col,
                            const char *text)
{
    char *copy;
    td_val_t v;

    if (!text)
        return td_sheet_set_cell(dst, row, col, TD_VAL_NULL);
    copy = td_arena_strdup(&dst->arena, text);
    if (!copy)
        return false;
    v.type = TD_TYPE_STR;
    v.as.s = td_slice_from_cstr(copy);
    return td_sheet_set_cell(dst, row, col, v);
}

static bool td_set_formatted_str_cell(td_sheet_t *dst, size_t row, size_t col,
                                      td_val_t v, td_type_t src_type)
{
    char stack_buf[256];
    size_t n;

    if (v.type == TD_TYPE_NULL)
        return td_sheet_set_cell(dst, row, col, TD_VAL_NULL);
    n = td_val_format(v, src_type, stack_buf, sizeof(stack_buf));
    if (n < sizeof(stack_buf))
        return td_set_str_cell(dst, row, col, stack_buf);

    char *dyn = malloc(n + 1);
    bool ok;

    if (!dyn)
        return false;
    (void)td_val_format(v, src_type, dyn, n + 1);
    ok = td_set_str_cell(dst, row, col, dyn);
    free(dyn);
    return ok;
}

static char *td_format_val_alloc(td_val_t v, td_type_t type,
                                 const char *null_fallback)
{
    char stack_buf[256];
    size_t n;
    char *out;

    if (v.type == TD_TYPE_NULL && null_fallback) {
        size_t len = strlen(null_fallback);

        out = malloc(len + 1);
        if (out)
            memcpy(out, null_fallback, len + 1);
        return out;
    }

    n = td_val_format(v, type, stack_buf, sizeof(stack_buf));
    out = malloc(n + 1);
    if (!out)
        return NULL;
    if (n < sizeof(stack_buf))
        memcpy(out, stack_buf, n + 1);
    else
        (void)td_val_format(v, type, out, n + 1);
    return out;
}

static char *td_make_derived_name(const char *base, const char *mid,
                                  const char *suffix)
{
    const char *b = (base && base[0]) ? base : "sheet";
    const char *m = mid ? mid : "";
    const char *s = suffix ? suffix : "";
    size_t len = strlen(b) + strlen(m) + strlen(s) + 4;
    char *out = malloc(len);

    if (!out)
        return NULL;
    if (m[0])
        snprintf(out, len, "%s_%s%s", b, m, s);
    else
        snprintf(out, len, "%s%s", b, s);
    return out;
}

static bool td_col_name_used(const td_sheet_t *s, const char *name)
{
    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].name && strcmp(s->cols[c].name, name) == 0)
            return true;
    }
    return false;
}

static char *td_unique_col_name(const td_sheet_t *s, const char *base)
{
    const char *b = (base && base[0]) ? base : "col";
    size_t cap = strlen(b) + 24;
    char *buf = malloc(cap);

    if (!buf)
        return NULL;
    snprintf(buf, cap, "%s", b);
    for (size_t n = 2; td_col_name_used(s, buf); n++)
        snprintf(buf, cap, "%s_%zu", b, n);
    return buf;
}

static size_t td_collect_cols(const td_sheet_t *s, bool keys_only,
                              bool nonkeys_only, size_t **out_cols)
{
    size_t *cols;
    size_t n = 0;

    *out_cols = NULL;
    if (!s || s->ncols == 0)
        return 0;
    cols = malloc(s->ncols * sizeof(*cols));
    if (!cols)
        return 0;
    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].width == 0)
            continue;
        if (keys_only && !s->cols[c].is_key)
            continue;
        if (nonkeys_only && s->cols[c].is_key)
            continue;
        cols[n++] = c;
    }
    if (n == 0) {
        free(cols);
        return 0;
    }
    *out_cols = cols;
    return n;
}

/* ---- Canonical Value Hashing & Grouping Hash Table ---- */

static uint64_t td_fnv1a_step(uint64_t h, const void *data, size_t len)
{
    const unsigned char *p = data;

    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static uint64_t td_val_hash(td_val_t v)
{
    uint64_t h = UINT64_C(14695981039346656037);

    switch (v.type) {
    case TD_TYPE_NULL:
        return UINT64_C(0x9e3779b97f4a7c15);
    case TD_TYPE_INT:
    case TD_TYPE_DATE:
    case TD_TYPE_FLOAT:
    case TD_TYPE_CURRENCY: {
        uint8_t tag = 1;
        long double ld = (v.type == TD_TYPE_INT || v.type == TD_TYPE_DATE)
                             ? (long double)v.as.i
                             : (long double)v.as.f;

        h = td_fnv1a_step(h, &tag, 1);
        if (ld >= (long double)INT64_MIN && ld <= (long double)INT64_MAX) {
            int64_t iv = (int64_t)ld;

            if ((long double)iv == ld)
                return td_fnv1a_step(h, &iv, sizeof(iv));
        }
        double d = (double)ld;
        uint64_t bits = 0;

        if (d == 0.0)
            d = 0.0;
        memcpy(&bits, &d, sizeof(bits));
        return td_fnv1a_step(h, &bits, sizeof(bits));
    }
    case TD_TYPE_ANY:
    case TD_TYPE_STR: {
        uint8_t tag = 2;

        h = td_fnv1a_step(h, &tag, 1);
        if (v.as.s.data && v.as.s.len > 0)
            h = td_fnv1a_step(h, v.as.s.data, v.as.s.len);
        return h;
    }
    case TD_TYPE_ERR: {
        uint8_t tag = 3;

        h = td_fnv1a_step(h, &tag, 1);
        if (v.as.s.data && v.as.s.len > 0)
            h = td_fnv1a_step(h, v.as.s.data, v.as.s.len);
        return h;
    }
    }
    return h;
}

static uint64_t td_keys_hash(const td_val_t *keys, size_t nkeys)
{
    uint64_t h = UINT64_C(0x517cc1b727220a95);

    for (size_t i = 0; i < nkeys; i++) {
        uint64_t kh = td_val_hash(keys[i]);

        h ^= kh + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
    }
    return h;
}

static bool td_keys_eq(const td_val_t *a, const td_val_t *b, size_t nkeys)
{
    for (size_t i = 0; i < nkeys; i++) {
        if (td_val_cmp(a[i], b[i]) != 0)
            return false;
    }
    return true;
}

typedef struct {
    td_val_t *keys;
    size_t *rows;
    size_t count;
    size_t cap;
    uint64_t hash;
} td_group_t;

typedef struct {
    size_t nkeys;
    td_group_t *groups;
    size_t ngroups;
    size_t group_cap;
    size_t *buckets;   /* 0 = empty, else group_idx + 1 */
    size_t nbuckets;
    bool track_rows;
} td_group_table_t;

static void td_group_table_init(td_group_table_t *t, size_t nkeys,
                                bool track_rows)
{
    memset(t, 0, sizeof(*t));
    t->nkeys = nkeys;
    t->track_rows = track_rows;
}

static void td_group_table_free(td_group_table_t *t)
{
    if (!t)
        return;
    for (size_t i = 0; i < t->ngroups; i++) {
        free(t->groups[i].keys);
        free(t->groups[i].rows);
    }
    free(t->groups);
    free(t->buckets);
    memset(t, 0, sizeof(*t));
}

static bool td_group_table_rehash(td_group_table_t *t, size_t new_cap)
{
    size_t *buckets = calloc(new_cap, sizeof(*buckets));

    if (!buckets)
        return false;
    for (size_t i = 0; i < t->ngroups; i++) {
        size_t mask = new_cap - 1;
        size_t slot = (size_t)t->groups[i].hash & mask;

        while (buckets[slot] != 0)
            slot = (slot + 1) & mask;
        buckets[slot] = i + 1;
    }
    free(t->buckets);
    t->buckets = buckets;
    t->nbuckets = new_cap;
    return true;
}

static const td_group_t *td_group_table_find(const td_group_table_t *t,
                                             const td_val_t *keys)
{
    uint64_t h;
    size_t mask;
    size_t slot;

    if (!t || t->ngroups == 0 || t->nbuckets == 0)
        return NULL;
    h = td_keys_hash(keys, t->nkeys);
    mask = t->nbuckets - 1;
    slot = (size_t)h & mask;
    while (t->buckets[slot] != 0) {
        size_t idx = t->buckets[slot] - 1;
        const td_group_t *g = &t->groups[idx];

        if (g->hash == h && td_keys_eq(g->keys, keys, t->nkeys))
            return g;
        slot = (slot + 1) & mask;
    }
    return NULL;
}

static bool td_group_table_add(td_group_table_t *t, const td_val_t *keys,
                               size_t row_id, size_t *group_idx_out)
{
    uint64_t h = td_keys_hash(keys, t->nkeys);
    size_t mask;
    size_t slot;
    size_t gidx = SIZE_MAX;

    if (t->nbuckets == 0) {
        if (!td_group_table_rehash(t, 64))
            return false;
    } else if ((t->ngroups + 1) * 10 >= t->nbuckets * 7) {
        if (t->nbuckets > SIZE_MAX / 2 ||
            !td_group_table_rehash(t, t->nbuckets * 2))
            return false;
    }

    mask = t->nbuckets - 1;
    slot = (size_t)h & mask;
    while (t->buckets[slot] != 0) {
        size_t idx = t->buckets[slot] - 1;

        if (t->groups[idx].hash == h &&
            td_keys_eq(t->groups[idx].keys, keys, t->nkeys)) {
            gidx = idx;
            break;
        }
        slot = (slot + 1) & mask;
    }

    if (gidx == SIZE_MAX) {
        td_group_t *g;

        if (t->ngroups == t->group_cap) {
            size_t new_cap = t->group_cap ? t->group_cap * 2 : 32;
            td_group_t *arr = realloc(t->groups, new_cap * sizeof(*arr));

            if (!arr)
                return false;
            t->groups = arr;
            t->group_cap = new_cap;
        }
        gidx = t->ngroups;
        g = &t->groups[gidx];
        memset(g, 0, sizeof(*g));
        g->keys = malloc(t->nkeys * sizeof(*g->keys));
        if (!g->keys)
            return false;
        memcpy(g->keys, keys, t->nkeys * sizeof(*g->keys));
        g->hash = h;
        t->ngroups++;
        t->buckets[slot] = gidx + 1;
    }

    if (t->track_rows) {
        td_group_t *g = &t->groups[gidx];

        if (g->count == g->cap) {
            size_t new_cap = g->cap ? g->cap * 2 : 8;
            size_t *rows = realloc(g->rows, new_cap * sizeof(*rows));

            if (!rows)
                return false;
            g->rows = rows;
            g->cap = new_cap;
        }
        g->rows[g->count++] = row_id;
    } else {
        t->groups[gidx].count++;
    }

    if (group_idx_out)
        *group_idx_out = gidx;
    return true;
}

/* ---- Core Aggregator Engine ---- */

static int td_cmp_doubles(const void *pa, const void *pb)
{
    double a = *(const double *)pa;
    double b = *(const double *)pb;

    return a < b ? -1 : (a > b ? 1 : 0);
}

static bool td_val_to_double(td_val_t v, double *out)
{
    switch (v.type) {
    case TD_TYPE_INT:
    case TD_TYPE_DATE:
        *out = (double)v.as.i;
        return true;
    case TD_TYPE_FLOAT:
    case TD_TYPE_CURRENCY:
        if (!isfinite(v.as.f))
            return false;
        *out = v.as.f;
        return true;
    default:
        return false;
    }
}

static td_val_t td_make_numeric_result(td_agg_kind_t agg, td_type_t col_type,
                                       double val, int64_t int_sum,
                                       bool int_exact)
{
    td_val_t out;

    if (agg == TD_AGG_SUM && col_type == TD_TYPE_INT && int_exact) {
        out.type = TD_TYPE_INT;
        out.as.i = int_sum;
        return out;
    }
    if (col_type == TD_TYPE_CURRENCY && agg != TD_AGG_STDEV) {
        out.type = TD_TYPE_CURRENCY;
        out.as.f = val;
        return out;
    }
    if (col_type == TD_TYPE_DATE &&
        (agg == TD_AGG_MEDIAN || agg == TD_AGG_PCT90 ||
         agg == TD_AGG_PCT95 || agg == TD_AGG_PCT99 || agg == TD_AGG_MEAN)) {
        out.type = TD_TYPE_DATE;
        out.as.i = (int64_t)(val >= 0.0 ? val + 0.5 : val - 0.5);
        return out;
    }
    out.type = TD_TYPE_FLOAT;
    out.as.f = val;
    return out;
}

td_val_t td_agg_compute_rows(const td_sheet_t *s, size_t col,
                             const size_t *phys_rows, size_t nrows,
                             td_agg_kind_t agg)
{
    size_t n;
    int64_t non_null = 0;
    int64_t nulls = 0;
    bool have_minmax = false;
    td_val_t best = TD_VAL_NULL;
    td_group_table_t distinct_tbl;
    double *nums = NULL;
    size_t num_n = 0;
    size_t num_cap = 0;
    double mean = 0.0;
    double m2 = 0.0;
    long double sum_ld = 0.0L;
    int64_t sum_i = 0;
    bool int_exact = true;
    td_type_t col_type;
    td_val_t out = TD_VAL_NULL;

    if (!s || col >= s->ncols)
        return TD_VAL_NULL;

    col_type = s->cols[col].type;
    n = phys_rows ? nrows : s->nrows;

    if (agg == TD_AGG_DISTINCT)
        td_group_table_init(&distinct_tbl, 1, false);
    else
        memset(&distinct_tbl, 0, sizeof(distinct_tbl));

    for (size_t i = 0; i < n; i++) {
        size_t phys = phys_rows ? phys_rows[i] : s->row_order[i];
        td_val_t v;

        if (phys >= s->phys_nrows)
            continue;
        v = td_sheet_eval_cell(s, phys, col);
        if (v.type == TD_TYPE_NULL) {
            nulls++;
            continue;
        }
        if (v.type == TD_TYPE_ERR)
            continue;
        non_null++;

        if (agg == TD_AGG_DISTINCT) {
            (void)td_group_table_add(&distinct_tbl, &v, 0, NULL);
            continue;
        }

        if (agg == TD_AGG_MIN || agg == TD_AGG_MAX) {
            if (!have_minmax) {
                best = v;
                have_minmax = true;
            } else {
                int c = td_val_cmp(v, best);

                if ((agg == TD_AGG_MIN && c < 0) ||
                    (agg == TD_AGG_MAX && c > 0))
                    best = v;
            }
            continue;
        }

        if (td_agg_is_numeric_only(agg)) {
            double dv = 0.0;

            if (!td_val_to_double(v, &dv))
                continue;
            if (agg == TD_AGG_SUM) {
                sum_ld += (long double)dv;
                if (col_type == TD_TYPE_INT && v.type == TD_TYPE_INT &&
                    int_exact) {
                    if ((v.as.i > 0 && sum_i > INT64_MAX - v.as.i) ||
                        (v.as.i < 0 && sum_i < INT64_MIN - v.as.i))
                        int_exact = false;
                    else
                        sum_i += v.as.i;
                } else {
                    int_exact = false;
                }
            } else if (agg == TD_AGG_MEAN || agg == TD_AGG_STDEV) {
                num_n++;
                double delta = dv - mean;

                mean += delta / (double)num_n;
                m2 += delta * (dv - mean);
            } else {
                if (num_n == num_cap) {
                    size_t ncap = num_cap ? num_cap * 2 : 32;
                    double *tmp = realloc(nums, ncap * sizeof(*tmp));

                    if (!tmp) {
                        free(nums);
                        return TD_VAL_ERR;
                    }
                    nums = tmp;
                    num_cap = ncap;
                }
                nums[num_n++] = dv;
            }
        }
    }

    switch (agg) {
    case TD_AGG_COUNT:
        out.type = TD_TYPE_INT;
        out.as.i = non_null;
        break;
    case TD_AGG_NULLS:
        out.type = TD_TYPE_INT;
        out.as.i = nulls;
        break;
    case TD_AGG_DISTINCT:
        out.type = TD_TYPE_INT;
        out.as.i = (int64_t)distinct_tbl.ngroups;
        td_group_table_free(&distinct_tbl);
        break;
    case TD_AGG_MIN:
    case TD_AGG_MAX:
        out = have_minmax ? best : TD_VAL_NULL;
        break;
    case TD_AGG_SUM:
        if (non_null == 0 || !td_type_is_numeric(col_type) ||
            col_type == TD_TYPE_DATE)
            out = TD_VAL_NULL;
        else
            out = td_make_numeric_result(agg, col_type, (double)sum_ld, sum_i,
                                         int_exact);
        break;
    case TD_AGG_MEAN:
        out = (num_n > 0)
                  ? td_make_numeric_result(agg, col_type, mean, 0, false)
                  : TD_VAL_NULL;
        break;
    case TD_AGG_STDEV:
        if (num_n >= 2) {
            out.type = TD_TYPE_FLOAT;
            out.as.f = sqrt(m2 / (double)(num_n - 1));
        } else {
            out = TD_VAL_NULL;
        }
        break;
    case TD_AGG_MEDIAN:
    case TD_AGG_PCT90:
    case TD_AGG_PCT95:
    case TD_AGG_PCT99:
        if (num_n > 0) {
            double p = 0.5;
            double pos;
            size_t lo, hi;
            double frac;
            double val;

            qsort(nums, num_n, sizeof(*nums), td_cmp_doubles);
            if (agg == TD_AGG_PCT90)
                p = 0.90;
            else if (agg == TD_AGG_PCT95)
                p = 0.95;
            else if (agg == TD_AGG_PCT99)
                p = 0.99;
            pos = p * (double)(num_n - 1);
            lo = (size_t)pos;
            hi = lo + 1 < num_n ? lo + 1 : lo;
            frac = pos - (double)lo;
            val = nums[lo] + frac * (nums[hi] - nums[lo]);
            out = td_make_numeric_result(agg, col_type, val, 0, false);
        }
        break;
    case TD_AGG_KIND_COUNT:
        break;
    }

    free(nums);
    return out;
}

bool td_sheet_toggle_agg(td_sheet_t *s, size_t col, const char *spec,
                         char *msg, size_t msg_cap,
                         char *err, size_t err_cap)
{
    uint8_t next[TD_MAX_COL_AGGS];
    uint8_t nnext = 0;
    char *copy;
    char *tok;
    char *save = NULL;
    size_t ntoks = 0;
    td_agg_kind_t first_kind = TD_AGG_COUNT;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!s || col >= s->ncols) {
        td_set_err(err, err_cap, "no column");
        return false;
    }

    if (!spec || !spec[0] || td_strieq(spec, "none") ||
        td_strieq(spec, "clear")) {
        if (!td_sheet_set_col_aggs(s, col, NULL, 0, "clear aggregators")) {
            td_set_err(err, err_cap, "failed to clear aggregators");
            return false;
        }
        if (msg && msg_cap > 0)
            snprintf(msg, msg_cap, "cleared aggregators on %s",
                     s->cols[col].name ? s->cols[col].name : "");
        return true;
    }

    memcpy(next, s->cols[col].aggs, sizeof(next));
    nnext = s->cols[col].naggs;

    copy = malloc(strlen(spec) + 1);
    if (!copy) {
        td_set_err(err, err_cap, "out of memory");
        return false;
    }
    strcpy(copy, spec);

    for (tok = strtok_r(copy, " ,\t", &save); tok;
         tok = strtok_r(NULL, " ,\t", &save)) {
        td_agg_kind_t k;

        if (!td_agg_parse(tok, &k)) {
            if (err && err_cap > 0)
                snprintf(err, err_cap, "unknown aggregator: %s", tok);
            free(copy);
            return false;
        }
        if (ntoks == 0)
            first_kind = k;
        ntoks++;

        size_t found = SIZE_MAX;

        for (size_t i = 0; i < nnext; i++) {
            if (next[i] == (uint8_t)k) {
                found = i;
                break;
            }
        }
        if (found != SIZE_MAX) {
            if (ntoks == 1 && !strtok_r(NULL, " ,\t", &save)) {
                memmove(&next[found], &next[found + 1],
                        (size_t)(nnext - found - 1));
                nnext--;
                if (!td_sheet_set_col_aggs(s, col, next, nnext,
                                           "remove aggregator")) {
                    free(copy);
                    td_set_err(err, err_cap, "out of memory");
                    return false;
                }
                free(copy);
                if (msg && msg_cap > 0)
                    snprintf(msg, msg_cap, "removed %s from %s",
                             td_agg_name(k),
                             s->cols[col].name ? s->cols[col].name : "");
                return true;
            }
            continue;
        }
        if (nnext >= TD_MAX_COL_AGGS) {
            free(copy);
            td_set_err(err, err_cap, "too many aggregators on column");
            return false;
        }
        next[nnext++] = (uint8_t)k;
    }
    free(copy);

    if (ntoks == 0) {
        if (!td_sheet_set_col_aggs(s, col, NULL, 0, "clear aggregators")) {
            td_set_err(err, err_cap, "failed to clear aggregators");
            return false;
        }
        if (msg && msg_cap > 0)
            snprintf(msg, msg_cap, "cleared aggregators on %s",
                     s->cols[col].name ? s->cols[col].name : "");
        return true;
    }

    if (!td_sheet_set_col_aggs(s, col, next, nnext, "add aggregator")) {
        td_set_err(err, err_cap, "out of memory");
        return false;
    }
    td_sheet_refresh_col_aggs(s);
    if (msg && msg_cap > 0) {
        if (ntoks == 1)
            snprintf(msg, msg_cap, "added %s to %s",
                     td_agg_name(first_kind),
                     s->cols[col].name ? s->cols[col].name : "");
        else
            snprintf(msg, msg_cap, "updated %u aggregators on %s",
                     (unsigned)nnext,
                     s->cols[col].name ? s->cols[col].name : "");
    }
    return true;
}

void td_sheet_refresh_col_aggs(td_sheet_t *s)
{
    if (!s)
        return;
    for (size_t c = 0; c < s->ncols; c++) {
        td_col_t *col = &s->cols[c];

        if (col->naggs == 0 || !col->agg_dirty)
            continue;
        for (uint8_t i = 0; i < col->naggs; i++) {
            col->agg_vals[i] = td_agg_compute_rows(
                s, c, NULL, 0, (td_agg_kind_t)col->aggs[i]);
        }
        col->agg_dirty = false;
    }
}

/* ---- Frequency Sheet (F / gF) ---- */

typedef struct {
    size_t offset;
    size_t count;
} td_freq_bin_t;

typedef struct {
    td_freq_bin_t *bins;
    size_t nbins;
    size_t *all_phys;
} td_freq_meta_t;

static void td_freq_meta_free(void *ptr)
{
    td_freq_meta_t *m = ptr;

    if (!m)
        return;
    free(m->bins);
    free(m->all_phys);
    free(m);
}

static td_freq_meta_t *td_freq_build_meta(const td_group_table_t *tbl,
                                          size_t total_rows)
{
    td_freq_meta_t *m = calloc(1, sizeof(*m));
    size_t off = 0;

    if (!m)
        return NULL;
    m->nbins = tbl->ngroups;
    if (m->nbins > 0) {
        m->bins = calloc(m->nbins, sizeof(*m->bins));
        if (!m->bins) {
            td_freq_meta_free(m);
            return NULL;
        }
    }
    if (total_rows > 0) {
        m->all_phys = malloc(total_rows * sizeof(*m->all_phys));
        if (!m->all_phys) {
            td_freq_meta_free(m);
            return NULL;
        }
    }
    for (size_t i = 0; i < tbl->ngroups; i++) {
        const td_group_t *g = &tbl->groups[i];

        m->bins[i].offset = off;
        m->bins[i].count = g->count;
        if (g->count > 0 && g->rows)
            memcpy(m->all_phys + off, g->rows, g->count * sizeof(*m->all_phys));
        off += g->count;
    }
    return m;
}

td_sheet_t *td_agg_freq_sheet(const td_sheet_t *s, const size_t *key_cols,
                              size_t nkeys, char *err, size_t err_cap)
{
    td_group_table_t tbl;
    td_val_t *key_buf;
    size_t *extra_cols = NULL;
    uint8_t *extra_aggs = NULL;
    size_t nextra = 0;
    td_sheet_t *freq = NULL;
    td_freq_meta_t *meta = NULL;
    char *name_buf = NULL;
    size_t max_count = 0;
    size_t count_col;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!s || !key_cols || nkeys == 0 || s->ncols == 0) {
        td_set_err(err, err_cap, "no key column");
        return NULL;
    }
    for (size_t k = 0; k < nkeys; k++) {
        if (key_cols[k] >= s->ncols) {
            td_set_err(err, err_cap, "invalid key column");
            return NULL;
        }
    }

    key_buf = malloc(nkeys * sizeof(*key_buf));
    if (!key_buf) {
        td_set_err(err, err_cap, "out of memory");
        return NULL;
    }

    td_group_table_init(&tbl, nkeys, true);
    for (size_t vr = 0; vr < s->nrows; vr++) {
        size_t phys = s->row_order ? s->row_order[vr] : vr;

        for (size_t k = 0; k < nkeys; k++)
            key_buf[k] = td_sheet_eval_cell(s, phys, key_cols[k]);
        if (!td_group_table_add(&tbl, key_buf, phys, NULL)) {
            free(key_buf);
            td_group_table_free(&tbl);
            td_set_err(err, err_cap, "out of memory");
            return NULL;
        }
    }
    free(key_buf);

    for (size_t i = 0; i < tbl.ngroups; i++) {
        if (tbl.groups[i].count > max_count)
            max_count = tbl.groups[i].count;
    }

    /* Collect attached aggregators on non-key visible columns. */
    for (size_t c = 0; c < s->ncols; c++) {
        bool is_grp_key = false;

        if (s->cols[c].width == 0 || s->cols[c].naggs == 0)
            continue;
        for (size_t k = 0; k < nkeys; k++) {
            if (key_cols[k] == c) {
                is_grp_key = true;
                break;
            }
        }
        if (!is_grp_key)
            nextra += s->cols[c].naggs;
    }
    if (nextra > 0) {
        size_t idx = 0;

        extra_cols = malloc(nextra * sizeof(*extra_cols));
        extra_aggs = malloc(nextra * sizeof(*extra_aggs));
        if (!extra_cols || !extra_aggs) {
            free(extra_cols);
            free(extra_aggs);
            td_group_table_free(&tbl);
            td_set_err(err, err_cap, "out of memory");
            return NULL;
        }
        for (size_t c = 0; c < s->ncols; c++) {
            bool is_grp_key = false;

            if (s->cols[c].width == 0 || s->cols[c].naggs == 0)
                continue;
            for (size_t k = 0; k < nkeys; k++) {
                if (key_cols[k] == c) {
                    is_grp_key = true;
                    break;
                }
            }
            if (is_grp_key)
                continue;
            for (uint8_t a = 0; a < s->cols[c].naggs; a++) {
                extra_cols[idx] = c;
                extra_aggs[idx] = s->cols[c].aggs[a];
                idx++;
            }
        }
    }

    name_buf = td_make_derived_name(
        s->name, nkeys == 1 ? s->cols[key_cols[0]].name : "keys", "_freq");
    freq = td_sheet_new(name_buf ? name_buf : "freq", s->source_path);
    free(name_buf);
    if (!freq) {
        free(extra_cols);
        free(extra_aggs);
        td_group_table_free(&tbl);
        td_set_err(err, err_cap, "out of memory");
        return NULL;
    }
    freq->source_id = s->id;
    freq->kind = TD_SHEET_FREQ;

    for (size_t k = 0; k < nkeys; k++) {
        const td_col_t *kc = &s->cols[key_cols[k]];
        size_t ci = td_sheet_add_col(freq, kc->name, kc->type,
                                     -TD_AUTO_WIDTH_CAP);

        if (ci == SIZE_MAX)
            goto oom;
        freq->cols[ci].is_key = true;
    }
    count_col = td_sheet_add_col(freq, "count", TD_TYPE_INT, -12);
    if (count_col == SIZE_MAX ||
        td_sheet_add_col(freq, "percent", TD_TYPE_FLOAT, -10) == SIZE_MAX ||
        td_sheet_add_col(freq, "histogram", TD_TYPE_STR,
                         -(TD_HIST_MAX_WIDTH + 2)) == SIZE_MAX)
        goto oom;

    for (size_t e = 0; e < nextra; e++) {
        const td_col_t *sc = &s->cols[extra_cols[e]];
        td_agg_kind_t ak = (td_agg_kind_t)extra_aggs[e];
        size_t cap = strlen(sc->name ? sc->name : "col") +
                     strlen(td_agg_name(ak)) + 8;
        char *cname = malloc(cap);

        if (!cname)
            goto oom;
        snprintf(cname, cap, "%s_%s", sc->name ? sc->name : "col",
                 td_agg_name(ak));
        size_t ci = td_sheet_add_col(freq, cname,
                                     td_agg_result_type(ak, sc->type),
                                     -TD_AUTO_WIDTH_CAP);
        free(cname);
        if (ci == SIZE_MAX)
            goto oom;
    }

    for (size_t i = 0; i < tbl.ngroups; i++) {
        const td_group_t *g = &tbl.groups[i];
        size_t r = td_sheet_add_row(freq);
        td_val_t vc, vp;
        char hist[TD_HIST_MAX_WIDTH + 1];
        size_t hlen = 0;

        if (r == SIZE_MAX)
            goto oom;
        for (size_t k = 0; k < nkeys; k++) {
            if (!td_set_cloned_cell(freq, r, k, g->keys[k]))
                goto oom;
        }
        vc.type = TD_TYPE_INT;
        vc.as.i = (int64_t)g->count;
        if (!td_sheet_set_cell(freq, r, count_col, vc))
            goto oom;

        vp.type = TD_TYPE_FLOAT;
        vp.as.f = s->nrows > 0 ? (100.0 * (double)g->count / (double)s->nrows)
                               : 0.0;
        if (!td_sheet_set_cell(freq, r, count_col + 1, vp))
            goto oom;

        if (max_count > 0 && g->count > 0) {
            hlen = (size_t)((double)TD_HIST_MAX_WIDTH * (double)g->count /
                                (double)max_count +
                            0.5);
            if (hlen == 0)
                hlen = 1;
            if (hlen > TD_HIST_MAX_WIDTH)
                hlen = TD_HIST_MAX_WIDTH;
        }
        memset(hist, '*', hlen);
        hist[hlen] = '\0';
        if (!td_set_str_cell(freq, r, count_col + 2, hist))
            goto oom;

        for (size_t e = 0; e < nextra; e++) {
            td_val_t av = td_agg_compute_rows(
                s, extra_cols[e], g->rows, g->count,
                (td_agg_kind_t)extra_aggs[e]);

            if (!td_set_cloned_cell(freq, r, count_col + 3 + e, av))
                goto oom;
        }
    }

    meta = td_freq_build_meta(&tbl, s->nrows);
    if (!meta)
        goto oom;
    freq->meta = meta;
    freq->meta_free = td_freq_meta_free;

    td_sheet_set_undo_recording(freq, false);
    (void)td_sheet_sort_col(freq, count_col, true);
    td_sheet_set_undo_recording(freq, true);
    freq->cursor_col = count_col;

    free(extra_cols);
    free(extra_aggs);
    td_group_table_free(&tbl);
    return freq;

oom:
    free(extra_cols);
    free(extra_aggs);
    td_group_table_free(&tbl);
    td_sheet_free(freq);
    td_set_err(err, err_cap, "out of memory");
    return NULL;
}

/* ---- Describe Sheet (I) ---- */

typedef struct {
    size_t *src_cols;
    size_t ncols;
} td_desc_meta_t;

static void td_desc_meta_free(void *ptr)
{
    td_desc_meta_t *m = ptr;

    if (!m)
        return;
    free(m->src_cols);
    free(m);
}

static size_t td_count_col_errors(const td_sheet_t *s, size_t col)
{
    size_t errs = 0;

    for (size_t vr = 0; vr < s->nrows; vr++) {
        size_t phys = s->row_order ? s->row_order[vr] : vr;

        if (td_sheet_eval_cell(s, phys, col).type == TD_TYPE_ERR)
            errs++;
    }
    return errs;
}

static td_val_t td_as_float_cell(td_val_t v)
{
    td_val_t out;

    if (v.type == TD_TYPE_NULL || v.type == TD_TYPE_ERR)
        return v;
    if (v.type == TD_TYPE_FLOAT)
        return v;
    if (v.type == TD_TYPE_CURRENCY) {
        out.type = TD_TYPE_FLOAT;
        out.as.f = v.as.f;
        return out;
    }
    if (v.type == TD_TYPE_INT) {
        out.type = TD_TYPE_FLOAT;
        out.as.f = (double)v.as.i;
        return out;
    }
    return TD_VAL_NULL;
}

td_sheet_t *td_agg_describe_sheet(const td_sheet_t *s, char *err,
                                  size_t err_cap)
{
    static const struct {
        const char *name;
        td_type_t type;
    } cols[12] = {
        { "column",   TD_TYPE_STR   },
        { "type",     TD_TYPE_STR   },
        { "count",    TD_TYPE_INT   },
        { "nulls",    TD_TYPE_INT   },
        { "errors",   TD_TYPE_INT   },
        { "distinct", TD_TYPE_INT   },
        { "min",      TD_TYPE_STR   },
        { "max",      TD_TYPE_STR   },
        { "sum",      TD_TYPE_FLOAT },
        { "mean",     TD_TYPE_FLOAT },
        { "median",   TD_TYPE_STR   },
        { "stdev",    TD_TYPE_FLOAT }
    };
    size_t *vis_cols = NULL;
    size_t nvis;
    char *name_buf;
    td_sheet_t *desc = NULL;
    td_desc_meta_t *meta = NULL;

    if (err && err_cap > 0)
        err[0] = '\0';
    nvis = td_collect_cols(s, false, false, &vis_cols);
    if (nvis == 0) {
        td_set_err(err, err_cap, "no visible columns to describe");
        return NULL;
    }

    name_buf = td_make_derived_name(s->name, NULL, "_describe");
    desc = td_sheet_new(name_buf ? name_buf : "describe", s->source_path);
    free(name_buf);
    if (!desc) {
        free(vis_cols);
        td_set_err(err, err_cap, "out of memory");
        return NULL;
    }
    desc->source_id = s->id;
    desc->kind = TD_SHEET_DESCRIBE;

    for (size_t c = 0; c < 12; c++) {
        size_t ci = td_sheet_add_col(desc, cols[c].name, cols[c].type,
                                     -TD_AUTO_WIDTH_CAP);

        if (ci == SIZE_MAX)
            goto oom;
        if (c == 0)
            desc->cols[ci].is_key = true;
    }

    meta = calloc(1, sizeof(*meta));
    if (!meta)
        goto oom;
    meta->src_cols = vis_cols;
    meta->ncols = nvis;
    desc->meta = meta;
    desc->meta_free = td_desc_meta_free;
    vis_cols = NULL;

    for (size_t i = 0; i < meta->ncols; i++) {
        size_t sc = meta->src_cols[i];
        td_type_t st = s->cols[sc].type;
        size_t r = td_sheet_add_row(desc);
        td_val_t v_err;

        if (r == SIZE_MAX)
            goto oom;
        if (!td_set_str_cell(desc, r, 0, s->cols[sc].name) ||
            !td_set_str_cell(desc, r, 1, td_type_name(st)))
            goto oom;

        if (!td_sheet_set_cell(desc, r, 2,
                td_agg_compute_rows(s, sc, NULL, 0, TD_AGG_COUNT)) ||
            !td_sheet_set_cell(desc, r, 3,
                td_agg_compute_rows(s, sc, NULL, 0, TD_AGG_NULLS)))
            goto oom;

        v_err.type = TD_TYPE_INT;
        v_err.as.i = (int64_t)td_count_col_errors(s, sc);
        if (!td_sheet_set_cell(desc, r, 4, v_err) ||
            !td_sheet_set_cell(desc, r, 5,
                td_agg_compute_rows(s, sc, NULL, 0, TD_AGG_DISTINCT)))
            goto oom;

        if (!td_set_formatted_str_cell(desc, r, 6,
                td_agg_compute_rows(s, sc, NULL, 0, TD_AGG_MIN), st) ||
            !td_set_formatted_str_cell(desc, r, 7,
                td_agg_compute_rows(s, sc, NULL, 0, TD_AGG_MAX), st) ||
            !td_sheet_set_cell(desc, r, 8, td_as_float_cell(
                td_agg_compute_rows(s, sc, NULL, 0, TD_AGG_SUM))) ||
            !td_sheet_set_cell(desc, r, 9, td_as_float_cell(
                td_agg_compute_rows(s, sc, NULL, 0, TD_AGG_MEAN))) ||
            !td_set_formatted_str_cell(desc, r, 10,
                td_agg_compute_rows(s, sc, NULL, 0, TD_AGG_MEDIAN), st) ||
            !td_sheet_set_cell(desc, r, 11, td_as_float_cell(
                td_agg_compute_rows(s, sc, NULL, 0, TD_AGG_STDEV))))
            goto oom;
    }
    return desc;

oom:
    free(vis_cols);
    td_sheet_free(desc);
    td_set_err(err, err_cap, "out of memory");
    return NULL;
}

/* ---- Pivot Sheet (W) ---- */

static int td_cmp_td_val_ptr(const void *pa, const void *pb)
{
    return td_val_cmp(*(const td_val_t *)pa, *(const td_val_t *)pb);
}

typedef struct {
    size_t col;
    td_agg_kind_t agg;
} td_pivot_val_spec_t;

td_sheet_t *td_agg_pivot_sheet(const td_sheet_t *s, size_t pivot_col,
                               char *err, size_t err_cap)
{
    size_t *key_cols = NULL;
    size_t nkeys;
    td_pivot_val_spec_t *vspecs = NULL;
    size_t nvspecs = 0;
    td_group_table_t row_tbl;
    td_group_table_t pval_tbl;
    td_group_table_t cell_tbl;
    td_val_t *pvals = NULL;
    td_val_t *key_buf = NULL;
    td_val_t *full_key = NULL;
    td_sheet_t *piv = NULL;
    char *name_buf = NULL;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!s || s->ncols == 0 || pivot_col >= s->ncols) {
        td_set_err(err, err_cap, "no pivot column");
        return NULL;
    }

    nkeys = td_collect_cols(s, true, false, &key_cols);
    if (nkeys == 0) {
        td_set_err(err, err_cap, "pin key column(s) with ! first");
        return NULL;
    }
    if (s->cols[pivot_col].width == 0 || s->cols[pivot_col].is_key) {
        free(key_cols);
        td_set_err(err, err_cap, "move cursor to a non-key pivot column");
        return NULL;
    }

    for (size_t c = 0; c < s->ncols; c++) {
        if (c == pivot_col || s->cols[c].width == 0 || s->cols[c].is_key)
            continue;
        nvspecs += s->cols[c].naggs;
    }
    if (nvspecs > 0) {
        size_t idx = 0;

        vspecs = malloc(nvspecs * sizeof(*vspecs));
        if (!vspecs) {
            free(key_cols);
            td_set_err(err, err_cap, "out of memory");
            return NULL;
        }
        for (size_t c = 0; c < s->ncols; c++) {
            if (c == pivot_col || s->cols[c].width == 0 || s->cols[c].is_key)
                continue;
            for (uint8_t a = 0; a < s->cols[c].naggs; a++) {
                vspecs[idx].col = c;
                vspecs[idx].agg = (td_agg_kind_t)s->cols[c].aggs[a];
                idx++;
            }
        }
    } else {
        nvspecs = 1;
        vspecs = malloc(sizeof(*vspecs));
        if (!vspecs) {
            free(key_cols);
            td_set_err(err, err_cap, "out of memory");
            return NULL;
        }
        vspecs[0].col = pivot_col;
        vspecs[0].agg = TD_AGG_COUNT;
    }

    td_group_table_init(&row_tbl, nkeys, false);
    td_group_table_init(&pval_tbl, 1, false);
    td_group_table_init(&cell_tbl, nkeys + 1, true);

    key_buf = malloc(nkeys * sizeof(*key_buf));
    full_key = malloc((nkeys + 1) * sizeof(*full_key));
    if (!key_buf || !full_key)
        goto oom;

    for (size_t vr = 0; vr < s->nrows; vr++) {
        size_t phys = s->row_order ? s->row_order[vr] : vr;
        td_val_t pv = td_sheet_eval_cell(s, phys, pivot_col);

        for (size_t k = 0; k < nkeys; k++) {
            key_buf[k] = td_sheet_eval_cell(s, phys, key_cols[k]);
            full_key[k] = key_buf[k];
        }
        full_key[nkeys] = pv;

        if (!td_group_table_add(&row_tbl, key_buf, phys, NULL) ||
            !td_group_table_add(&pval_tbl, &pv, phys, NULL) ||
            !td_group_table_add(&cell_tbl, full_key, phys, NULL))
            goto oom;
    }

    if (pval_tbl.ngroups > 0) {
        pvals = malloc(pval_tbl.ngroups * sizeof(*pvals));
        if (!pvals)
            goto oom;
        for (size_t i = 0; i < pval_tbl.ngroups; i++)
            pvals[i] = pval_tbl.groups[i].keys[0];
        qsort(pvals, pval_tbl.ngroups, sizeof(*pvals), td_cmp_td_val_ptr);
    }

    name_buf = td_make_derived_name(s->name, s->cols[pivot_col].name, "_pivot");
    piv = td_sheet_new(name_buf ? name_buf : "pivot", s->source_path);
    free(name_buf);
    name_buf = NULL;
    if (!piv)
        goto oom;
    piv->source_id = s->id;
    piv->kind = TD_SHEET_PIVOT;

    for (size_t k = 0; k < nkeys; k++) {
        const td_col_t *kc = &s->cols[key_cols[k]];
        size_t ci = td_sheet_add_col(piv, kc->name, kc->type,
                                     -TD_AUTO_WIDTH_CAP);

        if (ci == SIZE_MAX)
            goto oom;
        piv->cols[ci].is_key = true;
    }

    for (size_t p = 0; p < pval_tbl.ngroups; p++) {
        char *pstr = td_format_val_alloc(pvals[p], s->cols[pivot_col].type,
                                         "(null)");

        if (!pstr)
            goto oom;
        for (size_t v = 0; v < nvspecs; v++) {
            const td_col_t *vc = &s->cols[vspecs[v].col];
            td_type_t rt = td_agg_result_type(vspecs[v].agg, vc->type);
            size_t cap = strlen(pstr) + strlen(vc->name ? vc->name : "") +
                         strlen(td_agg_name(vspecs[v].agg)) + 8;
            char *raw_name = malloc(cap);
            char *uniq;

            if (!raw_name) {
                free(pstr);
                goto oom;
            }
            if (nvspecs == 1)
                snprintf(raw_name, cap, "%s", pstr[0] ? pstr : "(blank)");
            else
                snprintf(raw_name, cap, "%s_%s_%s",
                         pstr[0] ? pstr : "(blank)",
                         vc->name ? vc->name : "col",
                         td_agg_name(vspecs[v].agg));
            uniq = td_unique_col_name(piv, raw_name);
            free(raw_name);
            if (!uniq) {
                free(pstr);
                goto oom;
            }
            size_t ci = td_sheet_add_col(piv, uniq, rt, -TD_AUTO_WIDTH_CAP);
            free(uniq);
            if (ci == SIZE_MAX) {
                free(pstr);
                goto oom;
            }
        }
        free(pstr);
    }

    for (size_t r = 0; r < row_tbl.ngroups; r++) {
        const td_group_t *rg = &row_tbl.groups[r];
        size_t dst_row = td_sheet_add_row(piv);

        if (dst_row == SIZE_MAX)
            goto oom;
        for (size_t k = 0; k < nkeys; k++) {
            full_key[k] = rg->keys[k];
            if (!td_set_cloned_cell(piv, dst_row, k, rg->keys[k]))
                goto oom;
        }
        for (size_t p = 0; p < pval_tbl.ngroups; p++) {
            const td_group_t *cg;

            full_key[nkeys] = pvals[p];
            cg = td_group_table_find(&cell_tbl, full_key);
            if (!cg || cg->count == 0)
                continue;
            for (size_t v = 0; v < nvspecs; v++) {
                size_t dst_col = nkeys + p * nvspecs + v;
                td_val_t av = td_agg_compute_rows(s, vspecs[v].col, cg->rows,
                                                  cg->count, vspecs[v].agg);

                if (!td_set_cloned_cell(piv, dst_row, dst_col, av))
                    goto oom;
            }
        }
    }

    free(pvals);
    free(key_buf);
    free(full_key);
    free(vspecs);
    free(key_cols);
    td_group_table_free(&row_tbl);
    td_group_table_free(&pval_tbl);
    td_group_table_free(&cell_tbl);
    return piv;

oom:
    free(name_buf);
    free(pvals);
    free(key_buf);
    free(full_key);
    free(vspecs);
    free(key_cols);
    td_group_table_free(&row_tbl);
    td_group_table_free(&pval_tbl);
    td_group_table_free(&cell_tbl);
    td_sheet_free(piv);
    td_set_err(err, err_cap, "out of memory");
    return NULL;
}

/* ---- Melt Sheet (M) ---- */

td_sheet_t *td_agg_melt_sheet(const td_sheet_t *s, char *err, size_t err_cap)
{
    size_t *key_cols = NULL;
    size_t *val_cols = NULL;
    size_t nkeys;
    size_t nvals;
    char *name_buf;
    td_sheet_t *melt = NULL;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!s || s->ncols == 0) {
        td_set_err(err, err_cap, "no columns to melt");
        return NULL;
    }

    nkeys = td_collect_cols(s, true, false, &key_cols);
    nvals = td_collect_cols(s, false, true, &val_cols);
    if (nvals == 0) {
        free(key_cols);
        td_set_err(err, err_cap, "no non-key columns to melt");
        return NULL;
    }

    name_buf = td_make_derived_name(s->name, NULL, "_melt");
    melt = td_sheet_new(name_buf ? name_buf : "melt", s->source_path);
    free(name_buf);
    if (!melt)
        goto oom;
    melt->source_id = s->id;

    for (size_t k = 0; k < nkeys; k++) {
        const td_col_t *kc = &s->cols[key_cols[k]];
        size_t ci = td_sheet_add_col(melt, kc->name, kc->type,
                                     -TD_AUTO_WIDTH_CAP);

        if (ci == SIZE_MAX)
            goto oom;
        melt->cols[ci].is_key = true;
    }

    {
        char *var_name = td_unique_col_name(melt, "variable");
        size_t var_ci = SIZE_MAX;

        if (!var_name)
            goto oom;
        var_ci = td_sheet_add_col(melt, var_name, TD_TYPE_STR,
                                  -TD_AUTO_WIDTH_CAP);
        free(var_name);
        if (var_ci == SIZE_MAX)
            goto oom;

        char *val_name = td_unique_col_name(melt, "value");
        size_t val_ci = SIZE_MAX;

        if (!val_name)
            goto oom;
        val_ci = td_sheet_add_col(melt, val_name, TD_TYPE_ANY,
                                  -TD_AUTO_WIDTH_CAP);
        free(val_name);
        if (val_ci == SIZE_MAX)
            goto oom;
    }

    for (size_t vr = 0; vr < s->nrows; vr++) {
        size_t phys = s->row_order ? s->row_order[vr] : vr;

        for (size_t v = 0; v < nvals; v++) {
            size_t dst_row = td_sheet_add_row(melt);
            size_t sc = val_cols[v];

            if (dst_row == SIZE_MAX)
                goto oom;
            for (size_t k = 0; k < nkeys; k++) {
                if (!td_set_cloned_cell(melt, dst_row, k,
                        td_sheet_get_cell(s, phys, key_cols[k])))
                    goto oom;
            }
            if (!td_set_str_cell(melt, dst_row, nkeys, s->cols[sc].name) ||
                !td_set_cloned_cell(melt, dst_row, nkeys + 1,
                        td_sheet_eval_cell(s, phys, sc)))
                goto oom;
        }
    }

    free(key_cols);
    free(val_cols);
    return melt;

oom:
    free(key_cols);
    free(val_cols);
    td_sheet_free(melt);
    td_set_err(err, err_cap, "out of memory");
    return NULL;
}

/* ---- Sheet Joins (&) ---- */

static size_t td_find_col_index(const td_sheet_t *s, const char *name)
{
    if (!s || !name)
        return SIZE_MAX;
    for (size_t c = 0; c < s->ncols; c++) {
        if (s->cols[c].width != 0 && s->cols[c].name &&
            strcmp(s->cols[c].name, name) == 0)
            return c;
    }
    return SIZE_MAX;
}

static td_sheet_t *td_join_append(const td_sheet_t *left,
                                  const td_sheet_t *right,
                                  char *err, size_t err_cap)
{
    size_t *lvis = NULL;
    size_t *rvis = NULL;
    size_t nl = td_collect_cols(left, false, false, &lvis);
    size_t nr = td_collect_cols(right, false, false, &rvis);
    size_t *rmap = NULL;
    td_sheet_t *out = NULL;
    size_t cap = strlen(left->name ? left->name : "left") +
                 strlen(right->name ? right->name : "right") + 16;
    char *name_buf = malloc(cap);

    if (!name_buf) {
        free(lvis);
        free(rvis);
        td_set_err(err, err_cap, "out of memory");
        return NULL;
    }
    snprintf(name_buf, cap, "%s+%s_append",
             left->name ? left->name : "left",
             right->name ? right->name : "right");
    out = td_sheet_new(name_buf, left->source_path);
    free(name_buf);
    if (!out) {
        free(lvis);
        free(rvis);
        td_set_err(err, err_cap, "out of memory");
        return NULL;
    }

    for (size_t i = 0; i < nl; i++) {
        const td_col_t *c = &left->cols[lvis[i]];
        size_t ci = td_sheet_add_col(out, c->name, c->type, -TD_AUTO_WIDTH_CAP);

        if (ci == SIZE_MAX)
            goto oom;
        out->cols[ci].is_key = c->is_key;
    }

    if (nr > 0) {
        rmap = malloc(nr * sizeof(*rmap));
        if (!rmap)
            goto oom;
        for (size_t j = 0; j < nr; j++) {
            const td_col_t *rc = &right->cols[rvis[j]];
            size_t found = SIZE_MAX;

            for (size_t c = 0; c < out->ncols; c++) {
                if (out->cols[c].name && rc->name &&
                    strcmp(out->cols[c].name, rc->name) == 0) {
                    found = c;
                    break;
                }
            }
            if (found == SIZE_MAX) {
                found = td_sheet_add_col(out, rc->name, rc->type,
                                         -TD_AUTO_WIDTH_CAP);
                if (found == SIZE_MAX)
                    goto oom;
            }
            rmap[j] = found;
        }
    }

    for (size_t vr = 0; vr < left->nrows; vr++) {
        size_t phys = left->row_order ? left->row_order[vr] : vr;
        size_t dr = td_sheet_add_row(out);

        if (dr == SIZE_MAX)
            goto oom;
        for (size_t i = 0; i < nl; i++) {
            if (!td_set_cloned_cell(out, dr, i,
                    td_sheet_get_cell(left, phys, lvis[i])))
                goto oom;
        }
    }

    for (size_t vr = 0; vr < right->nrows; vr++) {
        size_t phys = right->row_order ? right->row_order[vr] : vr;
        size_t dr = td_sheet_add_row(out);

        if (dr == SIZE_MAX)
            goto oom;
        for (size_t j = 0; j < nr; j++) {
            if (!td_set_cloned_cell(out, dr, rmap[j],
                    td_sheet_get_cell(right, phys, rvis[j])))
                goto oom;
        }
    }

    free(lvis);
    free(rvis);
    free(rmap);
    return out;

oom:
    free(lvis);
    free(rvis);
    free(rmap);
    td_sheet_free(out);
    td_set_err(err, err_cap, "out of memory");
    return NULL;
}

static bool td_join_emit_row(td_sheet_t *out, const td_sheet_t *left,
                             size_t l_phys, const td_sheet_t *right,
                             size_t r_phys, const size_t *lkeys,
                             const size_t *rkeys, size_t nkeys,
                             const size_t *lnon, size_t nlnon,
                             const size_t *rnon, size_t nrnon)
{
    size_t dr = td_sheet_add_row(out);

    if (dr == SIZE_MAX)
        return false;
    for (size_t k = 0; k < nkeys; k++) {
        td_val_t kv = (l_phys != SIZE_MAX)
                          ? td_sheet_eval_cell(left, l_phys, lkeys[k])
                          : td_sheet_eval_cell(right, r_phys, rkeys[k]);

        if (!td_set_cloned_cell(out, dr, k, kv))
            return false;
    }
    for (size_t i = 0; i < nlnon; i++) {
        td_val_t lv = (l_phys != SIZE_MAX)
                          ? td_sheet_get_cell(left, l_phys, lnon[i])
                          : TD_VAL_NULL;

        if (!td_set_cloned_cell(out, dr, nkeys + i, lv))
            return false;
    }
    for (size_t j = 0; j < nrnon; j++) {
        td_val_t rv = (r_phys != SIZE_MAX)
                          ? td_sheet_get_cell(right, r_phys, rnon[j])
                          : TD_VAL_NULL;

        if (!td_set_cloned_cell(out, dr, nkeys + nlnon + j, rv))
            return false;
    }
    return true;
}

static bool td_join_rows_differ(const td_sheet_t *left, size_t l_phys,
                                const td_sheet_t *right, size_t r_phys,
                                const size_t *lnon, size_t nlnon,
                                const size_t *rnon, size_t nrnon)
{
    size_t shared = 0;

    for (size_t i = 0; i < nlnon; i++) {
        const char *lname = left->cols[lnon[i]].name;
        size_t rc = td_find_col_index(right, lname);

        if (rc != SIZE_MAX && !right->cols[rc].is_key) {
            td_val_t lv = td_sheet_eval_cell(left, l_phys, lnon[i]);
            td_val_t rv = td_sheet_eval_cell(right, r_phys, rc);

            shared++;
            if (td_val_cmp(lv, rv) != 0)
                return true;
        }
    }
    if (shared > 0)
        return false;
    if (nlnon != nrnon)
        return true;
    for (size_t i = 0; i < nlnon; i++) {
        td_val_t lv = td_sheet_eval_cell(left, l_phys, lnon[i]);
        td_val_t rv = td_sheet_eval_cell(right, r_phys, rnon[i]);

        if (td_val_cmp(lv, rv) != 0)
            return true;
    }
    return false;
}

td_sheet_t *td_agg_join_sheets(const td_sheet_t *left, const td_sheet_t *right,
                               td_join_kind_t kind, char *err, size_t err_cap)
{
    size_t *lkeys = NULL;
    size_t *rkeys = NULL;
    size_t *lnon = NULL;
    size_t *rnon = NULL;
    size_t nlk, nrk, nlnon, nrnon;
    td_group_table_t rtbl;
    td_val_t *kbuf = NULL;
    bool *rmatched = NULL;
    td_sheet_t *out = NULL;
    char *name_buf = NULL;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!left || !right) {
        td_set_err(err, err_cap, "need two sheets to join");
        return NULL;
    }
    if (kind == TD_JOIN_APPEND)
        return td_join_append(left, right, err, err_cap);

    nlk = td_collect_cols(left, true, false, &lkeys);
    nrk = td_collect_cols(right, true, false, &rkeys);
    if (nlk == 0 || nrk == 0) {
        free(lkeys);
        free(rkeys);
        td_set_err(err, err_cap, "pin key column(s) with ! on both sheets");
        return NULL;
    }
    if (nlk != nrk) {
        if (err && err_cap > 0)
            snprintf(err, err_cap, "key column count mismatch (%zu vs %zu)",
                     nlk, nrk);
        free(lkeys);
        free(rkeys);
        return NULL;
    }

    nlnon = td_collect_cols(left, false, true, &lnon);
    nrnon = td_collect_cols(right, false, true, &rnon);

    td_group_table_init(&rtbl, nlk, true);
    kbuf = malloc(nlk * sizeof(*kbuf));
    if (!kbuf)
        goto oom;
    if (right->nrows > 0) {
        rmatched = calloc(right->nrows, sizeof(*rmatched));
        if (!rmatched)
            goto oom;
    }

    for (size_t vr = 0; vr < right->nrows; vr++) {
        size_t phys = right->row_order ? right->row_order[vr] : vr;

        for (size_t k = 0; k < nlk; k++)
            kbuf[k] = td_sheet_eval_cell(right, phys, rkeys[k]);
        if (!td_group_table_add(&rtbl, kbuf, vr, NULL))
            goto oom;
    }

    {
        size_t cap = strlen(left->name ? left->name : "left") +
                     strlen(right->name ? right->name : "right") + 24;

        name_buf = malloc(cap);
        if (!name_buf)
            goto oom;
        snprintf(name_buf, cap, "%s+%s_%s",
                 left->name ? left->name : "left",
                 right->name ? right->name : "right",
                 td_join_name(kind));
        out = td_sheet_new(name_buf, left->source_path);
        free(name_buf);
        name_buf = NULL;
        if (!out)
            goto oom;
    }

    for (size_t k = 0; k < nlk; k++) {
        const td_col_t *kc = &left->cols[lkeys[k]];
        size_t ci = td_sheet_add_col(out, kc->name, kc->type,
                                     -TD_AUTO_WIDTH_CAP);

        if (ci == SIZE_MAX)
            goto oom;
        out->cols[ci].is_key = true;
    }
    for (size_t i = 0; i < nlnon; i++) {
        const td_col_t *lc = &left->cols[lnon[i]];
        char *uname = td_unique_col_name(out, lc->name);

        if (!uname)
            goto oom;
        size_t ci = td_sheet_add_col(out, uname, lc->type, -TD_AUTO_WIDTH_CAP);
        free(uname);
        if (ci == SIZE_MAX)
            goto oom;
    }
    for (size_t j = 0; j < nrnon; j++) {
        const td_col_t *rc = &right->cols[rnon[j]];
        char *uname = td_unique_col_name(out, rc->name);

        if (!uname)
            goto oom;
        size_t ci = td_sheet_add_col(out, uname, rc->type, -TD_AUTO_WIDTH_CAP);
        free(uname);
        if (ci == SIZE_MAX)
            goto oom;
    }

    for (size_t vr = 0; vr < left->nrows; vr++) {
        size_t l_phys = left->row_order ? left->row_order[vr] : vr;
        const td_group_t *g;

        for (size_t k = 0; k < nlk; k++)
            kbuf[k] = td_sheet_eval_cell(left, l_phys, lkeys[k]);
        g = td_group_table_find(&rtbl, kbuf);

        if (g && g->count > 0) {
            for (size_t m = 0; m < g->count; m++) {
                size_t rvr = g->rows[m];
                size_t r_phys = right->row_order ? right->row_order[rvr] : rvr;

                rmatched[rvr] = true;
                if (kind == TD_JOIN_DIFF) {
                    if (td_join_rows_differ(left, l_phys, right, r_phys,
                                            lnon, nlnon, rnon, nrnon)) {
                        if (!td_join_emit_row(out, left, l_phys, right, r_phys,
                                              lkeys, rkeys, nlk, lnon, nlnon,
                                              rnon, nrnon))
                            goto oom;
                    }
                    continue;
                }
                if (!td_join_emit_row(out, left, l_phys, right, r_phys,
                                      lkeys, rkeys, nlk, lnon, nlnon,
                                      rnon, nrnon))
                    goto oom;
                if (kind == TD_JOIN_EXTEND)
                    break;
            }
        } else if (kind != TD_JOIN_INNER) {
            if (!td_join_emit_row(out, left, l_phys, right, SIZE_MAX,
                                  lkeys, rkeys, nlk, lnon, nlnon,
                                  rnon, nrnon))
                goto oom;
        }
    }

    if (kind == TD_JOIN_FULL || kind == TD_JOIN_DIFF) {
        for (size_t rvr = 0; rvr < right->nrows; rvr++) {
            size_t r_phys;

            if (rmatched[rvr])
                continue;
            r_phys = right->row_order ? right->row_order[rvr] : rvr;
            if (!td_join_emit_row(out, left, SIZE_MAX, right, r_phys,
                                  lkeys, rkeys, nlk, lnon, nlnon,
                                  rnon, nrnon))
                goto oom;
        }
    }

    free(lkeys);
    free(rkeys);
    free(lnon);
    free(rnon);
    free(kbuf);
    free(rmatched);
    td_group_table_free(&rtbl);
    return out;

oom:
    free(name_buf);
    free(lkeys);
    free(rkeys);
    free(lnon);
    free(rnon);
    free(kbuf);
    free(rmatched);
    td_group_table_free(&rtbl);
    td_sheet_free(out);
    td_set_err(err, err_cap, "out of memory");
    return NULL;
}

/* ---- Dive From Derived Sheets (Enter) ---- */

td_sheet_t *td_agg_dive_row(const td_sheet_t *derived, const td_stack_t *stack,
                            char *err, size_t err_cap)
{
    const td_sheet_t *src;
    size_t phys;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!derived || !stack || derived->nrows == 0 ||
        derived->cursor_row >= derived->nrows) {
        td_set_err(err, err_cap, "nothing to dive into");
        return NULL;
    }
    if (derived->kind != TD_SHEET_FREQ && derived->kind != TD_SHEET_DESCRIBE) {
        td_set_err(err, err_cap, "dive requires a frequency or describe sheet");
        return NULL;
    }

    src = td_stack_find_by_id(stack, derived->source_id);
    if (!src) {
        td_set_err(err, err_cap, "source sheet is no longer open");
        return NULL;
    }

    phys = derived->row_order ? derived->row_order[derived->cursor_row]
                              : derived->cursor_row;

    if (derived->kind == TD_SHEET_FREQ) {
        const td_freq_meta_t *m = derived->meta;
        const td_freq_bin_t *bin;
        td_sheet_t *child;

        if (!m || phys >= m->nbins) {
            td_set_err(err, err_cap, "missing frequency bin metadata");
            return NULL;
        }
        bin = &m->bins[phys];
        child = td_sheet_extract_phys_rows(src, m->all_phys + bin->offset,
                                           bin->count, "[freq]");
        if (!child)
            td_set_err(err, err_cap, "out of memory");
        return child;
    }

    const td_desc_meta_t *dm = derived->meta;

    if (!dm || phys >= dm->ncols) {
        td_set_err(err, err_cap, "missing describe metadata");
        return NULL;
    }
    return td_agg_freq_sheet(src, &dm->src_cols[phys], 1, err, err_cap);
}
