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

## Cross-compile sanity

If you have `arm-none-eabi-gcc` or `riscv-none-elf-gcc` installed:

    make cross-arm-cortex-m7   # → build/arm-cortex-m7/liburbi.a
    make cross-riscv32         # → build/riscv32/liburbi.a

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

## ABI version policy

The public C API surface is versioned via `<urbi/version.h>` macros:
`URBI_API_VERSION_MAJOR`, `URBI_API_VERSION_MINOR`, `URBI_API_VERSION_PATCH`.
Strictly separate from `URBI_BYTECODE_VERSION_BYTE` (wire format) and
`urbi_version()` (project release string).

### Bump categories

- **MAJOR** — removed function, changed signature, removed/renumbered enum value, struct-layout change visible across the boundary, removed `URBI_ERR_*` slot.
- **MINOR** — additive: new function, new enum value appended, new `URBI_ERR_*` slot at the next free index, new build flag.
- **PATCH** — bug fix only, no header change at all.

### Pre-v1.0 escape clause

While `URBI_API_VERSION_MAJOR == 0`, MINOR bumps **may** break ABI per standard semver convention. Each MINOR bump must enumerate breakages in CHANGELOG. Strict policy goes live at v1.0.0.

### What versions track

| Concept | Macro / Getter | Scope | Bumps when |
|---|---|---|---|
| C API ABI | `URBI_API_VERSION_*` / `urbi_api_version()` | Public headers | Per categories above |
| Bytecode wire format | `URBI_BYTECODE_VERSION_BYTE` (0x16) | Loader/runtime | New opcode / wire layout change |
| Project release | `urbi_version()` | This file's heading | Each tag |

Keeping these independent matches Lua's `LUA_VERSION_NUM` / `LUAC_VERSION` / `LUA_RELEASE` pattern.

## Layout policy

Source files live under per-subsystem folders:

    src/lex/    src/parse/  src/emit/   src/chunk/  src/util/
    src/rt/     src/stdlib/ src/host/   src/repl/

Public C API lives in `include/urbi/`:

    include/urbi/types.h     UValue, the error codes, the allocator signature, opaque forward declarations
    include/urbi/urbi.h      The 44-function embedding API
    include/urbi/version.h   API and release version macros
    include/urbi/repl.h      The optional cooperative eval service
    include/urbi/require.h   URBI_REQUIRE and its embedder hook

### The layering rule

`src/rt/` is the runtime, and two rules make it portable. Both are
enforced by `tests/scripts/check_rt_layering.sh`, which `make test`
runs.

**It reaches four places outside itself and nowhere else:** `chunk/`
(the bytecode it executes), `urbi/` (the API it implements), `stdlib/`
(the boot table's installers) and `emit/ufront.h` (the single compile
entry point `load` needs). The lexer, the parser, the arena, the host
formatter and the eval service all sit above the core, and a core that
included one would stop being the thing a microcontroller links.

**It uses no libc beyond five headers:** `stdint.h`, `stddef.h`,
`stdbool.h`, `string.h`, `math.h`. Anything needing `snprintf`,
`malloc` or `assert` belongs in `src/host/`, which a freestanding build
omits, or in the frontend.

Within `src/rt/` the headers have a fixed include order — uvalue, ugc,
ustr, uobj, ulist, ustrand, usched, uexec, uwatch, urealm, uboot — and a
header may include only headers to its left. `src/stdlib/` reaches the
runtime through exactly one header, `rt/ustdlib_glue.h`.

A new global that is not `static` and does not start with `urbi_` fails
`make test-api-manifest`: an embedder links this archive statically, so
every exported name is a potential collision in their build.

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
| `chunk:` | Bytecode container: writer, loader, verifier (`src/chunk/`) |
| `util:` | Frontend-shared helpers: arena, varint, freestanding string ops (`src/util/`) |
| `rt:` | The runtime: collector, objects, strands, scheduler, unwinder, watchers, realms, boot table (`src/rt/`) |
| `stdlib:` | Built-in methods and `stdlib.u` (`src/stdlib/`) |
| `host:` | Public API whose implementation needs libc (`src/host/`) |
| `api:` | Public C API headers (`include/urbi/*.h`) |
| `repl:` | The cooperative eval service (`src/repl/`) |
| `tools:` | Anything under `tools/` |
| `ros:` | micro-ROS bridge (`src/ros/`) — parked; it returns with its own tag once a ROS environment is available |
| `port:` | ESP-IDF / STM32 / Xtensa integration — parked until its own tag |

The runtime is one prefix, not ten. Before the core re-foundation it was
`vm:`, `gc:`, `sched:`, `object:`, `realm:`, `react:` and `runtime:`
against seven directories; `src/rt/` replaced all of them, and a commit
touching the collector and the scheduler together is now ordinary rather
than a multi-subsystem split.

Multi-subsystem commits: pick the dominant one, or split. Splitting is almost always right.

### Style rules

- Imperative mood. "add", "fix", "refactor" — not "added", "fixes".
- One concern per commit. If you're typing "and" in the summary, split it.
- Body explains why, especially for non-obvious choices.
- No internal plan or phase markers in commits or tags.
- No emojis in commits, code, README, CHANGELOG, or anywhere in the public repo.

### AI-attribution rule

Nothing in the public repo mentions Claude, Anthropic, Copilot, Cursor, Aider, or AI assistance of any kind. This covers commit messages (no `Co-Authored-By: Claude …` trailers <!-- scrub-allow: policy doc listing prohibited pattern by name -->), PR descriptions, release notes, tag messages, README, CHANGELOG, CONTRIBUTING, and code comments.

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

    make cross-arm-cortex-m7   # if arm-none-eabi-gcc installed
    make cross-riscv32         # if riscv-none-elf-gcc installed

Or push and let CI catch it.

Wire-format hash capture: `tests/scripts/capture_wire_format_hashes.sh`
hashes the on-disk wire-format bytes for every `tests/chk/**/*.chk` fixture
(sensitive to header bytes, opcode-shape table, varint encoding, and
nested-proto round-trip).  `make test-wire-format-determinism` checks that
two captures of the same build are byte-identical (closes the v0.5.7.1
hotfix that fixed mktemp paths leaking into `source_name`).

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

This standing requirement was codified during Wave 5 (`v0.5.7-fixes`)
and held end-to-end through Wave 6 (`v0.5.8-cleanup`); every fix commit
across both waves landed with a paired regression test in the same
commit.  Discipline notes:

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

Three strict-tooling targets gate at hard-fail tier in releasetest;
all stand at 0 violations against the v0.5.8-cleanup baseline:

- **`make test-scan-build`** — Clang static analyzer.  Hard gate
  since releasetest's first cut; 0 bugs required.
- **`make test-cppcheck`** — cppcheck `--enable=all --inconclusive`
  strict checklist over `src/`.  Promoted to all-categories
  hard-fail at v0.5.8-cleanup Phase 19 (was 145 informational at
  v0.5.7-fixes shipping → 0 at v0.5.8-cleanup).  Suppressions live
  in `.cppcheck.suppressions` at the repo root with audit-ID
  rationale per block.  Two structurally false-positive categories
  are blanket-suppressed: `unusedFunction` (cppcheck scans `src/`
  only, every public-API symbol looks unused from its perspective)
  and a small set of per-file entries whose rationale is recorded
  beside them in that file.
- **`make test-tidy-strict`** — clang-tidy with bug-prone /
  cert-ish + readability checklist.  Promoted to all-categories
  hard-fail at v0.5.8-cleanup Phase 20 (was 23 informational at
  v0.5.7-fixes shipping → 0 at v0.5.8-cleanup).  Per-line
  `// NOLINT(<check>)` suppressions with rationale carry the
  design pins: `performance-no-int-to-ptr` for the UProtos
  high-bit pointer encoding (pre-M4 prototype-chain spec §7.2),
  the strand REASON_* payload-encoding contract, the UVAL_HOST_FN
  function-pointer storage, and arena alignment round-trips;
  `clang-analyzer-valist.Uninitialized` for the vararg log helpers
  whose `va_start` → `vsnprintf` → `va_end` triple the analyzer
  cannot trace through; `optin.performance.Padding` on `struct UVM`
  whose field order is pinned by 6 `_Static_assert`s and clusters
  fields by milestone for maintainability.

Suppression preference: prefer **inline** `// NOLINT(category)`
(clang-tidy) and `// cppcheck-suppress category` immediately above
the affected line, with a rationale comment.  Inline suppressions
move with the line they cover when surrounding code is edited;
file-level line-pinned entries in `.cppcheck.suppressions` /
`.clang-tidy.suppressions` are fragile (Phase 20 hit a CPPCHK-012
realignment after unrelated comment additions shifted the pinned
line number).  Reserve the suppression files for blanket
project-wide suppressions where inline placement isn't possible.

Future findings from category drift (new clang-tidy / cppcheck
releases adding checks) must either be fixed at source, suppressed
inline with audit-ID rationale, or — only when truly necessary —
added to the documented blanket suppressions in
`.cppcheck.suppressions` / `.clang-tidy.suppressions`.

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

## v0.5.x cleanup ramp (2026-05-06 to 2026-05-09)

The v0.5.x cleanup ramp was a 6-wave pre-M6 hygiene release between
the M5 reactive runtime (`v0.5.0-reactive`) and the M6 stdlib milestone.
Each wave addressed one tightly-scoped theme so that bisecting later
regressions stays cheap:

- `v0.5.3-layout` — folder reorg + filename renames
- `v0.5.4-decompose` — split four monster files into per-concern units
- `v0.5.5-naming` — function + public C API rename + header hygiene
- `v0.5.6-bytecode` — wire format v1.4 → v1.5 hard break
- `v0.5.7-fixes` — 123 bug-tier audit IDs closed; TDD per fix commit
  codified as a standing convention
- `v0.5.8-cleanup` — final wave: dead code, smells, docs, and
  strict-tooling close-out (cppcheck + tidy-strict + docstring-coverage
  all promoted to hard-fail releasetest gates)

Conventions established during the ramp are documented above.  See
`docs/milestones/v0.5.x-cleanup.md` for the full retrospective.

## License

BSD-3-Clause.
