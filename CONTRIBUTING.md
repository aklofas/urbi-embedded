# Contributing to urbi-embedded

## Building

Requires a C99 compiler (GCC or Clang) and GNU Make. No other dependencies.

    make          # build build/host/liburbi.a
    make test     # run the unit test suite

All build artifacts land under `build/<TARGET>/`. The default `TARGET` is `host`.
Override with `make TARGET=<name>` if you need a custom output tree.

## Test modes

    make test-debug    # -O0 -g, for debugging with gdb       → build/host-debug/
    make test-asan     # AddressSanitizer instrumentation     → build/host-asan/
    make test-ubsan    # UndefinedBehaviorSanitizer           → build/host-ubsan/
    make test-valgrind # valgrind memcheck (CI-gating)        → build/host-valgrind/

All four fast variants (`test`, `test-debug`, `test-asan`, `test-ubsan`) must
pass before any commit is merged; each variant has its own build tree, so
they coexist and no `make clean` is required when switching between them.
`test-valgrind` is enforced in CI but runs ~20–50× slower than a plain build
— run it periodically and always before a milestone tag, not on every commit.

## Coverage

`make coverage` produces a line-coverage HTML report at
`build/host-coverage/report.html` via gcovr. Threshold ≥85% line coverage
(enforced in `make releasetest`).

`make test-branch-coverage` reports branch + decision coverage via gcovr's
`--branches` + `--decisions` flags. As of v0.5.7-fixes the gate is
informational-only at 69% baseline; Phase 20 of the v0.5.7-fixes plan
closes coverage gaps and the gate enables (`--fail-under-branch 75`) once
baseline exceeds threshold. Drops below threshold flag PRs; either close
the gap in the same commit or document at the bottom of the affected file:

    // AUDIT: branch <description> covered indirectly via tests/path/test_other.c

## Cross-compile sanity

If you have `arm-none-eabi-gcc` or `riscv64-unknown-elf-gcc` installed:

    make cross-arm     # → build/arm-cortex-m7/liburbi.a
    make cross-riscv   # → build/riscv-rv32imc/liburbi.a

These verify portability; they don't run tests (no target execution environment on the build host).

## Indexing database for LSP editors

Generate a `compile_commands.json` for clangd, CLion, VS Code, or any LSP-based editor:

    make compile_commands.json

The file is gitignored — regenerate it after changing `CFLAGS`/`CPPFLAGS` or adding/removing source files.

## Adding a new test file

1. Create `tests/unit/test_<name>.c` containing a suite function `test_<name>_suite(void)` that calls `utest_run("...", static_fn)` for each test case
2. Add `extern void test_<name>_suite(void);` near the top of `tests/unit/runner.c`
3. Add `test_<name>_suite();` inside `main()` in `runner.c`
4. `make test` — verify the new cases appear in the output

## Coding style

See `docs/STYLE.md` for the full style guide — naming, memory model, const-correctness, initialization, error handling, headers, tests, and comment conventions. Mechanical rules are enforced by `.editorconfig`, `.clang-tidy`, and the Makefile's warning flags.

### Naming policy (summary)

Filenames:

- `u` prefix on all source files; snake_case for compound names.
- Subsystem-public header is `u<subsystem>.h` matching `u<subsystem>.c`.
- Sub-files within a subsystem use `u<subsystem>_<aspect>.{c,h}`.

Function names:

| Visibility | Convention | Example |
|---|---|---|
| Public C API (in `include/urbi/*.h`) | `urbi_<noun>_<verb>` | `urbi_realm_set_global` |
| Subsystem-public (in `src/<subsys>/u<subsys>.h`) | `u<subsystem>_<verb>` | `uvm_run`, `uemit_expr` |
| TU-local (`static`) | `<short_descriptive>` | `lex_number`, `parse_expr_atom` |

Other:

- No double-underscore (`__foo`) prefixes — reserved by C99/C11 §7.1.3.
- No milestone-tag prefixes in symbol names.
- Integer-literal suffixes uppercase: `1U`, `0ULL` — never `1u`, `0ull`.

## Header hygiene

- One header per `.c` when the `.c` exposes anything beyond its TU.
- Subsystem-public headers live in the subsystem folder (`src/<subsys>/u<subsys>.h`).
- Headers declare; never include implementation.
- TU-local types: forward-decl or stay in the `.c`.
- No `#include` cycles.
- Direct `#include` for everything used; do not rely on transitive pulls. `make tidy` flags `misc-include-cleaner` violations (system-wide sweep at v0.5.5; some intentional skips for the public/internal layer split).

## Layout policy

Source files live under per-subsystem folders:

    src/lex/      src/parse/    src/emit/     src/vm/
    src/gc/       src/sched/    src/watcher/  src/event/
    src/tag/      src/changed/  src/module/   src/value/
    src/runtime/  src/realm/    src/object/

Public C API lives in `include/urbi/`:

    include/urbi/types.h     UValue, UExecStatus, UErrCode, UVMError, UVMAllocFn, opaque struct fwd-decls
    include/urbi/urbi.h      Public lifecycle + control + step
    include/urbi/gc.h        GC strategy router
    include/urbi/sched.h     Per-scheduler API umbrella
    include/urbi/object.h    Object-model public API

## Commit messages

Format:

    <prefix>: <imperative summary, lowercase, ≤ 72 chars, no trailing period>

    <optional body, wrapped at 72 cols, explains WHY not WHAT>

### Subsystem prefixes

| Prefix | Scope |
|---|---|
| `tests:` | Test harness, test cases, fixtures |
| `chk:` | `.chk` conformance fixture files |
| `build:` | Makefile, build flags, cross-compile targets |
| `ci:` | `.github/workflows/*`, static analysis config |
| `docs:` | README, CHANGELOG, CONTRIBUTING, comments-only changes |
| `lex:` | Lexer (`src/lex/`) |
| `parse:` | Parser, AST (`src/parse/`) |
| `desugar:` | Desugaring pass |
| `emit:` | Bytecode emitter, register allocator (`src/emit/`) |
| `module:` | Bytecode module format, serializer/deserializer (`src/module/`) |
| `vm:` | Interpreter core, opcode dispatch (`src/vm/`) |
| `value:` | Value representation, shape system (`src/value/`) |
| `gc:` | Garbage collector (`src/gc/`) |
| `sched:` | Coroutine scheduler (`src/sched/`) |
| `react:` | Events, watchers, tags (`src/{event,watcher,tag,changed}/`) |
| `runtime:` | Cross-cutting runtime (`src/runtime/`) |
| `realm:` | Realms (`src/realm/`) |
| `object:` | Object model (`src/object/`) |
| `api:` | Public C API (`include/urbi/*.h`, `src/urbi.c`) |
| `tools:` | Anything under `tools/` |
| `repl:` | REPL service |
| `ros:` | micro-ROS bridge |
| `port:` | ESP-IDF / STM32 / Xtensa integration |

Multi-subsystem commits: pick the dominant one, or split. Splitting is almost always right.

### Style rules

- Imperative mood. "add", "fix", "refactor" — not "added", "fixes".
- One concern per commit. If you're typing "and" in the summary, split it.
- Body explains why, especially for non-obvious choices.
- No internal plan or phase markers in commits or tags.
- No emojis in commits, code, README, CHANGELOG, or anywhere in the public repo.

### AI-attribution rule

Nothing in the public repo mentions Claude, Anthropic, Copilot, Cursor, Aider, or AI assistance of any kind. This covers commit messages (no `Co-Authored-By: Claude …` trailers), PR descriptions, release notes, tag messages, README, CHANGELOG, CONTRIBUTING, and code comments.

## Branching

Default: main-only. Topic branches `topic/<short-slug>` for work taking
> 10 commits or that would break main mid-flight. Push for backup is fine.
Merge with `git merge --ff-only` (or `--no-ff` if explicitly preserving the
branch shape). No PRs. Delete the branch after merge.

Never force-push to `main`.

## Pre-commit verification

Required for every commit to `main`:

    make test         # Release -Os build
    make test-debug   # -O0 -g build

Both must pass.

For `src/*.c` changes:

    make test-asan
    make test-ubsan

For public-API or opcode-semantics changes:

    make cross-arm    # if arm-none-eabi-gcc installed
    make cross-riscv  # if riscv*-elf-gcc installed

Or push and let CI catch it.

Bytecode-byte-identical contract: any commit that touches `src/lex/`,
`src/parse/`, `src/emit/`, `src/value/`, `src/module/`, `src/object/`, or
`src/runtime/` should reproduce `tests/golden/v0.5.7-fixes-bytecode-hashes.txt`
exactly unless a deliberate codegen change is being made (which requires
re-capturing the golden table and bumping bytecode version).

The wire-format gate at `tests/golden/v0.5.7-fixes-wire-format-hashes.txt`
provides complementary coverage: the disasm-text hash is stable across
opcode renumber + version-byte advance and is blind to genuine wire-format
breaks; the wire-format hash is sensitive to header bytes, opcode-shape
table, varint encoding, and nested-proto round-trip.  Re-capture both
golden tables in lockstep when a codegen change is intentional.

### TDD per fix commit (Wave 5 onward)

Every fix commit (a commit that closes an audit-finding ID or fixes a
bug found during development) MUST follow strict test-driven development:

1. **Test demonstrates the bug**: write a failing unit test or `.chk`
   fixture that exercises the bug *before* applying the fix.  Verify the
   test fails on `main` (or the pre-fix branch state).
2. **Fix passes the test**: apply the fix; verify the test now passes.
3. **Both land in the same commit**: the fix + the regression test land
   together so the test cannot be silently disabled in a future
   regression.  No "test-only" or "fix-only" commits for fix work.

This standing requirement was codified during Wave 5 (`v0.5.7-fixes`).
Discipline notes:

- Internal-assertion paths that abort the test runner are a known gap;
  the URBI_TEST_ONLY assert-fire macro (filed in
  `docs/urbi-embedded-backlog.md` test-infrastructure section) will
  close it.  Until then, those paths land with a doc-only assertion fix
  combined with an audit-ID note on the test-coverage limitation.
- Coverage-only commits (closing COV-* IDs) are allowed without a
  paired bug fix — they are TDD's symmetric case (test demonstrates an
  *un*-exercised path; the path is verified correct).

Refactor commits, naming sweeps, and dead-code removal commits are
exempt — they ship under the existing bytecode-byte-identical or
test-suite-passes gates.

### Strict-tooling baselines

Three strict-tooling targets gate at three different tiers:

- **`make test-scan-build`** — Clang static analyzer.  **Hard gate
  in releasetest.**  Must be 0 bugs.  Runs on every PR.
- **`make test-tidy-strict`** — clang-tidy with bug-prone /
  cert-ish checklist.  **Hard gate in releasetest as of
  v0.5.8-cleanup Phase 20** (was 23 informational at v0.5.7-fixes
  shipping → 0 at v0.5.8-cleanup).  Per-line `// NOLINT(<check>)`
  suppressions with rationale carry the design pins:
  `performance-no-int-to-ptr` for the UProtos high-bit pointer
  encoding (pre-M4 prototype-chain spec §7.2), the strand REASON_*
  payload-encoding contract, the UVAL_HOST_FN function-pointer
  storage, and arena alignment round-trips;
  `clang-analyzer-valist.Uninitialized` for the vararg log helpers
  whose `va_start` → `vsnprintf` → `va_end` triple the analyzer
  cannot trace through; `optin.performance.Padding` on `struct UVM`
  whose field order is pinned by 6 `_Static_assert`s and clusters
  fields by milestone for maintainability.
- **`make test-cppcheck`** — cppcheck `--enable=all --inconclusive`
  strict checklist over `src/`.  **Hard gate in releasetest as of
  v0.5.8-cleanup Phase 19** (was 145 informational at v0.5.7-fixes
  shipping → 0 at v0.5.8-cleanup).  Suppressions live in
  `.cppcheck.suppressions` at the repo root with audit-ID rationale
  per block.  Two structurally false-positive categories are
  blanket-suppressed: `unusedFunction` (cppcheck scans `src/` only,
  every public-API symbol looks unused from its perspective) and
  `unusedLabelConfiguration` + `assignBoolToPointer` in
  `src/vm/uvm.c` (cppcheck cannot parse GCC's computed-goto
  `&&label` operator).

All three strict-tooling targets are now hard-fail releasetest gates
with 0 violations at the v0.5.8-cleanup baseline.  Future findings
from category drift (new clang-tidy / cppcheck releases adding
checks) must either be fixed at source, suppressed per-line with
audit-ID rationale at the suppression site, or added to the
documented blanket suppressions in `.cppcheck.suppressions` /
`.clang-tidy.strict`.

### Header docstring coverage

`make test-docstring-coverage` enforces that every function declaration
in a public-API or subsystem-public header carries an immediately
preceding `/* ... */` block comment (or `//` line comment).  **Hard
gate in releasetest as of v0.5.8-cleanup Phase 21.**

Scope:

- `include/urbi/*.h`              — public C API
- `src/<subsys>/u<subsys>.h`      — subsystem-public headers

Skipped: `_internal.h` (intentionally private inter-TU API) and
`umacros.h` (macro-only helper bag).

A docstring "cascades" through a contiguous run of declarations: a
comment above the first decl in a group covers later decls in the
same group as long as no blank line, function definition, or non-decl
content intervenes.  Forward declarations (`struct X;`, simple
`typedef`) and callback typedefs do not break the cascade — they
typically sit between a docstring and the function decl that uses
the type.

Required content per docstring (per Phase 21 of the v0.5.8-cleanup
plan):

- one-line summary of what the function does;
- preconditions (state any required caller-side setup);
- postconditions (state any guaranteed callee-side effects);
- ownership of pointer arguments (caller-owned, callee-owned, shared);
- return-value meaning + error codes when applicable;
- ISR-safety (note whether the function is ISR-safe).

Group-style docstrings are accepted for tightly-related decls.  See
the priority API in `include/urbi/sched.h:45-61` for the canonical
group-doc form.

### Full-corpus sanitizer gate

`make test-corpus-sanitize` runs every `.chk` fixture under ASan, UBSan,
and valgrind memcheck (full leak-check).  **Promoted to releasetest at
v0.5.7-fixes Phase 21.**  148 fixtures × 3 sanitizers; ~5-7 minutes
wall-clock under the 2-phase parallelization scheme (see
`Makefile:releasetest`).  Replaces the previous unit-test-only sanitizer
gate, which missed every bug surfacing only at fixture-level or
through specific .chk reactive runtime paths.

## Tag conventions

One annotated tag per milestone or wave. Format:

    v<MAJOR>.<MINOR>.<PATCH>-<codename>

Examples: `v0.5.3-layout`, `v0.5.4-decompose`, `v0.5.5-naming`.

## Per-file LOC-cap exceptions

The default soft cap is 1000 LOC per `.c` source file (enforced by
`make test-loc-cap`).  Files listed below are exempted with rationale.

- `loc-cap-exception:src/vm/uvm.c` — opcode dispatch loop. The body of
  `dispatch_loop_until_yield` (computed-goto dispatch + ~47 inline opcode
  handlers + safepoint / exit-strand / halt-error labels) is intentionally
  inlined in a single TU for cache locality of the dispatch table and to
  let the compiler keep the entire instruction-stream state machine in
  registers across opcodes.  Decomposing into per-opcode helpers would
  defeat the threading optimization that gives the VM ~10x dispatch-loop
  throughput on hosted builds and ~3x on Cortex-M7.  This exception is
  permitted by `docs/superpowers/specs/2026-05-05-v0.5.x-cleanup-design.md`
  §3.3 ("generated dispatch tables, opcode trampolines").

## License

BSD-3-Clause.
