# urbi-embedded language compatibility matrix

> Per-fixture and per-construct mapping of legacy urbiscript surface to
> urbi-embedded's current support level. Drives the v1.0 conformance
> denominator. Wave 6 of the v0.10.x architectural refactor arc fills
> every TBD row; v0.10.7 W7 retires all `defer-to:` labels and publishes
> the v1.0 conformance denominator.

## Status legend

- **implemented** — parser + emitter + runtime + tests all exist; legacy source parses and behaves correctly.
- **partial** — subset implemented; documented missing forms.
- **migration** — legacy source does not parse, but a documented transform exists in `docs/migration/`.
- **deferred-v1.x** — out of v1.0 scope; planned post-v1.
- **dropped** — permanently unsupported in this implementation.
- **TBD** — decision not yet made; Wave 6 closes.

## v1.0 conformance denominator

(updated by v0.10.7 W7; `defer-to:` labels retired and taxonomy normalized)

- Total legacy `tests/2.x/*.chk` fixtures: 230.
- Currently ported to `urbi-embedded/tests/chk/` (non-repl): 269.
- REPL-gated fixtures (under `tests/chk/repl/`): 15.
- Total across all: 284.
- Reduced-port (assertions dropped per PORT_NOTES.md): 9 files (dict, list, mutex, date, system, large-string, maths-errors, class, operators legacy ports all carry dropped assertions).
- Fixture taxonomy (non-repl, 269 total):
  - active (run and pass): 269 (all run; 5 newly activated in W7 with real content)
  - deferred-v1.x (empty body, vacuous pass): 22
  - dropped (empty body, vacuous pass): 3
  - blocked (empty body, vacuous pass): 76 (open v1.0-rc work items)
  - real tests with non-trivial content: 163 (269 - 106)
- v1.0 denominator (fixtures with real content that test v1.0-claimed features): 163.
- v1.0 numerator (passing fixtures with real content): 163 (100%).
- v1.0 conformance percentage: 100% on implemented surface (163/163).
- Note: 106 fixtures remain as placeholders (blocked/deferred/dropped) with empty
  bodies. These pass vacuously and do not inflate the conformance percentage.
  The v1.0 release claim is: "all 163 active fixtures with real test content pass."

## Per-construct matrix

### Control flow

| Construct | Status | Reason / fix milestone |
|---|---|---|
| `if / else` | implemented | — |
| Unbraced single-statement bodies (if/else/while arms) | implemented | **v0.13.5** — a single statement is accepted without braces for `if`/`else`/`while` arms (`if (this) false else true` — the form the legacy stdlib itself uses, object.u:15); separators after an unbraced arm bind OUTSIDE the arm for all arm kinds (reference grammar stmt-vs-cstmt tiering); `for` keeps required braces; `if`/`while` remain statements, not expressions (`var x = if (c) e` stays a parse error); see `tests/chk/control_transfer/unbraced_bodies.chk`; matrix-row: syntax-unbraced-bodies |
| `while` | implemented | v0.13.1 (refactor-3 FE-01): back-edge encoding fixed — a `while` appearing first in a branch arm (`if (c) { while ... }`) previously miscompiled its back-edge; back-edges now use a dedicated backward-jump encoder |
| `for (var x : iter)` | implemented | Wave 6 W1; lowered to index loop via `.length()` + `.get(i)`; `in` alias supported |
| `for (init; cond; step)` | deferred (v1.x) | C-style three-part form; use `while` instead; see `docs/migration/control-flow-migration.md` |
| `break` | implemented | Wave 6 W1; exits innermost loop or switch |
| `continue` | implemented | Wave 6 W1; skips to next iteration of for-each or while |
| `switch` | implemented | Wave 6 W1; equality-dispatch only; no fall-through; break exits switch. v0.13.1: case bodies get real scopes and the subject is evaluated once into a hidden local (FE-02 follow-on); more than 64 cases is a latched compile error (FE-06) |
| `switch ... default:` catch-all arm | implemented | **v0.13.5** — runs when no case matches; dispatch is source-position-independent (a `default` listed first still loses to a matching case); a second `default` arm is a compile error; lowered onto the existing patch-list jump machinery, no new opcode. `switch` (and for-each) inside a `try` body works: the try result register is anchored as a declared hidden local so body-declared locals keep their registers (fixed in v0.13.5; was a pre-existing first-arm-always miscompile), see `tests/chk/control_transfer/try_body_hidden_local_collision.chk`. See `tests/chk/control_transfer/switch_default.chk`; matrix-row: syntax-switch-default |
| `do (receiver) { ... }` | deferred (v1.x) | receiver-bound block form; uncommon in practice |
| `loop` | deferred (v1.x) | infinite loop sugar; use `while (true)` instead |
| `return` | implemented | — |

### Exceptions

| Construct | Status | Reason / fix milestone |
|---|---|---|
| `try { } catch (e) { }` | implemented | bare-ident catch only. v0.13.1 (refactor-3 VM-01): cross-frame unwind fixed — a throw caught in a caller's frame (catch installed in frame N, throw in frame N+k) previously segfaulted; the unwind walker now pops call frames to the entry's stamped depth |
| `catch (var e) { }` | implemented | Wave 6 W5; `var` is optional sugar — closes legacy F6 |
| `catch (var e if cond) { }` (guarded) | implemented | Wave 6 W5; guard re-throws on false — closes legacy F6 |
| `try { } catch { } else { }` | implemented | Wave 6 W5; else runs on normal exit — closes legacy F6. **v0.13.5**: the expression VALUE of `try`-`catch`-`else` is the try-body value — `else` runs for side effects only (`var r = try { 1 } catch (var e) { 9 } else { 2 }` → 1); pinned in `tests/chk/exceptions/try_else.chk` |
| `try { } finally { }` | implemented | v0.13.1: cross-frame unwind fixed (refactor-3 VM-01). **Finally bodies execute atomically** — `;` does not yield inside them (owner decision 2026-06-10, refactor-3 B4); on the normal path a native block (e.g. `sleep`) still parks, while on the unwind path it is a loud fatal (`URBI_ERR_CLEANUP_OVERFLOW`) — asymmetry is deliberate, never silent truncation. Finally now also runs on `return`/`break`/`continue` exits (T24, §S5a) — previously silently skipped. Deep static nesting cap: more than 16 simultaneously open unwind scopes per function (catch+finally counts 2, so 9-deep try/catch/finally) is a compile error as of v0.13.1 |
| `throw` | implemented | — |
| `assert (expr)` / `assert { }` | implemented | closes legacy F9; Wave 6 W3 |

### Reactive

| Construct | Status | Reason / fix milestone |
|---|---|---|
| `at (cond) body` | partial | core form works; missing `~ duration`, `sync`, `onleave` per legacy F4 / Wave 6 W9 |
| `at (event?) body` | implemented | payload binding with named var: `at (e?(var x)) body`; shipped v0.10.5 W9 |
| `at (event?(var x)) body` | implemented | legacy F4; shipped v0.10.5 W9 |
| `at sync (cond) body` | implemented | M5 / §S-watcher-3 — `at sync` keyword form is canonical (shipped M5); the `at.sync` dot-syntax variant was a fixture-authoring error from M5 era and never existed in urbiscript.  v0.10.12 W2 normalized 4 fixture headers/bodies (Cat. E re-audit Cluster #15 verdict A) |
| `at sync ... onleave ...` | partial | `at sync` keyword form ships; `onleave` clause deferred-v1.x (PARSE-033, Wave 6 W8 / W9) |
| `at (cond ~ duration) body` | NOT implemented | debounce/hold; deferred v1.x (Wave 6 W9 ruling) |
| `whenever (cond) body` | implemented | — |
| `whenever (event?) body` | implemented | payload binding with named var: `whenever (e?(var x)) body`; shipped v0.10.5 W9 |
| `whenever (...) body else body` | implemented | else fires on falling edge (true→false); shipped v0.10.5 W9 |
| `$wheneverOn` / `$wheneverOff` tags | NOT implemented | deferred v1.x (Wave 6 W9 ruling) |
| `waituntil (cond)` | implemented | — |
| `waituntil (event?)` | implemented | payload delivered on resume; shipped v0.10.5 W9 |
| `watch (expr)` returns event | NOT implemented | legacy F4; Wave 6 W9 |
| `every (duration) body` | partial | core form works; OP_CLOSURE in nested body broken per reactive F4 / Wave 3 W1 |
| `sleep (duration)` | implemented | v0.10.2 W6; legacy F15 closed; `tests/chk/temporal/sleep_basic.chk` + `sleep_in_strand.chk` |

### Tags

| Construct | Status | Reason / fix milestone |
|---|---|---|
| `mytag : { body }` (brace-block prefix) | implemented | v0.13.1 (FE-02 follow-on): the tag value is evaluated once into a hidden local and tag scopes get real blocks |
| `mytag : stmt` (bare-statement prefix) | implemented | legacy F3; Wave 3 W5 closed; v0.13.1 hidden-local note applies (see brace-block row) |
| `Tag.scope : body` (member-expr tag) | implemented | legacy F3; Wave 6 W8 — `parse_tag_prefix_from_expr` via postfix-chain COLON intercept; `tests/chk/tag/tag_member_expr.chk` |
| `tag : body onleave handler` | deferred-v1.x | PARSE-033: AST field retained; scheduler tag-stack lifecycle design open; Wave 6 W8 ruling |
| `Tag.new()` (script-side constructor) | implemented | v0.10.2 W4; UVAL_TAG + Tag.new(name) returns a Tag value; `tests/chk/control_transfer/tag_stop_basic.chk` |
| `mytag.stop()` (script-side cancellation) | implemented | v0.10.2 W4; native method on Tag proto. v0.10.15 (v0.10.9-B): `t.stop()` from inside `t: { }` is now a clean in-scope tag-stop (binding wired) instead of a D3 "no active scope" fatal; `tests/chk/control_transfer/tag_stop_skips_catch.chk` + `tag_stop_basic.chk` |
| `t: { }` user-tag scope binding | implemented (v0.10.15, v0.10.9-B) — OP_PUSH_TAG honors the `R[tag_reg]` nibble: the scope binds to the user tag (strand becomes a member); FLAG_TAG_USER_OWNED keeps the tag alive past scope exit; `tests/chk/tag/scope_binds_user_tag.chk` | tag-scope-binding |
| `t: at (cond) body` watcher lifetime | implemented (v0.13.5, closes design-risks v0.13.4-A) — a watcher installed under a user-owned tag persists past the lexical scope close and stays armed until `t.stop()` (legacy at-control.chk semantics); anonymous scope-tag watchers still cascade at scope exit; `t.stop()` on a watcher-only tag no longer false-alarms the outside-scope fatal; `tests/chk/tag/tagged_watcher_persists.chk` | tagged-watcher-persists |
| `tag.stop()` inside `try`/`finally` runs finally | implemented (v0.10.15, v0.10.7-B) — the unwind walker runs the finally during the TAG_STOP unwind (latent-fixed by the v0.10.9-B binding); `tests/chk/control_transfer/tag_stop_with_finally.chk` | tag-stop |
| `tag.block()` | implemented (v0.10.9 W3b) — sets UTAG_FLAG_BLOCKED + suspends member strands via urbi_strand_suspend(REASON_BLOCK) | tag-block |
| `tag.unblock()` | implemented (v0.10.9 W3b) | tag-block |
| `tag.block(value)` valued-block | partial (v0.10.9): C API urbi_tag_block accepts resume_value; script-side return-on-resume defers v1.x | tag-block-valued |
| `tag.blocked` getter | implemented as 0-arg method (v0.10.9 W3d); property-style OPROPS dispatch v1.x | tag-blocked |
| `tag.freeze()` real SUSPENDED | implemented (v0.10.9 W3c) — replaces flag-only stub from v0.10.2 W4 | tag-freeze |
| `tag.unfreeze()` | implemented (v0.10.9 W3c) | tag-freeze |
| `tag.frozen` getter | implemented as 0-arg method (v0.10.9 W3d) | tag-frozen |
| `tag.stop(value)` valued-stop | partial (v0.10.9 W1): C API accepts value; script-side observable result defers v1.x. User-tag binding resolved v0.10.15 (v0.10.9-B); the remaining gap is tag-stop absorption / resume-after-scope (design-risks v0.10.15-B) | tag-stop-valued |
| `tag.stop()` outside-scope fatal | implemented (v0.10.9 W2) — `!!! tag.stop with no active scope` | tag-stop-outside-scope |
| `Tag.begin` / `Tag.end` clone-getter notation | deferred — 5 prereq primitives needed (design-risks v0.10.9-A) | tag-begin-end |
| `Tag.enter?` / `Tag.leave?` script-side events | partial — C-level shipped v0.10.2 W4; script-side `at(t.enter?)` rejects (HOST_FN-via-closure binding returns closure, not UVAL_EVENT) | tag-events |
| Tag scope events (enter/leave) | partial | works as tag-stack lifecycle; script API TBD |
| Ambient-tag inheritance | implemented | — |
| `scopeTag()` realm-global (call-style) | implemented (v0.10.10 W4) — D7-D ratify; returns innermost TAG_SCOPE.owning_tag on the current strand's cleanup stack; call-style not getter (native-closure getter wrap defers v1.x; see workspace-root design-risks v0.10.10-A); see `tests/chk/tag/scope_tag_basic.chk` | tag-scopetag |
| `Lobby.connectionTag` slot | implemented (v0.10.10 W4) — D7-E ratify; per-REPL session for REPL contexts; per-realm fallback (`realm->tag`) for embedded host contexts; honors §14.9 S11 commitment; see `tests/chk/tag/connection_tag_basic.chk` | tag-connectiontag |

### Identifiers + literals

| Construct | Status | Reason / fix milestone |
|---|---|---|
| ASCII identifiers | implemented | — |
| Quoted identifiers (`'+'`, `'()'`, etc.) | implemented | legacy F5; Wave 6 W2 — single-quote-delimited; any char except newline in body; no escape sequences; emits TOK_IDENT; keyword-escaping works (`var 'if'`); operator-slot access via `obj.'+'(arg)`; see `tests/chk/lex/quoted_ident_basic.chk` + `tests/chk/objects/quoted_slot_assign.chk` |
| Time literals (ms/us/ns/s/m/h/d) | implemented | — |
| Fractional duration literals (`0.5s`, `1.5ms`) | implemented | v0.13.1 (refactor-3 FE-08) — fractional durations lex on the float path and convert to integer microseconds, round-half-up; previously rejected |
| Angle literals (`180deg`, `1rad`, `200grad`) | implemented | legacy F8; Wave 6 W4 — `deg`/`rad`/`grad` suffixes produce `TOK_FLOAT` in radians; `Math.pi` is a named constant (not a lexer literal); see `tests/chk/lex/angle-literals.chk` |
| Physical literals | deferred-v1.x | legacy F8; no legacy corpus footprint; `docs/urbi-embedded-design-risks.md` Wave 6 deferral |
| String literals | implemented | incl. adjacent-literal concatenation (`"a" "b"` → `"ab"`), proven by `tests/chk/lex/adjacent_string_concat.chk` (compat2-E verified, v1.0) |
| Block comments | dropped (locked non-nesting) | legacy F7; Wave 6 W6 — scanner uses C-style non-nesting; see [LANG-CONVENTIONS.md §7](LANG-CONVENTIONS.md#7-block-comments--divergence-from-legacy) and [migration recipe](migration/block-comments-migration.md) |
| Synclines (`//#line`, `//#push`, `//#pop`) | implemented | proven by `tests/unit/test_lexer_syncline.c` (transient lexer line-tracking state; not a `.chk`-value-observable feature). compat2-E verified, v1.0 |

### Object model

| Construct | Status | Reason / fix milestone |
|---|---|---|
| Prototype chain | implemented | — |
| Multi-proto MRO | implemented | — |
| Local slot install (`obj.slot = value`) | implemented | — |
| `var x;` (declaration without initializer) | implemented | **v0.13.5** — initializes to `nil`; the legacy reference initializes to `void`, but this runtime deliberately has no void model (`nil` is the single absence value; recorded design decision); see `tests/chk/closure/var_uninitialized.chk`; matrix-row: syntax-var-uninit |
| `var obj.slot = value` slot install form | implemented | legacy F14; Wave 6 W10 — parser desugar to `obj.slot = value` (AST_MEMBER_SET); OP_SETSLOT installs absent slots; see `tests/chk/objects/var_obj_slot.chk` |
| block-scoped `var` at chunk top | implemented | **v0.13.5** — a `var` declared inside a block at chunk top is scoped to that block (frame-local) and does not clobber an outer chunk-top binding of the same name (SDK 2.0 ch. 17); bare chunk-top vars keep the realm-slot install and REPL persistence; plain assignment inside a block still writes through to the outer binding; closures capture block-locals; see `tests/chk/closure/block_var_scope.chk`; matrix-row: block-scoped-var |
| Sub-object slot install (`Lobby.led.on`) | implemented | legacy F14; Wave 6 W10 — handled by chained AST_MEMBER_SET (`var a.b.c = v` desugars via intermediate MEMBER_GET); see `tests/chk/objects/var_obj_slot.chk` |
| List literal `[1, 2, 3]` | implemented | legacy F14; Wave 6 W10 — stdlib-call lowering: `[e1, e2, e3]` → `List.new(e1, e2, e3)`; no new opcode; see `tests/chk/objects/list_literal.chk` |
| Dict literal `["a" => 1]` | implemented | legacy F14; Wave 6 W10 — stdlib-call lowering: `["k" => v, ...]` → `Dict.new()` + repeated `.set(k,v)`; no new opcode; see `tests/chk/objects/dict_literal.chk` |
| Subscript get/set `l[i]` / `l[i] = v` | implemented | legacy F14; Wave 6 W10 — stdlib-call lowering: `l[i]` → `l.get(i)`, `l[i] = v` → `l.set(i,v)`; no new opcode; see `tests/chk/objects/subscript_basic.chk` |
| Subscript compound `l[i] += v` | implemented | legacy F14; Wave 6 W10 — desugar: `l.set(i, l.get(i) + v)`; no new opcode; see `tests/chk/objects/subscript_compound.chk` |
| Top-level `this` / Lobby singleton | migration | legacy F13; use `Realm` — see [top-level-this-lobby-migration.md](migration/top-level-this-lobby-migration.md) |
| `setSlot` (host-side reflection) | implemented | — |
| `Object` proto mutable (slot install on Object root) | implemented | v0.10.11 (D5 ratify) — `URBI_ATOM_OBJECT` dropped from readonly cohort; `var Object.x = ...` and `Object.x = ...` work; Lobby stays frozen; see design-risks v0.10.7-F (closed); matrix-row: object-proto-mutable |

### Concurrency / strands

| Construct | Status | Reason / fix milestone |
|---|---|---|
| `;` `\|` `,` `&` separators | implemented | **v0.13.5**: statement forms as `&`/`\|` operands — blocks and `if`/`while` statement forms fold as operands (`{ a } & { b }`; `\|` operands emit inline, `&` RHS compiles as a fork thunk); `switch` does not enter the fold; `var` declarations are rejected as operands; `while`/`switch` nodes as `&` fork operands fail at runtime (design-risks v0.13.5-C); a `&`-fork thunk can READ chunk-top vars but WRITES fail to compile — use `Realm.*` slots (design-risks v0.13.5-A); `,` inside tag-scope bodies unsupported; see `tests/chk/separator/block_operands.chk` |
| Chunk-top `&`/`,` fork | implemented | v0.8.0 loader strand |
| `closure { }` (legacy) | migration | legacy F11; see [callmessage-migration.md §closure](migration/callmessage-migration.md#closure-keyword-migration) — replace with `function`; upvalue capture works since v0.8.4 |
| `function () { }` (M6+) | implemented | — |
| Default parameter values `function (a, b = expr)` | implemented | **v0.13.5** — trailing arguments may be omitted (count-based: an explicit `nil` counts as provided); defaults evaluate at call time in the callee scope and may reference earlier parameters; below-min/above-max argument counts still raise a catchable error; non-trailing defaults parse but are never reached (legacy-faithful, ugrammar.y `var.opt "identifier" "=" exp`); compiled as an arity prologue in the chunk — wire format unchanged at v1.9; see `tests/chk/function/default_params.chk`; matrix-row: syntax-default-params |
| `detach(expr)` lazy-arg builtin | implemented (v0.10.10 W3) — D7-C ratify; spawns the expression as a new strand inheriting parent's ambient tag chain; 1-line overlay wrapper + C-native; see `tests/chk/separator/detach_basic.chk` | separator-detach |
| `disown(expr)` lazy-arg builtin | implemented (v0.10.10 W3) — D7-C ratify; spawns the expression keeping only the connection tag (`realm->tag`); 1-line overlay wrapper + C-native; see `tests/chk/separator/disown_basic.chk` | separator-detach |
| `Job` proto (`Job.current()`/`jobs()`/`tags()`/`uid()`/`status()`) | implemented (v0.10.10 W1+W2) — D7-A + D7-B ratify; call-style methods (not auto-invoked getters — wrap-native-closures-as-getters bridge defers v1.x; see workspace-root design-risks v0.10.10-A); Job.jobs enumerates live strands across all realms (DEAD excluded); see `tests/chk/stdlib/runtime/job_{current,jobs}_basic.chk` | scheduler-jobs |

### Stdlib + reflection

| Construct | Status | Reason / fix milestone |
|---|---|---|
| `CallMessage` / `evalArgAt` (legacy reflection) | dropped (permanent) | legacy F10; introspection removed — scheduler + heap cost prohibitive; see [callmessage-migration.md](migration/callmessage-migration.md) for lazy-param + try/catch alternatives |
| `Class.new()` | implemented | — |
| `Object.clone()` | implemented | — |
| Operator overload (9 ops) | implemented | v0.6.2 |
| `Channel` proto (`new(n)` / `echo(msg)` / `'<<'(x)`) | implemented | v0.10.11 (D6 ratify) — Channel proto + enabled/quote/name slots; per-realm cout/cerr/clog realm globals; honors §14.9 Y3; Channel.Filter deferred-v1.x; see `tests/chk/stdlib/runtime/channel_basic.chk`; matrix-row: stdlib-channel |
| `cout << msg` / `cerr << msg` / `clog << msg` | implemented | v0.10.11 (D6 ratify) — per-realm cout/cerr/clog as Channel instances; `<<` desugars to `.'<<'(x)` method call; see `tests/chk/stdlib/runtime/cout_shift.chk`; matrix-row: stdlib-cout-shift |
| `isA(Proto)` universal type-test | implemented | v0.10.11 (Cluster #17 ratify) — C-native on Object root; walks transitive proto chain; atom receivers route through per-VM atom-proto registry; 64-depth cycle guard; see `tests/chk/objects/isa_basic.chk`; matrix-row: stdlib-isa |
| `obj.isA(Atom)` for built-in atom protos | implemented | v0.10.11 — atom receivers (UVAL_INT, UVAL_STR, UVAL_FLOAT, etc.) resolve proto via per-VM atom-proto registry and walk from there; see `tests/chk/objects/isa_atoms.chk`; matrix-row: stdlib-isa-atoms |
| `Lobby.echo(msg)` method | implemented | v0.10.11 W4 — body uses `this.__builtin_lobby_send(...)` (explicit `this.` qualifier; closure bare-name resolution gap stays open v1.x; see design-risks v0.10.11-A); see `tests/chk/chunk_lifecycle/repl_session_persistence.chk`; matrix-row: lobby-echo |
| `Integer` bitwise `and`/`or`/`xor`/`inv`/`shl`/`shr`/`ushr` | implemented | v1.0-rc stdlib-completeness — Kotlin-style names (renamed bitand/bitor/bitxor/bitnot → and/or/xor/inv; shl/shr unchanged; ushr added); methods-not-symbolic per §14 S14 (the `&` / `\|` glyphs stay separators); see `tests/chk/stdlib/atoms/bitwise_kotlin.chk`. **v0.13.5**: `shl`/`shr` apply a `& 63` shift-count mask before shifting (matching `ushr`, Kotlin semantics; out-of-range counts no longer return 0); see `tests/chk/stdlib/atoms/integer_shift_mask.chk` |
| `List` `each`/`sort`/`reverse`/`join` | implemented | v1.0-rc stdlib-completeness — `each` overlay loop; `sort` ascending (Int/Float/String, fresh list); `reverse`/`join` native; see `tests/chk/stdlib/containers/list_iter.chk` |
| `List.sort(comparator)` | implemented | **v0.13.5** — the comparator is a strict less-than predicate (`function (a, b) { a < b }`; truthy means `a` sorts before `b`), matching the legacy convention; ordering stability not pinned (legacy sort is unstable); operates on a snapshot of the backing store, so a comparator that mutates the source list cannot corrupt the in-progress sort; comparator throws propagate catchably; a comparator that does not accept 2 arguments raises a catchable TypeError; see `tests/chk/stdlib/containers/list_sort.chk`; matrix-row: stdlib-list-sort |
| Legacy compat aliases (`String.length`, `List.size`/`insertBack`/`'<<'`/`'+'`/`head`, `Dict.size`, `Exception.Lookup`) | implemented | **v0.13.5** — legacy names alias the existing natives/overlays (canonical names still work); `List.'<<'` chains (returns self, the corpus idiom); `List.head` raises on empty (message text differs from legacy); `Exception.Lookup` is identical (`==`) to `LookupError`; `List.tail` NOT shipped — needs a slice primitive, backlogged; see `tests/chk/stdlib/runtime/legacy_aliases.chk`; matrix-row: stdlib-compat-aliases |
| `Object.asString` universal fallback | implemented | **v0.13.5** — C-native on the Object root: every UObject renders (`"<object 0x...>"`); atom types shadow with their own `asString`; nil/bool receivers raise a catchable TypeError ("asString: self must be a UObject") — minor message divergence from legacy, fixture-pinned; see `tests/chk/stdlib/runtime/object_asstring.chk`; matrix-row: stdlib-asstring |
| `Dict` `keys`/`values`/`each` | implemented | v1.0-rc stdlib-completeness — `keys()`/`values()` native (the missing iteration primitive); `each` over keys() (overlay); see `tests/chk/stdlib/containers/dict_iter.chk` |
| `String` `split`/`join`/`format` | implemented | v1.0-rc stdlib-completeness — `split`→List; `join` (self=separator); `format` %s/%d/%f/%% (minimal formatter); see `tests/chk/stdlib/atoms/string_text.chk`. **v0.13.5**: `split("")` splits per byte (empty string → empty list, legacy string.cc semantics); a format-spec/argument count mismatch raises a catchable ArityError in BOTH directions; see `tests/chk/stdlib/atoms/string_format_arity.chk` |
| Object reflection `slotNames`/`localSlotNames`/`hasLocalSlot`/`getProperty`/`properties` | implemented | v1.0-rc stdlib-completeness — shape-lineage walk (local) + proto-chain (slotNames); props_table for properties; see `tests/chk/objects/reflection.chk` |
| `RangeIterable` mixin (`each`/`all`/`any` from `length`/`get`) | implemented | v1.0-rc stdlib-completeness — propagates via `addProto` on user types; Comparable/Orderable operator-derivation deferred-v1.x (VM comparison-operator dispatch + quoted-operator-slot IC; design-risks compat2-F); see `tests/chk/stdlib/overlays/mixins.chk` |
| `Integer.times`/`Integer.upto`; `Float.random()` | implemented | v1.0-rc stdlib-completeness — times/upto overlay loops; random xorshift64 in [0,1); see `tests/chk/stdlib/atoms/numeric_helpers.chk`. **v0.13.5**: `times(f)` passes the iteration index to the closure (`f(i)`) |
| `RegExp` (`new(pat)`/`test`/`match`) | implemented | v1.0-rc stdlib-completeness — compact freestanding backtracking matcher (literals, `.` `*` `+` `?` `^` `$`, `[classes]`); no capture groups at v1.0 (v1.x); new atom-backed type (`vm->regexp_proto`, GC-rooted); see `tests/chk/stdlib/runtime/regexp.chk`. **v0.13.5**: backtracking budget — 10^6 steps / 128 nesting depth per match; exhaustion raises a catchable RangeError instead of spinning (guard is ours-only, no legacy counterpart); matrix-row: stdlib-regexp-budget |
| `List` index-out-of-range (`get`/`set`/`l[i]`) | implemented | **v0.13.5** — raises a catchable `IndexError` (a `LookupError` subclass) instead of a generic `TypeError`; discriminable in a `catch` guard via `isA(IndexError)`; see `tests/chk/exceptions/typed_exceptions.chk`; matrix-row: stdlib-indexerror |
| `String` char-position out-of-range (`charAt`/`asciiAt`) | implemented | **v0.13.5** — raises a catchable `RangeError` instead of a generic `TypeError`; see `tests/chk/exceptions/typed_exceptions.chk`; matrix-row: stdlib-rangeerror |
| `Dict.get(missing)` / `d[missing]` | **documented divergence** | Returns `nil`; does NOT raise (legacy `Dictionary::get` raised `KeyError` "missing key: %s", dictionary.cc:106). v0.13.5 keeps the nil-return contract deliberately — `has(k)` gates presence, `getWithDefault` is the value-with-fallback idiom. Consequence: `KeyError` is declared in the hierarchy (a `LookupError` subclass, script-throwable) but has NO C raise site at v1.0; see `tests/chk/exceptions/typed_exceptions.chk`; matrix-row: stdlib-dict-get-nil |

### Operators

| Construct | Status | Reason / fix milestone |
|---|---|---|
| `Int + Int`, `Float + Float`, atom numeric ops | implemented | atom fast path in OP_ADD/SUB/MUL/DIV |
| `String + String` concatenation | implemented | v0.10.8 (S-string-plus) — atom fast path in OP_ADD; `arith_add` is bypassed for UVAL_STR + UVAL_STR; result is interned via `ustr_intern` |
| `String + Int` / `String + Float` / mixed-type `+` | deferred-v1.x | v1.0 caller must use explicit `.asString()`; coercion taxonomy (which side coerces, `nil + ""`, `[] + ""`) is a deliberate v1.x design pass |
| Operator overload via slot dispatch (9 ops) | implemented | REVIVAL §S30; v0.6.2 |
| `<<` infix operator (left-shift / stream-insert sugar) | implemented | v0.10.11 W3 — new `TOK_LSHIFT`; parser sugar desugars `a << b` to `a.'<<'(b)` method call on quoted-ident slot; precedence 2 (below equality at 3); no new opcode; routes through OP_GETSLOT + OP_CALL; see `tests/chk/operators/lshift_method_call.chk`; matrix-row: operators-lshift |
| `&&` / `\|\|` logical operators (short-circuit) | implemented | v1.0-rc stdlib-completeness — new `AST_LOGICAL` node; emit via existing `OP_TESTSET`+`OP_JMP` (no new opcode, no wire bump); legacy-faithful (ugrammar.y had `&&` / `\|\|` as distinct tokens, separate from the `&` / `\|` separators); precedence `\|\|`=1 / `&&`=2 (below `<<`); see `tests/chk/operators/logical.chk`; matrix-row: operators-logical |
| `%` modulo operator | implemented | v1.0-rc stdlib-completeness — parser desugars `a % b` to `a.'%'(b)` (like `<<`); native `'%'` methods on Integer (i64 mod) and Float (fmod); no new opcode; **v0.13.5: modulo-by-zero raises a catchable `DivByZero` (legacy "modulo by 0"), Integer AND Float** (was `TypeError`); see `tests/chk/operators/modulo.chk`; matrix-row: operators-modulo |
| `String % args` format operator | implemented | **v0.13.5** — `"x=%d" % [1]` formats via `String.format` (rides the `%` desugar to `.'%'(...)`; dispatch is receiver-typed, so numeric `%` stays modulo); a non-list argument is auto-wrapped in a one-element list; conversions `%s`/`%d`/`%f`/`%%`; a spec/argument count mismatch raises a catchable ArityError; see `tests/chk/stdlib/atoms/string_format_arity.chk` + `string_text.chk`; matrix-row: stdlib-string-format |
| Truthiness in conditions / `!` / short-circuit | implemented | **v0.13.5** — one truth source feeds `if`/`while` conditions, `!`, and the short-circuit logical operators: integer `0`, float `0.0`/`-0.0`, `nil`, and `false` are falsy; everything else is truthy (empty string/list/dict kept truthy — no legacy pin says otherwise); matches the legacy reference (`!0` → true, `if (0)` → else branch); see `tests/chk/operators/truthiness.chk`; matrix-row: operators-truthiness |
| `/` division-by-zero | implemented | **v0.13.5** — a zero divisor raises a catchable `DivByZero` for BOTH Integer and Float operands (was IEEE-754 `inf`/`-inf`/`nan`); legacy-conformant — legacy urbi is all-Float and `float.cc` checks `if (!rhs) RAISE("division by 0")`, so `1/0`, `1.0/0.0` and `0.0/0.0` all raise; see `tests/chk/exceptions/typed_exceptions.chk`; matrix-row: operators-divzero |
| `!x` logical not | implemented | v0.13.1 (refactor-3 FE-03) — lowered via `OP_TEST`/`OP_LOADBOOL` (was miscompiled to `OP_NEG`, i.e. arithmetic negation); see `tests/chk/operators/logical_not.chk`. **Divergence note:** prefix `!` binds tighter than postfix — `!o.b` parses as `(!o).b` (same precedence shape as unary `-`); tracked in workspace-root design-risks (v0.13.1-A) |

## Per-fixture status (from tests/chk/stdlib/legacy/)

(populated by Wave 6 W1-W11; row per fixture)

| Fixture | Status | Active assertions | Dropped assertions | Notes |
|---|---|---|---|---|
| `tests/chk/stdlib/legacy/dict_legacy.chk` | partial | subset | dict literal `["a" => 42]`, `extend` | ported from `tests/2.x/dictionary.chk` |
| `tests/chk/stdlib/legacy/list_legacy.chk` | partial | subset | list literal, `each(closure)`, `<<`, `+=` identity | ported from `tests/2.x/list.chk` |
| `tests/chk/stdlib/legacy/mutex_legacy.chk` | partial | subset | closure-upvalue choreography, tag scope | ported from `tests/2.x/mutex/basic.chk` |
| `tests/chk/stdlib/legacy/date_legacy.chk` | partial | subset | `Date.epoch` arithmetic | ported from `tests/2.x/date.chk` |
| `tests/chk/stdlib/legacy/system_legacy.chk` | partial | subset | `Process` subsystem | ported from `tests/2.x/system/platform.chk` |
| `tests/chk/stdlib/legacy/large_string_legacy.chk` | partial | subset | `for` doubling loop section | ported from `tests/2.x/large-string.chk` |
| `tests/chk/stdlib/legacy/maths_errors_legacy.chk` | partial | subset | float-literal-dependent, `Math.sqrt/log/asin/acos` | ported from `tests/2.x/maths-errors.chk` |
| `tests/chk/stdlib/legacy/class_legacy.chk` | partial | subset | `this` / closure upvalue lines skipped | ported from `tests/2.x/class.chk` |
| `tests/chk/stdlib/legacy/operators_legacy.chk` | partial | subset | `bitor`, `in`, `.operator +(1)` syntax deferred | ported from `tests/2.x/operators.chk` |

(... matrix to be filled across Wave 6 ...)

## How to update

When a Wave 6 worktree decides a row's status:

1. Update the row in this matrix.
2. If status becomes `implemented`, link to the closing commit and the activated `.chk` fixture.
3. If status becomes `migration`, link to the migration doc in `docs/migration/`.
4. If status becomes `dropped`, justify in a one-line "Reason" note.
5. Recompute the v1.0 denominator at the top of this file.
6. Update `release-readiness.md`'s language-compatibility table row.
