# tests/chk/objects/

Legacy `.chk` fixtures for the M4 prototype-based object model.  The 8
fixtures listed below were originally triaged 2026-05-03 against v1.0
urbiscript capabilities (branch `topic/m4-followup-proto-inst-binding`,
HEAD `91c0f5f`, after T10 landed) and re-audited 2026-05-04 after the
spec #5 globals exposure landed (branch `topic/m5-reactive`, R7
complete).  All 8 are now active under
`topic/v0.6.0-stdlib-scaffold` / Phase 8.

## Status

Post-Wave-1 (`v0.6.0-stdlib-scaffold`, 2026-05-09): all 8 fixtures
activated.  5 of 8 ported wholesale; 3 of 8 ported as Wave-1 subsets
(remainder defers to Wave 2 with the List atom + atom auto-boxing +
fallback-protocol + call.message + `do` block).

| Fixture | Status | Notes |
|---|---|---|
| `lookup.chk` | active | Full port (legacy was 2 lookup-failure lines).  Diagnostic format adapted to v1.0's `TypeError: GETSLOT: slot 'NAME' not found`. |
| `inheritance.chk` | active partial | First 7 assertions ported; the legacy tail (`class Foo {}; Foo.setProtos([Global, Math]); Foo.protos == [Global, Math]` and the `Foo.protos = [Object, Object]` updateHook line) defers to Wave 2 — needs List literals. |
| `slot-cow-const.chk` | active | Ported with `var x` instead of `const var x`; v1.0 has no `const` parse sugar but the underlying COW semantics are identical. |
| `shared-protos.chk` | active | Full port.  `function p () { echo(...) }` adapted to `var p = function() { return ... }` (named-function decls retired; `echo` not yet a realm global). |
| `class.chk` | active | Full port (S-class-name-scope nested-shadow).  Same-line shadow chaining substitutes for the legacy fixture's bare-block scoping; semantically identical and overlaps with `class_decl_nested.chk`. |
| `fallback.chk` | active partial | Local-method path + COW Object root write ported.  `function fallback { echo("Fallback " + call.message) }` (fallback protocol on lookup-miss), `call.message` (call-object reflection), and `do (Object.new()) { ... }` (scoped install sugar) defer to Wave 2. |
| `atom-clone.chk` | active partial | Atom-clone short-circuit + arithmetic ported.  Third line `a.protos == [1]` defers — depends on the List literal print form. |
| `atoms.chk` | active partial | Atom-clone + arithmetic ported.  `var one.foo = "foobar"`, `1.foo`, `2.foo` lines defer — atom auto-boxing on slot install (needs `urbi_slot_set_slow` to allocate a fresh UObject inheriting from the atom proto and rebind the local) is not in the Wave 1 budget.

## Wave 2 trigger summary

The remaining deferrals across these 8 fixtures collapse to four
production-code gaps:

- **List atom + List literal lex** — closes the `inheritance.chk` tail
  (setProtos taking a list) and the `atom-clone.chk` third line
  (a.protos printing as `[1]`).
- **Atom auto-boxing on slot install** (≈50 LOC in
  `urbi_slot_set_slow`) — closes the `atoms.chk` `var one.foo` lines.
- **Fallback-protocol VM dispatch** + `call.message` (CallMessage
  stdlib) — closes the `fallback.chk` `function fallback` and
  `c.bar()` lines.
- **`do (recv) { body }` block sugar** — closes the `fallback.chk`
  override-fallback tail (`var x = do (Object.new()) { function
  fallback { echo("x") } }`).

Each fixture's own header comment documents which lines defer; new
Wave-2 work should add lines back rather than rewriting the headers.
