#ifndef TD_VAL_H
#define TD_VAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Zero-copy string slice: either arena-interned bytes or a window into an
 * mmap-backed source file. data == NULL means an absent slice. */
typedef struct {
    const char *data;
    uint32_t len;
} td_slice_t;

typedef enum {
    TD_TYPE_ANY = 0,   /* untyped raw string slice, sigil ' ' */
    TD_TYPE_STR,       /* explicit string, sigil '~' */
    TD_TYPE_INT,       /* int64_t, sigil '#' */
    TD_TYPE_FLOAT,     /* double, sigil '%' */
    TD_TYPE_CURRENCY,  /* double, sigil '$' */
    TD_TYPE_DATE,      /* int64_t UTC epoch seconds, sigil '@' */
    TD_TYPE_NULL,      /* missing value, no sigil */
    TD_TYPE_ERR        /* failed conversion, sigil '!' */
} td_type_t;

typedef struct {
    td_type_t type;
    union {
        td_slice_t s;
        int64_t i;
        double f;
    } as;
} td_val_t;

#define TD_VAL_NULL ((td_val_t){ .type = TD_TYPE_NULL, .as = { .i = 0 } })
#define TD_VAL_ERR  ((td_val_t){ .type = TD_TYPE_ERR, .as = { .s = { NULL, 0 } } })

td_slice_t td_slice_from_cstr(const char *s);
td_slice_t td_slice_trim(td_slice_t s);
bool td_slice_eq(td_slice_t a, td_slice_t b);
int td_slice_cmp(td_slice_t a, td_slice_t b);

const char *td_type_sigil(td_type_t t);
const char *td_type_name(td_type_t t);
bool td_type_is_numeric(td_type_t t);

td_val_t td_val_parse(td_slice_t raw, td_type_t target_type);
size_t td_val_format(td_val_t v, td_type_t col_type, char *buf, size_t cap);
int td_val_cmp(td_val_t a, td_val_t b);

#endif /* TD_VAL_H */
