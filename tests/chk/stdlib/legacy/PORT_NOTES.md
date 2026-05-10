# Legacy `.chk` fixture port notes (Wave 2 / Phase 11)

This directory holds ports of `legacy/repos/aldebaran-urbi/tests/2.x/`
fixtures, adapted to the v0.6.1 (Wave 2) language subset.

## Triage outcome

The plan envisioned 9-12 ports.  Most legacy fixtures depend on
language features that v0.6.1 does not yet ship — primarily:

- **No float literals.**  Lex rejects `1.5`, `3.14`.  Float values are
  reachable only via `Integer.asFloat()` and `Float`-returning native
  methods (`Math.pi`, `Math.e`, `System.time()`, etc.).
- **No List / Dict literals.**  `[1, 2, 3]`, `["a" => 1]` lex as
  errors.  Constructed via `List.new(...)` and `Dict.new()`.
- **No closure upvalue capture.**  `function () { outer_var }` raises
  `EMIT_UNRESOLVED_NAME`.  Blocks every legacy fixture using `for&`,
  `each(closure (x) { ... })`, fresh-default-value tricks, etc.
- **Multi-slot class bodies.**  `class C { var x; var y; method m() {} }`
  raised `EMIT_UNSUPPORTED_AST`.  Fixed in Wave 3 (Gap #2).
- **No `var x.foo = "..."` slot-install form.**  Wave 1 partial-port
  remainders (`atoms.chk`, `fallback.chk`) need this.
- **No `assert`, `echo` realm globals.**  Legacy fixtures lean on
  these heavily.
- **No string concat (`+`), no `<<`, no `for (...)`.**  Most legacy
  control flow is blocked.
- **Operator-via-slot-install** (`Date.'+' = function ...`) doesn't
  intercept inline VM opcodes.
- **No fallback protocol, no `call.message`, no `do (recv) { ... }`.**
  Wave 2 stdlib doesn't grow these.

These are tracked as Phase 10 v1.0 emit gaps and v1.x backlog items.

## What ported

Each fixture is a small, focused subset of its legacy counterpart.
The pattern is "take the assertions that exercise stdlib semantics
v0.6.1 does ship, drop everything else."

| Port | Legacy origin | Coverage |
|---|---|---|
| `dict_legacy.chk` | `tests/2.x/dictionary.chk` | Dict.new() + set/get/has/length/remove without `=>` literal sugar |
| `list_legacy.chk` | `tests/2.x/list.chk` (heavily reduced) | List.new() + add/get/set/length/contains; `+=` / `each(closure)` / list-literal sections all blocked |
| `mutex_legacy.chk` | `tests/2.x/mutex/basic.chk` (reduced) | Mutex.new()/lock/unlock/tryLock; threaded `func1`/`func2`/`func3` choreography blocked (closure upvalues + cooperative-semantic divergence) |
| `date_legacy.chk` | `tests/2.x/date.chk` | Date.fromSeconds/.seconds()/.asString(); legacy `Date.epoch - Date.epoch + N` arithmetic seam not implemented (Date.epoch absent at v1.0) |
| `system_legacy.chk` | `tests/2.x/system/platform.chk`, `system/hostname.chk` | System.Platform.kind in {`linux`,`darwin`,`windows`,`freertos`,`unknown`}; hostname blocked (Process subsystem absent) |
| `large_string_legacy.chk` | `tests/2.x/large-string.chk` | 4097-byte string lex via adjacent-literal concat, exercises UQueue 4 KiB grow path; `for (var i = 0; i < 12; i++)` doubling loop blocked |
| `maths_errors_legacy.chk` | `tests/2.x/maths-errors.chk` (very reduced) | Lookup-failure for missing `Math.sqrt`/`log`/`sqr`; the `1/0` legacy diagnostic returns `inf` at v1.0 (IEEE-754 semantics — REVIVAL §14.x divergence, not a bug) |

## What deferred to v1.x (full-fixture port backlog)

The following legacy fixtures depend on features the M6 stdlib
roadmap explicitly defers.  Each one becomes feasible once the
listed gaps close.  Tracked here as a v1.x port-completion backlog;
mirrored in `docs/urbi-embedded-backlog.md` under "legacy fixture
parity".

- `tests/2.x/list.chk` (full): list literals, `each(closure)`, `<<`,
  list `+=` identity preservation.
- `tests/2.x/dictionary.chk` (full): `["a" => 42]` dict literal, `extend`.
- `tests/2.x/string/{comparison,escape,split}.chk`: String comparison
  ops on string atoms (`<`, `>=`), `\B(...)` escape, `split(...)`.
- `tests/2.x/mutex/{basic,queued,tagged}.chk`: closure-upvalue captures
  in `function f(name) { var d = function(msg) { echo (name + ...) }`,
  tag scope (`m: { ... }`).  Legacy basic.chk also tests strict
  thread-style serialization that diverges from cooperative semantics.
- `tests/2.x/float/{aliases,arity,inplace,primitives}.chk`: float
  literal lex, `++`/`--`/`+=`/`-=` operator desugar, `1.'+'(0,0)`
  quoted-operator slot syntax.
- `tests/2.x/system/hostname.chk`: `Process.new("hostname", [...])` —
  Process subsystem deferred to v1.x.
- `tests/2.x/large-string.chk` (full): the doubling loop section
  needs `for (var i = 0; i < 12; i++)`, `s + " " + s`, and `echo`.
- `tests/2.x/maths-errors.chk` (full): `Math.sqrt(-10.0)` etc.
  require float literals + Math.sqrt/log/asin/acos methods (none
  exist in v0.6.1 namespaces.c — Math has constants pi/e/nan/infinity
  only).  `1/0` gives `inf` at v1.0 (IEEE-754); legacy raises a
  domain error.
- `tests/2.x/atoms.chk` Wave-1 deferred section: `var one.foo = ...`
  slot-install form not in v0.6.1 lex.
- `tests/2.x/fallback.chk` Wave-1 deferred section: `function fallback`
  + `call.message` + `do (recv) { ... }`.
- `tests/2.x/inheritance.chk` Wave-1 deferred section: list literals
  for `setProtos([Global, Math])`.

## Wave 3 additions (M6 Wave 3 / Phase 3-4, 2026-05-10)

- `class_legacy.chk`: port of `tests/2.x/class.chk`.  The original
  wraps tests in bare `{ ... }` blocks (illegal at top-level in v1.0
  line-oriented REPL); adapted with same-line shadow chaining.  Skipped
  lines involving `this` (Gap #3) and closure upvalue capture (Gap #1),
  which are not yet landed in Wave 3.  Multi-slot class body extension
  added (Gap #2, now closed).  This note is superseded for Gap #3 by
  Phase 2 (`this` keyword); the skipped line can be activated when that
  branch lands.

- `operators_legacy.chk`: subset port of `tests/2.x/operators.chk`.
  Tests that operator-named slots (+, -, *, /, ==) installed via
  `setSlot` are called by the VM's Gap #4 type-error fallback.
  Deferred: `.operator +(1)` explicit-method call syntax, `bitor`,
  and `in` operator are not in the v0.6.x parser.

- `this.chk` (deferred entirely to v1.x): port of `tests/2.x/this.chk`.
  The legacy fixture has 3 test lines, all using `this` at the top level
  to access the Lobby object (`this == this`, `this == { this }`,
  `this.type`).  At v1.0 `this` is only valid inside a method body;
  top-level `this` is a compile-time error (`EMIT_NO_THIS_OUTSIDE_METHOD`).
  The "Lobby" concept (a singleton global namespace object) is not
  modelled in the v1.0 runtime.  No subset is shippable: all three lines
  are structurally deferred.  Tracked in `docs/urbi-embedded-backlog.md`
  under "legacy fixture parity."

- `operator-parens.chk` (deferred to v1.x): requires the `'()'` slot
  name for the call operator and `call.evalArgs()` (CallMessage
  reflection).  Neither is available in v0.6.x.

- `edit-container.chk` (deferred to v1.x): tests `l[i] += N` compound
  subscript assignment on List and Dict.  Requires list/dict literal
  syntax (`[1, 2, 3]`, `["a" => 0]`) and compound-assignment desugar
  (`l[i] += v` → `l.set(i, l.get(i) + v)`), none of which exist in
  v0.6.x.

## Adjustments applied during port

- REPL prompt format: legacy `[NNNNN]` (5-digit) → ours `[NNNNNNNN]`
  (8-digit), zero-indexed.
- `.isA(Float)` on integer literals → either dropped (if Float check
  was the load-bearing assertion) or `.isA(Integer)` per
  language-and-runtime spec §2.4.
- `Date.epoch` arithmetic → `Date.fromSeconds(N)` constructor
  (legacy fixture's Date arithmetic seam doesn't ship at v1.0).
