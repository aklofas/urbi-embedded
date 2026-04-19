/* SPDX-License-Identifier: BSD-3-Clause */
/* Test runner. Links with utest.h-using test files. */

#include "utest.h"

utest_case utest_cases[UTEST_MAX_CASES];
int utest_count = 0;
int utest_checks = 0;
int utest_failures = 0;
const char *utest_current = NULL;

int main(void) {
    int case_failures = 0;
    clock_t t0 = clock();

    printf("Running %d test cases\n", utest_count);

    for (int i = 0; i < utest_count; i++) {
        utest_current = utest_cases[i].name;
        int before = utest_failures;
        utest_cases[i].fn();
        int after = utest_failures;
        if (after > before) {
            printf("  FAIL %s (%d check(s) failed)\n",
                utest_current, after - before);
            case_failures++;
        } else {
            printf("  PASS %s\n", utest_current);
        }
    }

    double elapsed = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("\n%d cases, %d checks, %d failed (%.3fs)\n",
        utest_count, utest_checks, utest_failures, elapsed);

    return case_failures > 0 ? 1 : 0;
}
