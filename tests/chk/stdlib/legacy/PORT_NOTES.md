# Legacy `.chk` fixture port notes (Wave 2 / Phase 11)

This directory holds ports of `legacy/repos/aldebaran-urbi/tests/2.x/`
fixtures, adapted to the v0.6.1 (Wave 2) language subset.

## Triage outcome

The plan envisioned 9-12 ports.  Most legacy fixtures depend on
language features that urbi-embedded does not yet ship — a summary of
each gap's current status follows.

- **Float literals:** supported since v0.6.2.  `1.5`, `3.14`, and other
  decimal literals lex and evaluate correctly.  Fixtures blocked solely
  on float literals can now be activated.

- **List / Dict literals: implemented (Wave 6 W10).**  `[1, 2, 3]` and
  `["a" => 1]` are supported.  They lower to `List.new(...)` and
  `Dict.new()` + `.set(k, v)` respectively; no new opcode.
  See `tests/chk/objects/list_literal.chk` and `dict_literal.chk`.

- **Closure upvalue capture:** supported since v0.8.4 (closure-lifetime
  GC promotion).  `function () { outer_var }` now captures upvalues from
  enclosing scopes correctly.  Fixtures that were blocked solely on
  upvalue capture can now be activated.  Note: the `closure` keyword
  itself is retired; replace with `function` — see
  `docs/migration/callmessage-migration.md` §closure keyword migration.

- **Multi-slot class bodies:** supported since v0.6.2 Wave 3 (Gap #2).
  `class C { var x; var y; var m = function() {} }` works.

- **`var obj.slot = value` slot-install form: implemented (Wave 6 W10).**
  `var obj.slot = value` desugars to `obj.slot = value` (AST_MEMBER_SET)
  at parse time.  OP_SETSLOT installs the slot when absent.  Deep chains
  `var a.b.c = v` also work via intermediate MEMBER_GET nodes.
  See `tests/chk/objects/var_obj_slot.chk`.

- **`echo` realm global:** shipped as a Lobby method since v0.9.1
  (`Lobby.echo(msg, tag, prefix)`).  Legacy fixtures that call `echo`
  at the top level will resolve it through the session Lobby in a
  REPL context; standalone fixtures that lack a Lobby context may still
  need adaptation.
- **`assert`:** implemented as a language keyword (Wave 6 W3, v0.10.5).
  `assert(expr)` lowers to `if (!expr) throw "assertion failed: <src>"`.
  `assert { block }` lowers to `if (!block) throw "assertion failed"`.
  Fixtures deferred for other reasons (not just `assert`) remain deferred.

- **No string `+` concatenation.**  `arith_add` handles numbers only;
  no `"+"` slot is registered on String proto (T46 explicitly dropped in
  atoms.c).  Fixtures that require string `+` remain blocked.
- **No `<<` append operator.**  Not in the lexer or parser.  Fixtures
  using `<<` remain blocked.
- **No `for (init; cond; step)` / `for (var x : iter)`.**  Wave 6 W1
  decides the ruling.  Fixtures using `for` remain deferred.

- **Operator-via-slot-install** (`Date.'+' = function ...`) still
  doesn't intercept inline VM opcodes.  The VM's Gap #4 slot-fallback
  handles the `+` operator only after the numeric fast-path fails, so
  operator methods on non-numeric protos work for custom types (see
  `operators_legacy.chk`) but cannot override the numeric fast path.
  Wave 6 W2 (quoted identifiers) may expand this surface.

- **No fallback protocol, no `call.message`, no `do (recv) { ... }`.**
  - `call.message` / `call.evalArgAt`: **PERMANENTLY DROPPED.**  See
    `docs/migration/callmessage-migration.md` for migration patterns
    (lazy-param alternatives, try/catch fallback, accepted losses).
  - Legacy `fallback` function + `do (recv) { ... }`: separate question
    from CallMessage; neither is implemented.  `do (recv)` is Wave 6 W1
    deferred.

These gaps are tracked in `docs/language-compatibility-matrix.md` (per-
construct rows) and `docs/urbi-embedded-backlog.md` (v1.x backlog).

## What ported

Each fixture is a small, focused subset of its legacy counterpart.
The pattern is "take the assertions that exercise stdlib semantics
v0.6.1 does ship, drop everything else."

| Port | Legacy origin | Coverage |
|---|---|---|
| `dict_legacy.chk` | `tests/2.x/dictionary.chk` | Dict.new() + set/get/has/length/remove; `["k" => v]` literal sugar now works (W10) |
| `list_legacy.chk` | `tests/2.x/list.chk` (heavily reduced) | List.new() + add/get/set/length/contains; list literals now work (W10); `each(closure)` / `<<` / `+=` identity preservation still blocked |
| `mutex_legacy.chk` | `tests/2.x/mutex/basic.chk` (reduced) | Mutex.new()/lock/unlock/tryLock; threaded `func1`/`func2`/`func3` choreography blocked (closure upvalues + cooperative-semantic divergence) |
| `date_legacy.chk` | `tests/2.x/date.chk` | Date.fromSeconds/.seconds()/.asString(); legacy `Date.epoch - Date.epoch + N` arithmetic seam not implemented (Date.epoch absent at v1.0) |
| `system_legacy.chk` | `tests/2.x/system/platform.chk`, `system/hostname.chk` | System.Platform.kind in {`linux`,`darwin`,`windows`,`freertos`,`unknown`}; hostname blocked (Process subsystem absent) |
| `large_string_legacy.chk` | `tests/2.x/large-string.chk` | 4097-byte string lex via adjacent-literal concat, exercises UQueue 4 KiB grow path; `for (var i = 0; i < 12; i++)` doubling loop blocked |
| `maths_errors_legacy.chk` | `tests/2.x/maths-errors.chk` (very reduced) | Lookup-failure for missing `Math.sqrt`/`log`/`sqr`; the `1/0` legacy diagnostic returns `inf` at v1.0 (IEEE-754 semantics — intentional v1.0 divergence, not a bug) |

## What deferred to v1.x (full-fixture port backlog)

The following legacy fixtures depend on features the M6 stdlib
roadmap explicitly defers.  Each one becomes feasible once the
listed gaps close.  Tracked here as a v1.x port-completion backlog;
mirrored in `docs/urbi-embedded-backlog.md` under "legacy fixture
parity".

- `tests/2.x/list.chk` (full): `each(closure)`, `<<`, list `+=` identity
  preservation (list literals and subscript now work via W10).
- `tests/2.x/dictionary.chk` (full): `extend` (dict literals now work via W10).
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
  slot-install form is now implemented (Wave 6 W10); this deferred item
  can be re-activated once any remaining atom-auto-boxing gaps are closed.
- `tests/2.x/fallback.chk` Wave-1 deferred section: `function fallback`
  + `call.message` + `do (recv) { ... }`.
- `tests/2.x/inheritance.chk` Wave-1 deferred section: `setProtos([Global, Math])`
  (list literals now work via W10; `setProtos` dispatch may still need attention).

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

- `this.chk` (migration — use `Realm`): port of `tests/2.x/this.chk`.
  The legacy fixture has 3 test lines, all using `this` at the top level
  to access the Lobby object (`this == this`, `this == { this }`,
  `this.type`).  At v1.0 `this` is only valid inside a method body;
  top-level `this` is a compile-time error (`EMIT_NO_THIS_OUTSIDE_METHOD`).
  Decision (v0.10.5-legacy-decisions W11, legacy F13): **migration** — replace
  `this` at the top level with `Realm`.  `Realm` is the per-realm global object
  (a singleton since v0.9.0) and is the direct functional replacement for the
  legacy Lobby singleton.  A migration-aware fixture is active at
  `tests/chk/objects/realm_as_lobby_at_top_level.chk`.  See
  `docs/migration/top-level-this-lobby-migration.md` for the full recipe.
  The `this == { this }` line (block returning `this`) remains unported
  because bare brace-block as expression is not in the v1.0 parser.
  `lobby.chk` (full Lobby class with `addProto` / `protos` / session hooks)
  remains deferred to v1.x — Lobby named global exists but the full class-method
  set is not ported.

- `operator-parens.chk` (deferred to v1.x): requires the `'()'` slot
  name for the call operator and `call.evalArgs()` (CallMessage
  reflection).  Neither is available in v0.6.x.

- `edit-container.chk` (partially activated — Wave 6 W10): `l[i] += N`
  compound subscript assignment is now implemented.  `l[i]` lowers to
  `l.get(i)`, `l[i] = v` to `l.set(i, v)`, `l[i] += v` to
  `l.set(i, l.get(i) + v)`.  List and Dict literals are also implemented.
  See `tests/chk/objects/subscript_basic.chk`,
  `subscript_compound.chk`, `list_literal.chk`, `dict_literal.chk`.

## Adjustments applied during port

- REPL prompt format: legacy `[NNNNN]` (5-digit) → ours `[NNNNNNNN]`
  (8-digit), zero-indexed.
- `.isA(Float)` on integer literals → either dropped (if Float check
  was the load-bearing assertion) or `.isA(Integer)` per
  language-and-runtime spec §2.4.
- `Date.epoch` arithmetic → `Date.fromSeconds(N)` constructor
  (legacy fixture's Date arithmetic seam doesn't ship at v1.0).
