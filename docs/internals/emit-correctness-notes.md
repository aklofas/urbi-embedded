# Emit correctness notes

This document records emit-correctness rationale that does not naturally
fit into source comments. Each section corresponds to a verified invariant
about a specific subsystem.

## OP_CLOSURE-clobber-event family

The `v0.5.2-scratch-frame-followup` release (commit `5bd98af`) fixed an
AST_AT_EVENT emit register-allocation desync where `emit_function_literal`
could allocate a body-register on top of a still-live event-pointer
register, after which OP_CLOSURE clobbered the event pointer. The fix
landed at two sibling sites: the AT_EVENT install (sync + async) and the
AT_SLOT_CHANGE install. AST_WATCHER and AST_WAITUNTIL were not affected
because their condition was wrapped in `emit_function_literal`
symmetrically.

By inspection, no other sibling site in the reactive emit pipeline carries
the same shape.

### Coverage rationale (verified clean)

- **AST_AT_EVENT** (sync + async) — fixed at `v0.5.2-scratch-frame-followup`
  (commit `5bd98af`).
- **AST_SLOT_CHANGE** — fixed at `v0.5.2-scratch-frame-followup` (commit
  `5bd98af`).
- **AST_WATCHER (AT / WHENEVER)** — symmetric `emit_function_literal` wrap:
  cond and body each scoped freereg restoration. No drift.
- **AST_WAITUNTIL** — same symmetric wrap as AST_WATCHER.
- **AST_AT_INSTALL family** — register drift between `next_reg` and
  `freereg` after the install teardown was a separate concern, closed in
  `v0.5.7-fixes` (commit `608af55`).
- **AST_TAG_PREFIX** — distinct concern (4-bit reg-nibble truncation on
  spill ≥ 16), closed in `v0.5.7-fixes` (commit `57a6317`).
- **AST_IF arms** — distinct concern (single-expr arm leaked nested `var`
  decls into the local-zone floor), closed in `v0.5.7-fixes` (commit
  `dc98956`).

### Future-defense

If a future emit addition introduces a new reactive-emit arm that uses
`emit_function_literal` to construct an event-bound body, this list must
be extended after the new arm lands. The "freereg/next_reg sync" rubric
applies: adding a new install opcode without a freereg-sync audit should
be treated as an oversight.
