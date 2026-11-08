#include "arena.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct td_arena_block {
    td_arena_block_t *next;
    size_t cap;
    size_t used;
    max_align_t data[];
};

static size_t td_align_up(size_t n)
{
    const size_t align = alignof(max_align_t);

    if (n > SIZE_MAX - (align - 1))
        return SIZE_MAX;
    return (n + align - 1) & ~(align - 1);
}

void td_arena_init(td_arena_t *a, size_t block_size)
{
    if (!a)
        return;
    a->head = NULL;
    a->block_size = block_size ? block_size : TD_ARENA_DEFAULT_BLOCK;
    a->total = 0;
}

static td_arena_block_t *td_arena_block_new(size_t cap)
{
    td_arena_block_t *b;

    if (cap > SIZE_MAX - sizeof(*b))
        return NULL;
    b = malloc(sizeof(*b) + cap);
    if (!b)
        return NULL;
    b->next = NULL;
    b->cap = cap;
    b->used = 0;
    return b;
}

void *td_arena_alloc(td_arena_t *a, size_t n)
{
    td_arena_block_t *b;
    size_t need;

    if (!a || n == 0)
        return NULL;

    need = td_align_up(n);
    if (need == SIZE_MAX)
        return NULL;

    b = a->head;
    if (!b || b->cap - b->used < need) {
        size_t cap = a->block_size;
        td_arena_block_t *fresh;

        /* Oversized requests get their own dedicated block. */
        if (cap < need)
            cap = need;

        fresh = td_arena_block_new(cap);
        if (!fresh)
            return NULL;
        fresh->next = a->head;
        a->head = fresh;
        a->total += cap;
        b = fresh;
    }

    void *p = (void *)((unsigned char *)b->data + b->used);
    b->used += need;
    return p;
}

void *td_arena_calloc(td_arena_t *a, size_t n)
{
    void *p = td_arena_alloc(a, n);

    if (p)
        memset(p, 0, n);
    return p;
}

char *td_arena_strndup(td_arena_t *a, const char *s, size_t n)
{
    char *p;
    size_t len = 0;

    if (!s)
        n = 0;
    while (len < n && s[len])
        len++;

    p = td_arena_alloc(a, len + 1);
    if (!p)
        return NULL;
    if (len)
        memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

char *td_arena_strdup(td_arena_t *a, const char *s)
{
    if (!s)
        s = "";
    return td_arena_strndup(a, s, strlen(s));
}

void td_arena_reset(td_arena_t *a)
{
    td_arena_block_t *b;
    td_arena_block_t *keep;

    if (!a)
        return;

    b = a->head;
    keep = b;
    while (b) {
        td_arena_block_t *next = b->next;

        if (b != keep)
            free(b);
        b = next;
    }

    if (keep) {
        keep->next = NULL;
        keep->used = 0;
        a->total = keep->cap;
    } else {
        a->total = 0;
    }
}

void td_arena_free(td_arena_t *a)
{
    td_arena_block_t *b;

    if (!a)
        return;

    b = a->head;
    while (b) {
        td_arena_block_t *next = b->next;

        free(b);
        b = next;
    }
    a->head = NULL;
    a->total = 0;
}
