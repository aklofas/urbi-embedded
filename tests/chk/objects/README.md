# tests/chk/objects/

Legacy `.chk` fixtures for M4 prototype-based object model.  Originally triaged
2026-05-03 against v1.0 urbiscript capabilities (branch `topic/m4-followup-proto-inst-binding`,
HEAD `91c0f5f`, after T10 landed).  Re-audited 2026-05-04 after spec #5 globals
exposure landed (branch `topic/m5-reactive`, R7 complete).

## v1.0 constraints affecting all fixtures

- **No globals** (`src/uemit.c:558`): ~~lifted by spec #5 (R7 2026-05-04)~~.
  `Object`, `Tag`, `Event`, atom singletons, `Realm`, `nil`, `void` are now
  reachable as realm globals.  Fixtures that only needed globals unblocked at
  the name-resolution level are noted below; most remain blocked on T38/T39.
- **No `class` declaration** (T38 deferred): the `class Foo { … }` syntactic
  form is not emitted.  Every fixture that opens with `class …` is blocked.
- **No `Class.new()` / stdlib clone wiring** (T39 deferred): `.new()` and
  `.clone()` are not callable from urbiscript even if a local UObject handle
  existed.
- **No atom-method dispatch** (M5/M6 stdlib gap): slot lookup starting from a
  `UTYPE_INT` / `UTYPE_FLOAT` / `UTYPE_STR` atom through its proto chain is
  not wired.  `1.clone()` fails even though globals are now exposed.
- **No getter/setter parse sugar** (T41 deferred): `get`/`set` slot attributes
  are not parsed.
- **`var obj.slot = v` as a statement** requires `obj` to already be a local
  — and there is no way to *create* a fresh UObject from urbiscript today
  (no `.new()`, no `.clone()`).

## Status

Post-globals re-audit (2026-05-04, R7 complete).  Globals blocker removed from
`lookup` and `fallback`; both retain T38/T39 blocks.  `atom-clone` and `atoms`
lose their "globals" tag but remain blocked on atom-method dispatch.
**No fixture revives at R7 alone.**

| Fixture | Status | Blocking subsystem(s) | Summary of legacy script |
|---|---|---|---|
| `lookup` | 🔴 blocked | T38 `class`, T39 `.new()` | Calls `Object.new()` — Object now resolves; `.new()` still T39. Also uses `class Foo {}` mid-fixture |
| `inheritance` | 🔴 blocked | T38 `class`, T39 `.new()` | Entire fixture is `class A { … }`, `class B { … }`, `B.clone()`, `.insertFront(A)` |
| `slot-cow-const` | 🔴 blocked | T38 `class`, T39 `.new()` | `class a { const var x = 0 }`, then `a.new()` — both forms deferred |
| `shared-protos` | 🔴 blocked | T38 `class`, T39 `.new()` | `class A { … }`, `class B { … }`, then `B.clone()` and `protos.insertFront(A)` |
| `class` | 🔴 blocked | T38 `class`, T39 `.new()` | Nested `class a { … }` + `class a : public a { … }` + `.new()` — all deferred |
| `fallback` | 🔴 blocked | T38 `class`, T39 `.new()` | `class C { function fallback … }`, `C.new()`, plus `var Object.blurg` — Object now resolves but COW write needs T39 |
| `atom-clone` | 🔴 blocked | atom-method dispatch (M5/M6 stdlib) | `var a = 1.clone()` — atom proto-chain not wired; globals no longer a separate blocker |
| `atoms` | 🔴 blocked | atom-method dispatch (M5/M6 stdlib) | `1.clone()`, `2.clone()`, then `var one.foo = "foobar"` — same atom-dispatch block |

**All 8 fixtures remain blocked.  7 of 8 block on T38+T39; 2 of those
(atom-clone, atoms) also need atom-method dispatch.**

## Fixture-by-fixture detail

### `lookup.chk`

Opens with `var top = Object.new()` — `Object` now resolves as a realm global
(spec #5 R7); `.new()` still fails (T39 deferred).  The second half of the file
also uses `class Foo {}`, `Foo.setProtos(…)`, and `Object.new.addProto(…)`.
Globals blocker removed; blocked on T38+T39.

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
`C.new()`.  Also writes `var Object.blurg = "blurg"` (COW slot write on the
Object proto) and uses `do (Object.new()) { … }`.  `Object` now resolves as a
realm global (spec #5 R7); the COW slot write (`var Object.blurg`) now reaches
the Object proto's slot table at parse time without a name-resolution error.
But the `class C` form and `.new()` call remain blocked on T38+T39.  Globals
blocker removed.

### `atom-clone.chk`

`var a = 1.clone()` — dispatches `.clone()` off an integer literal.  This
requires atom-method dispatch (routing a slot lookup starting from a `UTYPE_INT`
atom through its proto chain).  No `class` involved; globals are no longer a
separate blocker (spec #5 R7).  Sole remaining gate is atom-proto-chain stdlib
wiring (M5/M6 stdlib).  Best candidate once atom-dispatch lands.

### `atoms.chk`

Same as `atom-clone` but also adds `var one.foo = "foobar"` (COW clone slot
write) and confirms `1.foo` and `2.foo` are absent.  Blocked on atom-dispatch;
globals no longer a separate blocker.  The COW semantics are already implemented
at the C level (T8/T9/T10).

## Fixtures landing in T12

**None.**  All 8 fixtures were blocked by the v1.0 globals constraint and/or
T38/T39 stdlib subsystems.  T12 formally deferred all 8 to subsequent
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
- **When atom-method dispatch lands** (atom proto-chain stdlib wiring, M5/M6
  stdlib milestone): re-audit `atom-clone.chk` and `atoms.chk`.  These two are
  the lightest fixtures: once `1.clone()` dispatches correctly the rest is pure
  slot-COW, already implemented at the C level.
- ~~**When globals are exposed**~~: **DONE** (spec #5, R7 2026-05-04).
  `lookup.chk` and `fallback.chk` lose their globals-blocker tag.  No revival
  at this trigger alone — T38+T39 remain.
- **When T41 get/set parse sugar lands**: no fixture in this set specifically
  requires it, but `fallback.chk` uses `call.message` introspection that may
  interact with call-frame sugar.
