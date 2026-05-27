# urbi-embedded API surface tiers

Authoritative manifest for v1.0 freeze planning. Every public symbol
exported from `liburbi.a` MUST appear in this manifest. CI gate
`test-api-manifest` (see `tests/scripts/check-api-manifest.sh`) verifies
that no exported symbol is missing from this file.

Tiers:

- **Stable (T1)** — Frozen at v1.0. Removing requires a MAJOR bump.
- **Advanced (T2)** — Stable but non-hot-path. Used by embedders who need
  more than the basic API. Decorated with `URBI_ADVANCED` in the header.
- **Experimental (T3)** — RESERVED for v1.x. Compiler emits a deprecation
  warning when used. Do not depend on across releases. Decorated with
  `URBI_EXPERIMENTAL`. Present only when `URBI_SCHED_HAS_PRIORITY != 0`
  (cooperative builds hard-define to 0 so these symbols are absent).
- **Internal-leak (T4)** — Symbol is exported from `liburbi.a` but not
  declared in any `include/urbi/*.h` header. Should not be used by
  embedders. Will be moved to `src/` visibility in a future cleanup.

Note: Inline functions (`urbi_make_*`, `urbi_value_*`) and URBI_DEBUG-only
functions (`urbi_in_isr`, `urbi_get_determinism_checksum`,
`urbi_call_host_with_watchdog`) are declared in public headers but do not
appear as `T` symbols in `nm` output. They are documented here for
completeness but the CI gate only checks the "exported but undocumented"
direction (see `tests/scripts/check-api-manifest.sh`).

REPL symbols (`urbi_repl_serve`, `urbi_repl_stop`, etc.) are present only
when the library is built with `URBI_ENABLE_REPL=1`; they are declared in
`include/urbi/repl.h` but absent from the default build.

New public symbols require a PR-review-touch on this manifest.

---

## Tier 1 — Stable (frozen at v1.0)

### VM lifecycle

- `urbi_vm_create`, `urbi_vm_free`, `urbi_vm_sizeof`, `urbi_vm_alignof`
- `urbi_vm_has_live_work`, `urbi_vm_run`

### Realm

- `urbi_realm_global`, `urbi_realm_create`, `urbi_realm_create_repl`,
  `urbi_realm_destroy`, `urbi_realm_set_global`,
  `urbi_realm_set_global_const`, `urbi_realm_get_global`,
  `urbi_realm_set_writer`, `urbi_realm_set_compile_budget`,
  `urbi_realm_get_compile_budget`

### Run + compile

- `urbi_compile_source`, `urbi_run_chunk`, `urbi_run_script`,
  `urbi_load_chunk`, `urbi_unload`, `urbi_repl_eval`

### Event

- `urbi_event_register`, `urbi_event_unregister`, `urbi_inject_event`

### Host functions

- `urbi_register`, `urbi_make_native_closure`

### Slots

- `urbi_slot_get`, `urbi_slot_set`

### Watchers

- `urbi_register_watcher`, `urbi_unregister_watcher`

### Tags

- `urbi_tag_create`, `urbi_tag_info`, `urbi_tag_stop`,
  `urbi_tag_block`, `urbi_tag_unblock`,
  `urbi_tag_freeze`, `urbi_tag_unfreeze`

### Strand

- `urbi_strand_create`, `urbi_strand_spawn`, `urbi_strand_start`,
  `urbi_strand_destroy`, `urbi_strand_state`

### Error inspection

- `urbi_last_error`, `urbi_clear_error`, `urbi_set_error`

### References

- `urbi_ref`, `urbi_ref_get`, `urbi_unref`

### Atomic batching

- `urbi_atomic_begin`, `urbi_atomic_end`

### Stepping

- `urbi_step`

### Value constructors (inline — not exported as T symbols)

- `urbi_make_nil`, `urbi_make_bool`, `urbi_make_int`, `urbi_make_float`,
  `urbi_make_void`, `urbi_make_ptr`, `urbi_make_object`, `urbi_make_event`,
  `urbi_make_closure`, `urbi_make_tag`

### Value constructors (non-inline)

- `urbi_make_str_interned`

### Value accessors (inline — not exported as T symbols)

- `urbi_value_kind`, `urbi_value_as_bool`, `urbi_value_as_int`,
  `urbi_value_as_float`, `urbi_value_as_str`, `urbi_value_as_ptr`,
  `urbi_value_as_object`, `urbi_value_as_event`, `urbi_value_as_closure`

### Value predicates

- `urbi_value_is_*` (see `include/urbi/types.h` for the full set)

### Version

- `urbi_version`, `urbi_api_version`

### Aux library (T1, in liburbi_aux.a)

- `urbi_aux_check_version`, `urbi_aux_register_event_table`,
  `urbi_aux_register_function_table`, `urbi_aux_set_error`,
  `urbi_aux_load_and_run`, `urbi_aux_dump_value`,
  `urbi_aux_diag_to_stderr`
- `urbi_aux_value_to_int`, `urbi_aux_value_to_float`,
  `urbi_aux_value_to_bool`, `urbi_aux_value_to_str`,
  `urbi_aux_value_to_ptr`, `urbi_aux_value_to_object`,
  `urbi_aux_value_to_event`, `urbi_aux_value_to_closure`,
  `urbi_aux_value_to_tag`

### REPL (T1, present only when URBI_ENABLE_REPL=1)

- `urbi_repl_serve`, `urbi_repl_stop`, `urbi_repl_serve_init`,
  `urbi_repl_serve_step`, `urbi_repl_serve_shutdown`,
  `urbi_repl_register_transport`

### Object (in include/urbi/object.h)

- `urbi_object_root`, `urbi_object_atom`,
  `urbi_object_add_proto`, `urbi_object_remove_proto`,
  `urbi_object_set_protos`

### Setters

- `urbi_set_writer`, `urbi_set_diag_fn`, `urbi_set_time_us`,
  `urbi_set_wake_fn`, `urbi_set_watcher_body_done_fn`,
  `urbi_set_isr_check_fn`, `urbi_set_callback_watchdog_mode`

### Control-transfer

- `urbi_throw`, `urbi_return_val`, `urbi_tag_stop_local`,
  `urbi_strand_cancel`, `urbi_strand_panic`,
  `urbi_strand_unwind_status`, `urbi_strand_is_fatal`,
  `urbi_strand_reset`

### Chunk

- `urbi_chunk_from_bytes`, `urbi_chunk_free`

### Pinning

- `urbi_pin`, `urbi_unpin`

### Heap lock

- `urbi_lock_heap`

### Miscellaneous T1

- `urbi_require_fail`, `urbi_set_require_fail_hook`,
  `urbi_vm_write`, `urbi_vm_write_in_realm`

---

## Tier 2 — Advanced (stable, non-hot-path)

Decorated with `URBI_ADVANCED` in the respective header.

- `urbi_vm_init`, `urbi_vm_destroy` (static-allocation embedders; prefer
  `urbi_vm_create`/`urbi_vm_free` from T1 for new code)
- `urbi_in_isr` (URBI_DEBUG builds only — absent in release builds;
  regular extern function, not inline, decorated with `URBI_ADVANCED`)
- `urbi_get_determinism_checksum` (URBI_DEBUG builds only — absent in
  release builds; used by test harnesses)
- `urbi_chunk_instance_create`, `urbi_chunk_instance_destroy`
- `urbi_call_host_with_watchdog` (URBI_DEBUG builds only — collapses to
  a macro in non-debug builds)
- `urbi_panic`
- `urbi_chunk_translate_load_err`
- `urbi_register_event_drain`
- `urbi_gc_alloc`, `urbi_gc_slice`, `urbi_gc_walk_roots`,
  `urbi_gc_register_root_provider`, `urbi_gc_init`, `urbi_gc_destroy`,
  `urbi_gc_force_full`, `urbi_gc_bytes_allocated_inline`

---

## Tier 3 — Experimental (RESERVED for v1.x)

Decorated with `URBI_EXPERIMENTAL` in `include/urbi/sched.h`. Compiler
emits a deprecation warning when these symbols are used directly. Present
only when `URBI_SCHED_HAS_PRIORITY != 0`; the cooperative scheduler
hard-defines this to 0, so these symbols are absent in all shipped builds.

- `urbi_strand_set_priority`, `urbi_strand_get_priority`,
  `urbi_strand_get_sched_class`

---

## Tier 4 — Internal-leak

These symbols are exported from `liburbi.a` but not declared in any
`include/urbi/*.h` public header. Embedders MUST NOT use them — they are
implementation details subject to change without notice. The `test-api-manifest`
CI gate tracks this list to prevent silent growth.

### Atom / proto init

- `urbi_atom_family_name`, `urbi_atom_proto_for_value`,
  `urbi_atom_protos_mark_readonly`, `urbi_atom_protos_register`,
  `urbi_native_protos_init`

### Deferred slot-change ring

- `urbi_defer_slot_change`, `urbi_deferred_slot_changes_walk_roots`,
  `urbi_drain_deferred_slot_changes`, `urbi_emit_slot_change_slow`

### Encoding

- `urbi_encode_utf8`

### Event internals

- `urbi_event_create`

### GC internals (beyond the T2 public surface)

- `urbi_gc_bytes_allocated`, `urbi_gc_collect`, `urbi_gc_live_bytes`,
  `urbi_gc_pause`, `urbi_gc_phase`, `urbi_gc_threshold`,
  `urbi_gc_walk_all_cells`

### Chunk instance internals

- `urbi_get_or_create_chunk_instance`

### Host handle

- `urbi_handle_create`, `urbi_handle_get`, `urbi_handle_release`

### Lobby / session management

- `urbi_lobby_invoke_handleDisconnect`, `urbi_lobby_native_register`,
  `urbi_lobby_native_register_globals`, `urbi_lobby_register_session`,
  `urbi_lobby_unregister_session`

### Closure internals

- `urbi_native_closure_create`, `urbi_run_closure_on_scratch`,
  `urbi_run_closure_on_scratch_with_payload`

### Object internals

- `urbi_object_alloc`, `urbi_object_builtin_types_init`,
  `urbi_object_clone`, `urbi_object_get_or_create_change_event`,
  `urbi_object_get_slot`, `urbi_object_install_property`,
  `urbi_object_lookup`, `urbi_object_lookup_id_force_wrap`,
  `urbi_object_register_gc_roots`, `urbi_object_remove_property`,
  `urbi_object_remove_slot`, `urbi_object_resolve_slot`,
  `urbi_object_root_register`, `urbi_object_set_local_slot`,
  `urbi_object_set_property_value`, `urbi_object_set_protos_empty`,
  `urbi_object_set_protos_heap`, `urbi_object_set_protos_single`

### Periodic / temporal scheduler internals

- `urbi_periodic_body_completed`, `urbi_periodic_destroy_all`,
  `urbi_periodic_destroy_for_realm`, `urbi_periodic_earliest_wake_us`,
  `urbi_periodic_pump`, `urbi_periodic_table_walk_roots`

### Realm internals

- `urbi_populate_realm_globals`

### REPL introspection internals (URBI_ENABLE_REPL=1 only)

- `urbi_introspect_coros`, `urbi_introspect_events`, `urbi_introspect_gc`,
  `urbi_introspect_lobbies`, `urbi_introspect_profile`,
  `urbi_introspect_slots`, `urbi_introspect_stack`, `urbi_introspect_tags`,
  `urbi_introspect_watchers`

### Proto / ref internals

- `urbi_proto_list_create`, `urbi_proto_ref_acquire`,
  `urbi_proto_ref_release`, `urbi_proto_strand_ref_acquire`,
  `urbi_proto_strand_ref_release`, `urbi_protos_alloc`

### Error raise helpers

- `urbi_raise_arity`, `urbi_raise_lookup`, `urbi_raise_oom`,
  `urbi_raise_type`

### Type registry

- `urbi_register_type`

### Error internals

- `urbi_set_error_internal`

### Shape / IC internals

- `urbi_shape_find_slot`, `urbi_shape_root`,
  `urbi_shape_transition_add_slot`, `urbi_shape_transition_property`,
  `urbi_shape_transition_remove_slot`

### Slot fast-path internals

- `urbi_slot_get_slow`, `urbi_slot_set_slow`

### Slot handle

- `urbi_slothandle_read_value`, `urbi_slothandle_write_value`

### Stdlib init / teardown

- `urbi_isa_method_register`,
  `urbi_job_make`, `urbi_job_proto_register`, `urbi_job_proto_register_globals`,
  `urbi_stdlib_boot`, `urbi_stdlib_containers_destroy`,
  `urbi_stdlib_list_append_value`, `urbi_stdlib_list_new_empty`,
  `urbi_stdlib_list_remove_first_equal`,
  `urbi_stdlib_register_atom_methods`,
  `urbi_stdlib_register_container_globals`,
  `urbi_stdlib_register_containers`,
  `urbi_stdlib_register_namespace_globals`,
  `urbi_stdlib_register_namespaces`,
  `urbi_stdlib_register_primitives`,
  `urbi_stdlib_register_primitives_globals`,
  `urbi_stdlib_register_runtime_globals`,
  `urbi_stdlib_register_runtime_types`

### Strand internals

- `urbi_strand_arm_from_closure`, `urbi_strand_arm_init`,
  `urbi_strand_attach_ambient_tags`, `urbi_strand_capture_ambient_chain`,
  `urbi_strand_create_for_module`, `urbi_strand_register_stack_alloc`,
  `urbi_strand_register_stack_free`, `urbi_strand_register_stack_zero`,
  `urbi_strand_resume`, `urbi_strand_suspend`,
  `urbi_strand_scope_tag`

### Control stdlib internals

- `urbi_control_native_register_globals`

### Tag globals stdlib internals

- `urbi_tag_globals_register`, `urbi_tag_globals_register_globals`

### Temporal stdlib internals

- `urbi_temporal_native_register`, `urbi_temporal_native_register_globals`

### Time default

- `urbi_default_host_time_us`

### Unwind internals

- `urbi_unwind`

### Watcher internals

- `urbi_watcher_body_completed`, `urbi_watcher_unregister_internal`

---

## CI gate

`tests/scripts/check-api-manifest.sh` runs at `make releasetest` time.
Fails if any symbol in `nm liburbi.a | grep ' T urbi_'` is not listed
in this manifest. New public symbols require a PR-review-touch here.
