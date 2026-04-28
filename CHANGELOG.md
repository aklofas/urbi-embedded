# Changelog

## v0.3.0-concurrency — 2026-04-28

The M3 concurrency milestone. Adds six subsystems above the M2 expression
foundation: control transfer (exceptions, tags, unwind), chunk lifecycle
(realms, namespaces, step driver), cooperative scheduler (ISR-safe event ring,
strand C API), incremental tri-color GC (5-phase state machine, debt-triggered
slices, 3 barrier surfaces, host-handle pinning), tag/watcher data and eval
layer (UTag, UWatcher pool, read-set, watcher eval loop, pending-onleave drain),
and determinism infrastructure (checksum diagnostic, CI gate, time literals,
legacy corpus port). Bytecode bumped to v1.2; earlier `.urb` files are rejected
at load time.

### Breaking changes

- **Bytecode v1.2**: version byte incremented; loader rejects v1.1 and earlier
  modules with a diagnostic. Recompile all `.urb` files.

### Language

- Time and angle literals: `100ms`, `1s`, `2.5s`, `180deg` lexed to
  `TOK_DURATION` / `TOK_ANGLE`; `ms`/`s`/`m`/`h`/`d` suffixes emit
  microsecond integer values; `deg`/`rad` emit float radian values.
- `,` (parallel fire-and-forget) and `&` (parallel join) separator runtime
  activated. `,` spawns N-1 child strands + runs last child inline.
  `&` compiles rhs to closure, runs lhs inline, then OP_FORK_JOIN /
  OP_JOIN_WAIT; result is void. Child handles are `UVAL_STRAND` (kind=7).
- `try` / `catch` / `finally` / `throw` — full emit and runtime; exception
  value forwarded through catch register.
- Tag scopes — `mytag: { ... }` compiles to OP_PUSH_TAG / body / OP_POP_TAG;
  member-strand list maintained; `urbi_tag_stop` deposits UEXEC_TAG_STOP
  with C-1 priority.

### Unwind / exception model

- `urbi_unwind` walker: 5-kind absorption (OK / RETURN / THROW / TAG_STOP /
  CANCEL); replace-on-raise semantics; URBI_WARN_SUPPRESSED_UNWIND emitted
  via `host_log_fn`.
- `UExecStatus` enum (OK / RETURN / THROW / TAG_STOP / CANCEL / FATAL);
  `urbi_exec_status_name`.

### Chunk lifecycle and scheduler

- `URealm` + `UNamespace`: per-realm GC root provider, 4-function Realm C API,
  namespace resolution protocol.
- `urbi_step` 4-state driver (OK / QUIESCENT / FATAL / YIELD_BUDGET); 4
  chunk-execution wrappers.
- ISR-safe SPSC event ring: `urbi_inject_event` as the sole ISR-safe entry
  point; bounded drain at `urbi_step` entry.
- Strand C API: `urbi_strand_create` / `start` / `spawn` / `cancel` / `panic`
  / `reset`; ambient-tag attachment; cooperative FIFO run-queue.
- `URBI_DEBUG` build mode: ISR-safety assertions at all non-ISR entry points;
  callback watchdog (configurable warn / assert threshold).

### Incremental GC

- Tri-color mark-sweep with 5-phase state machine; `urbi_gc_slice(vm, budget)`
  incremental driver; `urbi_gc_force_full` synchronous path.
- Three barrier surfaces: `urbi_gc_slot_write` (forward Dijkstra + watcher
  dirty hook), `urbi_gc_register_write` (no-op), `urbi_gc_upvalue_write`.
- Root-provider registry: up to 8 providers; 5 registered at M3 (scheduler,
  realm list, intern table, host handles, watcher table).
- Host-handle table: `urbi_pin` / `urbi_unpin`; `urbi_register_type` with
  finalizer dispatch; `UType.destroy` called from sweep.
- GC pause max 2.8 µs measured (357× margin under 1 ms target).
- `make test-gc-pause` gated stress binary; `make test-stress` 4-program suite;
  `make test-gc-none-build` strategy-swap smoke; all wired into `make releasetest`.

### Tag / watcher subsystem

- `UTag` host-managed (via `alloc_fn`); ambient-tag inheritance via synthetic
  TAG_SCOPE cleanup entries; member-strand and member-watcher lists.
- `UWatcher` pool: 200-byte record, pre-allocated slab, freelist, `in_use` /
  `high_water` counters.
- Read-set capture: bit-6 (`UGC_HAS_WATCHER_OBSERVER`) lifecycle; tail-insert
  for deterministic eval order; install-time `last_value_cache` seed.
- Watcher eval loop: `watcher_eval_dirty` walks active list; edge/level firing
  per spec §6.2/§6.3; `UScratchFrame` (~280 B) allocated at `uvm_init`.
- Pending-onleave queue: drain reuses `in_watcher_eval` reentrancy guard;
  OP_POP_TAG and `urbi_tag_stop` cascade watchers before scope destruction.

### Determinism

- `urbi_get_determinism_checksum` (`URBI_DEBUG`): XOR-reduce over active-watcher
  list and dirty count; enables replay comparison.
- `make test-determinism` CI gate: two consecutive `urbi_step` sweeps with
  checksum equality assertion; wired into `make releasetest`.

### Tests

- Unit cases: 772 (up from 489, +283); debug variant 786.
- `.chk` fixtures: 127 (up from 18, +109) across 8 subdirectories
  (`control_transfer/`, `chunk_lifecycle/`, `scheduler/`, `gc/`, `tag/`,
  `separator/`, `time_literals/`, `determinism/`).
- Cross-build: ARM Cortex-M7 32 KB text / RISC-V rv32imc 41 KB text (host
  65 KB). All three targets verified at `make cross-arm` / `make cross-riscv`.
- All 8 gates green: `make test` / `test-debug` / `test-asan` / `test-ubsan` /
  `cross-arm` / `cross-riscv` / `test-stress` / `test-gc-none-build`.

### Known limitations / deferred

- **Watcher body and on-leave execution** deferred to M5. `spawn_body_coroutine`
  and `run_watcher_onleave` are M3 stubs; tests use `test_watcher_fire_hook`
  and `test_watcher_onleave_hook` on `UVM`.
- **`,` shared-frame semantics** (spec §7.1) deferred to M5+. Current
  implementation uses closure-spawn; correctness is unchanged, only
  per-child allocation overhead differs.
- **`at`/`whenever`/`waituntil`** — reactive runtime deferred to M5.
- **Object method dispatch** — deferred to M4.
- **UVM struct padding** — `clang-analyzer-optin.performance.Padding` reports
  36 bytes excess in `struct UVM`; full field reorder deferred to avoid
  destroying semantic row-grouping in the struct comments.
- **Most legacy `.chk` corpus fixtures** remain deferred (require M4 object
  model or M5 reactive runtime). 127 fixtures active or structured as
  deferred placeholders for future milestones.

## v0.2.0-expressions — 2026-04-25

The M2 expressions milestone. Adds the full expression language surface
above the M1 arithmetic core: variables, closures, control flow, function
definitions and calls, per-parameter lazy arguments, statement separators,
and multi-VM hardening. Bytecode bumped to v1.1; earlier `.urb` files are
rejected at load time.

### Language

- Bytecode v1.1: version byte incremented; loader rejects v1.0 modules
  with a diagnostic. Reserved opcode slots assigned for all M2 additions.
- Per-VM string interning table (`ustr_intern`): strings are canonical;
  pointer equality implies content equality within a VM.
- Lua-FuncState-adapted register allocator with named locals, lexical
  block scopes, and cascading upvalue capture across arbitrarily nested
  function definitions.
- Statement separators: `;` (sequential with yield) and `|` (sequential
  atomic) ship full runtime semantics. `,` (parallel fire-and-forget) and
  `&` (parallel join) are parsed and represented in the AST; runtime is
  deferred to M3.
- Per-parameter `lazy` keyword: `function f(lazy x) { ... }` compiles
  the argument to a sub-proto thunk; the callee's first read of `x`
  forces evaluation implicitly.
- Control flow: `if` / `else` with proper short-circuit jumps; `while`
  with back-edge `OP_CLOSE` for closure-in-loop correctness.
- Function definitions, calls, and `return`.
- Comparison operators (`==`, `!=`, `<`, `<=`, `>`, `>=`).
- Boolean and nil literals (`true`, `false`, `nil`).

### Multi-VM hardening

- Per-VM `intern_table` and `topology_gen` fields on `UVM`; no
  file-scope mutable state remains.
- `UModule` gains `origin_vm` field; stamped at compile time, checked at
  load time.
- 8-case isolation test matrix in `tests/unit/test_multi_vm.c`
  (3 cases deferred to M3+/M5+/M6+).
- `tools/audit-globals.sh` + `cppcoreguidelines-avoid-non-const-global-variables`
  clang-tidy check gated under `make lint`.

### Migration notes

- `bare function name { body }` → `function name() { body }`.
  The bare-function form (no formal parameter list) now produces
  `PARSE_BARE_FUNCTION` at parse time. The migration recipe is mechanical.
- `closure(x) { ... }` → `function(x) { ... }`.
  The `closure` keyword is retired; `function` captures lexical scope
  universally. Note the `this`-binding migration trap: legacy `closure`
  bound `this` to the definition site; v1.0 `function` binds `this` to
  the call site. Affected pattern: `var obj.m = closure(t) { this.f(t) }`.
  Migration recipe: capture the receiver explicitly before the closure:
  `var self = this; var obj.m = function(t) { self.f(t) }`.

### Build (infra)

- Added `make releasetest` aggregate target that runs every host-side
  CI gate in sequence (sanitizer matrix, valgrind memcheck, lint,
  docs-check, coverage). Invoked manually before tagging a release
  or pushing branches touching multiple subsystems. Cross-compile
  targets are excluded; CI remains authoritative for cross-compile
  verification.

### Documentation (test infra)

- Documented the tiered test-target convention in
  `docs/internals/test-harness.md` — `make test` and the three fast
  companion variants form the pre-commit gate (~30 s combined);
  `make releasetest` is the pre-release gate (~3–5 min). Extended
  the target-reference table with a Runtime column and the
  previously-undocumented `test-switch` and `releasetest` rows.
- Codified the `.chk` fixture header schema (`Milestone:` /
  `Covers:` comment lines) in the "Authoring a new fixture"
  subsection of `docs/internals/test-harness.md`. Applied the
  schema to `tests/chk/arithmetic/basic.chk`. Enables `grep`-based
  discovery across the corpus at scale.

### Tests (chk-layout)

- Reorganized `tests/chk/` into feature subdirectories. Moved
  `tests/chk/arithmetic.chk` → `tests/chk/arithmetic/basic.chk` and
  documented the `tests/chk/<feature>/<name>.chk` layout convention
  in `docs/internals/test-harness.md`. The `test-chk` Makefile target
  already uses `find ... -name` recursively; no build change required.

## v0.1.0-skeleton — 2026-04-24

The walking-skeleton milestone. A complete end-to-end compile-and-execute
pipeline — lexer, parser, arena allocator, bytecode emitter + module
format, register-based VM, interactive REPL, and the first `.chk`
conformance fixture (`arithmetic.chk`). Not a production runtime — the
language surface is an 8-opcode Int/Float arithmetic subset — but the
pipeline is genuinely end-to-end, and every subsystem is covered by
unit tests, sanitizers, a valgrind-gated memcheck job, and coverage
instrumentation. Freestanding-clean front end cross-compiles for
Cortex-M7 and RV32IMC.

### REPL

- New binary `urbi` — the M1 walking-skeleton REPL. Drives the full
  `ulex` → `uparse` → `uemit` → `uvm` pipeline. Five modes: `urbi -i`
  (interactive via vendored linenoise, with `~/.urbi_history` persistence
  and `[%08u] value` timestamp frames via `clock_gettime(CLOCK_MONOTONIC, …)`),
  `urbi -e <expr>` (evaluate and print), `urbi [-f] <file>` /
  `urbi <file>` (run script; no per-statement print per Unix convention),
  `urbi --dump-bytecode` (disassemble via `uemit_disassemble`, incompatible
  with `-i`), `urbi --version` / `urbi --help`. Persistent `UVM` across
  interactive lines; fresh `UModule` + `UArena` per line. Implicit `|`
  statement terminator appended if missing.
- Source in `tools/urbi.c`, outside `src/` to preserve the `cc src/*.c`
  drop-in invariant. Never built on cross-compile targets.

### Formatter

- New library module `src/uvalue.{c,h}` — `UValue`-to-string formatter
  with Lua-5.4-style number formatting. Integer via `%lld`, Float via
  `%.14g` (f64) or `%.7g` (f32) with trailing `.0` appended on
  whole-number floats for visual kind-distinction. Bool, Nil, Str also
  covered (Bool/Str/Nil unreachable from M1 source; ship complete for
  M2+). Buffer-based, no allocation, thread-safe.
  `__STDC_HOSTED__`-gated — contributes no symbols under freestanding.

### Vendored

- `tools/linenoise.c` / `tools/linenoise.h` — single-file line editor
  from `github.com/antirez/linenoise` at commit
  `a15597057991fc748b3759cc66e157c9ea8bdfff`, BSD-2, preserved verbatim.
  Provenance ledger at `tools/LINENOISE-UPSTREAM.md`. Only linked into
  the `urbi` binary; never enters `liburbi.a`.

### Build

- New Makefile targets: `$(BUILDDIR)/urbi` (the REPL binary),
  `urbi-bin` (phony), `test-integration` (phony running the shell
  harness). `test` aggregate now depends on `test-integration`, so
  unit and integration run together under every sanitizer variant.
- `make tidy` scope widened to include `tools/urbi.c`.
  `tools/linenoise.c` stays outside the first-party tidy scope.
- `tools/linenoise.c` compiles with `-D_POSIX_C_SOURCE=200809L
  -D_XOPEN_SOURCE=700 -w` to suppress vendored-code warnings; `tools/urbi.c`
  compiles under the standard strict `$(CFLAGS)` discipline.

### Tests

- Added `.chk` conformance-fixture runner at `tests/integration/run_chk.sh`
  and the first fixture `tests/chk/arithmetic.chk` covering the M1 8-opcode
  VM. Folded into the `test` aggregate via a new `test-chk` Make target,
  so every sanitizer variant runs the fixture corpus automatically.
- `tests/unit/test_uvalue.c` — ~25 unit cases covering all 5 UValKinds,
  edge cases (INT64_MAX/MIN, -0.0, NaN, Inf, whole-number floats,
  scientific notation), and truncation (cap=0/1/3).
- `tests/integration/repl_smoke.sh` — POSIX sh harness covering every
  CLI mode and error path (30 cases). Runs against `$(BUILDDIR)/urbi`
  as part of `make test`.

### VM

- New module `src/uvm.{c,h}` implements the M1 register-based bytecode
  interpreter. Handles the 8-opcode M1 set (LOADK, MOVE, ADD, SUB, MUL,
  DIV, NEG, RET) with type-dispatched arithmetic per
  `docs/LANG-CONVENTIONS.md` §1.3: Int+Int wraps two's-complement,
  Int+Float promotes to Float, DIV always produces Float.
- Persistent `UVM` struct with `init`/`run`/`destroy` lifecycle and a
  VM-owned allocator hook distinct from the UModule loader's allocator.
  The 128-byte fixed error-message buffer carries
  `source:line:`-prefixed diagnostics for `UVM_TYPE_ERROR` and
  `UVM_OOM`; freestanding-compilable with no dependency on `<stdio.h>`
  / `<string.h>` / `<stdlib.h>` (stdlib-realloc shim is
  `__STDC_HOSTED__`-gated).
- Dispatch macros (`CASE` / `DISPATCH` / `NEXT`) expand to computed-goto
  under `__GNUC__` / `__clang__` and to `switch`/`case`/`continue`
  otherwise. Opcode bodies are written once; a new
  `URBI_VM_FORCE_SWITCH` build flag overrides the detection to exercise
  the switch path on GCC/Clang hosts.

### Tests (VM)

- `tests/unit/test_vm.c` — new test suite covering lifecycle,
  per-opcode happy paths, arithmetic type matrix, wrap semantics
  (INT64_MAX+1, INT64_MIN*-1, etc.), IEEE 754 DIV corners (±Inf, NaN),
  TypeError paths, OOM path, diagnostic prefix variants (`source:line:`,
  `line N:`, `instr N:`), and DiagWriter truncation. Coverage on
  `src/uvm.c` reaches 97% line.
- `tests/fuzz/fuzz_vm.c` — libFuzzer harness deserializing arbitrary
  bytes and executing any accepted module. 100K-iteration smoke run
  passes clean under ASan+UBSan.

### VM build and tooling

- New Make targets: `test-switch` (build with `-DURBI_VM_FORCE_SWITCH=1`
  for switch-dispatch CI parity) and `fuzz-vm` (libFuzzer harness
  build + run). `fuzz-build` aggregate extended to include `fuzz_vm`.
- `.clang-tidy` — `-clang-diagnostic-gnu-label-as-value` suppressed
  for the intentional GCC/Clang computed-goto extension.
- CI `host` job matrix extended with `test-switch`, bringing the matrix
  to 5 host modes.
- `docs/internals/design-decisions.md` — new entry explaining the
  uniform `UValue` tagged-struct decision across all targets.
- `docs/internals/architecture.md` — VM marked shipped; source table
  updated with `uvm.{c,h}` and `test_vm.c`.

### Documentation

- New `docs/` tree covering the first tranche of audience-A / audience-B
  / audience-C docs per the documentation-strategy design: `docs/README.md`
  (hub), `docs/language/getting-started.md`, `docs/internals/architecture.md`,
  `docs/internals/bytecode-format.md`, `docs/internals/opcodes.md`,
  `docs/internals/test-harness.md`, `docs/internals/design-decisions.md`.
  ~1500 lines of prose total.
- `docs/internals/test-harness.md` gains a Conformance fixtures section
  covering the `.chk` format, normalization rule, `make test-chk` entry
  point, and authoring flow.
- `docs/internals/test-harness.md` and `CONTRIBUTING.md`: acknowledge
  `make test-valgrind` as CI-gating (too slow for every commit, required
  before a milestone tag).
- `docs/internals/test-harness.md` Coverage expectations block replaces
  the stale `gcov`-on-debug-build language with `make coverage` (gcovr +
  HTML report + advisory CI job).
- `make docs-check` infrastructure + gating CI job: markdownlint-cli2
  over the `docs/` tree + intra-repo link-check.
- `.markdownlint.yaml` ships the ruleset (MD013 off; MD025/MD040/MD041
  on; neutral ordered-list-increment and heading-style).
- `WORKFLOW.md` §7 milestone ritual gains a "docs-for-this-release"
  step; §9 CHANGELOG cadence gains a `Documentation` subsection rule.

### Fixed

- `uvarint_decode_u` now rejects 10-byte encodings whose terminal-byte
  payload exceeds `0x01` as `UVARINT_OVERSIZE`. The previous code
  silently truncated values like `0x02..0x7F` at the 10th byte (payload
  bit shifts fall off the end of `uint64_t`), which could mask a
  corrupt bytecode module during loader verification. Paired
  positive-boundary test (`UINT64_MAX` at 10 bytes must succeed) added.
  The fix is defense-in-depth only — the loader verifier would have
  caught the resulting mis-decoded value downstream via `LOADK Bx`
  bounds or opcode range checks.
- `uvarint_size_zz` and `uvarint_write_zz` replace the
  implementation-defined `(v >> 63)` signed shift with a portable
  sign-extended mask built from `(v < 0)`. Equivalent on every
  mainstream compiler; defined by the C standard on all conforming
  implementations.

### Portability

- Compiler front-end compiles under `-ffreestanding` on toolchains without a C library (e.g. `gcc-riscv64-unknown-elf` on Ubuntu). `uarena_init` and the internal stdlib-backed allocator pair are guarded behind `__STDC_HOSTED__`; `uarena_alloc` uses a local byte-fill in place of `memset`. Freestanding callers must use `uarena_init_ex` or `uarena_init_static`.
- `umodule.c` and `uemit.c` follow the same freestanding discipline: local `module_zero` / `module_memcpy` / `module_memcmp` helpers in place of `<string.h>`, `stdlib_alloc` and `vsnprintf`-based `set_errmsg` guarded behind `__STDC_HOSTED__`, pluggable allocator on `UModule` via `UModuleAllocFn`. UModules hot-loaded in embedded builds (future M7) use caller-supplied allocators.

### Tooling

- Static-analysis Make targets: `tidy` (gating clang-tidy via `run-clang-tidy --warnings-as-errors='*'`), `tidy-fix` (local `--fix` convenience), `cppcheck` (advisory), `analyzer` (advisory GCC `-fanalyzer` in dedicated `build/host-analyzer/`), and `lint` aggregate.
- CI `lint` job runs all three analyzers parallel to host and cross-compile jobs. Advisory-ness of cppcheck and `-fanalyzer` lives in their Makefile targets' exit codes; CI job itself is gating.
- `.clang-tidy` disables `cert-err33-c`, `bugprone-easily-swappable-parameters`, and `readability-identifier-length` with per-check rationale comments — these stay disabled even if the broader check set is later expanded.
- Correctness-tooling Make targets: `coverage` (gcovr-backed coverage summary + HTML report at `build/host-coverage/report.html`), `test-valgrind` (memcheck-gated; catches uninitialized reads ASan misses), `fuzz-lex` and `fuzz-parse` (clang libFuzzer harnesses over lexer and parser, local-only). CI gains a gating `valgrind` job and an advisory `coverage` job; the advisory-to-gating promotion for coverage follows the cppcheck/analyzer pattern once the noise floor is known. Bench + profile harness deferred to M2-era paired work — see an internal backlog entry.

### Added

- Bytecode emitter walks AST nodes into a `UModule`: register-based instruction stream (byte-aligned 8/8/8/8 encoding), single tagged constant pool with linear-scan dedup, Lua-5.5-style delta-encoded synclines with absolute-line checkpoints, stack-discipline register allocator with destination-reuse. 8-opcode M1 set (`LOADK`, `MOVE`, `ADD`, `SUB`, `MUL`, `DIV`, `NEG`, `RET`). Reserved opcode slots 8–255 for M2+ additions (locals, control flow, calls, reactive primitives).
- `.urb` on-disk format: 24-byte header (magic `"URBI"` + 16·major+minor version + 6-byte FTP/paste-corruption canary + 8-byte flavor descriptor) followed by varint-delimited sections (metadata, constants, 4-byte-aligned instruction stream, delta synclines). Per-target flavor pinned at compile time (`URBI_INT_WIDTH` / `URBI_FLOAT_TYPE` / `URBI_INSTR_WIDTH` / `URBI_ENDIANNESS`); loader refuses mismatches with field-specific diagnostics.
- Loader verifier sweep after byte-level decode: opcode range, register range, `LOADK` Bx bounds, terminal `OP_RET`, abs-line pc monotonicity, 4-byte instruction alignment. `OP_RET` B operand and `OP_MOVE`/`OP_NEG` C operand intentionally not enforced (unused bytes, no runtime effect).
- UEmitter and module APIs in new headers `uemit.h` / `umodule.h`: `UEmitter` accumulator (init / statement / finish), `UModule` struct, `umodule_deserialize`, `umodule_serialize`, `uemit_disassemble`, error-name tables. Compiler-internal — `urbi.h` unchanged.
- Streaming Pratt parser consumes the lexer's token stream and produces one `UAstNode` per statement (integer literal, identifier, unary, binary, error). Recursive-descent statements + precedence climber for `+ - * /` with parens, unary `+ -` (plus is parse-time no-op), panic-mode recovery via `|`, in-stream `AST_ERROR` nodes, OOM sentinel path. Public parser API in `uparse.h`: `UParser`, `uparse_init`, `uparse_next_statement`, `uparse_error_name`.
- Internal chunk-list bump-allocator arena (`uarena.h` / `uarena.c`) backing the AST and emit arenas. Three init variants — `uarena_init` (stdlib), `uarena_init_ex` (pluggable allocator for embedded), `uarena_init_static` (fixed caller buffer for freestanding) — plus `uarena_alloc`, `uarena_reset`, `uarena_destroy`. No copy between chunks; pointers stable across growth.
- Lexer scans integer literals (decimal, hex, binary, octal with underscores), identifiers, single-character operators (`+ - * /`), parentheses, and the statement separator `|`. Full synclines on every token.
- Structured lexer error codes: unknown character, unterminated block comment, ambiguous leading zero, empty radix, malformed hex/binary/octal, leading/trailing/adjacent underscores, integer overflow.
- Public lexer API in new header `ulex.h`: `UToken`, `ULexer`, `ulex_init`, `ulex_next`, `ulex_token_name`. No allocation; caller owns source buffer.

### Refactoring

- Added the `U` prefix to every public struct and enum in the source tree so
  host embedders can include any header without type-name collisions. `Lexer`
  → `ULexer`, `Token` → `UToken`, `TokenType` → `UTokenType`,
  `LexErrorCode` → `ULexError`, `Parser` → `UParser`,
  `ParseErrorCode` → `UParseError`, `AstNode` → `UAstNode`,
  `AstKind` → `UAstKind`, `UnaryOp` → `UAstUnaryOp`,
  `BinaryOp` → `UAstBinaryOp`, `Arena` → `UArena`,
  `ArenaChunk` → `UArenaChunk`, `Emitter` → `UEmitter`,
  `EmitError` → `UEmitError`, `AbsLine` → `UAbsLine`. Error-type suffix
  normalized: `LexErrorCode` and `ParseErrorCode` drop the redundant `Code`
  suffix to match the existing `UVMError`/`UEmitError` pattern. Enum tag
  values (`TOK_*`, `AST_*`, `LEX_*`, `PARSE_*`, `EMIT_*`, `OP_*`, `UOP_*`,
  `BOP_*`) are unchanged — they are namespaced by prefix already, and
  renaming them risks cross-family collisions (e.g. `UOP_*` already denotes
  unary-op values, so opcode values can't take the same prefix). `src/uvarint.h`'s
  include guard normalized from `URBI_UVARINT_H` to `UVARINT_H` to match
  every other header.
- Bytecode `Chunk` renamed to `UModule` across the codebase, and the
  `src/uchunk.{c,h}` + `tests/unit/test_chunk.c` module renamed to
  `src/umodule.{c,h}` + `tests/unit/test_module.c`. The type is the
  compilation-unit record — instructions + constants + synclines +
  metadata — so the new name reflects what it actually is. `UChunkLoadError`
  → `UModuleLoadError`, `UChunkAllocFn` → `UModuleAllocFn`, `uchunk_*`
  → `umodule_*`. The `ULOAD_*` error tags and the arena's internal
  chunk-list terminology are unchanged.
- `UConst` renamed to `UValue` across sources, tests, and internals docs. The
  type has always been the universal tagged-value cell — constants-pool entry,
  register-frame slot, arithmetic operand, `uvm_run` result — so the new name
  reflects what it actually is. The `UValKind` enum and `UVAL_*` tags are
  unchanged (they already wore the `val` prefix); `uconst_to_double` /
  `uconst_set_float` become `uvalue_to_double` / `uvalue_set_float`.
- LEB128 varint encode/decode extracted into a standalone freestanding module
  `uvarint.{c,h}` with its own error enum (`UVarintError`). `umodule.c` now
  consumes it via two translation wrappers that map `UVarintError` into
  `UModuleLoadError` at the boundary; `uemit.c` drops the four private `static`
  varint helpers and consumes the module directly. The test-only header
  `src/umodule_internal.h` is retired; varint coverage moves into a new
  `test_varint_suite` (11 cases) that exercises encode and decode directly,
  replacing the indirect serialize→deserialize-only encode coverage of prior
  state.

### Foundation

- Header-only test harness `utest.h` (zero dependencies, pure C99)
- Make targets: `test`, `test-asan`, `test-ubsan`, `test-debug`, `cross-arm`, `cross-riscv`
- GitHub Actions CI covering host (debug/release/ASan/UBSan) plus ARM Cortex-M7 and RISC-V rv32imc cross-compiles
- Initial placeholder API: `urbi_version()`

### Build system

- Per-TARGET build directories: all variants (release, debug, sanitizers, cross-compiles) land in `build/<TARGET>/` and coexist without requiring `make clean` between them
- `make all` as the default target
- `make compile_commands.json` — generates a clangd-compatible compilation database for LSP-based editors

### Developer environment

- `.editorconfig` — universal indent, newline, and charset rules
- Extended `.gitignore` covering editor state (JetBrains, VS Code, Vim, Emacs, Sublime, TextMate), tag databases (ctags, cscope, GNU Global), and IDE indexing artifacts (`compile_commands.json`, `.cache/`)
- `CONTRIBUTING.md` documents test modes, cross-compile, indexing database, and TARGET convention
