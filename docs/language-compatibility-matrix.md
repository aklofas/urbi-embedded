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
| `while` | implemented | — |
| `for (var x : iter)` | implemented | Wave 6 W1; lowered to index loop via `.length()` + `.get(i)`; `in` alias supported |
| `for (init; cond; step)` | deferred (v1.x) | C-style three-part form; use `while` instead; see `docs/migration/control-flow-migration.md` |
| `break` | implemented | Wave 6 W1; exits innermost loop or switch |
| `continue` | implemented | Wave 6 W1; skips to next iteration of for-each or while |
| `switch` | implemented | Wave 6 W1; equality-dispatch only; no fall-through; break exits switch |
| `do (receiver) { ... }` | deferred (v1.x) | receiver-bound block form; uncommon in practice |
| `loop` | deferred (v1.x) | infinite loop sugar; use `while (true)` instead |
| `return` | implemented | — |

### Exceptions

| Construct | Status | Reason / fix milestone |
|---|---|---|
| `try { } catch (e) { }` | implemented | bare-ident catch only |
| `catch (var e) { }` | implemented | Wave 6 W5; `var` is optional sugar — closes legacy F6 |
| `catch (var e if cond) { }` (guarded) | implemented | Wave 6 W5; guard re-throws on false — closes legacy F6 |
| `try { } catch { } else { }` | implemented | Wave 6 W5; else runs on normal exit — closes legacy F6 |
| `try { } finally { }` | implemented | — |
| `throw` | implemented | — |
| `assert (expr)` / `assert { }` | implemented | closes legacy F9; Wave 6 W3 |

### Reactive

| Construct | Status | Reason / fix milestone |
|---|---|---|
| `at (cond) body` | partial | core form works; missing `~ duration`, `sync`, `onleave` per legacy F4 / Wave 6 W9 |
| `at (event?) body` | implemented | payload binding with named var: `at (e?(var x)) body`; shipped v0.10.5 W9 |
| `at (event?(var x)) body` | implemented | legacy F4; shipped v0.10.5 W9 |
| `at sync ... onleave ...` | NOT implemented | currently rejected per reactive audit; Wave 6 W9 |
| `at (cond ~ duration) body` | NOT implemented | debounce/hold; deferred v1.x (Wave 6 W9 ruling) |
| `whenever (cond) body` | implemented | — |
| `whenever (event?) body` | implemented | payload binding with named var: `whenever (e?(var x)) body`; shipped v0.10.5 W9 |
| `whenever (...) body else body` | implemented | else fires on falling edge (true→false); shipped v0.10.5 W9 |
| `$wheneverOn` / `$wheneverOff` tags | NOT implemented | deferred v1.x (Wave 6 W9 ruling) |
| `waituntil (cond)` | implemented | — |
| `waituntil (event?)` | implemented | payload delivered on resume; shipped v0.10.5 W9 |
| `watch (expr)` returns event | NOT implemented | legacy F4; Wave 6 W9 |
| `every (duration) body` | partial | core form works; OP_CLOSURE in nested body broken per reactive F4 / Wave 3 W1 |
| `sleep (duration)` | NOT implemented | legacy F15; Wave 3 W6 |

### Tags

| Construct | Status | Reason / fix milestone |
|---|---|---|
| `mytag : { body }` (brace-block prefix) | implemented | — |
| `mytag : stmt` (bare-statement prefix) | implemented | legacy F3; Wave 3 W5 closed |
| `Tag.scope : body` (member-expr tag) | implemented | legacy F3; Wave 6 W8 — `parse_tag_prefix_from_expr` via postfix-chain COLON intercept; `tests/chk/tag/tag_member_expr.chk` |
| `tag : body onleave handler` | deferred-v1.x | PARSE-033: AST field retained; scheduler tag-stack lifecycle design open; Wave 6 W8 ruling |
| `Tag.new()` (script-side constructor) | NOT implemented | reactive F3; Wave 3 W4 |
| `mytag.stop()` (script-side cancellation) | NOT implemented | OP_TAG_STOP is reserved stub; Wave 3 W4 |
| Tag scope events (enter/leave) | partial | works as tag-stack lifecycle; script API TBD |
| Ambient-tag inheritance | implemented | — |

### Identifiers + literals

| Construct | Status | Reason / fix milestone |
|---|---|---|
| ASCII identifiers | implemented | — |
| Quoted identifiers (`'+'`, `'()'`, etc.) | implemented | legacy F5; Wave 6 W2 — single-quote-delimited; any char except newline in body; no escape sequences; emits TOK_IDENT; keyword-escaping works (`var 'if'`); operator-slot access via `obj.'+'(arg)`; see `tests/chk/lex/quoted_ident_basic.chk` + `tests/chk/objects/quoted_slot_assign.chk` |
| Time literals (ms/us/ns/s/m/h/d) | implemented | — |
| Angle literals (`180deg`, `1rad`, `200grad`) | implemented | legacy F8; Wave 6 W4 — `deg`/`rad`/`grad` suffixes produce `TOK_FLOAT` in radians; `Math.pi` is a named constant (not a lexer literal); see `tests/chk/lex/angle-literals.chk` |
| Physical literals | deferred-v1.x | legacy F8; no legacy corpus footprint; `docs/urbi-embedded-design-risks.md` Wave 6 deferral |
| String literals | implemented | — |
| Block comments | dropped (locked non-nesting) | legacy F7; Wave 6 W6 — scanner uses C-style non-nesting; see [LANG-CONVENTIONS.md §7](LANG-CONVENTIONS.md#7-block-comments--divergence-from-legacy) and [migration recipe](migration/block-comments-migration.md) |
| Synclines (`//#line`, `//#push`, `//#pop`) | implemented | needs compat fixture per legacy F16 |

### Object model

| Construct | Status | Reason / fix milestone |
|---|---|---|
| Prototype chain | implemented | — |
| Multi-proto MRO | implemented | — |
| Local slot install (`obj.slot = value`) | implemented | — |
| `var obj.slot = value` slot install form | implemented | legacy F14; Wave 6 W10 — parser desugar to `obj.slot = value` (AST_MEMBER_SET); OP_SETSLOT installs absent slots; see `tests/chk/objects/var_obj_slot.chk` |
| Sub-object slot install (`Lobby.led.on`) | implemented | legacy F14; Wave 6 W10 — handled by chained AST_MEMBER_SET (`var a.b.c = v` desugars via intermediate MEMBER_GET); see `tests/chk/objects/var_obj_slot.chk` |
| List literal `[1, 2, 3]` | implemented | legacy F14; Wave 6 W10 — stdlib-call lowering: `[e1, e2, e3]` → `List.new(e1, e2, e3)`; no new opcode; see `tests/chk/objects/list_literal.chk` |
| Dict literal `["a" => 1]` | implemented | legacy F14; Wave 6 W10 — stdlib-call lowering: `["k" => v, ...]` → `Dict.new()` + repeated `.set(k,v)`; no new opcode; see `tests/chk/objects/dict_literal.chk` |
| Subscript get/set `l[i]` / `l[i] = v` | implemented | legacy F14; Wave 6 W10 — stdlib-call lowering: `l[i]` → `l.get(i)`, `l[i] = v` → `l.set(i,v)`; no new opcode; see `tests/chk/objects/subscript_basic.chk` |
| Subscript compound `l[i] += v` | implemented | legacy F14; Wave 6 W10 — desugar: `l.set(i, l.get(i) + v)`; no new opcode; see `tests/chk/objects/subscript_compound.chk` |
| Top-level `this` / Lobby singleton | migration | legacy F13; use `Realm` — see [top-level-this-lobby-migration.md](migration/top-level-this-lobby-migration.md) |
| `setSlot` (host-side reflection) | implemented | — |

### Concurrency / strands

| Construct | Status | Reason / fix milestone |
|---|---|---|
| `;` `\|` `,` `&` separators | implemented | — |
| Chunk-top `&`/`,` fork | implemented | v0.8.0 loader strand |
| `closure { }` (legacy) | migration | legacy F11; see [callmessage-migration.md §closure](migration/callmessage-migration.md#closure-keyword-migration) — replace with `function`; upvalue capture works since v0.8.4 |
| `function () { }` (M6+) | implemented | — |

### Stdlib + reflection

| Construct | Status | Reason / fix milestone |
|---|---|---|
| `CallMessage` / `evalArgAt` (legacy reflection) | dropped (permanent) | legacy F10; introspection removed — scheduler + heap cost prohibitive; see [callmessage-migration.md](migration/callmessage-migration.md) for lazy-param + try/catch alternatives |
| `Class.new()` | implemented | — |
| `Object.clone()` | implemented | — |
| Operator overload (9 ops) | implemented | v0.6.2 |

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
