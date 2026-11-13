#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "expr.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "utf8.h"

#define TD_EXPR_MAX_DEPTH 128

typedef enum {
    TD_TOK_EOF = 0,
    TD_TOK_ERR,
    TD_TOK_INT,
    TD_TOK_FLOAT,
    TD_TOK_STR,
    TD_TOK_IDENT,
    TD_TOK_COL_REF,
    TD_TOK_TRUE,
    TD_TOK_FALSE,
    TD_TOK_NULL,
    TD_TOK_PLUS,
    TD_TOK_MINUS,
    TD_TOK_STAR,
    TD_TOK_POW,
    TD_TOK_SLASH,
    TD_TOK_PERCENT,
    TD_TOK_EQ,
    TD_TOK_NE,
    TD_TOK_LT,
    TD_TOK_LE,
    TD_TOK_GT,
    TD_TOK_GE,
    TD_TOK_AND,
    TD_TOK_OR,
    TD_TOK_BANG,
    TD_TOK_QMARK,
    TD_TOK_COLON,
    TD_TOK_LPAREN,
    TD_TOK_RPAREN,
    TD_TOK_COMMA
} td_tok_kind_t;

typedef struct {
    td_tok_kind_t kind;
    union {
        int64_t i;
        double f;
        struct {
            const char *data;
            size_t len;
        } s;
        size_t col_idx;
    } as;
} td_tok_t;

typedef enum {
    TD_FN_LEN = 0,
    TD_FN_LOWER,
    TD_FN_UPPER,
    TD_FN_TRIM,
    TD_FN_SUBSTR,
    TD_FN_REPLACE,
    TD_FN_CONTAINS,
    TD_FN_MATCH,
    TD_FN_ABS,
    TD_FN_ROUND,
    TD_FN_FLOOR,
    TD_FN_CEIL,
    TD_FN_MIN,
    TD_FN_MAX,
    TD_FN_LOG,
    TD_FN_SQRT,
    TD_FN_YEAR,
    TD_FN_MONTH,
    TD_FN_DAY,
    TD_FN_DATEDIFF,
    TD_FN_COUNT
} td_fn_kind_t;

typedef enum {
    TD_NODE_LIT = 0,
    TD_NODE_COL,
    TD_NODE_UNARY,
    TD_NODE_BIN,
    TD_NODE_TERNARY,
    TD_NODE_CALL
} td_node_kind_t;

typedef struct td_expr_node td_expr_node_t;

struct td_expr_node {
    td_node_kind_t kind;
    union {
        td_val_t lit;
        size_t col_idx;
        struct {
            td_tok_kind_t op;
            td_expr_node_t *operand;
        } unary;
        struct {
            td_tok_kind_t op;
            td_expr_node_t *left;
            td_expr_node_t *right;
        } bin;
        struct {
            td_expr_node_t *cond;
            td_expr_node_t *then_br;
            td_expr_node_t *else_br;
        } ternary;
        struct {
            td_fn_kind_t fn;
            td_expr_node_t **args;
            size_t nargs;
            regex_t *compiled_re;
        } call;
    } as;
};

typedef struct td_regex_item {
    regex_t re;
    struct td_regex_item *next;
} td_regex_item_t;

struct td_expr {
    td_arena_t arena;
    char *source;
    td_expr_node_t *root;
    td_regex_item_t *regexes;
};

typedef struct {
    const char *name;
    td_fn_kind_t kind;
    size_t min_args;
    size_t max_args;
} td_fn_desc_t;

static const td_fn_desc_t td_fn_table[TD_FN_COUNT] = {
    { "len",      TD_FN_LEN,      1, 1 },
    { "lower",    TD_FN_LOWER,    1, 1 },
    { "upper",    TD_FN_UPPER,    1, 1 },
    { "trim",     TD_FN_TRIM,     1, 1 },
    { "substr",   TD_FN_SUBSTR,   2, 3 },
    { "replace",  TD_FN_REPLACE,  3, 3 },
    { "contains", TD_FN_CONTAINS, 2, 2 },
    { "match",    TD_FN_MATCH,    2, 2 },
    { "abs",      TD_FN_ABS,      1, 1 },
    { "round",    TD_FN_ROUND,    1, 2 },
    { "floor",    TD_FN_FLOOR,    1, 1 },
    { "ceil",     TD_FN_CEIL,     1, 1 },
    { "min",      TD_FN_MIN,      1, 64 },
    { "max",      TD_FN_MAX,      1, 64 },
    { "log",      TD_FN_LOG,      1, 1 },
    { "sqrt",     TD_FN_SQRT,     1, 1 },
    { "year",     TD_FN_YEAR,     1, 1 },
    { "month",    TD_FN_MONTH,    1, 1 },
    { "day",      TD_FN_DAY,      1, 1 },
    { "datediff", TD_FN_DATEDIFF, 2, 3 }
};

typedef struct {
    const td_sheet_t *sheet;
    td_expr_t *expr;
    const char *src;
    const char *p;
    td_tok_t tok;
    int depth;
    bool failed;
    char err[256];
} td_parser_t;

static void td_set_err(char *err, size_t err_cap, const char *fmt, ...)
{
    va_list ap;

    if (!err || err_cap == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, err_cap, fmt, ap);
    va_end(ap);
}

static void td_parse_fail(td_parser_t *p, const char *fmt, ...)
{
    va_list ap;

    if (p->failed)
        return;
    p->failed = true;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof(p->err), fmt, ap);
    va_end(ap);
    p->tok.kind = TD_TOK_ERR;
}

static size_t td_lookup_col(const td_sheet_t *s, const char *name, size_t len)
{
    if (!s || !name)
        return SIZE_MAX;
    for (size_t c = 0; c < s->ncols; c++) {
        const char *cn = s->cols[c].name;

        if (cn && strlen(cn) == len && memcmp(cn, name, len) == 0)
            return c;
    }
    return SIZE_MAX;
}

static bool td_is_ident_start(unsigned char c)
{
    return isalpha(c) || c == '_';
}

static bool td_is_ident_cont(unsigned char c)
{
    return isalnum(c) || c == '_';
}

static char td_unescape_char(char c)
{
    switch (c) {
    case 'n':  return '\n';
    case 't':  return '\t';
    case 'r':  return '\r';
    case '\\': return '\\';
    case '"':  return '"';
    case '\'': return '\'';
    case '}':  return '}';
    default:   return c;
    }
}

static void td_scan_string(td_parser_t *p, char quote)
{
    const char *start = ++p->p;
    char *buf;
    size_t len = 0;

    while (*p->p && *p->p != quote) {
        if (*p->p == '\\' && p->p[1])
            p->p += 2;
        else
            p->p++;
        len++;
    }
    if (*p->p != quote) {
        td_parse_fail(p, "unterminated string literal");
        return;
    }

    buf = td_arena_alloc(&p->expr->arena, len + 1);
    if (!buf) {
        td_parse_fail(p, "out of memory");
        return;
    }

    size_t out = 0;

    for (const char *q = start; q < p->p; q++) {
        if (*q == '\\' && q + 1 < p->p) {
            q++;
            buf[out++] = td_unescape_char(*q);
        } else {
            buf[out++] = *q;
        }
    }
    buf[out] = '\0';
    p->p++; /* consume closing quote */
    p->tok.kind = TD_TOK_STR;
    p->tok.as.s.data = buf;
    p->tok.as.s.len = out;
}

static void td_scan_col_ref(td_parser_t *p)
{
    p->p++; /* skip '$' */
    if (*p->p == '{') {
        const char *start = ++p->p;
        char *buf;
        size_t raw_len = 0;
        size_t col;

        while (*p->p && *p->p != '}') {
            if (*p->p == '\\' && p->p[1])
                p->p += 2;
            else
                p->p++;
            raw_len++;
        }
        if (*p->p != '}') {
            td_parse_fail(p, "unterminated column reference '${...}'");
            return;
        }
        buf = td_arena_alloc(&p->expr->arena, raw_len + 1);
        if (!buf) {
            td_parse_fail(p, "out of memory");
            return;
        }
        size_t out = 0;

        for (const char *q = start; q < p->p; q++) {
            if (*q == '\\' && q + 1 < p->p) {
                q++;
                buf[out++] = td_unescape_char(*q);
            } else {
                buf[out++] = *q;
            }
        }
        buf[out] = '\0';
        p->p++; /* skip '}' */
        col = td_lookup_col(p->sheet, buf, out);
        if (col == SIZE_MAX) {
            td_parse_fail(p, "unknown column '%s'", buf);
            return;
        }
        p->tok.kind = TD_TOK_COL_REF;
        p->tok.as.col_idx = col;
        return;
    }

    if (isdigit((unsigned char)*p->p)) {
        char *end = NULL;
        unsigned long long idx;

        errno = 0;
        idx = strtoull(p->p, &end, 10);
        if (errno == ERANGE || end == p->p ||
            !p->sheet || idx >= p->sheet->ncols) {
            td_parse_fail(p, "column index out of range: $%.*s",
                          (int)(end > p->p ? end - p->p : 1), p->p);
            return;
        }
        p->p = end;
        p->tok.kind = TD_TOK_COL_REF;
        p->tok.as.col_idx = (size_t)idx;
        return;
    }

    td_parse_fail(p, "expected column index or '${name}' after '$'");
}

static void td_scan_number(td_parser_t *p)
{
    const char *start = p->p;
    bool is_float = false;

    while (isdigit((unsigned char)*p->p))
        p->p++;
    if (*p->p == '.') {
        is_float = true;
        p->p++;
        while (isdigit((unsigned char)*p->p))
            p->p++;
    }
    if (*p->p == 'e' || *p->p == 'E') {
        is_float = true;
        p->p++;
        if (*p->p == '+' || *p->p == '-')
            p->p++;
        if (!isdigit((unsigned char)*p->p)) {
            td_parse_fail(p, "invalid exponent in number");
            return;
        }
        while (isdigit((unsigned char)*p->p))
            p->p++;
    }

    size_t len = (size_t)(p->p - start);
    char stack_buf[64];
    char *num_str = stack_buf;

    if (len >= sizeof(stack_buf)) {
        num_str = td_arena_alloc(&p->expr->arena, len + 1);
        if (!num_str) {
            td_parse_fail(p, "out of memory");
            return;
        }
    }
    memcpy(num_str, start, len);
    num_str[len] = '\0';

    if (!is_float) {
        char *end = NULL;
        long long iv;

        errno = 0;
        iv = strtoll(num_str, &end, 10);
        if (errno != ERANGE && end == num_str + len) {
            p->tok.kind = TD_TOK_INT;
            p->tok.as.i = (int64_t)iv;
            return;
        }
        is_float = true;
    }

    {
        char *end = NULL;
        double fv;

        errno = 0;
        fv = strtod(num_str, &end);
        if (errno == ERANGE || end != num_str + len || !isfinite(fv)) {
            td_parse_fail(p, "numeric literal out of range: %s", num_str);
            return;
        }
        p->tok.kind = TD_TOK_FLOAT;
        p->tok.as.f = fv;
    }
}

static void td_next_tok(td_parser_t *p)
{
    if (p->failed) {
        p->tok.kind = TD_TOK_ERR;
        return;
    }
    while (*p->p && isspace((unsigned char)*p->p))
        p->p++;

    if (*p->p == '\0') {
        p->tok.kind = TD_TOK_EOF;
        return;
    }

    char c = *p->p;

    if (c == '"' || c == '\'') {
        td_scan_string(p, c);
        return;
    }
    if (c == '$') {
        td_scan_col_ref(p);
        return;
    }
    if (isdigit((unsigned char)c) ||
        (c == '.' && isdigit((unsigned char)p->p[1]))) {
        td_scan_number(p);
        return;
    }
    if (td_is_ident_start((unsigned char)c)) {
        const char *start = p->p;
        size_t len;

        while (td_is_ident_cont((unsigned char)*p->p))
            p->p++;
        len = (size_t)(p->p - start);
        if (len == 4 && memcmp(start, "true", 4) == 0) {
            p->tok.kind = TD_TOK_TRUE;
        } else if (len == 5 && memcmp(start, "false", 5) == 0) {
            p->tok.kind = TD_TOK_FALSE;
        } else if (len == 4 && (memcmp(start, "null", 4) == 0 ||
                                memcmp(start, "NULL", 4) == 0)) {
            p->tok.kind = TD_TOK_NULL;
        } else {
            p->tok.kind = TD_TOK_IDENT;
            p->tok.as.s.data = start;
            p->tok.as.s.len = len;
        }
        return;
    }

    p->p++;
    switch (c) {
    case '+': p->tok.kind = TD_TOK_PLUS; break;
    case '-': p->tok.kind = TD_TOK_MINUS; break;
    case '/': p->tok.kind = TD_TOK_SLASH; break;
    case '%': p->tok.kind = TD_TOK_PERCENT; break;
    case '?': p->tok.kind = TD_TOK_QMARK; break;
    case ':': p->tok.kind = TD_TOK_COLON; break;
    case '(': p->tok.kind = TD_TOK_LPAREN; break;
    case ')': p->tok.kind = TD_TOK_RPAREN; break;
    case ',': p->tok.kind = TD_TOK_COMMA; break;
    case '*':
        if (*p->p == '*') {
            p->p++;
            p->tok.kind = TD_TOK_POW;
        } else {
            p->tok.kind = TD_TOK_STAR;
        }
        break;
    case '=':
        if (*p->p == '=') {
            p->p++;
            p->tok.kind = TD_TOK_EQ;
        } else {
            td_parse_fail(p, "unexpected '=' (use '==' for equality)");
        }
        break;
    case '!':
        if (*p->p == '=') {
            p->p++;
            p->tok.kind = TD_TOK_NE;
        } else {
            p->tok.kind = TD_TOK_BANG;
        }
        break;
    case '<':
        if (*p->p == '=') {
            p->p++;
            p->tok.kind = TD_TOK_LE;
        } else {
            p->tok.kind = TD_TOK_LT;
        }
        break;
    case '>':
        if (*p->p == '=') {
            p->p++;
            p->tok.kind = TD_TOK_GE;
        } else {
            p->tok.kind = TD_TOK_GT;
        }
        break;
    case '&':
        if (*p->p == '&') {
            p->p++;
            p->tok.kind = TD_TOK_AND;
        } else {
            td_parse_fail(p, "unexpected '&' (use '&&' for logical AND)");
        }
        break;
    case '|':
        if (*p->p == '|') {
            p->p++;
            p->tok.kind = TD_TOK_OR;
        } else {
            td_parse_fail(p, "unexpected '|' (use '||' for logical OR)");
        }
        break;
    default:
        td_parse_fail(p, "unexpected character '%c'", c);
        break;
    }
}

static td_expr_node_t *td_alloc_node(td_parser_t *p, td_node_kind_t kind)
{
    td_expr_node_t *n = td_arena_calloc(&p->expr->arena, sizeof(*n));

    if (!n) {
        td_parse_fail(p, "out of memory");
        return NULL;
    }
    n->kind = kind;
    return n;
}

static const td_fn_desc_t *td_lookup_fn(const char *name, size_t len)
{
    for (size_t i = 0; i < TD_FN_COUNT; i++) {
        if (strlen(td_fn_table[i].name) == len &&
            memcmp(td_fn_table[i].name, name, len) == 0)
            return &td_fn_table[i];
    }
    return NULL;
}

static td_expr_node_t *td_parse_ternary(td_parser_t *p);

static td_expr_node_t *td_parse_primary(td_parser_t *p)
{
    if (p->failed)
        return NULL;
    if (++p->depth > TD_EXPR_MAX_DEPTH) {
        td_parse_fail(p, "expression nesting exceeds maximum depth");
        return NULL;
    }

    td_expr_node_t *node = NULL;

    switch (p->tok.kind) {
    case TD_TOK_INT:
        node = td_alloc_node(p, TD_NODE_LIT);
        if (node) {
            node->as.lit.type = TD_TYPE_INT;
            node->as.lit.as.i = p->tok.as.i;
        }
        td_next_tok(p);
        break;
    case TD_TOK_FLOAT:
        node = td_alloc_node(p, TD_NODE_LIT);
        if (node) {
            node->as.lit.type = TD_TYPE_FLOAT;
            node->as.lit.as.f = p->tok.as.f;
        }
        td_next_tok(p);
        break;
    case TD_TOK_STR:
        node = td_alloc_node(p, TD_NODE_LIT);
        if (node) {
            node->as.lit.type = TD_TYPE_STR;
            node->as.lit.as.s.data = p->tok.as.s.data;
            node->as.lit.as.s.len = p->tok.as.s.len > UINT32_MAX
                                        ? UINT32_MAX
                                        : (uint32_t)p->tok.as.s.len;
        }
        td_next_tok(p);
        break;
    case TD_TOK_TRUE:
        node = td_alloc_node(p, TD_NODE_LIT);
        if (node) {
            node->as.lit.type = TD_TYPE_INT;
            node->as.lit.as.i = 1;
        }
        td_next_tok(p);
        break;
    case TD_TOK_FALSE:
        node = td_alloc_node(p, TD_NODE_LIT);
        if (node) {
            node->as.lit.type = TD_TYPE_INT;
            node->as.lit.as.i = 0;
        }
        td_next_tok(p);
        break;
    case TD_TOK_NULL:
        node = td_alloc_node(p, TD_NODE_LIT);
        if (node)
            node->as.lit = TD_VAL_NULL;
        td_next_tok(p);
        break;
    case TD_TOK_COL_REF:
        node = td_alloc_node(p, TD_NODE_COL);
        if (node)
            node->as.col_idx = p->tok.as.col_idx;
        td_next_tok(p);
        break;
    case TD_TOK_LPAREN:
        td_next_tok(p);
        node = td_parse_ternary(p);
        if (!p->failed) {
            if (p->tok.kind != TD_TOK_RPAREN)
                td_parse_fail(p, "expected ')'");
            else
                td_next_tok(p);
        }
        break;
    case TD_TOK_IDENT: {
        const char *name = p->tok.as.s.data;
        size_t len = p->tok.as.s.len;

        td_next_tok(p);
        if (p->tok.kind == TD_TOK_LPAREN) {
            const td_fn_desc_t *fn = td_lookup_fn(name, len);
            td_expr_node_t *args_buf[64];
            size_t nargs = 0;

            if (!fn) {
                td_parse_fail(p, "unknown function '%.*s'", (int)len, name);
                break;
            }
            td_next_tok(p); /* skip '(' */
            if (p->tok.kind != TD_TOK_RPAREN) {
                for (;;) {
                    td_expr_node_t *arg = td_parse_ternary(p);

                    if (p->failed)
                        break;
                    if (nargs >= 64) {
                        td_parse_fail(p, "too many arguments to %s()",
                                      fn->name);
                        break;
                    }
                    args_buf[nargs++] = arg;
                    if (p->tok.kind == TD_TOK_COMMA) {
                        td_next_tok(p);
                        continue;
                    }
                    break;
                }
            }
            if (!p->failed && p->tok.kind != TD_TOK_RPAREN)
                td_parse_fail(p, "expected ')' after arguments to %s()",
                              fn->name);
            if (!p->failed)
                td_next_tok(p); /* skip ')' */
            if (!p->failed &&
                (nargs < fn->min_args || nargs > fn->max_args)) {
                td_parse_fail(p, "%s() expects %zu..%zu arguments, got %zu",
                              fn->name, fn->min_args, fn->max_args, nargs);
            }
            if (!p->failed) {
                node = td_alloc_node(p, TD_NODE_CALL);
                if (node) {
                    node->as.call.fn = fn->kind;
                    node->as.call.nargs = nargs;
                    if (nargs > 0) {
                        node->as.call.args = td_arena_alloc(
                            &p->expr->arena, nargs * sizeof(td_expr_node_t *));
                        if (!node->as.call.args) {
                            td_parse_fail(p, "out of memory");
                            break;
                        }
                        memcpy(node->as.call.args, args_buf,
                               nargs * sizeof(td_expr_node_t *));
                    }
                    if (fn->kind == TD_FN_MATCH && nargs == 2 &&
                        args_buf[1]->kind == TD_NODE_LIT &&
                        args_buf[1]->as.lit.type == TD_TYPE_STR) {
                        td_regex_item_t *item = calloc(1, sizeof(*item));
                        const char *pat = args_buf[1]->as.lit.as.s.data;
                        int rc;

                        if (!item) {
                            td_parse_fail(p, "out of memory");
                            break;
                        }
                        rc = regcomp(&item->re, pat ? pat : "",
                                     REG_EXTENDED | REG_NOSUB);
                        if (rc != 0) {
                            char rerr[128];

                            regerror(rc, &item->re, rerr, sizeof(rerr));
                            free(item);
                            td_parse_fail(p, "invalid regex in match(): %s",
                                          rerr);
                            break;
                        }
                        item->next = p->expr->regexes;
                        p->expr->regexes = item;
                        node->as.call.compiled_re = &item->re;
                    }
                }
            }
        } else {
            size_t col = td_lookup_col(p->sheet, name, len);

            if (col == SIZE_MAX) {
                td_parse_fail(p, "unknown column '%.*s'", (int)len, name);
                break;
            }
            node = td_alloc_node(p, TD_NODE_COL);
            if (node)
                node->as.col_idx = col;
        }
        break;
    }
    default:
        td_parse_fail(p, "expected expression");
        break;
    }

    p->depth--;
    return node;
}

static td_expr_node_t *td_parse_unary(td_parser_t *p);

static td_expr_node_t *td_parse_pow(td_parser_t *p)
{
    td_expr_node_t *left = td_parse_primary(p);

    if (p->failed)
        return NULL;
    if (p->tok.kind == TD_TOK_POW) {
        td_expr_node_t *right;
        td_expr_node_t *n;

        td_next_tok(p);
        right = td_parse_unary(p);
        if (p->failed)
            return NULL;
        n = td_alloc_node(p, TD_NODE_BIN);
        if (n) {
            n->as.bin.op = TD_TOK_POW;
            n->as.bin.left = left;
            n->as.bin.right = right;
        }
        return n;
    }
    return left;
}

static td_expr_node_t *td_parse_unary(td_parser_t *p)
{
    if (p->failed)
        return NULL;
    if (p->tok.kind == TD_TOK_BANG || p->tok.kind == TD_TOK_MINUS ||
        p->tok.kind == TD_TOK_PLUS) {
        td_tok_kind_t op = p->tok.kind;
        td_expr_node_t *operand;
        td_expr_node_t *n;

        if (++p->depth > TD_EXPR_MAX_DEPTH) {
            td_parse_fail(p, "expression nesting exceeds maximum depth");
            return NULL;
        }
        td_next_tok(p);
        operand = td_parse_unary(p);
        p->depth--;
        if (p->failed)
            return NULL;
        if (op == TD_TOK_PLUS)
            return operand;
        n = td_alloc_node(p, TD_NODE_UNARY);
        if (n) {
            n->as.unary.op = op;
            n->as.unary.operand = operand;
        }
        return n;
    }
    return td_parse_pow(p);
}

static td_expr_node_t *td_make_bin(td_parser_t *p, td_tok_kind_t op,
                                   td_expr_node_t *left, td_expr_node_t *right)
{
    td_expr_node_t *n = td_alloc_node(p, TD_NODE_BIN);

    if (n) {
        n->as.bin.op = op;
        n->as.bin.left = left;
        n->as.bin.right = right;
    }
    return n;
}

static td_expr_node_t *td_parse_mul(td_parser_t *p)
{
    td_expr_node_t *left = td_parse_unary(p);

    while (!p->failed &&
           (p->tok.kind == TD_TOK_STAR || p->tok.kind == TD_TOK_SLASH ||
            p->tok.kind == TD_TOK_PERCENT)) {
        td_tok_kind_t op = p->tok.kind;
        td_expr_node_t *right;

        td_next_tok(p);
        right = td_parse_unary(p);
        left = td_make_bin(p, op, left, right);
    }
    return left;
}

static td_expr_node_t *td_parse_add(td_parser_t *p)
{
    td_expr_node_t *left = td_parse_mul(p);

    while (!p->failed &&
           (p->tok.kind == TD_TOK_PLUS || p->tok.kind == TD_TOK_MINUS)) {
        td_tok_kind_t op = p->tok.kind;
        td_expr_node_t *right;

        td_next_tok(p);
        right = td_parse_mul(p);
        left = td_make_bin(p, op, left, right);
    }
    return left;
}

static td_expr_node_t *td_parse_rel(td_parser_t *p)
{
    td_expr_node_t *left = td_parse_add(p);

    while (!p->failed &&
           (p->tok.kind == TD_TOK_LT || p->tok.kind == TD_TOK_LE ||
            p->tok.kind == TD_TOK_GT || p->tok.kind == TD_TOK_GE)) {
        td_tok_kind_t op = p->tok.kind;
        td_expr_node_t *right;

        td_next_tok(p);
        right = td_parse_add(p);
        left = td_make_bin(p, op, left, right);
    }
    return left;
}

static td_expr_node_t *td_parse_eq(td_parser_t *p)
{
    td_expr_node_t *left = td_parse_rel(p);

    while (!p->failed &&
           (p->tok.kind == TD_TOK_EQ || p->tok.kind == TD_TOK_NE)) {
        td_tok_kind_t op = p->tok.kind;
        td_expr_node_t *right;

        td_next_tok(p);
        right = td_parse_rel(p);
        left = td_make_bin(p, op, left, right);
    }
    return left;
}

static td_expr_node_t *td_parse_and(td_parser_t *p)
{
    td_expr_node_t *left = td_parse_eq(p);

    while (!p->failed && p->tok.kind == TD_TOK_AND) {
        td_expr_node_t *right;

        td_next_tok(p);
        right = td_parse_eq(p);
        left = td_make_bin(p, TD_TOK_AND, left, right);
    }
    return left;
}

static td_expr_node_t *td_parse_or(td_parser_t *p)
{
    td_expr_node_t *left = td_parse_and(p);

    while (!p->failed && p->tok.kind == TD_TOK_OR) {
        td_expr_node_t *right;

        td_next_tok(p);
        right = td_parse_and(p);
        left = td_make_bin(p, TD_TOK_OR, left, right);
    }
    return left;
}

static td_expr_node_t *td_parse_ternary(td_parser_t *p)
{
    td_expr_node_t *cond = td_parse_or(p);

    if (!p->failed && p->tok.kind == TD_TOK_QMARK) {
        td_expr_node_t *then_br;
        td_expr_node_t *else_br;
        td_expr_node_t *n;

        if (++p->depth > TD_EXPR_MAX_DEPTH) {
            td_parse_fail(p, "expression nesting exceeds maximum depth");
            return NULL;
        }
        td_next_tok(p);
        then_br = td_parse_ternary(p);
        if (!p->failed && p->tok.kind != TD_TOK_COLON)
            td_parse_fail(p, "expected ':' in ternary expression");
        if (!p->failed)
            td_next_tok(p);
        else_br = td_parse_ternary(p);
        p->depth--;
        if (p->failed)
            return NULL;
        n = td_alloc_node(p, TD_NODE_TERNARY);
        if (n) {
            n->as.ternary.cond = cond;
            n->as.ternary.then_br = then_br;
            n->as.ternary.else_br = else_br;
        }
        return n;
    }
    return cond;
}

td_expr_t *td_expr_compile(const td_sheet_t *s, const char *src,
                           char *err, size_t err_cap)
{
    td_expr_t *e;
    td_parser_t p;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!src || !src[0]) {
        td_set_err(err, err_cap, "empty expression");
        return NULL;
    }

    e = calloc(1, sizeof(*e));
    if (!e) {
        td_set_err(err, err_cap, "out of memory");
        return NULL;
    }
    td_arena_init(&e->arena, 0);
    e->source = td_arena_strdup(&e->arena, src);
    if (!e->source) {
        td_expr_free(e);
        td_set_err(err, err_cap, "out of memory");
        return NULL;
    }

    memset(&p, 0, sizeof(p));
    p.sheet = s;
    p.expr = e;
    p.src = src;
    p.p = src;

    td_next_tok(&p);
    if (!p.failed && p.tok.kind == TD_TOK_EOF)
        td_parse_fail(&p, "empty expression");
    if (!p.failed)
        e->root = td_parse_ternary(&p);
    if (!p.failed && p.tok.kind != TD_TOK_EOF)
        td_parse_fail(&p, "unexpected trailing token");

    if (p.failed || !e->root) {
        td_set_err(err, err_cap, "%s", p.err[0] ? p.err : "parse error");
        td_expr_free(e);
        return NULL;
    }
    return e;
}

void td_expr_free(td_expr_t *e)
{
    td_regex_item_t *it;

    if (!e)
        return;
    it = e->regexes;
    while (it) {
        td_regex_item_t *next = it->next;

        regfree(&it->re);
        free(it);
        it = next;
    }
    td_arena_free(&e->arena);
    free(e);
}

const char *td_expr_source(const td_expr_t *e)
{
    return (e && e->source) ? e->source : "";
}

/* ---- Evaluation Helpers ---- */

typedef struct {
    const td_sheet_t *sheet;
    size_t phys_row;
    td_arena_t *scratch;
    int depth;
    char *err;
    size_t err_cap;
} td_eval_ctx_t;

typedef struct {
    bool is_int;
    bool is_currency;
    int64_t i;
    double f;
} td_num_t;

static void td_eval_err(td_eval_ctx_t *ctx, const char *fmt, ...)
{
    va_list ap;

    if (!ctx->err || ctx->err_cap == 0 || ctx->err[0] != '\0')
        return;
    va_start(ap, fmt);
    vsnprintf(ctx->err, ctx->err_cap, fmt, ap);
    va_end(ap);
}

static bool td_expr_as_num(td_val_t v, td_num_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (v.type) {
    case TD_TYPE_INT:
    case TD_TYPE_DATE:
        out->is_int = true;
        out->i = v.as.i;
        out->f = (double)v.as.i;
        return true;
    case TD_TYPE_FLOAT:
        out->f = v.as.f;
        return isfinite(out->f);
    case TD_TYPE_CURRENCY:
        out->is_currency = true;
        out->f = v.as.f;
        return isfinite(out->f);
    case TD_TYPE_ANY:
    case TD_TYPE_STR: {
        td_val_t pi = td_val_parse(v.as.s, TD_TYPE_INT);

        if (pi.type == TD_TYPE_INT) {
            out->is_int = true;
            out->i = pi.as.i;
            out->f = (double)pi.as.i;
            return true;
        }
        td_val_t pf = td_val_parse(v.as.s, TD_TYPE_FLOAT);

        if (pf.type == TD_TYPE_FLOAT) {
            out->f = pf.as.f;
            return isfinite(out->f);
        }
        return false;
    }
    default:
        return false;
    }
}

static bool td_expr_as_date(td_val_t v, int64_t *out)
{
    if (v.type == TD_TYPE_DATE || v.type == TD_TYPE_INT) {
        *out = v.as.i;
        return true;
    }
    if (v.type == TD_TYPE_ANY || v.type == TD_TYPE_STR) {
        td_val_t d = td_val_parse(v.as.s, TD_TYPE_DATE);

        if (d.type == TD_TYPE_DATE) {
            *out = d.as.i;
            return true;
        }
    }
    return false;
}

static bool td_expr_truthy(td_val_t v)
{
    switch (v.type) {
    case TD_TYPE_NULL:
    case TD_TYPE_ERR:
        return false;
    case TD_TYPE_INT:
    case TD_TYPE_DATE:
        return v.as.i != 0;
    case TD_TYPE_FLOAT:
    case TD_TYPE_CURRENCY:
        return v.as.f != 0.0 && isfinite(v.as.f);
    case TD_TYPE_ANY:
    case TD_TYPE_STR:
        return v.as.s.len > 0;
    }
    return false;
}

static td_val_t td_make_str_slice(td_arena_t *scratch, const char *data,
                                  size_t len)
{
    td_val_t v;
    char *copy;

    if (!scratch)
        return TD_VAL_ERR;
    copy = td_arena_alloc(scratch, len + 1);
    if (!copy)
        return TD_VAL_ERR;
    if (len > 0 && data)
        memcpy(copy, data, len);
    copy[len] = '\0';
    v.type = TD_TYPE_STR;
    v.as.s.data = copy;
    v.as.s.len = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
    return v;
}

static bool td_expr_to_slice(td_val_t v, td_arena_t *scratch,
                             td_slice_t *out)
{
    char stack_buf[128];
    size_t n;
    char *copy;

    if (v.type == TD_TYPE_STR || v.type == TD_TYPE_ANY) {
        *out = v.as.s;
        if (!out->data) {
            out->data = "";
            out->len = 0;
        }
        return true;
    }
    n = td_val_format(v, v.type, stack_buf, sizeof(stack_buf));
    if (n < sizeof(stack_buf)) {
        copy = td_arena_strndup(scratch, stack_buf, n);
        if (!copy)
            return false;
        out->data = copy;
        out->len = (uint32_t)n;
        return true;
    }
    copy = td_arena_alloc(scratch, n + 1);
    if (!copy)
        return false;
    (void)td_val_format(v, v.type, copy, n + 1);
    out->data = copy;
    out->len = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
    return true;
}

static bool td_i64_add(int64_t a, int64_t b, int64_t *out)
{
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return false;
    *out = a + b;
    return true;
}

static bool td_i64_sub(int64_t a, int64_t b, int64_t *out)
{
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
        return false;
    *out = a - b;
    return true;
}

static bool td_i64_mul(int64_t a, int64_t b, int64_t *out)
{
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b)
                return false;
        } else {
            if (b < INT64_MIN / a)
                return false;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b)
                return false;
        } else {
            if (a != 0 && b < INT64_MAX / a)
                return false;
        }
    }
    *out = a * b;
    return true;
}

static td_val_t td_make_float(double f)
{
    td_val_t v;

    if (!isfinite(f))
        return TD_VAL_ERR;
    if (f == 0.0)
        f = 0.0;
    v.type = TD_TYPE_FLOAT;
    v.as.f = f;
    return v;
}

static td_val_t td_make_int(int64_t i)
{
    td_val_t v;

    v.type = TD_TYPE_INT;
    v.as.i = i;
    return v;
}

static td_val_t td_eval_node(const td_expr_node_t *n, td_eval_ctx_t *ctx);

static td_val_t td_eval_call(const td_expr_node_t *n, td_eval_ctx_t *ctx)
{
    td_val_t args[64];
    size_t nargs = n->as.call.nargs;
    td_fn_kind_t fn = n->as.call.fn;

    for (size_t i = 0; i < nargs; i++) {
        args[i] = td_eval_node(n->as.call.args[i], ctx);
        if (args[i].type == TD_TYPE_ERR)
            return TD_VAL_ERR;
    }

    if (fn != TD_FN_MIN && fn != TD_FN_MAX) {
        for (size_t i = 0; i < nargs; i++) {
            if (args[i].type == TD_TYPE_NULL)
                return TD_VAL_NULL;
        }
    }

    switch (fn) {
    case TD_FN_LEN: {
        td_slice_t s;
        const char *p;
        const char *end;
        int64_t count = 0;

        if (!td_expr_to_slice(args[0], ctx->scratch, &s))
            return TD_VAL_ERR;
        p = s.data;
        end = s.data + s.len;
        while (p < end) {
            (void)td_utf8_decode(&p, end);
            count++;
        }
        return td_make_int(count);
    }
    case TD_FN_LOWER:
    case TD_FN_UPPER: {
        td_slice_t s;
        char *buf;
        bool upper = fn == TD_FN_UPPER;

        if (!td_expr_to_slice(args[0], ctx->scratch, &s))
            return TD_VAL_ERR;
        buf = td_arena_alloc(ctx->scratch, s.len + 1);
        if (!buf)
            return TD_VAL_ERR;
        for (uint32_t i = 0; i < s.len; i++) {
            unsigned char ch = (unsigned char)s.data[i];

            buf[i] = (char)(upper ? toupper(ch) : tolower(ch));
        }
        buf[s.len] = '\0';
        return (td_val_t){ .type = TD_TYPE_STR,
                           .as = { .s = { buf, s.len } } };
    }
    case TD_FN_TRIM: {
        td_slice_t s;

        if (!td_expr_to_slice(args[0], ctx->scratch, &s))
            return TD_VAL_ERR;
        s = td_slice_trim(s);
        return td_make_str_slice(ctx->scratch, s.data, s.len);
    }
    case TD_FN_SUBSTR: {
        td_slice_t s;
        td_num_t nstart;
        size_t total_cp = 0;
        int64_t start_idx;
        int64_t want_len = -1;
        const char *p;
        const char *end;
        const char *b_start = NULL;
        const char *b_end = NULL;
        size_t idx = 0;

        if (!td_expr_to_slice(args[0], ctx->scratch, &s) ||
            !td_expr_as_num(args[1], &nstart) || !nstart.is_int) {
            td_eval_err(ctx, "substr() expects (str, int [, int])");
            return TD_VAL_ERR;
        }
        if (nargs == 3) {
            td_num_t nlen;

            if (!td_expr_as_num(args[2], &nlen) || !nlen.is_int) {
                td_eval_err(ctx, "substr() length must be int");
                return TD_VAL_ERR;
            }
            want_len = nlen.i;
            if (want_len <= 0)
                return td_make_str_slice(ctx->scratch, "", 0);
        }

        p = s.data;
        end = s.data + s.len;
        while (p < end) {
            (void)td_utf8_decode(&p, end);
            total_cp++;
        }

        start_idx = nstart.i;
        if (start_idx < 0)
            start_idx += (int64_t)total_cp;
        if (start_idx < 0)
            start_idx = 0;
        if ((size_t)start_idx >= total_cp)
            return td_make_str_slice(ctx->scratch, "", 0);

        size_t end_cp = total_cp;

        if (nargs == 3 && (uint64_t)want_len < total_cp - (size_t)start_idx)
            end_cp = (size_t)start_idx + (size_t)want_len;

        p = s.data;
        while (p <= end) {
            if (idx == (size_t)start_idx)
                b_start = p;
            if (idx == end_cp) {
                b_end = p;
                break;
            }
            if (p == end)
                break;
            (void)td_utf8_decode(&p, end);
            idx++;
        }
        if (!b_start)
            b_start = s.data;
        if (!b_end)
            b_end = end;
        return td_make_str_slice(ctx->scratch, b_start,
                                 (size_t)(b_end - b_start));
    }
    case TD_FN_REPLACE: {
        td_slice_t s, old_s, new_s;
        size_t matches = 0;
        size_t pos = 0;
        size_t out_len;
        char *buf;
        size_t w = 0;

        if (!td_expr_to_slice(args[0], ctx->scratch, &s) ||
            !td_expr_to_slice(args[1], ctx->scratch, &old_s) ||
            !td_expr_to_slice(args[2], ctx->scratch, &new_s))
            return TD_VAL_ERR;
        if (old_s.len == 0)
            return td_make_str_slice(ctx->scratch, s.data, s.len);

        while (pos + old_s.len <= s.len) {
            if (memcmp(s.data + pos, old_s.data, old_s.len) == 0) {
                matches++;
                pos += old_s.len;
            } else {
                pos++;
            }
        }
        if (matches == 0)
            return td_make_str_slice(ctx->scratch, s.data, s.len);
        out_len = s.len - matches * old_s.len + matches * new_s.len;
        buf = td_arena_alloc(ctx->scratch, out_len + 1);
        if (!buf)
            return TD_VAL_ERR;
        pos = 0;
        while (pos < s.len) {
            if (pos + old_s.len <= s.len &&
                memcmp(s.data + pos, old_s.data, old_s.len) == 0) {
                if (new_s.len > 0)
                    memcpy(buf + w, new_s.data, new_s.len);
                w += new_s.len;
                pos += old_s.len;
            } else {
                buf[w++] = s.data[pos++];
            }
        }
        buf[w] = '\0';
        return (td_val_t){ .type = TD_TYPE_STR,
                           .as = { .s = { buf, (uint32_t)w } } };
    }
    case TD_FN_CONTAINS: {
        td_slice_t hay, needle;

        if (!td_expr_to_slice(args[0], ctx->scratch, &hay) ||
            !td_expr_to_slice(args[1], ctx->scratch, &needle))
            return TD_VAL_ERR;
        if (needle.len == 0)
            return td_make_int(1);
        if (needle.len > hay.len)
            return td_make_int(0);
        for (size_t i = 0; i <= hay.len - needle.len; i++) {
            if (memcmp(hay.data + i, needle.data, needle.len) == 0)
                return td_make_int(1);
        }
        return td_make_int(0);
    }
    case TD_FN_MATCH: {
        td_slice_t s;
        char *cstr;
        int rc;

        if (!td_expr_to_slice(args[0], ctx->scratch, &s))
            return TD_VAL_ERR;
        cstr = td_arena_strndup(ctx->scratch, s.data, s.len);
        if (!cstr)
            return TD_VAL_ERR;

        if (n->as.call.compiled_re) {
            rc = regexec(n->as.call.compiled_re, cstr, 0, NULL, 0);
            return td_make_int(rc == 0 ? 1 : 0);
        }

        td_slice_t pat;
        char *pat_cstr;
        regex_t re;

        if (!td_expr_to_slice(args[1], ctx->scratch, &pat))
            return TD_VAL_ERR;
        pat_cstr = td_arena_strndup(ctx->scratch, pat.data, pat.len);
        if (!pat_cstr)
            return TD_VAL_ERR;
        rc = regcomp(&re, pat_cstr, REG_EXTENDED | REG_NOSUB);
        if (rc != 0) {
            char rerr[128];

            regerror(rc, &re, rerr, sizeof(rerr));
            td_eval_err(ctx, "match() invalid regex: %s", rerr);
            return TD_VAL_ERR;
        }
        rc = regexec(&re, cstr, 0, NULL, 0);
        regfree(&re);
        return td_make_int(rc == 0 ? 1 : 0);
    }
    case TD_FN_ABS: {
        td_num_t num;

        if (!td_expr_as_num(args[0], &num)) {
            td_eval_err(ctx, "abs() expects a number");
            return TD_VAL_ERR;
        }
        if (num.is_int) {
            if (num.i == INT64_MIN)
                return td_make_float(-(double)INT64_MIN);
            return td_make_int(num.i < 0 ? -num.i : num.i);
        }
        td_val_t out = td_make_float(fabs(num.f));

        if (out.type == TD_TYPE_FLOAT && num.is_currency)
            out.type = TD_TYPE_CURRENCY;
        return out;
    }
    case TD_FN_ROUND: {
        td_num_t num;
        int64_t digits = 0;

        if (!td_expr_as_num(args[0], &num)) {
            td_eval_err(ctx, "round() expects a number");
            return TD_VAL_ERR;
        }
        if (nargs == 2) {
            td_num_t dnum;

            if (!td_expr_as_num(args[1], &dnum) || !dnum.is_int ||
                dnum.i < 0 || dnum.i > 12) {
                td_eval_err(ctx, "round() digits must be 0..12");
                return TD_VAL_ERR;
            }
            digits = dnum.i;
        }
        if (num.is_int && digits == 0)
            return td_make_int(num.i);
        double scale = pow(10.0, (double)digits);
        double r = round(num.f * scale) / scale;

        if (!isfinite(r))
            return TD_VAL_ERR;
        if (r == 0.0)
            r = 0.0;
        if (digits == 0 && r >= -9223372036854774784.0 &&
            r <= 9223372036854774784.0)
            return td_make_int((int64_t)r);
        return td_make_float(r);
    }
    case TD_FN_FLOOR:
    case TD_FN_CEIL: {
        td_num_t num;
        double r;

        if (!td_expr_as_num(args[0], &num)) {
            td_eval_err(ctx, "%s() expects a number",
                        fn == TD_FN_FLOOR ? "floor" : "ceil");
            return TD_VAL_ERR;
        }
        if (num.is_int)
            return td_make_int(num.i);
        r = fn == TD_FN_FLOOR ? floor(num.f) : ceil(num.f);
        return td_make_float(r);
    }
    case TD_FN_MIN:
    case TD_FN_MAX: {
        bool have = false;
        td_val_t best = TD_VAL_NULL;
        bool want_max = fn == TD_FN_MAX;

        for (size_t i = 0; i < nargs; i++) {
            if (args[i].type == TD_TYPE_NULL)
                continue;
            if (!have) {
                best = args[i];
                have = true;
            } else {
                int cmp = td_val_cmp(args[i], best);

                if (want_max ? (cmp > 0) : (cmp < 0))
                    best = args[i];
            }
        }
        return best;
    }
    case TD_FN_LOG: {
        td_num_t num;

        if (!td_expr_as_num(args[0], &num)) {
            td_eval_err(ctx, "log() expects a number");
            return TD_VAL_ERR;
        }
        if (num.f <= 0.0) {
            td_eval_err(ctx, "log() domain error");
            return TD_VAL_ERR;
        }
        return td_make_float(log(num.f));
    }
    case TD_FN_SQRT: {
        td_num_t num;

        if (!td_expr_as_num(args[0], &num)) {
            td_eval_err(ctx, "sqrt() expects a number");
            return TD_VAL_ERR;
        }
        if (num.f < 0.0) {
            td_eval_err(ctx, "sqrt() domain error");
            return TD_VAL_ERR;
        }
        return td_make_float(sqrt(num.f));
    }
    case TD_FN_YEAR:
    case TD_FN_MONTH:
    case TD_FN_DAY: {
        int64_t epoch;
        time_t t;
        struct tm tm;

        if (!td_expr_as_date(args[0], &epoch)) {
            td_eval_err(ctx, "date function expects a date");
            return TD_VAL_ERR;
        }
        t = (time_t)epoch;
        if (!gmtime_r(&t, &tm))
            return TD_VAL_ERR;
        if (fn == TD_FN_YEAR)
            return td_make_int((int64_t)tm.tm_year + 1900);
        if (fn == TD_FN_MONTH)
            return td_make_int((int64_t)tm.tm_mon + 1);
        return td_make_int((int64_t)tm.tm_mday);
    }
    case TD_FN_DATEDIFF: {
        int64_t d1, d2;
        double scale = 86400.0;

        if (!td_expr_as_date(args[0], &d1) ||
            !td_expr_as_date(args[1], &d2)) {
            td_eval_err(ctx, "datediff() expects two dates");
            return TD_VAL_ERR;
        }
        if (nargs == 3) {
            td_slice_t u;

            if (!td_expr_to_slice(args[2], ctx->scratch, &u))
                return TD_VAL_ERR;
            if ((u.len == 1 && u.data[0] == 'd') ||
                (u.len == 3 && memcmp(u.data, "day", 3) == 0) ||
                (u.len == 4 && memcmp(u.data, "days", 4) == 0))
                scale = 86400.0;
            else if ((u.len == 1 && u.data[0] == 'h') ||
                     (u.len == 4 && memcmp(u.data, "hour", 4) == 0) ||
                     (u.len == 5 && memcmp(u.data, "hours", 5) == 0))
                scale = 3600.0;
            else if ((u.len == 1 && u.data[0] == 'm') ||
                     (u.len == 3 && memcmp(u.data, "min", 3) == 0) ||
                     (u.len == 6 && memcmp(u.data, "minute", 6) == 0) ||
                     (u.len == 7 && memcmp(u.data, "minutes", 7) == 0))
                scale = 60.0;
            else if ((u.len == 1 && u.data[0] == 's') ||
                     (u.len == 3 && memcmp(u.data, "sec", 3) == 0) ||
                     (u.len == 6 && memcmp(u.data, "second", 6) == 0) ||
                     (u.len == 7 && memcmp(u.data, "seconds", 7) == 0))
                scale = 1.0;
            else {
                td_eval_err(ctx, "datediff() unknown unit");
                return TD_VAL_ERR;
            }
        }
        return td_make_float(((double)d1 - (double)d2) / scale);
    }
    case TD_FN_COUNT:
        break;
    }
    return TD_VAL_ERR;
}

static td_val_t td_eval_cmp(td_tok_kind_t op, td_val_t l, td_val_t r,
                            td_eval_ctx_t *ctx)
{
    td_num_t nl, nr;
    bool l_num, r_num;
    int cmp = 0;

    if (l.type == TD_TYPE_ERR || r.type == TD_TYPE_ERR)
        return TD_VAL_ERR;

    if (op == TD_TOK_EQ || op == TD_TOK_NE) {
        bool eq;

        if (l.type == TD_TYPE_NULL && r.type == TD_TYPE_NULL)
            return td_make_int(op == TD_TOK_EQ ? 1 : 0);
        if (l.type == TD_TYPE_NULL || r.type == TD_TYPE_NULL)
            return td_make_int(op == TD_TOK_EQ ? 0 : 1);

        l_num = td_expr_as_num(l, &nl);
        r_num = td_expr_as_num(r, &nr);
        if (l_num && r_num) {
            if (nl.is_int && nr.is_int)
                eq = nl.i == nr.i;
            else
                eq = nl.f == nr.f;
        } else if (!l_num && !r_num) {
            td_slice_t sl, sr;

            if (!td_expr_to_slice(l, ctx->scratch, &sl) ||
                !td_expr_to_slice(r, ctx->scratch, &sr))
                return TD_VAL_ERR;
            eq = td_slice_eq(sl, sr);
        } else {
            eq = false;
        }
        return td_make_int(op == TD_TOK_EQ ? (eq ? 1 : 0) : (eq ? 0 : 1));
    }

    if (l.type == TD_TYPE_NULL || r.type == TD_TYPE_NULL)
        return td_make_int(0);

    l_num = td_expr_as_num(l, &nl);
    r_num = td_expr_as_num(r, &nr);
    if (l_num && r_num) {
        if (nl.is_int && nr.is_int)
            cmp = nl.i == nr.i ? 0 : (nl.i < nr.i ? -1 : 1);
        else
            cmp = nl.f == nr.f ? 0 : (nl.f < nr.f ? -1 : 1);
    } else {
        td_slice_t sl, sr;

        if (!td_expr_to_slice(l, ctx->scratch, &sl) ||
            !td_expr_to_slice(r, ctx->scratch, &sr))
            return TD_VAL_ERR;
        cmp = td_slice_cmp(sl, sr);
    }

    switch (op) {
    case TD_TOK_LT: return td_make_int(cmp < 0 ? 1 : 0);
    case TD_TOK_LE: return td_make_int(cmp <= 0 ? 1 : 0);
    case TD_TOK_GT: return td_make_int(cmp > 0 ? 1 : 0);
    case TD_TOK_GE: return td_make_int(cmp >= 0 ? 1 : 0);
    default:        return TD_VAL_ERR;
    }
}

static td_val_t td_eval_bin(const td_expr_node_t *n, td_eval_ctx_t *ctx)
{
    td_tok_kind_t op = n->as.bin.op;
    td_val_t l, r;

    if (op == TD_TOK_AND) {
        l = td_eval_node(n->as.bin.left, ctx);
        if (l.type == TD_TYPE_ERR)
            return TD_VAL_ERR;
        if (!td_expr_truthy(l))
            return td_make_int(0);
        r = td_eval_node(n->as.bin.right, ctx);
        if (r.type == TD_TYPE_ERR)
            return TD_VAL_ERR;
        return td_make_int(td_expr_truthy(r) ? 1 : 0);
    }
    if (op == TD_TOK_OR) {
        l = td_eval_node(n->as.bin.left, ctx);
        if (l.type == TD_TYPE_ERR)
            return TD_VAL_ERR;
        if (td_expr_truthy(l))
            return td_make_int(1);
        r = td_eval_node(n->as.bin.right, ctx);
        if (r.type == TD_TYPE_ERR)
            return TD_VAL_ERR;
        return td_make_int(td_expr_truthy(r) ? 1 : 0);
    }

    l = td_eval_node(n->as.bin.left, ctx);
    if (l.type == TD_TYPE_ERR)
        return TD_VAL_ERR;
    r = td_eval_node(n->as.bin.right, ctx);
    if (r.type == TD_TYPE_ERR)
        return TD_VAL_ERR;

    if (op == TD_TOK_EQ || op == TD_TOK_NE || op == TD_TOK_LT ||
        op == TD_TOK_LE || op == TD_TOK_GT || op == TD_TOK_GE)
        return td_eval_cmp(op, l, r, ctx);

    if (l.type == TD_TYPE_NULL || r.type == TD_TYPE_NULL)
        return TD_VAL_NULL;

    td_num_t nl, nr;
    bool l_num = td_expr_as_num(l, &nl);
    bool r_num = td_expr_as_num(r, &nr);

    if (op == TD_TOK_PLUS &&
        (l.type == TD_TYPE_STR || r.type == TD_TYPE_STR || !l_num || !r_num)) {
        td_slice_t sl, sr;
        char *buf;
        size_t len;

        if (!td_expr_to_slice(l, ctx->scratch, &sl) ||
            !td_expr_to_slice(r, ctx->scratch, &sr))
            return TD_VAL_ERR;
        len = (size_t)sl.len + (size_t)sr.len;
        buf = td_arena_alloc(ctx->scratch, len + 1);
        if (!buf)
            return TD_VAL_ERR;
        if (sl.len > 0)
            memcpy(buf, sl.data, sl.len);
        if (sr.len > 0)
            memcpy(buf + sl.len, sr.data, sr.len);
        buf[len] = '\0';
        return (td_val_t){ .type = TD_TYPE_STR,
                           .as = { .s = { buf, len > UINT32_MAX
                                                   ? UINT32_MAX
                                                   : (uint32_t)len } } };
    }

    if (!l_num || !r_num) {
        td_eval_err(ctx, "arithmetic operator requires numeric operands");
        return TD_VAL_ERR;
    }

    if (op == TD_TOK_MINUS && l.type == TD_TYPE_DATE && r.type == TD_TYPE_DATE) {
        int64_t diff;

        if (td_i64_sub(l.as.i, r.as.i, &diff))
            return td_make_int(diff);
        return td_make_float(nl.f - nr.f);
    }
    if (op == TD_TOK_MINUS && l.type == TD_TYPE_DATE && nr.is_int) {
        int64_t res;

        if (td_i64_sub(l.as.i, nr.i, &res))
            return (td_val_t){ .type = TD_TYPE_DATE, .as = { .i = res } };
    }
    if (op == TD_TOK_PLUS && l.type == TD_TYPE_DATE && nr.is_int) {
        int64_t res;

        if (td_i64_add(l.as.i, nr.i, &res))
            return (td_val_t){ .type = TD_TYPE_DATE, .as = { .i = res } };
    }

    bool any_currency = nl.is_currency || nr.is_currency;

    switch (op) {
    case TD_TOK_PLUS:
        if (nl.is_int && nr.is_int) {
            int64_t res;

            if (td_i64_add(nl.i, nr.i, &res))
                return td_make_int(res);
        }
        {
            td_val_t v = td_make_float(nl.f + nr.f);

            if (v.type == TD_TYPE_FLOAT && any_currency)
                v.type = TD_TYPE_CURRENCY;
            return v;
        }
    case TD_TOK_MINUS:
        if (nl.is_int && nr.is_int) {
            int64_t res;

            if (td_i64_sub(nl.i, nr.i, &res))
                return td_make_int(res);
        }
        {
            td_val_t v = td_make_float(nl.f - nr.f);

            if (v.type == TD_TYPE_FLOAT && any_currency)
                v.type = TD_TYPE_CURRENCY;
            return v;
        }
    case TD_TOK_STAR:
        if (nl.is_int && nr.is_int) {
            int64_t res;

            if (td_i64_mul(nl.i, nr.i, &res))
                return td_make_int(res);
        }
        {
            td_val_t v = td_make_float(nl.f * nr.f);

            if (v.type == TD_TYPE_FLOAT && any_currency)
                v.type = TD_TYPE_CURRENCY;
            return v;
        }
    case TD_TOK_SLASH:
        if (nr.f == 0.0) {
            td_eval_err(ctx, "division by zero");
            return TD_VAL_ERR;
        }
        return td_make_float(nl.f / nr.f);
    case TD_TOK_PERCENT:
        if (nl.is_int && nr.is_int) {
            if (nr.i == 0) {
                td_eval_err(ctx, "modulo by zero");
                return TD_VAL_ERR;
            }
            if (nl.i == INT64_MIN && nr.i == -1)
                return td_make_int(0);
            return td_make_int(nl.i % nr.i);
        }
        if (nr.f == 0.0) {
            td_eval_err(ctx, "modulo by zero");
            return TD_VAL_ERR;
        }
        return td_make_float(fmod(nl.f, nr.f));
    case TD_TOK_POW: {
        if (nl.is_int && nr.is_int && nr.i >= 0 && nr.i <= 62) {
            int64_t acc = 1;
            int64_t base = nl.i;
            int64_t exp = nr.i;
            bool ok = true;

            while (exp > 0) {
                if (exp & 1) {
                    if (!td_i64_mul(acc, base, &acc)) {
                        ok = false;
                        break;
                    }
                }
                exp >>= 1;
                if (exp > 0 && !td_i64_mul(base, base, &base)) {
                    ok = false;
                    break;
                }
            }
            if (ok)
                return td_make_int(acc);
        }
        return td_make_float(pow(nl.f, nr.f));
    }
    default:
        return TD_VAL_ERR;
    }
}

static td_val_t td_eval_node(const td_expr_node_t *n, td_eval_ctx_t *ctx)
{
    if (!n)
        return TD_VAL_ERR;
    if (++ctx->depth > TD_EXPR_MAX_DEPTH) {
        td_eval_err(ctx, "evaluation depth exceeded");
        ctx->depth--;
        return TD_VAL_ERR;
    }

    td_val_t res = TD_VAL_ERR;

    switch (n->kind) {
    case TD_NODE_LIT:
        if (n->as.lit.type == TD_TYPE_STR)
            res = td_make_str_slice(ctx->scratch, n->as.lit.as.s.data,
                                    n->as.lit.as.s.len);
        else
            res = n->as.lit;
        break;
    case TD_NODE_COL:
        res = td_sheet_eval_cell(ctx->sheet, ctx->phys_row, n->as.col_idx);
        break;
    case TD_NODE_UNARY: {
        td_val_t v = td_eval_node(n->as.unary.operand, ctx);

        if (v.type == TD_TYPE_ERR) {
            res = TD_VAL_ERR;
            break;
        }
        if (n->as.unary.op == TD_TOK_BANG) {
            res = td_make_int(td_expr_truthy(v) ? 0 : 1);
            break;
        }
        if (v.type == TD_TYPE_NULL) {
            res = TD_VAL_NULL;
            break;
        }
        td_num_t num;

        if (!td_expr_as_num(v, &num)) {
            td_eval_err(ctx, "unary '-' requires a numeric operand");
            res = TD_VAL_ERR;
            break;
        }
        if (num.is_int) {
            if (num.i == INT64_MIN)
                res = td_make_float(-(double)INT64_MIN);
            else
                res = td_make_int(-num.i);
        } else {
            res = td_make_float(-num.f);
            if (res.type == TD_TYPE_FLOAT && num.is_currency)
                res.type = TD_TYPE_CURRENCY;
        }
        break;
    }
    case TD_NODE_BIN:
        res = td_eval_bin(n, ctx);
        break;
    case TD_NODE_TERNARY: {
        td_val_t cond = td_eval_node(n->as.ternary.cond, ctx);

        if (cond.type == TD_TYPE_ERR) {
            res = TD_VAL_ERR;
            break;
        }
        res = td_eval_node(td_expr_truthy(cond) ? n->as.ternary.then_br
                                                : n->as.ternary.else_br,
                           ctx);
        break;
    }
    case TD_NODE_CALL:
        res = td_eval_call(n, ctx);
        break;
    }

    ctx->depth--;
    return res;
}

td_val_t td_expr_eval(const td_expr_t *e, const td_sheet_t *s,
                      size_t phys_row, td_arena_t *scratch,
                      char *err, size_t err_cap)
{
    td_eval_ctx_t ctx;
    td_arena_t local_scratch;
    bool use_local = false;
    td_val_t v;

    if (err && err_cap > 0)
        err[0] = '\0';
    if (!e || !e->root || !s || phys_row >= s->phys_nrows) {
        td_set_err(err, err_cap, "invalid expression evaluation arguments");
        return TD_VAL_ERR;
    }
    if (!scratch) {
        td_arena_init(&local_scratch, 0);
        scratch = &local_scratch;
        use_local = true;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.sheet = s;
    ctx.phys_row = phys_row;
    ctx.scratch = scratch;
    ctx.err = err;
    ctx.err_cap = err_cap;

    v = td_eval_node(e->root, &ctx);
    if (use_local) {
        /* If caller passed no scratch arena, scalar values don't reference it;
         * strings cannot outlive local_scratch so caller must pass scratch
         * whenever string results are needed. */
        if (v.type == TD_TYPE_STR || v.type == TD_TYPE_ANY)
            v = TD_VAL_ERR;
        td_arena_free(&local_scratch);
    }
    return v;
}
