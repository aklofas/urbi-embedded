# Code Style

This doc captures the style decisions that aren't obvious from the code alone. Mechanical rules are enforced by `.editorconfig`, `.clang-tidy`, and the Makefile — this guide covers the judgments behind those rules and the patterns that follow.

Scope: everything under `src/` and `tests/`. Build system and CI config follow their own conventions; see `README.md` and `CONTRIBUTING.md` for that.

---

## Intent

Four priorities shape every style decision:

1. **Lua-grade embeddability.** Zero external dependencies. Pure C99. `cc src/*.c` builds a static library. No heap allocation; the host plugs in an allocator. No global mutable state; multiple VM instances coexist.
2. **Embedded-first discipline.** The runtime must fit a Cortex-M4 with 16 KB of RAM. Every line is measured against that target. Convenience that can't be stripped on embedded is convenience we don't add.
3. **Path toward safety-critical.** The long-term roadmap includes a hard-real-time subset and the option of formal certification later. Styles that make that path cheaper are preferred now, even at small readability cost today. Full `const`-correctness is the most visible example.
4. **Honest terseness.** Self-documenting names, one-line purpose comments at the top of files, `/* why */` over `/* what */`. Never emojis.

---

## File layout

```text
src/
├── ulex.h          Public lexer header (only public header for its subsystem)
├── ulex.c          Lexer implementation
├── urbi.h          Public top-level API header
├── urbi.c          Top-level entry points (thin; dispatches into subsystems)
├── u<subsys>.c     Subsystem implementations (uparse, uemit, uvm, …)
└── u<subsys>.h     Public header, only when a subsystem exposes more than urbi.h does
```

Guidelines:

- **Flat `src/`.** No nested source directories. A subsystem is one `.c` (possibly with one sibling `.h`), not a folder.
- **Every public header maps to exactly one `.c`.** Implementation detail that ends up in two files is a sign the subsystem is poorly factored.
- **Internal helpers stay `static` in the `.c`.** Don't publish anything that isn't part of the stable contract.
- **Test files mirror the subsystem being tested.** `src/ulex.c` is tested by `tests/unit/test_lexer.c`. One test file per subsystem, one `test_<subsys>_suite` function per file.

---

## Naming

| Kind | Form | Examples |
|---|---|---|
| Public types | `U` + `TitleCase` | `UToken`, `ULexer`, `UTokenType`, `ULexError` |
| Public functions | `u<subsys>_<verb>` | `ulex_init`, `ulex_next`, `urbi_version` |
| Public enum values | `SCREAMING_CASE` with subsystem prefix | `TOK_EOF`, `LEX_UNKNOWN_CHAR` |
| Static helpers | `snake_case` | `scan_radix`, `make_error`, `is_ident_start` |
| Verb prefixes for helpers | | `scan_*` consumes tokens · `make_*` constructs value types · `is_*` returns bool-equivalent · `skip_*` advances without emitting |
| Locals | `snake_case` | `start_col`, `digit_seen`, `looks_digitish` |
| File-scope tables | `SCREAMING_CASE` | `TOKEN_NAMES`, `ERR_MSG` |

Abbreviations are fine when the context is tight (`l` for a `ULexer *` inside lexer code, `t` for a `UToken` inside a constructor). Prefer expansion at public surfaces (`UTokenType` not `TT`).

---

## Memory model

- **No heap allocation inside the library.** The public API exposes a pluggable allocator to the host; the library itself never calls `malloc` / `free` / `calloc` / `realloc` directly.
- **No I/O inside the library.** No `printf`, `fprintf`, `stderr`, `fopen`. The host plugs in output sinks.
- **No POSIX API calls.** No threading primitives, no `time(2)`, no `signal(2)`, no sockets. The host provides time and scheduling hooks if the subsystem needs them.
- **No global mutable state.** State lives on caller-owned structs (`ULexer`, and later `urbi_state_t`). Multiple instances must coexist without interference.
- **Stack-allocated state structs.** Callers declare `ULexer l;` on the stack and call `ulex_init(&l, ...)`. No `ulex_destroy` function — nothing to clean up.
- **Zero-copy where feasible.** `UToken.u.str.start` points into the caller's source buffer rather than copying. Document the lifetime contract at the API level.
- **Freestanding-compilable.** Every `src/*.c` file must compile under `-ffreestanding` on a toolchain without a C library. See "Freestanding discipline" below for the rule.

---

## Freestanding discipline

Every file under `src/` compiles under `-ffreestanding`, including on toolchains that ship no C library at all (e.g. `gcc-riscv64-unknown-elf` on Ubuntu). CI enforces this via the `cross-riscv` job.

Rules:

- **Only C99-mandated freestanding headers are unconditional.** `<float.h>`, `<iso646.h>`, `<limits.h>`, `<stdarg.h>`, `<stdbool.h>`, `<stddef.h>`, `<stdint.h>`. These are provided by the compiler, not the libc, so they are always available.
- **Hosted headers are guarded.** `<stdlib.h>`, `<string.h>`, `<stdio.h>`, `<time.h>`, `<assert.h>`, etc. Include them behind `#if __STDC_HOSTED__ / #endif`. GCC sets `__STDC_HOSTED__` to 0 under `-ffreestanding`.
- **Any function that reaches a hosted-only feature is guarded the same way.** If a convenience wrapper calls `malloc`, it and its prototype in the public header both sit inside `#if __STDC_HOSTED__`. Freestanding callers are expected to use the injection-based API (custom allocator, static buffer) that the library already provides.
- **Don't depend on libc for leaf utilities.** `memset`, `memcpy`, `strlen` are all hosted. Write small local replacements when needed (`arena_zero` in `src/uarena.c` is the pattern). Mark the buffer `volatile` to prevent the compiler from recognizing the loop and lowering it back to a libc call under `-Os`.
- **Test files are exempt.** `tests/unit/*.c` link against the host toolchain and may freely use hosted headers. The library itself is what must stay freestanding.

The RISC-V CI job is the acceptance test. If your change makes `make cross-riscv` fail, the change is wrong — not the test.

`src/uvalue.c` uses `<stdio.h>` for `snprintf` and is therefore gated behind
`#if __STDC_HOSTED__`. The header `src/uvalue.h` declares the API
unconditionally so callers can include it on any target; freestanding callers
that attempt to link `uvalue_format` get a clear undefined-symbol error at link
time rather than a silent miscompile. This is the same pattern as
`src/uarena.c`'s `stdlib_alloc` gating: the header is unconditional, the
implementation symbols are hosted-only.

---

## Const-correctness

- **Pointer-to-const for read-only parameter data.** `const char *src`, `const ULexer *l`. Mutates through a pointer arg require non-const (`ULexer *lex`).
- **Top-level const on by-value function parameters, in definitions only.** `static int digit_value(const char c, const int base)`. Header declarations stay un-const-qualified — C strips top-level const from prototypes, so it's noise there.
- **Const on read-only locals.** Any local that's computed or assigned once and then only read gets `const`. Loop accumulators and cursor-driven state stay mutable.
- **Const on tables.** `static const char *TOKEN_NAMES[]`, `static const char *ERR_MSG[]`. Makes it explicit that these are compile-time-frozen lookup data.

Rationale: the long-term path includes formal-audit territory where const-correctness is a rule-set requirement. Leaning into it now costs nothing and avoids a retrofit later.

---

## Initialization

Use aggregate zero-initialization for value-type structs:

```c
UToken t = {0};
t.type = TOK_EOF;
t.line = lex->line;
```

Not `memset(&t, 0, sizeof(t))`. Same result semantically, but on our target compilers at `-Os` the `= {0}` form collapses cleanly with the subsequent field stores (dead-store elimination of the zero), producing materially smaller code. Measured on this codebase: −4.6% on `ulex.o` `.text` when switched.

Exception: if a future use case does involve `memcmp` / hashing / wire-serialization of the struct including padding, switch that specific call site back to `memset` with a comment explaining why. We don't have any today.

---

## Error handling

- **Structured error codes in an enum.** One enum per subsystem (`ULexError`), densely numbered, with `LEX_OK = 0` as the never-emitted sentinel.
- **Static message table indexed by code.** `static const char *ERR_MSG[] = {...}`, one string per code, order mirrors the enum. Messages describe the error class, not the specific input (input position is tracked separately).
- **In-stream errors.** Errors surface as a token variant (`TOK_ERROR`) with a structured payload, not as return codes or out-parameters. The caller chooses recovery policy: stop on first error, or keep scanning for subsequent tokens.
- **Static error messages, never allocated.** Error tokens carry a pointer to the compile-time table; no `strdup`, no formatting at error time.
- **Recovery: advance one past the bad input.** After `TOK_ERROR`, the subsystem's cursor has moved past the offending byte. A caller calling again gets the next sensible token, not a re-entry into the same bad input.

---

## Value types and tagged unions

- **Prefer value-returned structs over out-parameters** for small aggregates. `ulex_next` returns `UToken` by value; no allocation, no lifetime question.
- **Named unions, not anonymous.** `union { ... } u;` instead of `union { ... };`. Anonymous unions are a C11 feature; we're strict C99. Access is `t.u.i`, `t.u.str.start`, `t.u.err.code`.
- **Tagged unions with a discriminator.** `UToken.type` is the tag; only the union member matching the tag is valid to read. Other members are zero-initialized (per the `= {0}` rule) and must not be interpreted.

---

## Headers

- **One public header per subsystem**, at most. `src/ulex.h` is the only header a consumer needs to use the lexer.
- **`extern "C"` guards on every public header.** The embedded / robotics audience often mixes C and C++; public headers must be includable from both.
- **Include guards follow the header basename.** `#ifndef ULEX_H` / `#define ULEX_H` / `#endif`.
- **Minimal system header use.** Public headers include only what they need for the declared types: `<stddef.h>` for `size_t`, `<stdint.h>` for `int64_t`, etc. Implementation-detail includes (`<string.h>`, `<limits.h>`) stay in the `.c`.
- **Document the public API at the declaration.** Every function declaration in a public header gets a block-comment describing its contract: what it does, argument lifetimes, thread-safety (if relevant), idempotency guarantees. Struct fields get one-line comments.

---

## Tests

- **TDD throughout.** Every new subsystem behavior: write a failing test, see it fail, write the minimal implementation that makes it pass, see it pass, commit. Not "write code, then add tests."
- **One test file per subsystem** under `tests/unit/test_<subsys>.c`. Wire it into `tests/unit/runner.c` via an `extern` declaration and a call inside `main`.
- **Each test case is a static function with a descriptive name.** `int_overflow`, `hex_leading_underscore`, `sync_line_across_block_comment`. Register with `utest_run("name", fn);` inside a `test_<subsys>_suite()` function.
- **Const on test locals.** `const UToken t = ulex_next(&l);` — tokens are write-once in tests. Matches the production-code const rule.
- **Meaningful checks, not tautologies.** A test that reads `UASSERT_EQ(t.type, TOK_EOF)` after `ulex_next` on `""` is meaningful. A test that reads `UASSERT_EQ(1, 1)` is noise.
- **Every reachable error code gets a test.** If `LEX_FOO_BAR` can be emitted by production code, a test must exercise it and assert the specific code.

Coverage targets: ≥ 90% line coverage, ≥ 80% branch coverage per subsystem. Verified via `gcov` once per-milestone coverage reporting is wired.

---

## Comments and documentation

- **SPDX license identifier on line 1 of every source file.** `/* SPDX-License-Identifier: BSD-3-Clause */` — required, tool-consumable, lawyer-friendly.
- **One-line purpose comment on line 2.** `/* ULexer. */`. What this file is, not how it works.
- **`/* why */` over `/* what */` in function bodies.** Well-named identifiers describe the what. Comments explain non-obvious choices: invariants, subtle ordering, workarounds for a specific compiler or platform.
- **Block comments on public API declarations.** See Headers, above.
- **No emojis.** Anywhere. Commits, code, comments, docs.
- **Avoid dead or speculative comments.** No `TODO`, no `XXX`, no `FIXME` without an owner and context. If a thing needs doing, either open an issue or do it. Drive-by TODOs turn into permanent lies.

---

## Commits

Commit hygiene is covered in `CONTRIBUTING.md`. Briefly:

- Subsystem prefix: `lex:`, `parse:`, `vm:`, `gc:`, `tests:`, `build:`, `ci:`, `docs:`, etc.
- Imperative mood, ≤ 72-char subject, no trailing period.
- Body explains WHY. One concern per commit.
- Commits stand alone. Don't reference internal workflow artifacts that live outside the public repo.

---

## What is mechanically enforced

| File | Enforces |
|---|---|
| `.editorconfig` | Indent style, trailing whitespace, line endings, final newline |
| `.clang-tidy` | A chosen subset of clang-tidy checks (bugprone, performance, portability, clang-analyzer); disables C++-focused categories and three individually-noisy checks |
| `Makefile:tidy` | Gating clang-tidy via `run-clang-tidy --warnings-as-errors='*'`, reading `compile_commands.json` |
| `Makefile:cppcheck` | Advisory cppcheck sweep over the same compile database; exits 0 regardless of findings |
| `Makefile:analyzer` | Advisory GCC `-fanalyzer` compile-time pass in the dedicated `build/host-analyzer/` variant |
| `Makefile` | C99 standard, `-Wall -Wextra -Wpedantic`, sanitizer variants, zero-deps static-library build |
| `.gitignore` | Build artifacts, per-editor state, AI-tool config files |

If a rule can be machine-enforced, it should be. This doc codifies only the decisions that can't be (or that benefit from explanation alongside the mechanical rule).

---

## Revisions

Style decisions are not immutable. When a rule in this doc turns out to be wrong:

1. Open an issue or raise it in a commit body first (don't change the doc silently).
2. If the change is adopted, update this doc in the same PR / commit series that changes the code. Don't leave the guide stale.
3. Decisions that shaped a past debate stay here with their rationale, so future-me doesn't re-derive them.
