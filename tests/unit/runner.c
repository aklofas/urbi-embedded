/* SPDX-License-Identifier: BSD-3-Clause */
/* Test runner. Invokes each test suite in sequence.
 *
 * Optional env-var sharding for slow wrappers (valgrind):
 *   URBI_SHARD_TOTAL=N URBI_SHARD_INDEX=I
 * Each shard runs suites where (suite_index % N == I). When unset or
 * TOTAL<=1, all suites run. Distribution is by suite-list position; if a
 * shard runs long, reorder suites or rebalance N. */

#include "utest.h"
#include <stdlib.h>
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
extern void test_lex_keywords_suite(void);
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
extern void test_tag_lifecycle_suite(void);
extern void test_strand_spawn_inheritance_suite(void);
extern void test_tag_stop_realm_suite(void);
extern void test_watcher_pool_suite(void);
extern void test_watcher_dirty_suite(void);
extern void test_fork_suite(void);
extern void test_determinism_suite(void);
extern void test_sched_fifo_suite(void);
extern void test_sched_pool_exhaust_suite(void);
extern void test_pipe_budget_exhaust_suite(void);
extern void test_determinism_two_runs_suite(void);
extern void test_determinism_tunable_pin_suite(void);
extern void test_uobject_suite(void);
extern void test_uslothandle_suite(void);
extern void test_ushape_suite(void);
extern void test_uic_suite(void);
extern void test_topology_gen_suite(void);
extern void test_ugc_object_cells_suite(void);
extern void test_gc_strand_walker_suite(void);
extern void test_scheduler_invariant_suite(void);
extern void test_op_allocation_suite(void);
extern void test_disasm_suite(void);
extern void test_gc_byte_suite(void);
extern void test_ast_alloc_suite(void);
extern void test_uwatcher_layout_suite(void);
extern void test_ustrand_layout_suite(void);
extern void test_uevent_suite(void);
extern void test_utag_gc_suite(void);
extern void test_uchanged_node_suite(void);
extern void test_uvm_trace_fields_suite(void);
extern void test_uvm_deferred_ring_suite(void);
extern void test_strand_arm_suite(void);

struct suite_entry {
    const char *name;
    void (*fn)(void);
};

static const struct suite_entry suites[] = {
    {"version",                    test_version_suite},
    {"lexer",                      test_lexer_suite},
    {"lex_keywords",               test_lex_keywords_suite},
    {"arena",                      test_arena_suite},
    {"parser",                     test_parser_suite},
    {"varint",                     test_varint_suite},
    {"module",                     test_module_suite},
    {"emit",                       test_emit_suite},
    {"vm",                         test_vm_suite},
    {"pipeline",                   test_pipeline_suite},
    {"uvalue",                     test_uvalue_suite},
    {"multi_vm",                   test_multi_vm_suite},
    {"intern",                     test_intern_suite},
    {"funcstate",                  test_funcstate_suite},
    {"separators",                 test_separators_suite},
    {"function",                   test_function_suite},
    {"lazy",                       test_lazy_suite},
    {"strand",                     test_strand_suite},
    {"cleanup",                    test_cleanup_suite},
    {"scheduler_cooperative",      test_scheduler_cooperative_suite},
    {"dispatch_loop",              test_dispatch_loop_suite},
    {"unwind",                     test_unwind_suite},
    {"capi_unwind",                test_capi_unwind_suite},
    {"realm",                      test_realm_suite},
    {"step_driver",                test_step_driver_suite},
    {"chunk_apis",                 test_chunk_apis_suite},
    {"event_ring",                 test_event_ring_suite},
    {"callback_watchdog",          test_callback_watchdog_suite},
    {"ugc_color_invariants",       test_ugc_color_invariants_suite},
    {"ugc_state_machine",          test_ugc_state_machine_suite},
    {"ugc_barrier",                test_ugc_barrier_suite},
    {"ugc_walk_roots",             test_ugc_walk_roots_suite},
    {"ugc_handle",                 test_ugc_handle_suite},
    {"ugc_finalizer",              test_ugc_finalizer_suite},
    {"tag_lifecycle",              test_tag_lifecycle_suite},
    {"strand_spawn_inheritance",   test_strand_spawn_inheritance_suite},
    {"tag_stop_realm",             test_tag_stop_realm_suite},
    {"watcher_pool",               test_watcher_pool_suite},
    {"watcher_dirty",              test_watcher_dirty_suite},
    {"fork",                       test_fork_suite},
    {"determinism",                test_determinism_suite},
    {"sched_fifo",                 test_sched_fifo_suite},
    {"sched_pool_exhaust",         test_sched_pool_exhaust_suite},
    {"pipe_budget_exhaust",        test_pipe_budget_exhaust_suite},
    {"determinism_two_runs",       test_determinism_two_runs_suite},
    {"determinism_tunable_pin",    test_determinism_tunable_pin_suite},
    {"uobject",                    test_uobject_suite},
    {"uslothandle",                test_uslothandle_suite},
    {"ushape",                     test_ushape_suite},
    {"uic",                        test_uic_suite},
    {"topology_gen",               test_topology_gen_suite},
    {"ugc_object_cells",           test_ugc_object_cells_suite},
    {"gc_strand_walker",           test_gc_strand_walker_suite},
    {"scheduler_invariant",        test_scheduler_invariant_suite},
    {"op_allocation",              test_op_allocation_suite},
    {"disasm",                     test_disasm_suite},
    {"gc_byte",                    test_gc_byte_suite},
    {"ast_alloc",                  test_ast_alloc_suite},
    {"uwatcher_layout",            test_uwatcher_layout_suite},
    {"ustrand_layout",             test_ustrand_layout_suite},
    {"uevent",                     test_uevent_suite},
    {"utag_gc",                    test_utag_gc_suite},
    {"uchanged_node",              test_uchanged_node_suite},
    {"uvm_trace_fields",           test_uvm_trace_fields_suite},
    {"uvm_deferred_ring",          test_uvm_deferred_ring_suite},
    {"strand_arm",                 test_strand_arm_suite},
    /* Add new suites here as test files are added. */
};

int main(void) {
    clock_t t0 = clock();

    int shard_total = 1;
    int shard_index = 0;
    const char *st = getenv("URBI_SHARD_TOTAL");
    const char *si = getenv("URBI_SHARD_INDEX");
    if (st && *st) shard_total = atoi(st);
    if (si && *si) shard_index = atoi(si);
    if (shard_total < 1) shard_total = 1;
    if (shard_index < 0 || shard_index >= shard_total) shard_index = 0;

    if (shard_total > 1) {
        printf("Running test suites (shard %d/%d)\n",
            shard_index, shard_total);
    } else {
        printf("Running test suites\n");
    }

    size_t n = sizeof(suites) / sizeof(suites[0]);
    for (size_t i = 0; i < n; i++) {
        if ((int)(i % (size_t)shard_total) == shard_index) {
            suites[i].fn();
        }
    }

    double elapsed = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("\n%d cases, %d checks, %d failed (%.3fs)\n",
        utest_cases_run, utest_checks, utest_cases_failed, elapsed);

    return utest_cases_failed > 0 ? 1 : 0;
}
