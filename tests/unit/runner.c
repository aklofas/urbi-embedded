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
extern void test_lex_string_suite(void);
extern void test_lex_float_literals_suite(void);
extern void test_lex_unicode_suite(void);
extern void test_ast_string_suite(void);
extern void test_emit_string_suite(void);
extern void test_string_literal_e2e_suite(void);
extern void test_api_version_suite(void);
extern void test_arena_suite(void);
extern void test_aux_version_check_suite(void);
extern void test_parser_suite(void);
extern void test_varint_suite(void);
extern void test_module_suite(void);
extern void test_emit_suite(void);
extern void test_vm_suite(void);
extern void test_pipeline_suite(void);
extern void test_uvalue_suite(void);
extern void test_uvalue_layout_suite(void);
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
extern void test_gc_scratch_rooting_suite(void);
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
extern void test_strand_destroy_suite(void);
extern void test_watcher_spawn_suite(void);
extern void test_watcher_completed_suite(void);
extern void test_watcher_lifecycle_suite(void);
extern void test_watcher_gc_invariants_suite(void);
extern void test_watcher_ownership_suite(void);
extern void test_emit_function_literal_suite(void);
extern void test_cond_side_effect_suite(void);
extern void test_emit_diag_suite(void);
extern void test_emit_watcher_suite(void);
extern void test_install_skeleton_suite(void);
extern void test_resolve_owning_tag_suite(void);
extern void test_install_trace_suite(void);
extern void test_waituntil_install_suite(void);
extern void test_at_install_dispatch_suite(void);
extern void test_at_fire_paths_suite(void);
extern void test_parse_at_event_suite(void);
extern void test_parse_emit_postfix_suite(void);
extern void test_uevent_subscribe_suite(void);
extern void test_at_event_dispatch_suite(void);
extern void test_event_emit_async_suite(void);
extern void test_event_emit_sync_suite(void);
extern void test_event_waituntil_suite(void);
extern void test_waituntil_tag_stop_suite(void);
extern void test_event_native_suite(void);
extern void test_event_new_scripted_suite(void);
extern void test_tag_native_suite(void);
extern void test_tag_enter_leave_suite(void);
extern void test_event_gc_suite(void);
extern void test_isr_event_drain_suite(void);
extern void test_slot_change_install_suite(void);
extern void test_op_getslot_change_event_suite(void);
extern void test_parse_at_slot_change_suite(void);
extern void test_emit_at_slot_change_suite(void);
extern void test_slot_change_emit_suite(void);
extern void test_slot_change_callsites_suite(void);
extern void test_slot_change_reentrancy_suite(void);
extern void test_registry_table_suite(void);
extern void test_realm_populate_suite(void);
extern void test_emit_global_lookup_suite(void);
extern void test_emit_global_var_suite(void);
extern void test_op_load_realm_global_suite(void);
extern void test_const_global_suite(void);
extern void test_realm_globals_api_suite(void);
extern void test_uwatcher_scratch_suite(void);
extern void test_at_scripted_e2e_suite(void);
extern void test_at_sync_scripted_suite(void);
extern void test_tag_stop_onleave_scripted_suite(void);
extern void test_event_sync_emit_scripted_suite(void);
extern void test_utest_e2e_helpers_suite(void);
extern void test_emit_freereg_drift_suite(void);
extern void test_emit_line_delta_suite(void);
extern void test_emit_error_paths_suite(void);
extern void test_vm_dispatch_ownership_suite(void);
extern void test_gc_sweep_accounting_suite(void);
extern void test_sched_state_aliasing_suite(void);
extern void test_event_runtime_suite(void);
extern void test_tag_barrier_suite(void);
extern void test_object_in_place_barrier_suite(void);
extern void test_module_loader_hardening_suite(void);
extern void test_module_alloc_nested_suite(void);
extern void test_foundations_suite(void);
extern void test_public_api_suite(void);
extern void test_atom_dispatch_suite(void);
extern void test_object_root_suite(void);
extern void test_atom_protos_suite(void);
extern void test_class_decl_parse_suite(void);
extern void test_class_decl_emit_suite(void);
extern void test_bake_tool_suite(void);
extern void test_stdlib_boot_suite(void);
extern void test_lock_heap_suite(void);
extern void test_emit_class_multi_slot_suite(void);
extern void test_vm_operator_overload_suite(void);
extern void test_emit_this_suite(void);
extern void test_emit_closure_capture_suite(void);
extern void test_vm_init_oom_suite(void);
extern void test_watcher_body_done_fn_suite(void);
extern void test_event_payload_layout_suite(void);
extern void test_make_native_closure_suite(void);
extern void test_value_kind_drift_suite(void);
extern void test_make_value_suite(void);
extern void test_value_as_suite(void);
extern void test_make_str_interned_suite(void);
extern void test_slot_get_suite(void);
extern void test_slot_set_suite(void);
extern void test_tag_create_suite(void);
extern void test_tag_info_suite(void);
extern void test_set_writer_suite(void);
extern void test_set_time_us_suite(void);
extern void test_set_wake_fn_suite(void);
extern void test_register_host_fn_suite(void);
extern void test_register_dup_name_suite(void);
extern void test_atomic_batch_suite(void);
extern void test_atomic_nesting_suite(void);
extern void test_atomic_watchdog_suite(void);
extern void test_event_register_success_suite(void);
extern void test_event_register_errors_suite(void);
extern void test_event_unregister_suite(void);
extern void test_drain_routing_registered_suite(void);
extern void test_drain_routing_unregistered_suite(void);
extern void test_register_watcher_callback_suite(void);
extern void test_watcher_auto_unregister_suite(void);
extern void test_unregister_watcher_suite(void);
extern void test_watcher_done_fanout_suite(void);

struct suite_entry {
    const char *name;
    void (*fn)(void);
};

static const struct suite_entry suites[] = {
    {"version",                    test_version_suite},
    {"lexer",                      test_lexer_suite},
    {"lex_keywords",               test_lex_keywords_suite},
    {"lex_string",                 test_lex_string_suite},
    {"lex_float_literals",         test_lex_float_literals_suite},
    {"lex_unicode",                test_lex_unicode_suite},
    {"ast_string",                 test_ast_string_suite},
    {"emit_string",                test_emit_string_suite},
    {"string_literal_e2e",         test_string_literal_e2e_suite},
    {"api_version",                test_api_version_suite},
    {"arena",                      test_arena_suite},
    {"aux_version_check",          test_aux_version_check_suite},
    {"parser",                     test_parser_suite},
    {"varint",                     test_varint_suite},
    {"module",                     test_module_suite},
    {"emit",                       test_emit_suite},
    {"vm",                         test_vm_suite},
    {"pipeline",                   test_pipeline_suite},
    {"uvalue",                     test_uvalue_suite},
    {"uvalue_layout",              test_uvalue_layout_suite},
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
    {"gc_scratch_rooting",         test_gc_scratch_rooting_suite},
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
    {"strand_destroy",             test_strand_destroy_suite},
    {"watcher_spawn",              test_watcher_spawn_suite},
    {"watcher_completed",          test_watcher_completed_suite},
    {"watcher_lifecycle",          test_watcher_lifecycle_suite},
    {"watcher_gc_invariants",      test_watcher_gc_invariants_suite},
    {"watcher_ownership",          test_watcher_ownership_suite},
    {"emit_function_literal",      test_emit_function_literal_suite},
    {"cond_side_effect",           test_cond_side_effect_suite},
    {"emit_diag",                  test_emit_diag_suite},
    {"emit_watcher",               test_emit_watcher_suite},
    {"install_skeleton",           test_install_skeleton_suite},
    {"resolve_owning_tag",         test_resolve_owning_tag_suite},
    {"install_trace",              test_install_trace_suite},
    {"waituntil_install",          test_waituntil_install_suite},
    {"at_install_dispatch",        test_at_install_dispatch_suite},
    {"at_fire_paths",              test_at_fire_paths_suite},
    {"parse_at_event",             test_parse_at_event_suite},
    {"parse_emit_postfix",         test_parse_emit_postfix_suite},
    {"uevent_subscribe",           test_uevent_subscribe_suite},
    {"at_event_dispatch",          test_at_event_dispatch_suite},
    {"event_emit_async",           test_event_emit_async_suite},
    {"event_emit_sync",            test_event_emit_sync_suite},
    {"event_waituntil",            test_event_waituntil_suite},
    {"waituntil_tag_stop",         test_waituntil_tag_stop_suite},
    {"event_native",               test_event_native_suite},
    {"event_new_scripted",         test_event_new_scripted_suite},
    {"tag_native",                 test_tag_native_suite},
    {"tag_enter_leave",            test_tag_enter_leave_suite},
    {"event_gc",                   test_event_gc_suite},
    {"isr_event_drain",            test_isr_event_drain_suite},
    {"slot_change_install",        test_slot_change_install_suite},
    {"op_getslot_change_event",    test_op_getslot_change_event_suite},
    {"parse_at_slot_change",       test_parse_at_slot_change_suite},
    {"emit_at_slot_change",        test_emit_at_slot_change_suite},
    {"slot_change_emit",           test_slot_change_emit_suite},
    {"slot_change_callsites",      test_slot_change_callsites_suite},
    {"slot_change_reentrancy",     test_slot_change_reentrancy_suite},
    {"registry_table",            test_registry_table_suite},
    {"realm_populate",            test_realm_populate_suite},
    {"emit_global_lookup",        test_emit_global_lookup_suite},
    {"emit_global_var",           test_emit_global_var_suite},
    {"op_load_realm_global",      test_op_load_realm_global_suite},
    {"const_global",              test_const_global_suite},
    {"realm_globals_api",         test_realm_globals_api_suite},
    {"uwatcher_scratch",          test_uwatcher_scratch_suite},
    {"at_scripted_e2e",           test_at_scripted_e2e_suite},
    {"at_sync_scripted",          test_at_sync_scripted_suite},
    {"tag_stop_onleave_scripted", test_tag_stop_onleave_scripted_suite},
    {"event_sync_emit_scripted",  test_event_sync_emit_scripted_suite},
    {"utest_e2e_helpers",         test_utest_e2e_helpers_suite},
    {"emit_freereg_drift",        test_emit_freereg_drift_suite},
    {"emit_line_delta",           test_emit_line_delta_suite},
    {"emit_error_paths",          test_emit_error_paths_suite},
    {"vm_dispatch_ownership",     test_vm_dispatch_ownership_suite},
    {"gc_sweep_accounting",       test_gc_sweep_accounting_suite},
    {"sched_state_aliasing",      test_sched_state_aliasing_suite},
    {"event_runtime",             test_event_runtime_suite},
    {"tag_barrier",               test_tag_barrier_suite},
    {"object_in_place_barrier",   test_object_in_place_barrier_suite},
    {"module_loader_hardening",   test_module_loader_hardening_suite},
    {"module_alloc_nested",       test_module_alloc_nested_suite},
    {"foundations",               test_foundations_suite},
    {"public_api",                test_public_api_suite},
    {"atom_dispatch",             test_atom_dispatch_suite},
    {"object_root",               test_object_root_suite},
    {"atom_protos",               test_atom_protos_suite},
    {"class_decl_parse",          test_class_decl_parse_suite},
    {"class_decl_emit",           test_class_decl_emit_suite},
    {"bake_tool",                 test_bake_tool_suite},
    {"stdlib_boot",               test_stdlib_boot_suite},
    {"lock_heap",                 test_lock_heap_suite},
    {"emit_class_multi_slot",    test_emit_class_multi_slot_suite},
    {"vm_operator_overload",     test_vm_operator_overload_suite},
    {"emit_this",                test_emit_this_suite},
    {"emit_closure_capture",    test_emit_closure_capture_suite},
    {"vm_init_oom",             test_vm_init_oom_suite},
    {"watcher_body_done_fn",    test_watcher_body_done_fn_suite},
    {"event_payload_layout",   test_event_payload_layout_suite},
    {"make_native_closure",    test_make_native_closure_suite},
    {"value_kind_drift",       test_value_kind_drift_suite},
    {"make_value",             test_make_value_suite},
    {"value_as",               test_value_as_suite},
    {"make_str_interned",      test_make_str_interned_suite},
    {"slot_get",               test_slot_get_suite},
    {"slot_set",               test_slot_set_suite},
    {"tag_create",             test_tag_create_suite},
    {"tag_info",               test_tag_info_suite},
    {"set_writer",             test_set_writer_suite},
    {"set_time_us",            test_set_time_us_suite},
    {"set_wake_fn",            test_set_wake_fn_suite},
    {"register_host_fn",      test_register_host_fn_suite},
    {"register_dup_name",     test_register_dup_name_suite},
    {"atomic_batch",          test_atomic_batch_suite},
    {"atomic_nesting",        test_atomic_nesting_suite},
    {"atomic_watchdog",       test_atomic_watchdog_suite},
    {"event_register_success", test_event_register_success_suite},
    {"event_register_errors",  test_event_register_errors_suite},
    {"event_unregister",             test_event_unregister_suite},
    {"drain_routing_registered",     test_drain_routing_registered_suite},
    {"drain_routing_unregistered",   test_drain_routing_unregistered_suite},
    {"register_watcher_callback",    test_register_watcher_callback_suite},
    {"watcher_auto_unregister",      test_watcher_auto_unregister_suite},
    {"unregister_watcher",           test_unregister_watcher_suite},
    {"watcher_done_fanout",          test_watcher_done_fanout_suite},
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
