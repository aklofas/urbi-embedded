# tests/chk/objects/

Legacy `.chk` fixtures for M4 prototype-based object model.  Triaged
2026-05-03 against v1.0 urbiscript capabilities (branch `topic/m4-followup-proto-inst-binding`,
HEAD `91c0f5f`, after T10 landed).

## v1.0 constraints affecting all fixtures

- **No globals** (`src/uemit.c:558`): `Object`, atom singletons (`Integer`,
  `Float`, etc.) and any other top-level name is unreachable from urbiscript.
  Any fixture that references `Object`, a class name as a global, or an atom
  method dispatched off a literal (`1.clone()`) cannot run today.
- **No `class` declaration** (T38 deferred): the `class Foo { … }` syntactic
  form is not emitted.  Every fixture that opens with `class …` is blocked.
- **No `Class.new()` / stdlib clone wiring** (T39 deferred): `.new()` and
  `.clone()` are not callable from urbiscript even if a local UObject handle
  existed.
- **No getter/setter parse sugar** (T41 deferred): `get`/`set` slot attributes
  are not parsed.
- **`var obj.slot = v` as a statement** requires `obj` to already be a local
  — and there is no way to *create* a fresh UObject from urbiscript today
  (no globals, no `.new()`, no `.clone()`).

## Status

| Fixture | Status | Blocking subsystem(s) | Summary of legacy script |
|---|---|---|---|
| `lookup` | 🔴 blocked | globals, T38 `class`, T39 `.new()` | Calls `Object.new()` on line 1; also uses `class Foo {}` mid-fixture |
| `inheritance` | 🔴 blocked | T38 `class`, T39 `.new()` | Entire fixture is `class A { … }`, `class B { … }`, `B.clone()`, `.insertFront(A)` |
| `slot-cow-const` | 🔴 blocked | T38 `class`, T39 `.new()` | `class a { const var x = 0 }`, then `a.new()` — both forms deferred |
| `shared-protos` | 🔴 blocked | T38 `class`, T39 `.new()` | `class A { … }`, `class B { … }`, then `B.clone()` and `protos.insertFront(A)` |
| `class` | 🔴 blocked | T38 `class`, T39 `.new()` | Nested `class a { … }` + `class a : public a { … }` + `.new()` — all deferred |
| `fallback` | 🔴 blocked | T38 `class`, T39 `.new()`, globals | `class C { function fallback … }`, `C.new()`, plus `var Object.blurg` (writes to `Object` global) |
| `atom-clone` | 🔴 blocked | globals (atom dispatch) | `var a = 1.clone()` — dispatches `.clone()` off an integer literal; no atom-method dispatch today |
| `atoms` | 🔴 blocked | globals (atom dispatch) | `1.clone()`, `2.clone()`, then `var one.foo = "foobar"` — same atom-dispatch block |

**All 8 fixtures are blocked.**

## Fixture-by-fixture detail

### `lookup.chk`

Opens with `var top = Object.new()` — `Object` is a global; immediately fails
at `src/uemit.c:558`.  The second half of the file also uses `class Foo {}`,
`Foo.setProtos(…)`, and `Object.new.addProto(…)`.  Blocked on: globals
exposure and T38/T39.

### `inheritance.chk`

Three statements: `class A { function p … }`, `class B { function p … }`,
then `var C = B.clone()`.  Pure T38+T39 block.

### `slot-cow-const.chk`

`class a { const var x = 0 }` then `var b = a.new()`.  Shortest fixture of
the eight — 8 lines.  Pure T38+T39 block; no globals issue beyond that.
Best candidate for a quick port once T38/T39 land.

### `shared-protos.chk`

`class A { … }`, `class B { … }`, `var C = B.clone()`,
`C.protos.insertFront(A)`.  Pure T38+T39 block.

### `class.chk`

Nested-scope `class a { var foo = 40 }`, then `class a : public a { var bar
= 2 }`, then `a.new()`.  Tests that the class name is not in scope during its
own definition.  Pure T38+T39 block; no atom dispatch.

### `fallback.chk`

`class C { function fallback { echo("Fallback " + call.message) } }` then
`C.new()`.  Also writes `var Object.blurg = "blurg"` (global-slot mutation)
and uses `do (Object.new()) { … }`.  Blocked on T38, T39, and globals.

### `atom-clone.chk`

`var a = 1.clone()` — dispatches `.clone()` off an integer literal.  This
requires atom-method dispatch (routing a slot lookup starting from a `UTYPE_INT`
atom through its proto chain).  No `class` involved; the atom-dispatch layer
alone unblocks this.  Good T39/atom-stdlib candidate.

### `atoms.chk`

Same as `atom-clone` but also adds `var one.foo = "foobar"` (COW clone slot
write) and confirms `1.foo` and `2.foo` are absent.  Blocked on atom-dispatch;
the COW semantics are already implemented at the C level (T8/T9/T10).

## Fixtures landing in T12

**None.**  All 8 fixtures are blocked by the v1.0 globals constraint and/or
T38/T39 stdlib subsystems.  T12 will formally defer all 8 to subsequent
milestones.  The dispatch-arm changes (T8/T9/T10) are tested via unit-test
diagnostic-message coverage in `tests/test_vm.c` and the existing
`tests/chk/closure/` + `tests/chk/separator/` suites that exercise slot
get/set on locally-created objects.

## Future re-audit triggers

- **When T38 lands** (class declaration emit + parse sugar): re-audit
  `class.chk`, `inheritance.chk`, `shared-protos.chk`, `slot-cow-const.chk`,
  `fallback.chk`.
- **When T39 lands** (stdlib `Class.new()` / `.clone()` wiring): re-audit
  any fixture that calls `.new()` or `.clone()` — effectively all five above,
  plus `atom-clone.chk` and `atoms.chk` once atom-dispatch is also live.
- **When atom-method dispatch lands** (atom proto-chain stdlib wiring, likely
  M5/M6 stdlib milestone): re-audit `atom-clone.chk` and `atoms.chk`.  These
  two are the lightest fixtures: once `1.clone()` dispatches correctly the
  rest is pure slot-COW, which is already implemented at the C level.
- **When globals are exposed** (post-v1.0 or v1.x feature): re-audit
  `lookup.chk` and `fallback.chk` (the two that also mutate or read `Object`
  as a global).
- **When T41 get/set parse sugar lands**: no fixture in this set specifically
  requires it, but `fallback.chk` uses `call.message` introspection that may
  interact with call-frame sugar.
