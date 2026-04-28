/* SPDX-License-Identifier: BSD-3-Clause */
/* Test runner. Invokes each test suite in sequence. */

#include "utest.h"
#include <time.h>

int utest_checks = 0;
int utest_failures = 0;
int utest_cases_run = 0;
int utest_cases_failed = 0;

void utest_run(const char *name, void (*fn)(void)) {
    int before = utest_failures;
    fn();
    utest_cases_run++;
    if (utest_failures > before) {
        utest_cases_failed++;
        printf("  FAIL %s (%d check(s) failed)\n",
            name, utest_failures - before);
    } else {
        printf("  PASS %s\n", name);
    }
    fflush(stdout);
}

/* Test suite declarations — one per test_*.c file. */
extern void test_version_suite(void);
extern void test_lexer_suite(void);
extern void test_arena_suite(void);
extern void test_parser_suite(void);
extern void test_varint_suite(void);
extern void test_module_suite(void);
extern void test_emit_suite(void);
extern void test_vm_suite(void);
extern void test_pipeline_suite(void);
extern void test_uvalue_suite(void);
extern void test_multi_vm_suite(void);
extern void test_intern_suite(void);
extern void test_funcstate_suite(void);
extern void test_separators_suite(void);
extern void test_function_suite(void);
extern void test_lazy_suite(void);
extern void test_strand_suite(void);
extern void test_cleanup_suite(void);
extern void test_scheduler_cooperative_suite(void);
extern void test_dispatch_loop_suite(void);
extern void test_unwind_suite(void);
extern void test_capi_unwind_suite(void);
extern void test_realm_suite(void);
extern void test_step_driver_suite(void);
extern void test_chunk_apis_suite(void);
extern void test_event_ring_suite(void);
extern void test_callback_watchdog_suite(void);
extern void test_ugc_color_invariants_suite(void);
extern void test_ugc_state_machine_suite(void);
extern void test_ugc_barrier_suite(void);
extern void test_ugc_walk_roots_suite(void);
extern void test_ugc_handle_suite(void);
extern void test_ugc_finalizer_suite(void);

int main(void) {
    clock_t t0 = clock();

    printf("Running test suites\n");

    test_version_suite();
    test_lexer_suite();
    test_arena_suite();
    test_parser_suite();
    test_varint_suite();
    test_module_suite();
    test_emit_suite();
    test_vm_suite();
    test_pipeline_suite();
    test_uvalue_suite();
    test_multi_vm_suite();
    test_intern_suite();
    test_funcstate_suite();
    test_separators_suite();
    test_function_suite();
    test_lazy_suite();
    test_strand_suite();
    test_cleanup_suite();
    test_scheduler_cooperative_suite();
    test_dispatch_loop_suite();
    test_unwind_suite();
    test_capi_unwind_suite();
    test_realm_suite();
    test_step_driver_suite();
    test_chunk_apis_suite();
    test_event_ring_suite();
    test_callback_watchdog_suite();
    test_ugc_color_invariants_suite();
    test_ugc_state_machine_suite();
    test_ugc_barrier_suite();
    test_ugc_walk_roots_suite();
    test_ugc_handle_suite();
    test_ugc_finalizer_suite();
    /* Add new suites here as test files are added. */

    double elapsed = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("\n%d cases, %d checks, %d failed (%.3fs)\n",
        utest_cases_run, utest_checks, utest_cases_failed, elapsed);

    return utest_cases_failed > 0 ? 1 : 0;
}
