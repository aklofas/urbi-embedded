# Harvest ledger — runtime-internals unit tests

Candidates: 187 files under `tests/unit/` matching the Step 1 private-include regex.
Verdicts: 163 DELETE, 24 KEEP.

Method. Question 1 was run mechanically on every candidate: copy the file to a
scratch probe, strip the private `#include` directives matched by the Step 1
regex, then compile the probe with
`cc -std=c99 -fsyntax-only -Iinclude -Isrc -Itests/unit`. Two corrections were
needed and are recorded per row.

- Feature-gated suites (`URBI_ENABLE_REPL`, `URBI_ENABLE_ROS2`, `URBI_DEBUG`,
  `URBI_MEM_DEBUG`, `URBI_PERF_COUNTERS`) compile *vacuously* with the gate
  undefined — the whole body is preprocessed away, so the bare command reports
  a pass that proves nothing. Every non-FAIL candidate was re-probed with the
  gates defined and its live preprocessed line count measured; 22 files flipped
  to FAIL and 2 remain unverifiable (ROS2 needs external headers). Rows say
  `vacuous under bare probe`.
- GCC 13 accepts a call to an undeclared function as a *warning*, so a file can
  syntax-check while still depending on private-header functions. Rows whose
  only such dependency is `urbi_zero` / `urbi_strlen` (`static inline` libc
  shims in `src/runtime/umacros.h`) are KEEP — the strip is a one-line
  substitution. Rows that implicitly declare a real private function are DELETE
  and say `implicit decl of <fn>`.

`covered by` cells cite only ACTIVE fixtures. The corpus holds 387 `.chk` files,
86 of which are annotated placeholders (`# blocked:` / `# deferred:` /
`# dropped:`) that never execute; a placeholder does not count as coverage. Where
a placeholder already records the wanted behavior, the harvest cell names it with
`(activate placeholder)` so Task 5 fills in that file rather than adding a second
one.

| file | verdict | reason | harvest |
|---|---|---|---|
| test_at_event_dispatch.c | DELETE | drives `urbi_watcher_install_at_event_runtime` directly; asserts `event->at_watchers_head` link order, watcher mode byte, back-pointer, pool exhaustion | none (event-chain link fields and watcher-pool exhaustion; the `at (e?)` subscribe-and-fire behavior is covered by reactive/at_event_payload.chk) |
| test_at_event_unlink_on_tag_stop.c | DELETE | asserts `e->at_watchers_head == NULL` and `strand_runnable_count` deltas after tag-stop | reactive/event/at_event_unsubscribes_on_tag_stop.chk: an `at (e?)` watcher installed inside a tag scope stops running its body on emissions that happen after that tag is stopped |
| test_at_fire_paths.c | DELETE | injects behavior through `test_watcher_condition_hook` / `_fire_hook` / `_onleave_hook` fields instead of running bytecode | covered by reactive/at/at_rising_edge.chk, reactive/at/whenever_level.chk, reactive/at/at_onleave.chk, reactive/waituntil_event.chk; plus reactive/at/at_sync_body_runs_before_next_statement.chk: an `at sync (cond)` body finishes before the statement that flipped the condition returns, unlike async `at` |
| test_athandler_wedge_repro.c | DELETE | bisection harness for the `pending_refire_count` widening; reads watcher counter fields and drain throughput | reactive/event/event_burst_fires_body_each_emission.chk: N back-to-back emissions on one event each run the `at (e?)` body once, so a counter incremented in the body reaches N rather than saturating at 2 |
| test_at_install_dispatch.c | DELETE | implicit decl of `urbi_watcher_unregister_internal`; asserts `active_watchers_head` non-NULL after install | reactive/at/waituntil_true_at_install_does_not_park.chk: `waituntil(cond)` whose condition is already true at install returns immediately and the next statement on the same line still runs |
| test_atom_dispatch.c | DELETE | calls `urbi_atom_proto_for_value` and inspects shape sentinels | covered by objects/atom_method_dispatch.chk, objects/atoms.chk, objects/slot-cow-const.chk |
| test_atomic_batch.c | DELETE | implicit decl of `uevent_ring_drain`; asserts ISR-ring occupancy while `atomic_active` is set | none (host ISR-ring batching has no urbiscript surface; the script-level atomic separator is covered by separator/inplace-atomic.chk) |
| test_atom_protos.c | DELETE | implicit decl of `urbi_atom_proto_for_value`; asserts realm-global proto identity and singleton pointers | covered by objects/atoms.chk, objects/atom_proto_clone.chk, stdlib/atoms/boolean.chk, stdlib/runtime/bool_nil_asstring.chk |
| test_at_scripted_e2e.c | DELETE | implicit decl of `urbi_watcher_unregister_internal`; otherwise a real scripted rising-edge run read back through `urbi_realm_get_global` | covered by reactive/at/at_rising_edge.chk |
| test_at_sync_scripted.c | DELETE | implicit decl of `urbi_watcher_unregister_internal`; scripted `at sync` run read back through `urbi_realm_get_global` | reactive/at/at_sync_body_runs_before_next_statement.chk (same fixture as test_at_fire_paths.c row) |
| test_batch_error_surfacing.c | KEEP | compiles with private includes removed; only `urbi_zero` is lost | — |
| test_budget_rearm.c | DELETE | zeroes `safepoint_budget_remaining` and sets `gc_pending` by hand, then counts GC slices | scheduler/strand_budget_yield_completes_program.chk: a long-running loop driven with a small per-step budget still reaches its final value across repeated steps instead of stalling once the budget is first exhausted |
| test_callback_watchdog.c | KEEP | compiles with private includes removed | — |
| test_capi_unwind.c | DELETE | asserts unwind-status enum values deposited by `urbi_strand_cancel` / `urbi_throw` / `urbi_tag_stop_local` on a bare strand | covered by control_transfer/throw_uncaught.chk, control_transfer/tag_stop_basic.chk, control_transfer/tag_stop_no_target.chk, scheduler/dispatch_safepoint_pending_unwind.chk |
| test_channel_proto.c | DELETE | asserts `vm->channel_proto` is non-NULL and pokes a capture writer | covered by stdlib/runtime/channel_basic.chk, stdlib/runtime/cout_shift.chk |
| test_class_decl_emit.c | DELETE | asserts desugaring shape and proto insertion order via slot read-back on internal handles | covered by objects/class_decl_basic.chk, objects/class_decl_multi_proto.chk, objects/class_decl_nested.chk, objects/class-multi-slot.chk |
| test_cleanup.c | DELETE | pins `sizeof(UCleanupEntry) == 48` and push/pop depth counters | none (struct size and LIFO bookkeeping) |
| test_cleanup_yield.c | DELETE | asserts `URBI_STEP_FATAL` plus strand DEAD-and-off-every-queue after a blocking cleanup body | control_transfer/finally_that_sleeps_is_fatal.chk: a `finally` body that calls `sleep` reports a loud uncaught error rather than silently truncating the cleanup |
| test_closure_gc.c | DELETE | asserts cell survival on the all-cells list across `urbi_gc_force_full` | covered by gc/reactive_churn.chk, gc/container_element_survives.chk, closure/counter.chk |
| test_deferred_slot_change_ring_roots.c | DELETE | implicit decl of `urbi_deferred_slot_changes_walk_roots`; hand-writes ring entries and calls the walker | none (GC root-provider plumbing for the deferred ring; new core has a stress mode) |
| test_determinism.c | DELETE | vacuous under bare probe (URBI_DEBUG gate); asserts checksum stability over VM-internal topology and IC state | none (URBI_DEBUG diagnostic checksum over internal counters) |
| test_determinism_tunable_pin.c | DELETE | mutates `safepoint_budget_remaining` and counts `urbi_step` calls to completion | covered by scheduler/strand_budget_yield_completes_program.chk (named in the test_budget_rearm.c row) |
| test_dispatch_loop.c | DELETE | hand-encodes bytecode and calls `urbi_vm_dispatch_loop_until_yield` | covered by separator/yield_seq.chk, control_transfer/return_basic.chk, control_transfer/try_finally.chk, control_transfer/throw_caught.chk, scheduler/gc_slice_at_safepoint.chk, scheduler/safepoint_backward_branch.chk |
| test_drain_routing_registered.c | DELETE | implicit decl of `uevent_ring_drain`; asserts destructure-fn invocation and raw payload bytes | none (host event-registry marshalling has no urbiscript surface; the `at (e?)` body-fires-on-emit half is covered by reactive/at_event_payload.chk) |
| test_drain_routing_unregistered.c | DELETE | implicit decl of `uevent_ring_drain`; asserts legacy-handler fallthrough by event id range | none (host drain-handler routing; no urbiscript surface) |
| test_emit_class_multi_slot.c | DELETE | reads emitted slots back through internal object handles | covered by objects/class-multi-slot.chk, objects/class_decl_basic.chk |
| test_emit_this.c | KEEP | compiles with private includes removed | — |
| test_emit_watcher.c | DELETE | asserts which install opcode the emitter produced and builds an AST by hand | none (opcode selection and a compile-time side-effect warning; install behavior covered by reactive/at/at_rising_edge.chk and reactive/waituntil_event.chk) |
| test_error_ring_cascade.c | DELETE | implicit decl of `urbi_set_error_internal`; asserts error-ring depth and wrap | none (host error-ring buffer; `urbi_last_error` is a C-API surface) |
| test_event_emit_async.c | DELETE | asserts watcher-list walk order and `USTRAND_WAIT_EVENT` state after a manual emit | reactive/event/emit_multi_subscriber_fifo.chk: two `at (e?)` subscribers on the same event run their bodies in registration order on a single emission |
| test_event_emit_sync.c | DELETE | asserts `in_watcher_scratch` flag cycling via a log hook and body-strand spawn counts | reactive/event/event_sync_emit.chk (activate placeholder): a sync emission runs every subscriber body to completion before the emitting statement returns |
| test_event_gc.c | DELETE | roots cells through a test root-provider and asserts all-cells membership after GC | none (GC walker coverage for UEvent / UTag / strand payload) |
| test_event_native.c | DELETE | calls `closure->native_fn` directly and counts proto slots | covered by objects/event_new_emit.chk, reactive/whenever_event_dispatches.chk, globals/tag_event_globals.chk |
| test_event_new_scripted.c | DELETE | implicit decl of `urbi_watcher_unregister_internal`; otherwise a real scripted `Event.new()` + `at` + emit run | covered by objects/event_new_emit.chk, reactive/at_event_payload.chk |
| test_event_register_success.c | DELETE | implicit decl of `uvalue_as_event`; asserts registry ids and `UVAL_EVENT` kind of an installed global | none (host event-registration ids; the script-visible half, a registered event name resolving as a global, is covered by globals/tag_event_globals.chk) |
| test_event_ring.c | DELETE | SPSC ring indices, overflow counters, payload alignment, a 100k-iteration thread fuzz | none (ISR-safe ring internals; no urbiscript surface) |
| test_event_runtime.c | DELETE | asserts native receiver-kind validation and OOM propagation through `urbi_event_native_register` | covered by exceptions/runtime_errors_catchable.chk, exceptions/vm_typeerror_catchable.chk |
| test_event_sync_emit_scripted.c | DELETE | scripted `at sync (event?)` but asserts through a caller-owned module and internal payload plumbing | covered by reactive/event/event_sync_emit.chk (activate placeholder, named in the test_event_emit_sync.c row) |
| test_event_unregister.c | KEEP | compiles with private includes removed; only `urbi_strlen` is lost | — |
| test_event_waituntil.c | DELETE | asserts the strand lands on `e->waiters_head` in `USTRAND_WAIT_EVENT` | covered by reactive/waituntil_event.chk, scheduler/wait_event_basic.chk |
| test_fork.c | DELETE | drives fork opcodes and asserts child-strand linkage and quiescent counts | covered by separator/fork_operand_forms.chk, separator/comma.chk, separator/detach_basic.chk, separator/comma_amp_chunk_top.chk, tag/ambient_inherit_separator.chk |
| test_foundations.c | DELETE | handle-table wraparound, varint decode UB, arena overflow, format truncation, type-id collision | none (allocator, handle-table and encoding primitives below the language surface) |
| test_function.c | DELETE | asserts `module->nested[]` population, `ic_index` assignment and UProto field values | covered by function/definition.chk, function/closure_call.chk, function/recursion.chk, closure/counter.chk, closure/two_level.chk, migration/bare_function.chk, migration/closure_keyword.chk |
| test_gc_byte.c | DELETE | pins `gc_byte` bit assignments and checks for collisions | none (GC header bit layout) |
| test_gc_rooting_matrix.c | DELETE | for each internal field, constructs "this field is the only reference" and asserts survival across two collections | none (executable rooting matrix over internal fields; the new core keeps a stress mode, and gc/reactive_churn.chk plus gc/container_element_survives.chk hold the language-visible half) |
| test_gc_scratch_rooting.c | DELETE | asserts the scratch strand's register window is visited by `urbi_gc_sched_walk_roots` | none (root-walker reachability) |
| test_gc_strand_walker.c | DELETE | asserts strand membership in `realm.strands_head` and that DEAD strands are filtered | none (walker iteration order and list membership) |
| test_gc_stress_mode.c | KEEP | compiles with private includes removed | — |
| test_gc_sweep_accounting.c | KEEP | compiles with private includes removed | — |
| test_idle_vm_pump.c | DELETE | asserts `in_eval` save/restore across a nested drain and orphan-push bookkeeping | covered by scheduler/host_write_wakes_waituntil.chk, reactive/at/onleave_cascade.chk |
| test_imu_integration.c | KEEP | compiles with private includes removed | — |
| test_install_skeleton.c | DELETE | asserts `UWATCHER_INSTALL_*` return codes, pool exhaustion, bit-6 marking and list append | none (watcher-install return codes and pool bookkeeping) |
| test_install_trace.c | DELETE | asserts `trace_read_set[]` contents, dedupe and overflow clamping | none (read-set capture internals; the observable consequence, a watcher re-firing when any read slot changes, is covered by reactive/at/at_rising_edge.chk) |
| test_introspect_each.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); implicit decl of the nine `urbi_introspect_*` primitives | covered by repl/introspect_coros.chk, repl/debug_coros.chk |
| test_isr_event_drain.c | DELETE | implicit decl of `uevent_ring_drain`; asserts drain-handler call counts and FIFO order | none (ISR drain callback; no urbiscript surface) |
| test_job_proto.c | DELETE | asserts the `__strand` slot holds a raw pointer cast to `UVAL_INT` | covered by stdlib/runtime/job_current_basic.chk, stdlib/runtime/job_jobs_basic.chk |
| test_json_parse.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); exercises the REPL JSON parser directly | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_lexer_syncline.c | KEEP | compiles with private includes removed; only `urbi_zero` is lost | — |
| test_lex_operators.c | KEEP | compiles with private includes removed; only `urbi_strlen` is lost | — |
| test_loaded_protos_registry.c | KEEP | compiles with private includes removed; only `urbi_zero` is lost | — |
| test_loader_strand_persistence.c | DELETE | asserts the loader strand persists on `realm->strands_head` and is non-transient | covered by temporal/sleep_basic.chk, separator/comma_amp_chunk_top.chk, chunk_lifecycle/script_at_persists.chk, reactive/waituntil_event.chk |
| test_lock_heap.c | KEEP | compiles with private includes removed | — |
| test_make_native_closure.c | DELETE | asserts `closure->native_fn` field identity and NULL/OOM returns | none (C-API constructor contract) |
| test_mem_debug.c | DELETE | vacuous under bare probe (URBI_MEM_DEBUG gate); redzone, poison, quarantine and handle-leak reporting | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_module_refcount_fused.c | DELETE | asserts `root_proto->refcount` deltas on strand bind and closure alloc | none (refcount fusion accounting) |
| test_object_in_place_barrier.c | DELETE | asserts Dijkstra barrier shading, shape aliasing and `topology_gen` bumps | covered by objects/slot-cow-const.chk, objects/shared-protos.chk, objects/slot_ic_polymorphic_site.chk, gc/reactive_churn.chk |
| test_object_root.c | KEEP | compiles with private includes removed | — |
| test_object_unfrozen.c | DELETE | asserts the `readonly` flag bit on the Object and Lobby protos | covered by objects/object_proto_mutable.chk, repl/object_readonly.chk, repl/lobby_readonly.chk |
| test_op_closure_invariants.c | DELETE | builds a malformed UProto tree by hand to trip three dispatch guards | none (guards defend programmatically-built bytecode the parser cannot produce) |
| test_op_getslot_change_event.c | DELETE | asserts the `UValue` kind returned by `urbi_object_get_or_create_change_event` | covered by reactive/slot-change/slot_change_no_install_emit.chk, reactive/at/cascade_same_pass.chk |
| test_perf_counters.c | KEEP | compiles with private includes removed, gates defined, body non-vacuous | — |
| test_periodic_cadence.c | DELETE | asserts `next_fire_us` re-arm arithmetic on the periodic record | covered by reactive/every/basic.chk, reactive/every/float_seconds.chk, reactive/every/tag_stop_cancels.chk; plus reactive/every/every_rejects_zero_and_nonfinite_period.chk: `every(0)` and a non-finite period raise a catchable error instead of spinning |
| test_pipe_budget_exhaust.c | DELETE | asserts `URBI_STEP_RUNNING` versus `QUIESCENT` return codes from `urbi_step` | covered by scheduler/strand_budget_yield_completes_program.chk (named in the test_budget_rearm.c row), scheduler/quiescent_clean.chk |
| test_public_api.c | DELETE | NULL-argument defence on `urbi_panic` / `urbi_throw` / `urbi_tag_stop_local` plus a `URBI_VERSION` string pin; needs `UStrand` internals | none (C-API NULL-safety and a version-string pin) |
| test_realm.c | DELETE | asserts realm linked-list stitching, id monotonicity, flag bits and OOM returns | covered by chunk_lifecycle/realm_isolation.chk, chunk_lifecycle/realm_global_default.chk, globals/realm_self_ref.chk, repl/lobby_isolation.chk |
| test_realm_destroy_with_parked_loader.c | KEEP | compiles with private includes removed; only `urbi_zero` is lost | — |
| test_realm_globals_api.c | DELETE | asserts const-slot enforcement is limited to slot indices 0-7 and distinguishes OOM from const-reject | globals/const_builtin_global_rejects_write.chk: assigning to a const built-in global such as `Object` raises a catchable error and leaves the original binding intact |
| test_realm_populate.c | DELETE | counts the 15 built-in globals installed at realm create | covered by stdlib/namespaces/global.chk, globals/object_proto.chk, globals/tag_event_globals.chk |
| test_recursive_emit.c | DELETE | asserts `ic_index` DFS pre-order numbering and `total_proto_count` | covered by closure/recursive_emit_smoke.chk, closure/nested_factory.chk |
| test_ref_gc_root.c | DELETE | asserts a cell is collected when unrooted and survives while `urbi_ref` holds it | none (host ref-table rooting; no urbiscript surface) |
| test_regexp.c | KEEP | compiles with private includes removed; only `urbi_strlen` is lost | — |
| test_register_watcher_callback.c | DELETE | implicit decl of `uevent_ring_drain`; asserts host-watcher callback argc and firing order | none (host watcher callbacks; no urbiscript surface) |
| test_registry_table.c | DELETE | asserts the static built-in registry has 15 const entries in a fixed order | covered by stdlib/namespaces/global.chk |
| test_repl_auth_flow.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); constant-time token compare and per-IP rate limiter | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_backpressure.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); EAGAIN handling in the reader thread | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_buffer_transport.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); in-process loopback transport | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_chk_corpus.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); drives tests/chk/repl fixtures through the dispatcher | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_dispatcher.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); queue, ringbuf and dispatcher round-trips | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_global_atom.c | KEEP | compiles with private includes removed, gates defined, body non-vacuous | — |
| test_repl_multi_client.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); four real TCP clients | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_ndjson_emit.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); NDJSON response emitter | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_ndjson_parse.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); NDJSON request parser | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_oom_paths.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); OOM injection on session create | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_queue.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); ring overflow frame alignment | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_security_bind_auth.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); non-loopback bind without a token | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_security_compile_budget.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); compile-budget denial envelope | covered by repl/budget_nodes.chk, repl/budget_depth.chk, repl/budget_source.chk |
| test_repl_security_malformed.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); 50 malformed NDJSON inputs | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_security_output_isolation.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); per-session output ringbufs | covered by repl/lobby_isolation.chk, repl/echo_routes_to_session.chk, repl/output_streaming.chk |
| test_repl_security_rate_limit.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); per-session job rate limit | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_serve_step_cooperative.c | KEEP | compiles with private includes removed, gates defined, body non-vacuous | — |
| test_repl_stop_path.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); session teardown on stop | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_tcp_loopback.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); real TCP listener round-trip | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_uart_pty.c | DELETE | vacuous under bare probe (URBI_ENABLE_REPL gate); pty-pair UART transport | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_repl_uproto_readonly.c | DELETE | asserts the `URBI_OBJ_FLAG_READONLY` bit on the 15 built-in atom protos | covered by repl/object_readonly.chk, repl/lobby_readonly.chk, objects/object_proto_mutable.chk |
| test_rescued_protos.c | DELETE | asserts `vm->rescued_protos` membership when `root_proto->refcount` is non-zero at chunk destroy | covered by chunk_lifecycle/script_at_persists.chk, chunk_lifecycle/script_one_every.chk, repl/hot_reload.chk |
| test_resolve_owning_tag.c | DELETE | calls `urbi_watcher_resolve_owning_tag` and walks the cleanup stack by hand | covered by tag/tagged_watcher_persists.chk, tag/scope_binds_user_tag.chk, tag/hierarchical.chk |
| test_ros_marshal.c | DELETE | ROS2-gated; the bare probe compiles it vacuously and URBI_ENABLE_ROS2 needs external headers, so question 1 is unverifiable | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_ros_subscribe.c | DELETE | ROS2-gated; the bare probe compiles it vacuously and URBI_ENABLE_ROS2 needs external headers, so question 1 is unverifiable | none (parked feature; covered by its own smoke/chk presets when re-attached) |
| test_sched_fifo.c | DELETE | asserts each of five state transitions tail-inserts on the ready queue | covered by separator/yield_seq.chk, separator/comma.chk, scheduler/wait_sleep_basic.chk |
| test_sched_pool_exhaust.c | DELETE | patches `alloc_fn` to NULL and asserts `urbi_strand_create` returns NULL | none (allocator-failure degradation) |
| test_sched_post_dispatch_alt_driver.c | DELETE | asserts the four post-dispatch fix-up steps on strand and sleep-queue fields | covered by scheduler/wait_sleep_basic.chk, scheduler/quiescent_clean.chk, scheduler/quiescent_with_sleep_q.chk, reactive/every/basic.chk |
| test_sched_state_aliasing.c | DELETE | asserts state-byte and reason-nibble encodings and debug-assert aborts on re-block or re-yield | none (strand state-byte encoding) |
| test_scheduler_cooperative.c | DELETE | asserts ready-queue and sleep-queue linkage, sorted insertion and quiescence counters | covered by scheduler/wait_sleep_basic.chk, scheduler/quiescent_clean.chk, scheduler/quiescent_with_sleep_q.chk, separator/yield_seq.chk, temporal/sleep_basic.chk |
| test_scheduler_invariant.c | DELETE | asserts every state transition preserves membership in `realm.strands_head` | none (list-membership invariant assumed by the GC walker) |
| test_scratch_cur_strand.c | DELETE | asserts a slot fault lands on the scratch strand rather than the outer one, via internal strand pointers | covered by exceptions/vm_typeerror_uncaught.chk, exceptions/runtime_errors_catchable.chk, reactive/at/at_rising_edge.chk |
| test_scratch_strand_safety.c | DELETE | asserts the scratch strand is removed from scheduler queues, via queue-head inspection | covered by reactive/at/cascade_same_pass.chk, reactive/at/onleave_cascade.chk, temporal/sleep_tag_stop.chk |
| test_set_wake_fn.c | KEEP | compiles with private includes removed | — |
| test_slot_change_callsites.c | DELETE | calls the three C write paths directly and counts spawned body strands | reactive/slot-change/slot_change_skips_install_fires_on_write.chk: `at (obj.x.changed?)` does not fire when `x` is first created on the object and does fire on a later assignment to it |
| test_slot_change_emit.c | DELETE | asserts the bit-7 fast-path short-circuit and the deferred-ring route | none (subscriber-presence bit and deferred-ring routing; the observable half is the fixture named in the test_slot_change_callsites.c row) |
| test_slot_change_install.c | DELETE | asserts UEvent identity per slot name and chain length on the changed-events list | covered by reactive/slot-change/slot_change_no_install_emit.chk |
| test_slot_change_reentrancy.c | DELETE | asserts deferred-ring drain order and the overflow warn flag | covered by reactive/slot_change_reentrancy_determinism.chk, reactive/at/cascade_same_pass.chk |
| test_slot_get.c | DELETE | implicit decl of `urbi_object_alloc` and `urbi_object_set_local_slot`; asserts C-API return codes | covered by objects/lookup.chk, objects/inheritance.chk, objects/atom_method_dispatch.chk |
| test_slot_set.c | DELETE | asserts `URBI_ERR_CONST_SLOT_WRITE` and `URBI_ERR_OOM` return codes from `urbi_slot_set` | covered by objects/slot-cow-const.chk, objects/get-set/set_basic.chk |
| test_stdlib_install.c | KEEP | compiles with private includes removed | — |
| test_step_driver.c | DELETE | asserts the five liveness counters and their underflow behavior | covered by scheduler/quiescent_clean.chk, scheduler/quiescent_with_sleep_q.chk, scheduler/wait_sleep_basic.chk, gc/expect_host_call.chk |
| test_strand_arm.c | DELETE | asserts `pc`, `cur_consts`, `R` and `frame_count` after `urbi_strand_arm_from_closure` | none (execution-state field initialisation) |
| test_strand.c | DELETE | asserts state-byte encoding, DORMANT-to-READY transitions and FIFO spawn order via queue inspection | covered by scheduler/dormant_attach_tag_then_start.chk, separator/comma.chk, separator/detach_basic.chk |
| test_strand_cancel_wake.c | DELETE | asserts a cancelled parked strand is unlinked from sleep, event, join and watcher queues | covered by temporal/sleep_tag_stop.chk, tag/stop_waituntil_mid_eval.chk, tag/stop_join_parked.chk |
| test_strand_destroy.c | DELETE | allocator spy proving `s->stack` is not double-freed | none (free-idempotence contract) |
| test_strand_destroy_during_event_wait.c | DELETE | asserts `UEvent.waiters_head` splicing and joiner wake on destroy | covered by tag/stop_join_parked.chk, chunk_lifecycle/realm_isolation.chk |
| test_strand_root_proto_bind.c | DELETE | asserts `UStrand.root_proto` aliases the module root and is inherited by fork children | none (fast-path field population) |
| test_strand_spawn_inheritance.c | DELETE | asserts the captured ambient-tag chain array and its bottom-up order | covered by tag/ambient_inherit_separator.chk, tag/hierarchical.chk, tag/implicit.chk |
| test_strand_unpark.c | DELETE | asserts third-party links (`joiners_head`, `w->waiter_strand`) are scrubbed when a parked strand is tag-stopped | covered by tag/stop_join_parked.chk, tag/stop_waituntil_mid_eval.chk, tag/stop_waituntil_nested.chk |
| test_tag_barrier.c | DELETE | asserts the Dijkstra barrier fires when `enter_event` / `leave_event` are lazily allocated | covered by tag/tag_enter_leave_minimal.chk |
| test_tag_create.c | DELETE | asserts `urbi_tag_create` return state, parent pointer and name interning | covered by tag/scope_tag_basic.chk, globals/tag_event_globals.chk, tag/state.chk |
| test_tag_enter_leave.c | DELETE | installs watchers through `urbi_watcher_install_at_event_runtime` and reads `strand_runnable_count` | reactive/tag/tag_enter.chk and reactive/tag/tag_leave.chk (activate placeholders): entering a tag scope runs an `at (t.enter?)` body and leaving it runs an `at (t.leave?)` body |
| test_tag_gate_matrix.c | DELETE | asserts SUSPENDED state bytes and `strand_suspended_count` across gate orderings | tag/block_freeze_independent_gates.chk: on a tag whose member strand is both blocked and frozen, unblocking alone does not resume it and unfreezing alone does not either; tag/tag_stop_resumes_suspended_member.chk: stopping a tag whose member is suspended wakes that member so its finally still runs |
| test_tag_info.c | DELETE | asserts the `urbi_tag_info` struct fields including `member_count` | covered by tag/state.chk, tag/freeze_basic.chk |
| test_tag_lifecycle.c | DELETE | asserts tag member-list bookkeeping across OP_PUSH_TAG / OP_POP_TAG and cleanup-stack overflow | covered by tag/scope.chk, tag/scope_tag_basic.chk, tag/hierarchical.chk, tag/tag_scope_locals.chk |
| test_tag_native.c | DELETE | calls `urbi_tag_enter_getter` directly and asserts lazy-alloc idempotence | covered by tag/tag_enter_leave_minimal.chk, plus the two placeholders named in the test_tag_enter_leave.c row |
| test_tag_self_block.c | DELETE | asserts the strand parks SUSPENDED mid-dispatch by reading the state byte | tag/self_block_in_own_scope.chk: `t: { t.block(); ... }` suspends before the rest of the scope body runs, and a later `t.unblock()` lets the remainder finish |
| test_tag_state.c | DELETE | asserts `urbi_strand_suspend` / `_resume_if_ungated` state transitions and queue membership | covered by tag/state.chk, tag/freeze_basic.chk, tag/block_freeze_independent_gates.chk (named in the test_tag_gate_matrix.c row) |
| test_tag_stop_onleave_scripted.c | DELETE | asserts the watcher reaches `vm->pending_onleave_head` after `urbi_tag_stop` | tag/tag_stop_cascades.chk (activate placeholder): stopping an outer tag runs the inner scope's onleave before the outer one |
| test_tag_stop_realm.c | DELETE | asserts unwind-status deposit priority and `host_call_pending_count` deltas | covered by control_transfer/tag_stop_basic.chk, control_transfer/tag_stop_skips_catch.chk, control_transfer/tag_stop_with_finally.chk, chunk_lifecycle/realm_isolation.chk |
| test_topology_gen.c | DELETE | asserts which of twelve mutation surfaces bump `vm->topology_gen` | covered by objects/slot_ic_polymorphic_site.chk, operators/op_overload_redefine.chk, operators/op_overload_polymorphic_site.chk, objects/shared-protos.chk |
| test_uchanged_node.c | DELETE | pins `sizeof(UObject)` and `sizeof(UChangedNode)` | none (struct size pins) |
| test_uchunk_instance_lifetime.c | DELETE | asserts unlinking from `vm->module_instances_head` on proto destroy | covered by repl/hot_reload.chk, chunk_lifecycle/repl_session_persistence.chk |
| test_uevent.c | DELETE | pins `sizeof(UEvent)`, the type tag and NULL-at-alloc list heads | none (struct layout and type-table registration) |
| test_uevent_subscribe.c | DELETE | asserts FIFO append and unlink on the `at_watchers` chain | covered by reactive/event/emit_multi_subscriber_fifo.chk (named in the test_event_emit_async.c row) |
| test_ugc_barrier.c | DELETE | hand-paints GC colors and asserts the barrier state table | none (GC internals; new core has stress mode) |
| test_ugc_color_invariants.c | KEEP | compiles with private includes removed | — |
| test_ugc_finalizer.c | KEEP | compiles with private includes removed | — |
| test_ugc_handle.c | DELETE | asserts handle-table growth, pin bits and fixed-cell sweep survival | none (host handle table and pin bits) |
| test_ugc_object_cells.c | DELETE | asserts every UTYPE tag is registered and unreferenced cells are reclaimed | none (type-table registration and sweep accounting) |
| test_ugc_state_machine.c | DELETE | asserts the five-phase transitions, graylist enrolment and threshold updates | none (GC state machine; new core has stress mode) |
| test_ugc_walk_roots.c | DELETE | implicit decl of `urbi_shape_root`; counts registered root providers | none (root-provider registry count) |
| test_uic.c | DELETE | pins `sizeof(UIC) == 144`, flag bits and per-VM IC table independence | covered by objects/slot_ic_polymorphic_site.chk, operators/op_overload_polymorphic_site.chk, operators/op_overload_redefine.chk, chunk_lifecycle/realm_isolation.chk |
| test_ulist.c | DELETE | exercises the intrusive list macros in `src/runtime/ulist.h` on a private node type | none (container macros below the language surface) |
| test_unregister_watcher.c | DELETE | implicit decl of `uevent_ring_drain`; asserts deferred-removal semantics for host watcher handles | none (host watcher handles; no urbiscript surface) |
| test_unwind.c | DELETE | drives `urbi_unwind` over a hand-built cleanup stack and asserts register zeroing | covered by control_transfer/return_basic.chk, control_transfer/try_finally.chk, control_transfer/nested_finally.chk, control_transfer/throw_uncaught.chk, control_transfer/scope_crossing_exits.chk, control_transfer/throw_in_finally.chk, exceptions/cross_frame_catch.chk, tag/throw_through_tag_scope.chk |
| test_uobject.c | DELETE | pins `sizeof(UObject)`, field order, atom-family values and proto-list forms | covered by objects/inheritance.chk, objects/shared-protos.chk, objects/lookup.chk, objects/fallback.chk, objects/atom-clone.chk, objects/isa_atoms.chk, objects/atoms.chk |
| test_uproto_owning_mi.c | KEEP | compiles with private includes removed | — |
| test_uproto_root_backptr.c | KEEP | compiles with private includes removed; only `urbi_strlen` is lost | — |
| test_urbi_unload.c | KEEP | compiles with private includes removed; only `urbi_zero` is lost | — |
| test_ushape.c | DELETE | pins `sizeof(UShape) == 56`, field offsets and transition-cache identity | covered by objects/shared-protos.chk, objects/slot_ic_polymorphic_site.chk, objects/class-multi-slot.chk |
| test_uslothandle.c | DELETE | asserts cached `shape_at_create` refresh and invalidation on the handle struct | covered by objects/get-set/get_basic.chk, objects/get-set/set_basic.chk, objects/lookup.chk |
| test_ustrand_layout.c | DELETE | pins `sizeof(UStrand)` and the `USTRAND_WAIT_WATCHER` constant | none (struct size and constant pins) |
| test_utag_gc.c | DELETE | asserts UTag allocation routes through `urbi_gc_alloc` and pins `sizeof(UTag)` | none (allocation routing and struct size) |
| test_uvm_deferred_ring.c | DELETE | implicit decl of `urbi_drain_deferred_slot_changes`; asserts ring head, tail and capacity at create | none (ring field initialisation) |
| test_uwatcher_layout.c | DELETE | pins `sizeof(UWatcher)` and the presence of the refire counter fields | none (struct layout pins; the refire behavior is the fixture named in the test_athandler_wedge_repro.c row) |
| test_uwatcher_scratch.c | DELETE | calls `urbi_run_closure_on_scratch` directly with a hand-wrapped closure | covered by reactive/at/at_rising_edge.chk, reactive/at/cascade_same_pass.chk, exceptions/vm_typeerror_uncaught.chk |
| test_vm.c | DELETE | fabricates modules instruction by instruction and asserts arithmetic, wraparound, type-error diagnostics and opcode-name table completeness | covered by arithmetic/basic.chk, operators/add.chk, operators/sub.chk, operators/mul.chk, operators/div.chk, operators/neg.chk, operators/eq.chk, operators/neq.chk, operators/lt.chk, operators/le.chk, operators/truthiness.chk, exceptions/vm_typeerror_catchable.chk, exceptions/vm_typeerror_uncaught.chk, exceptions/runtime_errors_catchable.chk, stdlib/atoms/float_conversion.chk, stdlib/legacy/maths_errors_legacy.chk |
| test_vm_dispatch_ownership.c | DELETE | asserts pool-OOM propagation and operand kind-checks on the reactive-install and fork opcodes | covered by exceptions/runtime_errors_catchable.chk, separator/fork_operand_forms.chk, separator/detach-error.chk |
| test_vm_liveness.c | DELETE | asserts `active_count` symmetry and the QUIESCENT verdict ladder on counters | covered by scheduler/quiescent_clean.chk, scheduler/quiescent_with_sleep_q.chk, chunk_lifecycle/script_at_persists.chk |
| test_vm_operator_overload.c | DELETE | asserts IC caching and staleness on the overload dispatch site | covered by operators/op_overload_redefine.chk, operators/op_overload_polymorphic_site.chk, operators/operator_overload_throw.chk, operators/compare_overload_mirror.chk, objects/class.chk |
| test_waituntil_install.c | DELETE | drives the cond through a hook and asserts `active_watchers_head` and the `USTRAND_WAIT_WATCHER` state byte | covered by reactive/at/waituntil_true_at_install_does_not_park.chk (named in the test_at_install_dispatch.c row), reactive/waituntil_event.chk |
| test_waituntil_tag_stop.c | DELETE | asserts waiter unlink from `e->waiters_head` on stop, cancel and panic | covered by tag/stop_waituntil_mid_eval.chk, tag/stop_waituntil_nested.chk, tag/stop_straightline_ret.chk |
| test_watcher_auto_unregister.c | DELETE | implicit decl of `uevent_ring_drain`; host watcher callback returning the auto-unregister sentinel | none (host watcher callbacks; no urbiscript surface) |
| test_watcher_completed.c | DELETE | asserts `w->body_strand` and `s->watcher_body_owner` are NULLed and a warn is logged | none (back-pointer clearing and a host log call; the respawn-on-pending-refire behavior is the fixture named in the test_athandler_wedge_repro.c row) |
| test_watcher_dirty.c | DELETE | asserts bit-6 lifecycle, dirty counters, active-list tail insertion, onleave-queue drain order and root-walker visit counts | covered by reactive/at/at_rising_edge.chk, reactive/at/whenever_level.chk, reactive/at/onleave_cascade.chk, reactive/at/cascade_same_pass.chk, tag/tagged_watcher_persists.chk |
| test_watcher_done_fanout.c | DELETE | implicit decl of `uevent_ring_drain`; asserts the host `done_fn` fanout and handle values | none (host completion callback; no urbiscript surface) |
| test_watcher_gc_invariants.c | DELETE | URBI_DEBUG-only call to `urbi_watcher_check_invariants` | none (debug-build invariant assertion) |
| test_watcher_lifecycle.c | DELETE | asserts drain defers while `w->body_strand` is non-NULL | covered by reactive/at/onleave_cascade.chk, reactive/at/cascade_same_pass.chk |
| test_watcher_mode_predicates.c | DELETE | asserts `whenever_event` watchers are unlinked from the event chain on unregister and pool destroy | covered by reactive/event/at_event_unsubscribes_on_tag_stop.chk (named in the test_at_event_unlink_on_tag_stop.c row), reactive/whenever_event_dispatches.chk |
| test_watcher_ownership.c | DELETE | asserts pool-free aliasing, double-detach and unknown-mode handling on watcher records | none (watcher-pool ownership defence) |
| test_watcher_pool.c | DELETE | asserts freelist threading, `in_use` count, exhaustion and the high-water mark | none (pool bookkeeping) |
| test_watcher_spawn.c | DELETE | asserts body-strand spawn OOM fail-soft paths and the refire-queue saturation counter | none (spawn-time OOM degradation; the saturation behavior is the fixture named in the test_athandler_wedge_repro.c row) |

## Harvest fixtures named

Seventeen distinct fixtures, four of which are existing placeholders to activate
rather than new files.

| fixture | behavior |
|---|---|
| reactive/event/at_event_unsubscribes_on_tag_stop.chk | an `at (e?)` watcher installed inside a tag scope stops running its body on emissions after that tag is stopped |
| reactive/event/event_burst_fires_body_each_emission.chk | N back-to-back emissions each run the `at (e?)` body once |
| reactive/event/emit_multi_subscriber_fifo.chk | two subscribers on one event run in registration order |
| reactive/event/event_sync_emit.chk (activate placeholder) | `emit sync` runs every subscriber body before the emitting statement returns |
| reactive/at/at_sync_body_runs_before_next_statement.chk | an `at sync (cond)` body finishes before the statement that flipped the condition returns |
| reactive/at/waituntil_true_at_install_does_not_park.chk | `waituntil(cond)` with an already-true condition returns without parking |
| reactive/slot-change/slot_change_skips_install_fires_on_write.chk | `at (obj.x.changed?)` is silent on slot creation and fires on a later write |
| reactive/every/every_rejects_zero_and_nonfinite_period.chk | `every(0)` and a non-finite period raise a catchable error |
| reactive/tag/tag_enter.chk (activate placeholder) | entering a tag scope runs an `at (t.enter?)` body |
| reactive/tag/tag_leave.chk (activate placeholder) | leaving a tag scope runs an `at (t.leave?)` body |
| scheduler/strand_budget_yield_completes_program.chk | a loop driven with a small per-step budget still completes across repeated steps |
| tag/self_block_in_own_scope.chk | `t: { t.block(); ... }` suspends before the rest of the scope runs; `t.unblock()` resumes it |
| tag/block_freeze_independent_gates.chk | block and freeze are independent gates; clearing one alone does not resume the strand |
| tag/tag_stop_resumes_suspended_member.chk | stopping a tag wakes a suspended member so its finally runs |
| tag/tag_stop_cascades.chk (activate placeholder) | stopping an outer tag runs the inner scope's onleave before the outer one |
| control_transfer/finally_that_sleeps_is_fatal.chk | a `finally` body that sleeps reports a loud error instead of silently truncating |
| globals/const_builtin_global_rejects_write.chk | assigning to a const built-in global raises and leaves the binding intact |
