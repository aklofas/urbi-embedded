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
extern void test_lex_every_suite(void);
extern void test_lex_string_suite(void);
extern void test_lex_float_literals_suite(void);
extern void test_lex_unicode_suite(void);
extern void test_ast_string_suite(void);
extern void test_emit_string_suite(void);
extern void test_string_literal_e2e_suite(void);
extern void test_api_version_suite(void);
extern void test_port_allocator_mock_suite(void);
extern void test_arena_suite(void);
extern void test_arena_reuse_suite(void);
extern void test_aux_version_check_suite(void);
extern void test_parser_suite(void);
extern void test_varint_suite(void);
extern void test_module_suite(void);
extern void test_emit_suite(void);
extern void test_pipeline_suite(void);
extern void test_uvalue_suite(void);
extern void test_uvalue_layout_suite(void);
extern void test_multi_vm_suite(void);
extern void test_intern_suite(void);
extern void test_funcstate_suite(void);
extern void test_separators_suite(void);
extern void test_lazy_suite(void);
extern void test_chunk_apis_suite(void);
extern void test_callback_watchdog_suite(void);
extern void test_ugc_color_invariants_suite(void);
extern void test_gc_stress_mode_suite(void);
extern void test_ugc_finalizer_suite(void);
extern void test_determinism_two_runs_suite(void);
extern void test_ic_polymorphic_suite(void);
extern void test_op_allocation_suite(void);
extern void test_disasm_suite(void);
extern void test_ast_alloc_suite(void);
extern void test_uvm_trace_fields_suite(void);
extern void test_emit_function_literal_suite(void);
extern void test_cond_side_effect_suite(void);
extern void test_emit_diag_suite(void);
extern void test_emit_const_index_suite(void);
extern void test_emit_patch_limit_suite(void);
extern void test_parse_bounds_suite(void);
extern void test_parse_at_event_suite(void);
extern void test_parse_every_suite(void);
extern void test_parse_emit_postfix_suite(void);
extern void test_parse_at_slot_change_suite(void);
extern void test_emit_at_slot_change_suite(void);
extern void test_emit_global_lookup_suite(void);
extern void test_emit_global_var_suite(void);
extern void test_op_load_realm_global_suite(void);
extern void test_const_global_suite(void);
extern void test_utest_e2e_helpers_suite(void);
extern void test_emit_freereg_drift_suite(void);
extern void test_emit_line_delta_suite(void);
extern void test_emit_error_paths_suite(void);
extern void test_gc_sweep_accounting_suite(void);
extern void test_module_loader_hardening_suite(void);
extern void test_module_alloc_nested_suite(void);
extern void test_object_root_suite(void);
extern void test_class_decl_parse_suite(void);
extern void test_bake_tool_suite(void);
extern void test_stdlib_boot_suite(void);
extern void test_lock_heap_suite(void);
extern void test_emit_this_suite(void);
extern void test_emit_closure_capture_suite(void);
extern void test_vm_init_oom_suite(void);
extern void test_watcher_body_done_fn_suite(void);
extern void test_event_payload_layout_suite(void);
extern void test_value_kind_drift_suite(void);
extern void test_make_value_suite(void);
extern void test_value_as_suite(void);
extern void test_make_str_interned_suite(void);
extern void test_set_writer_suite(void);
extern void test_set_time_us_suite(void);
extern void test_set_wake_fn_suite(void);
extern void test_set_diag_fn_suite(void);
extern void test_register_host_fn_suite(void);
extern void test_register_dup_name_suite(void);
extern void test_atomic_nesting_suite(void);
extern void test_atomic_watchdog_suite(void);
extern void test_event_register_errors_suite(void);
extern void test_event_unregister_suite(void);
extern void test_athandler_class_method_fatal_suite(void);
extern void test_chunktop_realm_closure_suite(void);
extern void test_emit_nary_freereg_drift_suite(void);
extern void test_unwind_method_try_catch_suite(void);
extern void test_whenever_double_fire_suite(void);
extern void test_parse_block_in_at_body_suite(void);
extern void test_pipe_middle_stmt_suite(void);
extern void test_waituntil_cascade_suite(void);
extern void test_waituntil_loader_wake_suite(void);
extern void test_dangling_cl_function_body_install_suite(void);
extern void test_proto_refcount_suite(void);
extern void test_module_refcount_suite(void);
extern void test_fork_amp_at_body_suite(void);
extern void test_imu_integration_suite(void);
extern void test_last_error_populated_suite(void);
extern void test_error_string_lifetime_suite(void);
extern void test_ref_basic_suite(void);
extern void test_ref_capacity_suite(void);
extern void test_aux_event_table_suite(void);
extern void test_aux_function_table_suite(void);
extern void test_aux_set_error_suite(void);
extern void test_aux_load_and_run_suite(void);
extern void test_aux_dump_value_suite(void);
extern void test_detect_blob_suite(void);
extern void test_draw_crosshair_suite(void);
extern void test_test_helper_uaf_repro_suite(void);
extern void test_uproto_root_backptr_suite(void);
extern void test_module_grain_lifetime_suite(void);
extern void test_uproto_owning_mi_suite(void);
extern void test_loaded_protos_registry_suite(void);
extern void test_multi_realm_suite(void);
extern void test_urbi_unload_suite(void);
extern void test_realm_destroy_with_parked_loader_suite(void);
extern void test_lexer_syncline_suite(void);
extern void test_repl_per_realm_writer_suite(void);
extern void test_repl_global_atom_suite(void);
extern void test_compile_budget_suite(void);
extern void test_repl_serve_step_cooperative_suite(void);
extern void test_require_suite(void);
extern void test_verify_chunk_bounds_suite(void);
extern void test_ic_index_dfs_suite(void);
extern void test_value_predicates_suite(void);
extern void test_error_model_unified_suite(void);
extern void test_vm_create_opaque_suite(void);
extern void test_vm_create_oom_suite(void);
extern void test_vm_first_arg_suite(void);
extern void test_vm_slot_helpers_suite(void);
extern void test_vm_tag_scope_suite(void);
extern void test_vm_reactive_install_suite(void);
extern void test_isa_method_suite(void);
extern void test_detach_disown_suite(void);
extern void test_scope_tag_suite(void);
extern void test_connection_tag_suite(void);
extern void test_lshift_parse_suite(void);
extern void test_lobby_echo_suite(void);
extern void test_runtime_typed_throw_suite(void);
extern void test_trace_suite(void);
extern void test_perf_counters_suite(void);
extern void test_ros_proto_suite(void);
extern void test_ros_mock_suite(void);
extern void test_ros_registry_suite(void);
extern void test_ros_publish_suite(void);
extern void test_ros_service_suite(void);
extern void test_ros_pump_suite(void);
extern void test_ros_bridge_lifetime_suite(void);
extern void test_ulist_build_suite(void);
extern void test_regexp_suite(void);
extern void test_object_reflection_suite(void);
extern void test_lex_operators_suite(void);
extern void test_atoms_random_suite(void);
extern void test_verifier_cross_byte_suite(void);
extern void test_batch_error_surfacing_suite(void);
extern void test_chunk_loader_hardening_suite(void);
extern void test_default_params_suite(void);
extern void test_message_polish_suite(void);
extern void test_stdlib_install_suite(void);

struct suite_entry {
    const char *name;
    void (*fn)(void);
};

static const struct suite_entry suites[] = {
    {"version",                    test_version_suite},
    {"lexer",                      test_lexer_suite},
    {"lex_keywords",               test_lex_keywords_suite},
    {"lex_every",                  test_lex_every_suite},
    {"lex_string",                 test_lex_string_suite},
    {"lex_float_literals",         test_lex_float_literals_suite},
    {"lex_unicode",                test_lex_unicode_suite},
    {"ast_string",                 test_ast_string_suite},
    {"emit_string",                test_emit_string_suite},
    /* Ordered before the first VM-running suite (string_literal_e2e) so the
     * stress arm executes under test-gc-stress before the known baseline
     * boot crash kills the runner (refactor-3 TEST-GAP-01). */
    {"gc_stress_mode",             test_gc_stress_mode_suite},
    /* Same early ordering as gc_stress_mode: the rooting-matrix cases must
     * execute under test-gc-stress before the known baseline boot crash in
     * string_literal_e2e kills the runner (refactor-3 GC-17). */
    {"string_literal_e2e",         test_string_literal_e2e_suite},
    {"api_version",                test_api_version_suite},
    {"port_allocator_mock",        test_port_allocator_mock_suite},
    {"arena",                      test_arena_suite},
    {"arena_reuse",                test_arena_reuse_suite},
    {"aux_version_check",          test_aux_version_check_suite},
    {"parser",                     test_parser_suite},
    {"varint",                     test_varint_suite},
    {"module",                     test_module_suite},
    {"emit",                       test_emit_suite},
    {"pipeline",                   test_pipeline_suite},
    {"uvalue",                     test_uvalue_suite},
    {"uvalue_layout",              test_uvalue_layout_suite},
    {"multi_vm",                   test_multi_vm_suite},
    {"intern",                     test_intern_suite},
    {"funcstate",                  test_funcstate_suite},
    {"separators",                 test_separators_suite},
    {"lazy",                       test_lazy_suite},
    {"chunk_apis",                 test_chunk_apis_suite},
    {"callback_watchdog",          test_callback_watchdog_suite},
    {"ugc_color_invariants",       test_ugc_color_invariants_suite},
    {"ugc_finalizer",              test_ugc_finalizer_suite},
    {"determinism_two_runs",       test_determinism_two_runs_suite},
    {"ic_polymorphic",             test_ic_polymorphic_suite},
    {"op_allocation",              test_op_allocation_suite},
    {"disasm",                     test_disasm_suite},
    {"ast_alloc",                  test_ast_alloc_suite},
    {"uvm_trace_fields",           test_uvm_trace_fields_suite},
    {"urbi_emit_function_literal",      test_emit_function_literal_suite},
    {"cond_side_effect",           test_cond_side_effect_suite},
    {"emit_diag",                  test_emit_diag_suite},
    {"emit_const_index",           test_emit_const_index_suite},
    {"emit_patch_limit",           test_emit_patch_limit_suite},
    {"parse_bounds",               test_parse_bounds_suite},
    {"parse_at_event",             test_parse_at_event_suite},
    {"parse_every",                test_parse_every_suite},
    {"parse_emit_postfix",         test_parse_emit_postfix_suite},
    {"parse_at_slot_change",       test_parse_at_slot_change_suite},
    {"emit_at_slot_change",        test_emit_at_slot_change_suite},
    {"emit_global_lookup",        test_emit_global_lookup_suite},
    {"emit_global_var",           test_emit_global_var_suite},
    {"op_load_realm_global",      test_op_load_realm_global_suite},
    {"const_global",              test_const_global_suite},
    {"utest_e2e_helpers",         test_utest_e2e_helpers_suite},
    {"emit_freereg_drift",        test_emit_freereg_drift_suite},
    {"emit_line_delta",           test_emit_line_delta_suite},
    {"emit_error_paths",          test_emit_error_paths_suite},
    {"gc_sweep_accounting",       test_gc_sweep_accounting_suite},
    {"module_loader_hardening",   test_module_loader_hardening_suite},
    {"module_alloc_nested",       test_module_alloc_nested_suite},
    {"object_root",               test_object_root_suite},
    {"class_decl_parse",          test_class_decl_parse_suite},
    {"bake_tool",                 test_bake_tool_suite},
    {"stdlib_boot",               test_stdlib_boot_suite},
    {"lock_heap",                 test_lock_heap_suite},
    {"emit_this",                test_emit_this_suite},
    {"emit_closure_capture",    test_emit_closure_capture_suite},
    {"vm_init_oom",             test_vm_init_oom_suite},
    {"watcher_body_done_fn",    test_watcher_body_done_fn_suite},
    {"event_payload_layout",   test_event_payload_layout_suite},
    {"value_kind_drift",       test_value_kind_drift_suite},
    {"make_value",             test_make_value_suite},
    {"value_as",               test_value_as_suite},
    {"make_str_interned",      test_make_str_interned_suite},
    {"set_writer",             test_set_writer_suite},
    {"set_time_us",            test_set_time_us_suite},
    {"set_wake_fn",            test_set_wake_fn_suite},
    {"set_diag_fn",            test_set_diag_fn_suite},
    {"register_host_fn",      test_register_host_fn_suite},
    {"register_dup_name",     test_register_dup_name_suite},
    {"atomic_nesting",        test_atomic_nesting_suite},
    {"atomic_watchdog",       test_atomic_watchdog_suite},
    {"event_register_errors",  test_event_register_errors_suite},
    {"event_unregister",             test_event_unregister_suite},
    {"athandler_class_method_fatal", test_athandler_class_method_fatal_suite},
    {"chunktop_realm_closure",       test_chunktop_realm_closure_suite},
    {"emit_nary_freereg_drift",      test_emit_nary_freereg_drift_suite},
    {"unwind_method_try_catch",      test_unwind_method_try_catch_suite},
    {"whenever_double_fire",         test_whenever_double_fire_suite},
    {"parse_block_in_at_body",       test_parse_block_in_at_body_suite},
    {"pipe_middle_stmt",             test_pipe_middle_stmt_suite},
    {"waituntil_cascade",            test_waituntil_cascade_suite},
    {"waituntil_loader_wake",        test_waituntil_loader_wake_suite},
    {"dangling_cl_function_body_install", test_dangling_cl_function_body_install_suite},
    {"proto_refcount",               test_proto_refcount_suite},
    {"module_refcount",              test_module_refcount_suite},
    {"fork_amp_at_body",             test_fork_amp_at_body_suite},
    {"imu_integration",              test_imu_integration_suite},
    {"last_error_populated",         test_last_error_populated_suite},
    {"error_string_lifetime",        test_error_string_lifetime_suite},
    {"ref_basic",                    test_ref_basic_suite},
    {"ref_capacity",                 test_ref_capacity_suite},
    {"aux_event_table",              test_aux_event_table_suite},
    {"aux_function_table",           test_aux_function_table_suite},
    {"aux_set_error",                test_aux_set_error_suite},
    {"aux_load_and_run",             test_aux_load_and_run_suite},
    {"aux_dump_value",               test_aux_dump_value_suite},
    {"detect_blob",                  test_detect_blob_suite},
    {"draw_crosshair",               test_draw_crosshair_suite},
    {"test_helper_uaf_repro",        test_test_helper_uaf_repro_suite},
    {"uproto_root_backptr",          test_uproto_root_backptr_suite},
    {"module_grain_lifetime",        test_module_grain_lifetime_suite},
    {"uproto_owning_mi",             test_uproto_owning_mi_suite},
    {"loaded_protos_registry",       test_loaded_protos_registry_suite},
    {"multi_realm",                  test_multi_realm_suite},
    {"urbi_unload",                  test_urbi_unload_suite},
    {"realm_destroy_with_parked_loader", test_realm_destroy_with_parked_loader_suite},
    {"lexer_syncline",                   test_lexer_syncline_suite},
    {"repl_per_realm_writer",            test_repl_per_realm_writer_suite},
    {"repl_global_atom",                 test_repl_global_atom_suite},
    {"compile_budget",                   test_compile_budget_suite},
    {"repl_serve_step_cooperative",      test_repl_serve_step_cooperative_suite},
    {"require",                          test_require_suite},
    {"verify_chunk_bounds",              test_verify_chunk_bounds_suite},
    {"ic_index_dfs",                     test_ic_index_dfs_suite},
    {"value_predicates",                 test_value_predicates_suite},
    {"error_model_unified",              test_error_model_unified_suite},
    {"vm_create_opaque",                 test_vm_create_opaque_suite},
    {"vm_create_oom",                    test_vm_create_oom_suite},
    {"vm_first_arg",                     test_vm_first_arg_suite},
    {"vm_slot_helpers",                  test_vm_slot_helpers_suite},
    {"vm_tag_scope",                     test_vm_tag_scope_suite},
    {"urbi_vm_reactive_install",              test_vm_reactive_install_suite},
    {"isa_method",                       test_isa_method_suite},
    {"detach_disown",                    test_detach_disown_suite},
    {"scope_tag",                        test_scope_tag_suite},
    {"connection_tag",                   test_connection_tag_suite},
    {"lshift_parse",                     test_lshift_parse_suite},
    {"lobby_echo",                       test_lobby_echo_suite},
    {"runtime_typed_throw",              test_runtime_typed_throw_suite},
    {"trace",                            test_trace_suite},
    {"perf_counters",                    test_perf_counters_suite},
    {"ros_proto",                        test_ros_proto_suite},
    {"ros_mock",                         test_ros_mock_suite},
    {"ros_registry",                     test_ros_registry_suite},
    {"ros_publish",                      test_ros_publish_suite},
    {"ros_service",                      test_ros_service_suite},
    {"ros_pump",                         test_ros_pump_suite},
    {"ros_bridge_lifetime",              test_ros_bridge_lifetime_suite},
    {"ulist_build",                      test_ulist_build_suite},
    {"regexp",                           test_regexp_suite},
    {"object_reflection",                test_object_reflection_suite},
    {"lex_operators",                    test_lex_operators_suite},
    {"atoms_random",                     test_atoms_random_suite},
    {"verifier_cross_byte",                   test_verifier_cross_byte_suite},
    {"batch_error_surfacing",                 test_batch_error_surfacing_suite},
    {"chunk_loader_hardening",                test_chunk_loader_hardening_suite},
    {"default_params",                        test_default_params_suite},
    {"message_polish",                        test_message_polish_suite},
    {"stdlib_install",                        test_stdlib_install_suite},
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
        printf("================================================================\n"
               "== SHARDED RUN (%d/%d) — URBI_SHARD_TOTAL/URBI_SHARD_INDEX set.\n"
               "== Only suites with (suite_index %% %d == %d) execute below.\n"
               "== This is NOT a full pass; do not cite it as one.\n"
               "================================================================\n",
            shard_index, shard_total, shard_total, shard_index);
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
