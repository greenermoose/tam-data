#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#include "arena.h"
#include "test.h"

void test_arena(void)
{
    td_arena_t a;
    char *s;

    td_arena_init(&a, 64);
    TD_CHECK_INT(a.head == NULL, 1);

    void *p = td_arena_alloc(&a, 10);
    TD_CHECK(p != NULL);
    TD_CHECK_INT((uintptr_t)p % alignof(max_align_t), 0);

    void *q = td_arena_alloc(&a, 200);
    TD_CHECK(q != NULL);
    TD_CHECK_INT((uintptr_t)q % alignof(max_align_t), 0);
    TD_CHECK(q != p);

    void *z = td_arena_calloc(&a, 32);
    TD_CHECK(z != NULL);
    for (int i = 0; i < 32; i++)
        TD_CHECK_INT(((unsigned char *)z)[i], 0);

    s = td_arena_strdup(&a, "hello");
    TD_CHECK(s != NULL);
    TD_CHECK_STR(s, "hello");

    s = td_arena_strndup(&a, "abcdef", 3);
    TD_CHECK_STR(s, "abc");

    s = td_arena_strndup(&a, "ab", 10);
    TD_CHECK_STR(s, "ab");

    s = td_arena_strdup(&a, NULL);
    TD_CHECK_STR(s, "");

    TD_CHECK(td_arena_alloc(&a, 0) == NULL);

    size_t total = a.total;
    TD_CHECK(total > 0);
    td_arena_reset(&a);
    TD_CHECK(a.head != NULL);
    TD_CHECK(a.total <= total);

    p = td_arena_alloc(&a, 16);
    TD_CHECK(p != NULL);
    TD_CHECK_INT((uintptr_t)p % alignof(max_align_t), 0);

    td_arena_free(&a);
    TD_CHECK_INT(a.head == NULL, 1);
    TD_CHECK_INT(a.total, 0);

    /* A reset of an empty arena stays empty. */
    td_arena_init(&a, 0);
    td_arena_reset(&a);
    TD_CHECK_INT(a.head == NULL, 1);
    td_arena_free(&a);
}
