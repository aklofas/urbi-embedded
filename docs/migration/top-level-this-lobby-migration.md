# Migration: Top-level `this` / Lobby singleton

**Status:** migration — use `Realm` instead.  See
[language-compatibility-matrix.md §Object model](../language-compatibility-matrix.md#object-model).

**Audit findings closed:** legacy F13 (top-level `this` / Lobby singleton
model unresolved).

---

## Background

In legacy urbiscript (Gostai / Aldebaran, pre-2014) the identifier `this` at
the top level of a script or REPL line resolved to the **Lobby** singleton — a
special global namespace object that held user-defined bindings made at the
top level:

```urbiscript
// legacy — top-level in .u files and at the REPL
this.x = 42;     // installs slot x on the Lobby
this.type;       // => "Lobby"
this == this;    // => true
```

The Lobby was also the implicit receiver for bare method calls at the top level:
calling `echo("hi")` without a receiver was equivalent to
`this.echo("hi")` (or `Lobby.echo("hi")`).

In urbi-embedded the Lobby model is **not retained**.  The runtime does not
allocate a Lobby singleton object, and top-level `this` is a compile-time error
(`EMIT_NO_THIS_OUTSIDE_METHOD`).  The rationale: embedded targets have tight
heap budgets; a per-realm singleton object that is semantically identical to the
already-existing `Realm` global object would be a redundant allocation with no
behavioural distinction from the code's perspective.

---

## Replacement: `Realm`

`Realm` is the urbi-embedded equivalent of the legacy Lobby.  It is a
per-realm singleton object (`URealm.global_object`) that holds user-defined
top-level bindings.  It is available as a read-only const global in every
realm from v0.9.0 onwards.

| Legacy idiom | urbi-embedded replacement |
|---|---|
| `this.x = 42` | `Realm.x = 42` |
| `this.x` | `Realm.x` |
| `this == this` | `Realm == Realm` |
| `this.type` | no direct replacement (Realm has no `type` slot; `"Lobby"` string not modelled) |
| `Lobby.echo("hi")` | `Lobby.echo("hi")` (Lobby is still a named global; see below) |
| `Lobby.wall("msg")` | `Lobby.wall("msg")` (unchanged) |

`Realm` is already the idiomatic pattern in urbi-embedded — all reactive
fixtures that need a shared mutable cell at the top level use `Realm.x`:

```urbiscript
// urbi-embedded idiom (since v0.9.0)
Realm.counter = 0;
at (Realm.counter > 3) Realm.abort_flag = true;
```

---

## Migration recipe

### Step 1 — global slot installs at top level

Replace every `this.<slot> = <value>` at the top level with
`Realm.<slot> = <value>`:

```urbiscript
// before (legacy)
this.x = 42;
this.y = "hello";

// after (urbi-embedded)
Realm.x = 42;
Realm.y = "hello";
```

### Step 2 — global slot reads at top level

Replace `this.<slot>` reads with `Realm.<slot>`:

```urbiscript
// before
echo(this.x);

// after
echo(Realm.x);
```

### Step 3 — identity / type checks on the global receiver

The legacy `this == this` tautology and `this.type` checks are uncommon
outside test fixtures.  Replace as follows:

```urbiscript
// before
this == this;    // => true
this.type;       // => "Lobby"

// after — identity works
Realm == Realm;  // => true

// there is no Realm.type slot; the "Lobby" type string is not modelled.
// Scripts that branch on this.type == "Lobby" should be restructured
// to not depend on the type string.  If a type tag is needed, install
// a custom slot: Realm.kind = "global".
```

### Step 4 — `Lobby` named global

`Lobby` is separately available as a named realm global (bound at
`urbi_populate_realm_globals` via `lobby_native.c`, since v0.9.1).  Code that
explicitly names `Lobby.echo` or `Lobby.wall` does not need to change; only
code that used `this` to reach the Lobby indirectly is affected.

---

## `this` inside method bodies

`this` **is still valid inside method bodies** — it resolves to the receiver
of the enclosing method call via `OP_LOAD_RECV`.  Only top-level `this` (where
there is no enclosing method frame) is replaced:

```urbiscript
// urbi-embedded — valid use of `this` inside a method body
var obj = Object.clone();
obj.greet = function () { "Hello from " + this.name };
obj.name = "world";
obj.greet();  // => "Hello from world"
```

Top-level `this` outside any method raises a compile-time error:

```urbiscript
// urbi-embedded — compile-time error
this.x = 1;
// error: EMIT_NO_THIS_OUTSIDE_METHOD
```

---

## Scope of affected legacy fixtures

The following legacy fixtures are fully deferred due to top-level `this` /
Lobby dependency:

- `tests/2.x/this.chk` — all 3 lines use top-level `this` as Lobby alias
  (`this == this`, `this == { this }`, `this.type`).  Ported subset would be
  `Realm == Realm` (true), `Realm.type` (returns `"Object"` not `"Lobby"`).
  Activation deferred because the type-string divergence makes a direct port
  misleading; a migration-aware fixture is provided at
  `tests/chk/objects/realm_as_lobby_at_top_level.chk`.

- `tests/2.x/lobby.chk` — depends on Lobby as a class with `addProto`,
  `protos`, `handleConnect` / `handleDisconnect` hooks in a session model.
  The `Lobby` named global exists in urbi-embedded v0.9.1+ but the full
  class-method set is not ported; full activation is a v1.x item.
