#ifndef TD_ARENA_H
#define TD_ARENA_H

#include <stddef.h>

/* Chained bump allocator. Blocks are never freed individually: reset() keeps
 * the first block for reuse, free() releases every block. */
typedef struct td_arena_block td_arena_block_t;

typedef struct {
    td_arena_block_t *head;
    size_t block_size;  /* preferred page size for new blocks */
    size_t total;       /* bytes reserved in blocks (payload capacity) */
} td_arena_t;

#define TD_ARENA_DEFAULT_BLOCK (64u * 1024u)

void td_arena_init(td_arena_t *a, size_t block_size);
void *td_arena_alloc(td_arena_t *a, size_t n);
void *td_arena_calloc(td_arena_t *a, size_t n);
char *td_arena_strdup(td_arena_t *a, const char *s);
char *td_arena_strndup(td_arena_t *a, const char *s, size_t n);
void td_arena_reset(td_arena_t *a);
void td_arena_free(td_arena_t *a);

#endif /* TD_ARENA_H */
