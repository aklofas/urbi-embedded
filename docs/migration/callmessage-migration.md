# Migration: CallMessage / evalArgAt / call.message reflection

**Status:** permanently dropped — see
[language-compatibility-matrix.md §Stdlib + reflection](../language-compatibility-matrix.md#stdlib--reflection).

**Audit findings closed:** legacy F10 (migration impact under-owned),
legacy F11 (closure migration guardrails partly defined).

---

## Why CallMessage is permanently dropped

Legacy urbiscript exposed per-call-frame introspection through a special
`call` object available inside every function body:

```urbiscript
// legacy — share/urbi/lazy.u (Gostai 2008-2012)
var f = function (var x) {
    if (call.message.size > 1) {
        var arg = call.evalArgAt(1);   // fetch caller's unevaluated AST
        // ...
    }
};
```

This required the VM to retain caller-frame metadata (the `UMessage`
struct, argument AST nodes, source location) across the full call
lifetime.  In urbi-embedded that metadata is not retained:

- The scheduler uses a cooperative single-VM-per-thread model with a
  fixed per-strand register file.  Keeping per-call frame metadata would
  require a heap-allocated frame chain that conflicts with the embedded
  heap budget targets (ARM Cortex-M7: 240 KB total; see
  `docs/internals/gc.md` for the allocation budget).
- Argument AST nodes are freed after the emitter closes a function body.
  Retaining them post-emit would require a persistent AST arena, which
  doubles the heap pressure during compilation.
- The `evalArgAt(N)` primitive forces re-entry into the emitter from
  inside a running VM dispatch loop.  This scheduler-level re-entrancy
  is incompatible with the cooperative strand model.

Re-implementation of `CallMessage` is permanently out of scope for
urbi-embedded v1.0 and v1.x.  A narrowed `CallContext` debug-only
reflection API (e.g. exposing only `call.arity` and `call.name`) is
**not recommended** for v1.0 — it would impose non-trivial frame-header
overhead on every call site.  Defer to post-v1.0 if the community
demands it.

---

## Migration patterns

### Pattern A: lazy argument evaluation

**Legacy idiom** — a function detects whether an argument was passed as a
thunk (unevaluated expression) by inspecting `call.message`:

```urbiscript
// legacy
var maybeDefer = function (var x) {
    if (call.message.args.size > 0 && call.message.args[0].isThunk) {
        var result = call.evalArgAt(0);
        // result was computed lazily
    }
};
```

**urbi-embedded replacement** — declare parameters with the `lazy`
keyword, which has been supported by the parser since M3 (v0.3.0):

```urbiscript
// urbi-embedded v1.0
var maybeDefer = function (lazy x) {
    // x is a zero-argument function wrapping the caller's expression.
    // Call x() to force evaluation:
    var result = x();
    // ...
};

// Callsite: passes expression unevaluated
maybeDefer(expensiveComputation());
```

`lazy` parameters are declared with the `lazy` keyword immediately before
the parameter name.  The caller's expression is wrapped in an implicit
thunk; the body calls `x()` (no arguments) to force it.  Multiple lazy
parameters are listed independently: `function (lazy a, lazy b)`.

**Cost:** every callsite that previously relied on implicit per-call
introspection must now use explicit `lazy` parameters.  The migration is
mechanical for functions where laziness is documented; it is structural
for functions that conditionally defer based on runtime arity.

**Key differences from legacy:**

| Legacy `call.evalArgAt(N)` | urbi-embedded `lazy param` |
|---|---|
| Any argument can be lazy on demand | Only declared-lazy params are thunks |
| Caller unaware; no source change needed | Caller expression is wrapped transparently |
| Arity check possible via `call.message.size` | Arity must be validated explicitly or overloaded |
| Multiple arities share one function body | Overloaded arities require separate functions |

---

### Pattern B: fallback diagnostics

**Legacy idiom** — the `fallback` protocol dispatched unknown slot accesses
through a user-defined `fallback` function that could inspect `call.message`
to report missing slots:

```urbiscript
// legacy
function fallback {
    echo("Missing slot: " + call.message.name);
    throw Exception.new("no slot " + call.message.name)
};
```

**urbi-embedded status:** no fallback protocol and no `call.message` access.
This use case is an **accepted loss** for v1.0.

**Partial workaround** — catch the `TypeError` that the VM raises on unknown
slot access:

```urbiscript
// urbi-embedded v1.0 — try/catch replaces runtime-dispatch fallback
try {
    obj.unknownSlot()
} catch (e) {
    // e is a TypeError from the VM; message text identifies the slot
    // (format: "object has no slot '...'")
};
```

The `try/catch` form cannot intercept the access before the error is raised
(no pre-dispatch hook), but it covers the common case of conditional
fallback and diagnostic logging.  There is no replacement for frameworks
that use `fallback` as a generic proxy (e.g. dynamic delegation, lazy
initializers keyed on slot name).  Those patterns require explicit
`if`/`switch` dispatch or a factory function.

---

### Pattern C: operator call syntax via `'()'`

**Legacy idiom** — the call operator was overridable by installing a slot
named `'()'` (a quoted identifier) on a proto:

```urbiscript
// legacy — operator-parens.chk
var MyCallable = Object.clone();
MyCallable.'()' = function (var arg) {
    echo("called with: " + arg.asString())
};
MyCallable(42);   // dispatches to '()' slot
```

**urbi-embedded status:** quoted identifiers (`'...'`) are not yet
implemented (legacy F5; Wave 6 W2 decision).  Once W2 lands, the slot
install form `obj.'()' = ...` will be expressible.  The VM dispatch path
for the call operator via a `'()'` slot is a separate question; see W2's
migration doc for the operator-slot recipe.

**Interim workaround** — use `setProperty` (available since v0.6.1) to
register a named property, then dispatch via an explicit method:

```urbiscript
// urbi-embedded interim — setProperty + explicit dispatch
var MyCallable = Object.clone();
setProperty(MyCallable, "call_impl",
    function (arg) { /* ... */ });
// Caller must use: MyCallable.call_impl(42)
// No transparent MyCallable(42) syntax until W2 quoted-ident lands
```

This is not a transparent replacement; it changes the callsite.  Files
that depend on transparent `()` operator dispatch are listed under
"accepted losses" below.

---

## Accepted losses: third-party corpus files blocked on CallMessage

**Canonical file count: 23 files** contain direct uses of
`call.message`, `call.evalArgAt`, `call.argv`, `call.args`, or
`call.name` in the third-party corpus.  Derived by:

```sh
cd legacy/repos/third-party
rg -l --type-add 'urbi:*.u' --type urbi \
   'call\.(message|evalArgAt|argv|args|name)' | wc -l
```

Each of these 23 files requires hand-rewriting of the
CallMessage-dependent logic and is excluded from the v1.0 conformance
numerator.

Reference: `docs/third-party-corpus-compatibility.md` §Cross-Cutting
Feature Incidence, row `CallMessage/evalArgAt`.

### jouve-urbi-2.7.5 (11 files)

| File | CallMessage use | Migration difficulty |
|---|---|---|
| `share/urbi/lazy.u` | Core lazy evaluation via `call.message` + `call.evalArgAt(N)` | High — 200+ LOC; must redesign using `lazy` params |
| `share/urbi/component.u` | UObject slot bindings via `evalArgAt` | High — structural coupling to UObject binding |
| `tests/2.x/*.chk` (~9 files) | Introspection-based validation framework | Medium — replace `call.message.size` checks with explicit arity guards |

### urbi-debian (10 additional files beyond jouve mirror)

| File | CallMessage use | Migration difficulty |
|---|---|---|
| `share/urbi/lazy.u` | Extended lazy semantics (local additions to jouve lazy.u) | High |
| `tests/test.u` | CallMessage-based test validation framework | High — test harness uses implicit AST checks across many tests |
| Local extension files (~8 files) | `call.message.args.size` arity guards | Medium — mechanical rewrite with explicit arity params |

### xcs (2 files)

| File | CallMessage use | Migration difficulty |
|---|---|---|
| 2 files (names unrecorded in audit) | CallMessage introspection | Medium |

### UrIRC (callsite count not audited separately)

| File | CallMessage use | Migration difficulty |
|---|---|---|
| Core plugin files | Unspecified introspection patterns | Unknown — requires re-audit |

**Summary:** 23 files across 3 repos (jouve, urbi-debian, xcs) contain
direct CallMessage introspection uses.  All require manual review.
The `lazy.u` rewrite is the highest-value target because it enables the
`Lazy` stdlib class that other corpus files depend on; prioritize it
when a community port effort begins.

---

## CallContext debug API: not recommended for v1.0

A narrowed `CallContext` object exposing only `call.arity` (argument count
at the call site) and `call.name` (the slot name through which the function
was invoked) has been considered and is **not recommended** for v1.0.
Reasons:

1. Even a minimal frame header (8-16 bytes per active call on the strand
   stack) adds measurable overhead on Cortex-M7 with 240 KB total heap.
2. The only practical use is diagnostic logging — already covered by the
   `try/catch` workaround (Pattern B above).
3. `call.arity` is not sufficient to replace `evalArgAt` for lazy.u-style
   patterns; any community port that needs real laziness must use `lazy`
   parameters instead.

**Decision:** defer `CallContext` to post-v1.0 if community demand is
demonstrated by real porting work.

---

## closure keyword migration

The `closure` keyword was retired in v1.0.  The lexer recognises it and
emits a migration-error diagnostic (`EMIT_CLOSURE_RETIRED`).  Replace with
`function`:

```urbiscript
// legacy
var f = closure (var x) { x + 1 };

// urbi-embedded v1.0
var f = function (var x) { x + 1 };
```

Since v0.8.4 (closure-lifetime GC promotion), `function` closures capture
upvalues from enclosing scopes correctly — the earlier restriction that
blocked upvalue capture is gone.  See the `### Concurrency / strands` row
in the [language-compatibility-matrix](../language-compatibility-matrix.md)
for the formal status.

**Migration is mechanical:** a global `sed` replacement of `closure` to
`function` is safe provided the replacement is applied to the bare keyword
(word-boundary match: `\bclosure\b`).

**`this` binding note:** legacy `closure { ... }` and legacy
`function () { ... }` had different `this` binding semantics in some
edge cases (the original intent was that `closure` captured `this` from
the enclosing scope, while `function` rebound it).  In urbi-embedded v1.0
`function` always captures `this` from the method receiver at call time.
If a legacy closure was capturing `this` from the *definition* site (not
the call site), the behaviour difference is observable.  In practice this
only matters for closures stored in slots and later called from a different
proto — test those call paths explicitly after migration.
