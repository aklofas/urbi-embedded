# Emit correctness notes

This document records audit-level emit-correctness rationale that does not
naturally fit into source comments. Each section corresponds to a verified
invariant about a specific subsystem.

## OP_CLOSURE-clobber-event family — verified clean as of v0.5.7-fixes

**Audit ID:** EMIT-043
**Closed by:** Wave 5 (v0.5.7-fixes)

The v0.5.2-scratch-frame-followup release (commit `5bd98af`) fixed an
AST_AT_EVENT emit register-allocation desync where `emit_function_literal`
could allocate a body-register on top of a still-live event-pointer
register, after which OP_CLOSURE clobbered the event pointer. The fix
landed at TWO sibling sites: the AT_EVENT install (sync + async) and the
AT_SLOT_CHANGE install. AST_WATCHER and AST_WAITUNTIL were not affected
because their condition was wrapped in `emit_function_literal`
symmetrically.

EMIT-043 was the audit's request to verify by inspection that no other
sibling sites in the M5 reactive emit pipeline carry the same shape.

### Coverage rationale (verified clean)

- **AST_AT_EVENT** (sync + async) — fixed at v0.5.2 (commit `5bd98af`).
- **AST_SLOT_CHANGE** — fixed at v0.5.2 (commit `5bd98af`).
- **AST_WATCHER (AT / WHENEVER)** — symmetric `emit_function_literal`
  wrap: cond and body each scoped freereg restoration. No drift.
- **AST_WAITUNTIL** — same symmetric wrap as AST_WATCHER.
- **AST_AT_INSTALL family** — register drift between `next_reg` and
  `freereg` after the install teardown was a separate concern; closed
  by EMIT-010 in v0.5.7-fixes T9 (commit `608af55`).
- **AST_TAG_PREFIX** — distinct concern (4-bit reg-nibble truncation
  on spill ≥ 16); closed by EMIT-015 in v0.5.7-fixes T14 (commit
  `57a6317`).
- **AST_IF arms** — distinct concern (single-expr arm leaked nested
  `var` decls into the local-zone floor); closed by EMIT-016 in
  v0.5.7-fixes T15 (commit `dc98956`).

### Future-defense

If a future M6+ emit pass adds a new sibling shape (a new reactive-emit
arm that uses `emit_function_literal` to construct an event-bound body),
this list MUST be extended after the new arm lands. The audit-finding
template at `docs/superpowers/specs/` calls out the "freereg/next_reg
sync" rubric explicitly; adding a new install opcode without a
freereg-sync audit pass should be treated as an oversight.

### Cross-references

- v0.5.2 retrospective: `docs/superpowers/plans/2026-05-05-v0.5.2-scratch-frame-followup.md`
- v0.5.7-fixes plan: `docs/superpowers/plans/2026-05-07-v0.5.7-fixes.md` (T8-T19)
- Audit findings (EMIT cluster): `docs/superpowers/specs/2026-05-05-v0.5.x-cleanup-audit-findings.md`
