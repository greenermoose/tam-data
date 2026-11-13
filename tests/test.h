#ifndef TD_TEST_H
#define TD_TEST_H

#include <stdio.h>
#include <string.h>

extern int td_tests_run;
extern int td_tests_failed;

void td_test_fail(const char *file, int line, const char *expr);

#define TD_CHECK(cond)                                                        \
    do {                                                                      \
        td_tests_run++;                                                       \
        if (!(cond))                                                          \
            td_test_fail(__FILE__, __LINE__, #cond);                          \
    } while (0)

#define TD_CHECK_INT(actual, expected)                                        \
    do {                                                                      \
        long long _a = (long long)(actual);                                   \
        long long _e = (long long)(expected);                                 \
        td_tests_run++;                                                       \
        if (_a != _e) {                                                       \
            char _msg[128];                                                   \
            snprintf(_msg, sizeof(_msg), "%s == %s (%lld != %lld)", #actual,  \
                     #expected, _a, _e);                                      \
            td_test_fail(__FILE__, __LINE__, _msg);                           \
        }                                                                     \
    } while (0)

#define TD_CHECK_STR(actual, expected)                                        \
    do {                                                                      \
        const char *_a = (actual);                                            \
        const char *_e = (expected);                                          \
        td_tests_run++;                                                       \
        if (!_a || !_e || strcmp(_a, _e) != 0) {                              \
            char _msg[256];                                                   \
            snprintf(_msg, sizeof(_msg), "%s == %s (\"%s\" != \"%s\")",       \
                     #actual, #expected, _a ? _a : "(null)",                  \
                     _e ? _e : "(null)");                                     \
            td_test_fail(__FILE__, __LINE__, _msg);                           \
        }                                                                     \
    } while (0)

void test_arena(void);
void test_val(void);
void test_sheet(void);
void test_csv(void);
void test_term(void);
void test_input(void);

#endif /* TD_TEST_H */
