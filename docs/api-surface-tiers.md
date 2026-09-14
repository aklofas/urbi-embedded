# urbi-embedded API surface tiers

Authoritative manifest of everything `liburbi.a` exports. The CI gate
`test-api-manifest` (`tests/scripts/check-api-manifest.sh`) fails if an
exported symbol is missing from this file, and if a Tier 1 symbol stops
being exported.

Two tiers while the core is being re-founded:

- **Stable (T1)** — declared in `include/urbi/urbi.h` or
  `include/urbi/version.h`. This is the whole embedding surface.
  Functions whose subsystem has not landed yet are already here with
  their final signature; they return `URBI_ERR_INVALID_STATE` until it
  does.
- **Internal-leak (T4)** — exported from the archive but declared in no
  public header. All of these belong to the kept compiler frontend
  (lexer, parser, emitter, chunk loader). They are not for embedders and
  lose their external linkage in the clean-up task.

Inline functions (`urbi_make_*`, `urbi_value_is_*`, `urbi_value_as_*` in
`include/urbi/types.h`) are part of the stable surface but never appear
as `T` symbols, so they are not listed here.

New public symbols require a PR-review-touch on this manifest.

---

## Tier 1 — Stable

### Lifecycle and host hooks

- `urbi_open`
- `urbi_close`
- `urbi_step`
- `urbi_has_live_work`
- `urbi_set_clock`
- `urbi_set_diag`
- `urbi_set_writer`
- `urbi_set_wake`

### Realms

- `urbi_realm_new`
- `urbi_realm_free`
- `urbi_realm_main`

### Code

- `urbi_compile`
- `urbi_load`
- `urbi_run`
- `urbi_call`
- `urbi_chunk_free`

### Values

- `urbi_make_string`
- `urbi_value_to_string`
- `urbi_ref`
- `urbi_unref`

### Globals and slots

- `urbi_global_get`
- `urbi_global_set`
- `urbi_slot_get`
- `urbi_slot_set`

### Host functions

- `urbi_register`
- `urbi_throw`

### Events and watchers

- `urbi_event_new`
- `urbi_event_emit`
- `urbi_inject_event`
- `urbi_watch`

### Tags

- `urbi_tag_new`
- `urbi_tag_stop`
- `urbi_tag_block`
- `urbi_tag_unblock`
- `urbi_tag_freeze`
- `urbi_tag_unfreeze`

### Errors

- `urbi_last_error`
- `urbi_clear_error`

### Garbage collector

- `urbi_gc_collect`
- `urbi_gc_stats`

### Version

- `urbi_version`
- `urbi_api_version`

---

## Tier 4 — Internal-leak (compiler frontend)

Cross-translation-unit helpers of the kept frontend. Not declared in any
public header; not for embedders.

- `urbi_chunk_decode_verify`
- `urbi_chunk_verify_bounds`
- `urbi_emit_abandon`
- `urbi_emit_add_const_float`
- `urbi_emit_add_const_int`
- `urbi_emit_add_const_str`
- `urbi_emit_assert_arm`
- `urbi_emit_assign_arm`
- `urbi_emit_at_event_arm`
- `urbi_emit_at_slot_change_arm`
- `urbi_emit_bin_sep_arm`
- `urbi_emit_binary_arm`
- `urbi_emit_binop_to_opcode`
- `urbi_emit_block_arm`
- `urbi_emit_bool_arm`
- `urbi_emit_break_arm`
- `urbi_emit_call_arm`
- `urbi_emit_class_decl_arm`
- `urbi_emit_compare_arm`
- `urbi_emit_cond_has_direct_side_effect`
- `urbi_emit_continue_arm`
- `urbi_emit_diag_error`
- `urbi_emit_diag_format_first_error`
- `urbi_emit_diag_free_all`
- `urbi_emit_diag_warn`
- `urbi_emit_dict_lit_arm`
- `urbi_emit_disasm_opnames_complete`
- `urbi_emit_expr`
- `urbi_emit_float_arm`
- `urbi_emit_for_each_arm`
- `urbi_emit_fs_temp_floor`
- `urbi_emit_function_arm`
- `urbi_emit_function_literal`
- `urbi_emit_grow`
- `urbi_emit_ident_arm`
- `urbi_emit_if_arm`
- `urbi_emit_instr`
- `urbi_emit_instr_count`
- `urbi_emit_int_arm`
- `urbi_emit_lazy_thunk`
- `urbi_emit_list_lit_arm`
- `urbi_emit_logical_arm`
- `urbi_emit_member_get_arm`
- `urbi_emit_member_set_arm`
- `urbi_emit_nary_arm`
- `urbi_emit_nil_arm`
- `urbi_emit_noop_arm`
- `urbi_emit_patch_instr`
- `urbi_emit_property_decl_arm`
- `urbi_emit_proto_grow`
- `urbi_emit_reserve_global_slot`
- `urbi_emit_return_arm`
- `urbi_emit_scope_crossings`
- `urbi_emit_string_arm`
- `urbi_emit_subscript_get_arm`
- `urbi_emit_subscript_set_arm`
- `urbi_emit_switch_arm`
- `urbi_emit_tag_prefix_arm`
- `urbi_emit_this_arm`
- `urbi_emit_throw_arm`
- `urbi_emit_try_arm`
- `urbi_emit_unary_arm`
- `urbi_emit_var_decl_arm`
- `urbi_emit_waituntil_arm`
- `urbi_emit_watcher_arm`
- `urbi_emit_while_arm`
- `urbi_encode_utf8`
- `urbi_opcode_shapes`
- `urbi_parse_arena_grow_node_array`
- `urbi_parse_assert`
- `urbi_parse_at`
- `urbi_parse_atom`
- `urbi_parse_block`
- `urbi_parse_consume`
- `urbi_parse_desugar_postfix_emit`
- `urbi_parse_every`
- `urbi_parse_expression`
- `urbi_parse_expression_cont`
- `urbi_parse_function`
- `urbi_parse_ident_equals`
- `urbi_parse_if`
- `urbi_parse_inner_tier`
- `urbi_parse_kEmitMethodName`
- `urbi_parse_kErrorMessages`
- `urbi_parse_make_binary`
- `urbi_parse_make_error`
- `urbi_parse_make_ident`
- `urbi_parse_make_int`
- `urbi_parse_make_nil_node`
- `urbi_parse_make_node`
- `urbi_parse_make_unary`
- `urbi_parse_outer_tier`
- `urbi_parse_peek`
- `urbi_parse_peek2`
- `urbi_parse_pipe_amp_fold`
- `urbi_parse_prefix`
- `urbi_parse_property_decl`
- `urbi_parse_statement_or_expr`
- `urbi_parse_tag_prefix`
- `urbi_parse_tag_prefix_from_expr`
- `urbi_parse_throw`
- `urbi_parse_try`
- `urbi_parse_waituntil`
- `urbi_parse_whenever`
- `urbi_parse_while`
- `urbi_require_fail`
- `urbi_set_require_fail_hook`
- `urbi_vm_find_or_install_upvalue`
