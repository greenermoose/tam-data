#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utf8.h"

typedef enum {
    J_NULL = 0,
    J_BOOL,
    J_INT,
    J_FLOAT,
    J_STR,
    J_ARR,
    J_OBJ
} jkind_t;

typedef struct jval jval_t;

typedef struct {
    char *key;
    jval_t *val;
} jkv_t;

struct jval {
    jkind_t kind;
    union {
        bool b;
        int64_t i;
        double d;
        char *s;
        struct {
            jval_t **items;
            size_t len;
            size_t cap;
        } arr;
        struct {
            jkv_t *pairs;
            size_t len;
            size_t cap;
        } obj;
    } as;
};

static void jerr(char *err, size_t cap, const char *msg)
{
    if (err && cap > 0)
        snprintf(err, cap, "%s", msg ? msg : "");
}

static void jval_free(jval_t *v)
{
    if (!v)
        return;
    switch (v->kind) {
    case J_STR:
        free(v->as.s);
        break;
    case J_ARR:
        for (size_t i = 0; i < v->as.arr.len; i++)
            jval_free(v->as.arr.items[i]);
        free(v->as.arr.items);
        break;
    case J_OBJ:
        for (size_t i = 0; i < v->as.obj.len; i++) {
            free(v->as.obj.pairs[i].key);
            jval_free(v->as.obj.pairs[i].val);
        }
        free(v->as.obj.pairs);
        break;
    default:
        break;
    }
    free(v);
}

static jval_t *jval_new(jkind_t kind)
{
    jval_t *v = calloc(1, sizeof(*v));

    if (v)
        v->kind = kind;
    return v;
}

typedef struct {
    const char *p;
    const char *end;
    int depth;
    char *err;
    size_t err_cap;
} jparse_t;

static void jskip_ws(jparse_t *jp)
{
    while (jp->p < jp->end) {
        char c = *jp->p;

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            jp->p++;
        else
            break;
    }
}

static int jhex4(const char *p, uint32_t *out)
{
    uint32_t v = 0;

    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)p[i];

        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (uint32_t)(c - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

static char *jparse_string_raw(jparse_t *jp)
{
    tam_str_t s;

    if (jp->p >= jp->end || *jp->p != '"') {
        jerr(jp->err, jp->err_cap, "expected '\"'");
        return NULL;
    }
    jp->p++;
    s = tam_str_new("");
    if (!s)
        return NULL;

    while (jp->p < jp->end) {
        unsigned char c = (unsigned char)*jp->p++;

        if (c == '"') {
            char *out = strdup(s);

            tam_str_free(s);
            return out;
        }
        if (c < 0x20) {
            tam_str_free(s);
            jerr(jp->err, jp->err_cap, "control char in JSON string");
            return NULL;
        }
        if (c != '\\') {
            char ch = (char)c;

            s = tam_str_cat_len(s, &ch, 1);
            if (!s)
                return NULL;
            continue;
        }
        if (jp->p >= jp->end) {
            tam_str_free(s);
            jerr(jp->err, jp->err_cap, "unterminated JSON escape");
            return NULL;
        }
        c = (unsigned char)*jp->p++;
        switch (c) {
        case '"':
        case '\\':
        case '/': {
            char ch = (char)c;
            s = tam_str_cat_len(s, &ch, 1);
            break;
        }
        case 'b': s = tam_str_cat_len(s, "\b", 1); break;
        case 'f': s = tam_str_cat_len(s, "\f", 1); break;
        case 'n': s = tam_str_cat_len(s, "\n", 1); break;
        case 'r': s = tam_str_cat_len(s, "\r", 1); break;
        case 't': s = tam_str_cat_len(s, "\t", 1); break;
        case 'u': {
            uint32_t cp = 0;
            char u8[4];
            int ulen;

            if (jp->end - jp->p < 4 || jhex4(jp->p, &cp) != 0) {
                tam_str_free(s);
                jerr(jp->err, jp->err_cap, "invalid \\u escape");
                return NULL;
            }
            jp->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && jp->end - jp->p >= 6 &&
                jp->p[0] == '\\' && jp->p[1] == 'u') {
                uint32_t low = 0;

                if (jhex4(jp->p + 2, &low) == 0 &&
                    low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                    jp->p += 6;
                }
            }
            ulen = td_utf8_encode(cp, u8);
            s = tam_str_cat_len(s, u8, (size_t)ulen);
            break;
        }
        default:
            tam_str_free(s);
            jerr(jp->err, jp->err_cap, "invalid JSON escape");
            return NULL;
        }
        if (!s)
            return NULL;
    }

    tam_str_free(s);
    jerr(jp->err, jp->err_cap, "unterminated JSON string");
    return NULL;
}

static jval_t *jparse_value(jparse_t *jp);

static jval_t *jparse_number(jparse_t *jp)
{
    const char *start = jp->p;
    bool is_float = false;
    char buf[128];
    size_t len;
    jval_t *v;

    if (jp->p < jp->end && *jp->p == '-')
        jp->p++;
    if (jp->p >= jp->end || !isdigit((unsigned char)*jp->p)) {
        jerr(jp->err, jp->err_cap, "invalid JSON number");
        return NULL;
    }
    while (jp->p < jp->end && isdigit((unsigned char)*jp->p))
        jp->p++;
    if (jp->p < jp->end && *jp->p == '.') {
        is_float = true;
        jp->p++;
        if (jp->p >= jp->end || !isdigit((unsigned char)*jp->p)) {
            jerr(jp->err, jp->err_cap, "invalid JSON fraction");
            return NULL;
        }
        while (jp->p < jp->end && isdigit((unsigned char)*jp->p))
            jp->p++;
    }
    if (jp->p < jp->end && (*jp->p == 'e' || *jp->p == 'E')) {
        is_float = true;
        jp->p++;
        if (jp->p < jp->end && (*jp->p == '+' || *jp->p == '-'))
            jp->p++;
        if (jp->p >= jp->end || !isdigit((unsigned char)*jp->p)) {
            jerr(jp->err, jp->err_cap, "invalid JSON exponent");
            return NULL;
        }
        while (jp->p < jp->end && isdigit((unsigned char)*jp->p))
            jp->p++;
    }

    len = (size_t)(jp->p - start);
    if (len >= sizeof(buf)) {
        jerr(jp->err, jp->err_cap, "JSON number too long");
        return NULL;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';

    if (!is_float) {
        char *endp = NULL;
        long long iv;

        errno = 0;
        iv = strtoll(buf, &endp, 10);
        if (errno == 0 && endp && *endp == '\0') {
            v = jval_new(J_INT);
            if (v)
                v->as.i = (int64_t)iv;
            return v;
        }
    }

    {
        char *endp = NULL;
        double dv;

        errno = 0;
        dv = strtod(buf, &endp);
        if (!endp || *endp != '\0') {
            jerr(jp->err, jp->err_cap, "invalid JSON number");
            return NULL;
        }
        v = jval_new(J_FLOAT);
        if (v)
            v->as.d = dv;
        return v;
    }
}

static jval_t *jparse_array(jparse_t *jp)
{
    jval_t *arr;

    if (jp->depth >= 128) {
        jerr(jp->err, jp->err_cap, "JSON nesting too deep");
        return NULL;
    }
    jp->depth++;
    jp->p++; /* '[' */
    arr = jval_new(J_ARR);
    if (!arr) {
        jp->depth--;
        return NULL;
    }

    jskip_ws(jp);
    if (jp->p < jp->end && *jp->p == ']') {
        jp->p++;
        jp->depth--;
        return arr;
    }

    for (;;) {
        jval_t *item = jparse_value(jp);

        if (!item) {
            jval_free(arr);
            jp->depth--;
            return NULL;
        }
        if (arr->as.arr.len == arr->as.arr.cap) {
            size_t ncap = arr->as.arr.cap ? arr->as.arr.cap * 2 : 8;
            jval_t **ni = realloc(arr->as.arr.items, ncap * sizeof(*ni));

            if (!ni) {
                jval_free(item);
                jval_free(arr);
                jp->depth--;
                return NULL;
            }
            arr->as.arr.items = ni;
            arr->as.arr.cap = ncap;
        }
        arr->as.arr.items[arr->as.arr.len++] = item;

        jskip_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') {
            jp->p++;
            continue;
        }
        if (jp->p < jp->end && *jp->p == ']') {
            jp->p++;
            break;
        }
        jerr(jp->err, jp->err_cap, "expected ',' or ']' in JSON array");
        jval_free(arr);
        jp->depth--;
        return NULL;
    }

    jp->depth--;
    return arr;
}

static jval_t *jparse_object(jparse_t *jp)
{
    jval_t *obj;

    if (jp->depth >= 128) {
        jerr(jp->err, jp->err_cap, "JSON nesting too deep");
        return NULL;
    }
    jp->depth++;
    jp->p++; /* '{' */
    obj = jval_new(J_OBJ);
    if (!obj) {
        jp->depth--;
        return NULL;
    }

    jskip_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') {
        jp->p++;
        jp->depth--;
        return obj;
    }

    for (;;) {
        char *key;
        jval_t *val;

        jskip_ws(jp);
        key = jparse_string_raw(jp);
        if (!key) {
            jval_free(obj);
            jp->depth--;
            return NULL;
        }
        jskip_ws(jp);
        if (jp->p >= jp->end || *jp->p != ':') {
            free(key);
            jerr(jp->err, jp->err_cap, "expected ':' in JSON object");
            jval_free(obj);
            jp->depth--;
            return NULL;
        }
        jp->p++;
        val = jparse_value(jp);
        if (!val) {
            free(key);
            jval_free(obj);
            jp->depth--;
            return NULL;
        }

        if (obj->as.obj.len == obj->as.obj.cap) {
            size_t ncap = obj->as.obj.cap ? obj->as.obj.cap * 2 : 8;
            jkv_t *np = realloc(obj->as.obj.pairs, ncap * sizeof(*np));

            if (!np) {
                free(key);
                jval_free(val);
                jval_free(obj);
                jp->depth--;
                return NULL;
            }
            obj->as.obj.pairs = np;
            obj->as.obj.cap = ncap;
        }
        obj->as.obj.pairs[obj->as.obj.len].key = key;
        obj->as.obj.pairs[obj->as.obj.len].val = val;
        obj->as.obj.len++;

        jskip_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') {
            jp->p++;
            continue;
        }
        if (jp->p < jp->end && *jp->p == '}') {
            jp->p++;
            break;
        }
        jerr(jp->err, jp->err_cap, "expected ',' or '}' in JSON object");
        jval_free(obj);
        jp->depth--;
        return NULL;
    }

    jp->depth--;
    return obj;
}

static jval_t *jparse_value(jparse_t *jp)
{
    jskip_ws(jp);
    if (jp->p >= jp->end) {
        jerr(jp->err, jp->err_cap, "unexpected end of JSON");
        return NULL;
    }
    switch (*jp->p) {
    case '"': {
        char *str = jparse_string_raw(jp);
        jval_t *v;

        if (!str)
            return NULL;
        v = jval_new(J_STR);
        if (!v) {
            free(str);
            return NULL;
        }
        v->as.s = str;
        return v;
    }
    case '{':
        return jparse_object(jp);
    case '[':
        return jparse_array(jp);
    case 't':
        if (jp->end - jp->p >= 4 && memcmp(jp->p, "true", 4) == 0) {
            jval_t *v = jval_new(J_BOOL);

            jp->p += 4;
            if (v)
                v->as.b = true;
            return v;
        }
        break;
    case 'f':
        if (jp->end - jp->p >= 5 && memcmp(jp->p, "false", 5) == 0) {
            jval_t *v = jval_new(J_BOOL);

            jp->p += 5;
            if (v)
                v->as.b = false;
            return v;
        }
        break;
    case 'n':
        if (jp->end - jp->p >= 4 && memcmp(jp->p, "null", 4) == 0) {
            jp->p += 4;
            return jval_new(J_NULL);
        }
        break;
    default:
        if (*jp->p == '-' || isdigit((unsigned char)*jp->p))
            return jparse_number(jp);
        break;
    }
    jerr(jp->err, jp->err_cap, "unexpected token in JSON");
    return NULL;
}

static tam_str_t jval_stringify(tam_str_t out, const jval_t *v)
{
    if (!v)
        return tam_str_cat(out, "null");

    switch (v->kind) {
    case J_NULL:
        return tam_str_cat(out, "null");
    case J_BOOL:
        return tam_str_cat(out, v->as.b ? "true" : "false");
    case J_INT:
        return tam_str_cat_printf(out, "%lld", (long long)v->as.i);
    case J_FLOAT:
        if (isfinite(v->as.d))
            return tam_str_cat_printf(out, "%.15g", v->as.d);
        return tam_str_cat(out, "null");
    case J_STR:
        out = tam_str_cat_len(out, "\"", 1);
        out = tam_str_cat_json(out, v->as.s ? v->as.s : "");
        return tam_str_cat_len(out, "\"", 1);
    case J_ARR:
        out = tam_str_cat_len(out, "[", 1);
        for (size_t i = 0; i < v->as.arr.len; i++) {
            if (i > 0)
                out = tam_str_cat_len(out, ",", 1);
            out = jval_stringify(out, v->as.arr.items[i]);
        }
        return tam_str_cat_len(out, "]", 1);
    case J_OBJ:
        out = tam_str_cat_len(out, "{", 1);
        for (size_t i = 0; i < v->as.obj.len; i++) {
            if (i > 0)
                out = tam_str_cat_len(out, ",", 1);
            out = tam_str_cat_len(out, "\"", 1);
            out = tam_str_cat_json(out, v->as.obj.pairs[i].key);
            out = tam_str_cat_len(out, "\":", 2);
            out = jval_stringify(out, v->as.obj.pairs[i].val);
        }
        return tam_str_cat_len(out, "}", 1);
    }
    return out;
}

static td_val_t td_make_str_val(const char *s, size_t len)
{
    td_val_t v;

    v.type = TD_TYPE_STR;
    v.as.s.data = s;
    v.as.s.len = (uint32_t)len;
    return v;
}

static td_val_t jval_to_td(td_sheet_t *s, const jval_t *v)
{
    if (!v || v->kind == J_NULL)
        return TD_VAL_NULL;
    switch (v->kind) {
    case J_BOOL:
        return td_make_str_val(v->as.b ? "true" : "false", v->as.b ? 4 : 5);
    case J_INT: {
        td_val_t out;

        out.type = TD_TYPE_INT;
        out.as.i = v->as.i;
        return out;
    }
    case J_FLOAT: {
        td_val_t out;

        out.type = TD_TYPE_FLOAT;
        out.as.f = v->as.d;
        return out;
    }
    case J_STR: {
        const char *src = v->as.s ? v->as.s : "";
        size_t len = strlen(src);
        char *copy = td_arena_strdup(&s->arena, src);

        return copy ? td_make_str_val(copy, len) : TD_VAL_NULL;
    }
    case J_ARR:
    case J_OBJ: {
        tam_str_t tmp = jval_stringify(tam_str_new(""), v);
        char *copy = tmp ? td_arena_strdup(&s->arena, tmp) : NULL;
        size_t len = tmp ? tam_str_len(tmp) : 0;

        tam_str_free(tmp);
        return copy ? td_make_str_val(copy, len) : TD_VAL_NULL;
    }
    default:
        return TD_VAL_NULL;
    }
}

static size_t jsheet_find_or_add_col(td_sheet_t *s, const char *name)
{
    for (size_t c = 0; c < s->ncols; c++) {
        if (strcmp(s->cols[c].name, name) == 0)
            return c;
    }
    return td_sheet_add_col(s, name, TD_TYPE_ANY, -TD_AUTO_WIDTH_CAP);
}

static void jsheet_infer_col_types(td_sheet_t *s)
{
    for (size_t c = 0; c < s->ncols; c++) {
        bool has_int = false;
        bool has_float = false;
        bool has_other = false;

        for (size_t r = 0; r < s->nrows; r++) {
            td_val_t v = td_sheet_get_cell(s, r, c);

            if (v.type == TD_TYPE_NULL)
                continue;
            if (v.type == TD_TYPE_INT)
                has_int = true;
            else if (v.type == TD_TYPE_FLOAT)
                has_float = true;
            else {
                has_other = true;
                break;
            }
        }
        if (has_other)
            s->cols[c].type = TD_TYPE_STR;
        else if (has_float)
            s->cols[c].type = TD_TYPE_FLOAT;
        else if (has_int)
            s->cols[c].type = TD_TYPE_INT;
    }
}

static td_sheet_t *jrows_to_sheet(const char *name, const char *source_path,
                                  jval_t *const *rows, size_t nrows)
{
    td_sheet_t *s = td_sheet_new(name, source_path);
    bool any_obj = false;

    if (!s)
        return NULL;

    for (size_t i = 0; i < nrows; i++) {
        if (rows[i] && rows[i]->kind == J_OBJ) {
            any_obj = true;
            break;
        }
    }

    if (any_obj) {
        for (size_t i = 0; i < nrows; i++) {
            jval_t *row = rows[i];
            size_t ridx = td_sheet_add_row(s);

            if (ridx == SIZE_MAX) {
                td_sheet_free(s);
                return NULL;
            }
            if (!row)
                continue;
            if (row->kind == J_OBJ) {
                for (size_t k = 0; k < row->as.obj.len; k++) {
                    size_t col = jsheet_find_or_add_col(s, row->as.obj.pairs[k].key);

                    if (col == SIZE_MAX) {
                        td_sheet_free(s);
                        return NULL;
                    }
                    td_sheet_set_cell(s, ridx, col,
                                      jval_to_td(s, row->as.obj.pairs[k].val));
                }
            } else {
                size_t col = jsheet_find_or_add_col(s, "value");

                if (col == SIZE_MAX) {
                    td_sheet_free(s);
                    return NULL;
                }
                td_sheet_set_cell(s, ridx, col, jval_to_td(s, row));
            }
        }
    } else {
        size_t col = td_sheet_add_col(s, "value", TD_TYPE_ANY, -TD_AUTO_WIDTH_CAP);

        if (col == SIZE_MAX) {
            td_sheet_free(s);
            return NULL;
        }
        for (size_t i = 0; i < nrows; i++) {
            size_t ridx = td_sheet_add_row(s);

            if (ridx == SIZE_MAX) {
                td_sheet_free(s);
                return NULL;
            }
            td_sheet_set_cell(s, ridx, col, jval_to_td(s, rows[i]));
        }
    }

    jsheet_infer_col_types(s);
    td_sheet_set_undo_recording(s, true);
    return s;
}

td_sheet_t *td_json_load_mem(const char *name, const char *source_path,
                             const char *buf, size_t len, bool jsonl,
                             char *err, size_t err_cap)
{
    jparse_t jp = { buf, buf + len, 0, err, err_cap };
    td_sheet_t *s = NULL;

    if (!buf && len > 0) {
        jerr(err, err_cap, "invalid JSON buffer");
        return NULL;
    }

    if (jsonl) {
        jval_t **items = NULL;
        size_t n = 0;
        size_t cap = 0;

        while (jp.p < jp.end) {
            jval_t *v;

            jskip_ws(&jp);
            if (jp.p >= jp.end)
                break;
            v = jparse_value(&jp);
            if (!v) {
                for (size_t i = 0; i < n; i++)
                    jval_free(items[i]);
                free(items);
                return NULL;
            }
            if (n == cap) {
                size_t ncap = cap ? cap * 2 : 16;
                jval_t **ni = realloc(items, ncap * sizeof(*ni));

                if (!ni) {
                    jval_free(v);
                    for (size_t i = 0; i < n; i++)
                        jval_free(items[i]);
                    free(items);
                    return NULL;
                }
                items = ni;
                cap = ncap;
            }
            items[n++] = v;
        }
        s = jrows_to_sheet(name ? name : "jsonl", source_path, items, n);
        for (size_t i = 0; i < n; i++)
            jval_free(items[i]);
        free(items);
        return s;
    }

    {
        jval_t *root;

        jskip_ws(&jp);
        if (jp.p >= jp.end) {
            return td_sheet_new(name ? name : "json", source_path);
        }
        root = jparse_value(&jp);
        if (!root)
            return NULL;
        if (root->kind == J_ARR) {
            s = jrows_to_sheet(name ? name : "json", source_path,
                               root->as.arr.items, root->as.arr.len);
        } else {
            s = jrows_to_sheet(name ? name : "json", source_path, &root, 1);
        }
        jval_free(root);
        return s;
    }
}

td_sheet_t *td_json_load_file(const char *path, bool jsonl,
                              char *err, size_t err_cap)
{
    size_t len = 0;
    char *buf;
    td_sheet_t *s;

    if (!path) {
        jerr(err, err_cap, "missing JSON path");
        return NULL;
    }
    buf = tam_fs_read_file(path, &len);
    if (!buf) {
        if (err && err_cap > 0)
            snprintf(err, err_cap, "cannot read %s: %s", path, strerror(errno));
        return NULL;
    }
    s = td_json_load_mem(path, path, buf, len, jsonl, err, err_cap);
    free(buf);
    return s;
}

static tam_str_t td_append_json_cell(tam_str_t out, const td_sheet_t *s,
                                     size_t phys_row, size_t col)
{
    td_val_t v = td_sheet_eval_cell(s, phys_row, col);
    char buf[256];

    switch (v.type) {
    case TD_TYPE_NULL:
        return tam_str_cat(out, "null");
    case TD_TYPE_INT:
        return tam_str_cat_printf(out, "%lld", (long long)v.as.i);
    case TD_TYPE_FLOAT:
    case TD_TYPE_CURRENCY:
        if (isfinite(v.as.f))
            return tam_str_cat_printf(out, "%.15g", v.as.f);
        return tam_str_cat(out, "null");
    default:
        td_val_format(v, s->cols[col].type, buf, sizeof(buf));
        out = tam_str_cat_len(out, "\"", 1);
        out = tam_str_cat_json(out, buf);
        return tam_str_cat_len(out, "\"", 1);
    }
}

tam_str_t td_json_format_sheet(const td_sheet_t *s, bool jsonl)
{
    tam_str_t out;

    if (!s)
        return NULL;
    out = tam_str_new(jsonl ? "" : "[\n");
    if (!out)
        return NULL;

    for (size_t vr = 0; vr < s->nrows; vr++) {
        size_t phys = s->row_order[vr];
        bool first_col = true;

        if (!jsonl && vr > 0)
            out = tam_str_cat(out, ",\n");
        out = tam_str_cat(out, jsonl ? "{" : "  {");

        for (size_t c = 0; c < s->ncols; c++) {
            if (td_sheet_col_width(s, c) <= 0)
                continue;
            if (!first_col)
                out = tam_str_cat_len(out, ", ", 2);
            first_col = false;
            out = tam_str_cat_len(out, "\"", 1);
            out = tam_str_cat_json(out, s->cols[c].name);
            out = tam_str_cat_len(out, "\": ", 3);
            out = td_append_json_cell(out, s, phys, c);
            if (!out)
                return NULL;
        }
        out = tam_str_cat(out, jsonl ? "}\n" : "}");
        if (!out)
            return NULL;
    }

    if (!jsonl)
        out = tam_str_cat(out, s->nrows > 0 ? "\n]\n" : "]\n");
    return out;
}

bool td_json_save_sheet(td_sheet_t *s, const char *path, bool jsonl,
                        char *err, size_t err_cap)
{
    tam_str_t out;
    bool ok;

    if (!s || !path || !path[0]) {
        jerr(err, err_cap, "invalid save path");
        return false;
    }
    out = td_json_format_sheet(s, jsonl);
    if (!out) {
        jerr(err, err_cap, "out of memory formatting JSON");
        return false;
    }
    ok = tam_fs_write_atomic(path, out, tam_str_len(out), 0644);
    if (!ok && err && err_cap > 0)
        snprintf(err, err_cap, "failed to save %s: %s", path, strerror(errno));
    tam_str_free(out);
    return ok;
}
