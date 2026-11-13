#include "test.h"

int td_tests_run;
int td_tests_failed;

void td_test_fail(const char *file, int line, const char *expr)
{
    td_tests_failed++;
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
}

int main(void)
{
    test_arena();
    test_val();
    test_sheet();
    test_csv();
    test_term();
    test_input();
    test_clipboard();
    test_cmdlog();

    if (td_tests_failed) {
        fprintf(stderr, "%d of %d checks failed\n", td_tests_failed,
                td_tests_run);
        return 1;
    }
    printf("ok: %d checks passed\n", td_tests_run);
    return 0;
}
