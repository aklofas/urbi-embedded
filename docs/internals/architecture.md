# Architecture

## Pipeline overview

The urbi-embedded compiler and runtime are organized as a linear pipeline: a
source buffer enters at one end; a tagged-value result exits at the other.
Each stage is a single C translation unit. Each stage produces an owned data
structure that the next stage consumes. Ownership boundaries are explicit — no
stage calls directly into another stage's internals — which makes every stage
independently testable.

```text
source (const char *)
     │
     ▼  [ulex.c]     produces  UToken stream with line/col synclines
     │
     ▼  [uparse.c]   produces  UAstNode statements (UArena-allocated)
     │
     ▼  [uemit.c]    produces  UModule (bytecode, constants, synclines, max_reg)
     │
     ▼  [uvm.c]      produces  result (UValue tagged value)
     │
     ▼  [urbi CLI]   prints    result to REPL output
```

The key invariant is that the hand-off between stages is a small, typed struct
— `UToken`, `UAstNode *`, `UModule *`, `UValue` — not an implicit shared global.
Changing the emitter's register-allocation strategy does not touch the lexer.
Adding a new opcode to the VM does not touch the parser. The boundaries are
the design.

At the v0.1.0-skeleton tag all stages described here are shipped: lexer,
parser, arena, emitter, VM, formatter (`uvalue`), and the `urbi` REPL binary.
The architecture described here is the shape the v0.1.0-skeleton release
implements.

---

## Lexer

**Source:** `src/ulex.c` / `src/ulex.h`

The lexer consumes a caller-owned, null-terminated-or-length-bounded source
buffer and produces a stream of `UToken` values via repeated calls to
`ulex_next`. It performs no allocation of any kind: the `ULexer` struct is
stack-allocated by the caller and initialized with `ulex_init`; each `UToken`
is returned by value.

Lexemes are zero-copy. When the lexer produces `TOK_IDENT`, the resulting
`UToken` carries a `(const char *start, int len)` pair that points directly
into the caller's source buffer. The buffer must outlive the `ULexer` and any
`UToken` derived from it. No copy is made.

Every `UToken` carries its source position: `line` and `col` are 1-based,
matching the convention used throughout the pipeline. The `len` field records
the span in source bytes. This position information flows downstream into AST
nodes, then into the emitter's synclines, and ultimately into the `.urb`
bytecode as the delta-encoded line table — ensuring error messages from the VM
can name the original source line.

When the lexer encounters malformed input it returns a `TOK_ERROR` token
rather than aborting. The error variant carries a `ULexError` and a static
diagnostic string. The twelve error codes cover the observable failure modes:

- `LEX_UNKNOWN_CHAR` — a byte that begins no valid token.
- `LEX_UNTERMINATED_BLOCK_COMMENT` — `/*` with no matching `*/`.
- `LEX_AMBIGUOUS_LEADING_ZERO` — a bare leading zero where a radix prefix
  (`0x`, `0b`, `0o`) is required.
- `LEX_EMPTY_RADIX`, `LEX_MALFORMED_HEX`, `LEX_MALFORMED_BIN`,
  `LEX_MALFORMED_OCT` — invalid digits in a radix literal.
- `LEX_LEADING_UNDERSCORE`, `LEX_TRAILING_UNDERSCORE`,
  `LEX_ADJACENT_UNDERSCORES` — underscore separator rules.
- `LEX_INT_OVERFLOW` — integer literal exceeds `INT64_MAX`.

After a `TOK_ERROR` the cursor has advanced past the offending byte; the
caller may continue lexing for error recovery.

Eleven token types cover the walking-skeleton grammar: `TOK_EOF`, `TOK_INT`,
`TOK_IDENT`, `TOK_PLUS`, `TOK_MINUS`, `TOK_STAR`, `TOK_SLASH`,
`TOK_LPAREN`, `TOK_RPAREN`, `TOK_PIPE` (the statement separator `|`), and
`TOK_ERROR`.

**Public API:** `ulex_init`, `ulex_next`, `ulex_token_name`.

---

## Parser

**Source:** `src/uparse.c` / `src/uparse.h` / `src/uast.h`

The parser consumes a `ULexer` and a caller-provided `UArena`, and produces one
`UAstNode *` per statement via `uparse_next_statement`. It is streaming: each
call parses exactly one statement and returns. The caller processes the tree,
then calls `uarena_reset` on the arena before the next statement. Long
programs never accumulate unbounded AST memory.

The `UParser` struct is stack-allocated by the caller and initialized with
`uparse_init`. It borrows both the `ULexer` and the `UArena`; both must outlive
the `UParser` and any `UAstNode` returned from it.

### Expression parsing

Expressions use a Pratt-style precedence climber. Precedence levels are
statically encoded in the parser; at the walking-skeleton stage the hierarchy
is: additive (`+`, `-`) < multiplicative (`*`, `/`). Parentheses group via
standard recursive descent. Unary negation (`-`) is handled as a prefix
operator at the right-associative unary binding power.

The five `UAstKind` values in the tagged union are:

| Kind | Active union field | Contents |
|---|---|---|
| `AST_INT` | `u.i` | Parsed `int64_t` value |
| `AST_IDENT` | `u.ident` | Zero-copy `(start, len)` into source buffer |
| `AST_UNARY` | `u.unary` | `UAstUnaryOp` + pointer to operand node |
| `AST_BINARY` | `u.binary` | `UAstBinaryOp` + pointers to left and right operand nodes |
| `AST_ERROR` | `u.err` | `UParseError` + static message string |

Position fields `line` and `col` are 1-based on every node, matching the
lexer. For `AST_BINARY` the position points at the operator token; for
`AST_ERROR` it points at the detection site.

### Parse error handling

On a parse error the parser builds an `AST_ERROR` node at the detection
point, discards any partial subtree, and advances the lexer past the next
`TOK_PIPE` or to EOF — panic-mode recovery via the statement separator. The
next `uparse_next_statement` call starts cleanly from the following token.
Callers that want fine-grained error recovery inspect nodes of kind
`AST_ERROR` and continue the parse loop; callers that treat any error as
fatal inspect the `kind` field and stop.

On allocator exhaustion the function returns an OOM sentinel (a statically
allocated `AST_ERROR` with code `PARSE_OOM`), which is a valid `UAstNode *`
the caller can pass to the emitter without a null check. The emitter rejects
it with `EMIT_AST_ERROR`.

`uparse_next_statement` returns `NULL` at EOF; further calls are idempotent.

**Public API:** `uparse_init`, `uparse_next_statement`, `uparse_error_name`.

---

## Arena allocator

**Source:** `src/uarena.c` / `src/uarena.h`

The `UArena` is a chunk-list bump allocator used by the parser to allocate
`UAstNode` trees and by the emitter's working memory. All allocations from a
given arena are freed together — there is no per-node `free`. This matches
the pipeline's access pattern: parse a statement, emit it, reset the arena,
repeat.

Three initialization modes share the same `uarena_alloc`, `uarena_reset`, and
`uarena_destroy` operations:

- **`uarena_init`** (hosted only, guarded by `__STDC_HOSTED__`) — uses
  `stdlib` `malloc` / `free` for backing allocation. Zero arguments beyond
  the optional `chunk_size` hint; appropriate for development builds, the
  CLI, and hosted-platform embeddings.

- **`uarena_init_ex`** — takes a `chunk_size` hint and a pluggable
  `(UAllocFn, UFreeFn, void *ud)` allocator pair. The host supplies any
  allocator pair that respects the signatures; the arena never calls `malloc`
  directly. Used at runtime init when the host registers a custom allocator via
  the C API.

- **`uarena_init_static`** — takes a fixed caller-owned buffer. No dynamic
  allocation is ever performed; the arena issues `OOM` when the buffer is
  exhausted. `uarena_destroy` is a no-op. Used on freestanding targets
  (Cortex-M, RV32) where there is no heap at all.

Pointer stability: once an `UAstNode *` is returned from `uarena_alloc`, it
remains valid at the same address until `uarena_reset` or `uarena_destroy`.
UModule-list growth never moves existing allocations. This allows the emitter to
hold raw pointers into AST trees without any pinning protocol.

`uarena_alloc` zero-fills all returned memory and aligns to 16 bytes —
sufficient for `long double` and SIMD on all v1.0 targets.

---

## Emitter

**Source:** `src/uemit.c` / `src/uemit.h`

The emitter consumes an `UAstNode` tree and writes bytecode into a `UModule`.
It is initialized once per module with `uemit_init`, then driven with one
`uemit_statement(e, stmt)` call per top-level statement, and finalized with
`uemit_finish(e)`. After `uemit_finish`, the caller owns a fully populated
`UModule` ready for the VM or for serialization.

The `UEmitter` struct is stack-allocated by the caller. It borrows the `UModule`
and the `UArena`; both must outlive the `UEmitter`.

### Register allocation

The emitter uses a stack-discipline allocator: registers are assigned from
slot 0 upward as expression nodes are recursively compiled; when a subtree
is complete, its destination register slot is available for reuse by the
enclosing expression. This means the register count at any point equals the
depth of the expression tree, not the total number of nodes visited.

A sticky `max_reg_seen` watermark tracks the highest register index used.
After `uemit_finish`, this value is written into `module->max_reg`; the VM
allocates exactly `max_reg + 1` tagged-value slots — no waste, no guessing.

### Constant pool

Integer and float constants are stored in the module's constant pool, not
inlined into instructions. Before emitting a `LOADK` instruction the emitter
scans the existing pool for a duplicate; if found, it reuses the existing
index. The scan is linear, which is efficient for the constant-pool sizes that
arise in expression compilation. The 16-bit Bx field in `OP_LOADK` supports
up to 65 536 constants per module; see [opcodes.md](opcodes.md) for the
encoding.

### Synclines

The emitter tracks source line numbers via a Lua-5.5-style delta encoding.
One signed byte is emitted per instruction: a delta from the previous
instruction's source line. When the delta would overflow an `int8_t`, the
value `INT8_MIN` is emitted as a sentinel and an absolute-line checkpoint is
written into a parallel table. The result is a compact per-instruction line
table with a constant one-byte overhead per instruction and bounded overhead
for absolute checkpoints. See [bytecode-format.md](bytecode-format.md#synclines-delta-encoding)
for the full encoding specification.

### Emit error handling

The `UEmitter` maintains a sticky error field. The first error latches;
subsequent `uemit_statement` calls return the same error without touching the
`UModule`. After `uemit_finish` the accumulated error is returned. Seven error
codes cover the observable failure modes: `EMIT_OOM`, `EMIT_AST_ERROR`,
`EMIT_UNSUPPORTED_AST`, `EMIT_REG_EXHAUSTED`, `EMIT_CONSTANT_POOL_FULL`,
`EMIT_LINE_OVERFLOW`, and `EMIT_FINISHED`.

### Opcode set

The eight opcodes at the walking-skeleton stage are described in full in
[opcodes.md](opcodes.md). Summary:

| Opcode | Form | Semantics |
|--------|------|-----------|
| `OP_LOADK` | ABx | `R[A] := K[Bx]` |
| `OP_MOVE` | ABC | `R[A] := R[B]` |
| `OP_ADD` | ABC | `R[A] := R[B] + R[C]` |
| `OP_SUB` | ABC | `R[A] := R[B] - R[C]` |
| `OP_MUL` | ABC | `R[A] := R[B] * R[C]` |
| `OP_DIV` | ABC | `R[A] := R[B] / R[C]` (always Float) |
| `OP_NEG` | ABC | `R[A] := -R[B]` |
| `OP_RET` | ABC | `return R[A]` |

**Public API:** `uemit_init`, `uemit_statement`, `uemit_finish`,
`uemit_error_name`, `uemit_disassemble`, `umodule_serialize`.

---

## UModule

**Source:** `src/umodule.c` / `src/umodule.h`

The `UModule` is the interface between the front end (emitter) and the back end
(VM). It is a plain struct that carries five owned arrays:

- `instructions` — array of `uint32_t`, 4-byte aligned.
- `constants` — array of `UValue` (16-byte tagged-value records).
- `line_deltas` — array of `int8_t`, one per instruction.
- `abs_lines` — array of `(pc, line)` checkpoint records.
- `source_name` — null-terminated string, or `NULL` if absent.

Plus two scalar fields: `max_reg` (the highest register index, set at emit
time) and the pluggable allocator pair `(alloc_fn, alloc_ud)`.

The `UModule` can be populated in two ways: by the emitter (in-process, no
serialize/deserialize round-trip) or by `umodule_deserialize` (loading a
serialized `.urb` file). Both paths produce the same struct layout with the
same ownership contract — every field, including `source_name`, is allocated
through the module's own allocator and freed by `umodule_destroy`. The
`uemit_init` `source_name` parameter is borrowed and copied into the module at
init time; the caller's string does not need to outlive the module. The VM
does not distinguish between the two population paths.

### On-disk format

`umodule_serialize` (declared in `uemit.h`, implemented in `uemit.c`) writes
the `.urb` binary format: a 24-byte header carrying a magic number, a version
byte, a 6-byte FTP/paste canary, and an 8-byte flavor descriptor. The body
contains varint-prefixed sections for metadata, the constant pool, the
instruction stream (4-byte aligned), and the synclines. The complete format is
specified in [bytecode-format.md](bytecode-format.md).

### Loader and verifier

`umodule_deserialize` both reads and verifies the byte stream. It checks the
header, all structural invariants (varint bounds, section counts, alignment
pad), and then sweeps every instruction to verify opcode range, register
range, `OP_LOADK` Bx bounds, and a terminal `OP_RET`. The full verification
checklist is in [bytecode-format.md](bytecode-format.md#loader-verification).

On any check failure, `umodule_deserialize` stops, writes a diagnostic string
into the caller-supplied buffer, and returns a `UModuleLoadError` code.
`umodule_load_error_name` maps codes to static strings for debug output.

### Pluggable allocator

The `UModule` allocator follows realloc semantics: a single callback
`UModuleAllocFn` handles allocate, reallocate, and free based on whether `ptr`
and `nbytes` are null/zero. The callback and its `ud` cookie are stored on the
struct; `umodule_destroy` frees all owned arrays through the same callback that
allocated them. This ensures that hosted targets using `stdlib` realloc and
embedded targets using a pool allocator each free through the allocator that
made the allocations. The design rationale is in
[design-decisions.md](design-decisions.md#pluggable-allocator-on-umodule-via-umoduleallocfn).

`umodule_destroy` zeros the struct after freeing; it is safe to call on a
zero-initialized `UModule`.

**Public API:** `umodule_deserialize`, `umodule_destroy`, `umodule_load_error_name`.

---

## VM

**Source:** `src/uvm.c` / `src/uvm.h`

The VM is a register-based interpreter. It takes a populated `UModule`,
allocates a register frame of `max_reg + 1` tagged-value slots, and dispatches
each instruction using a computed-goto table under GCC/Clang (`__GNUC__` /
`__clang__` detected at compile time) or a `switch`-based loop otherwise.
The `URBI_VM_FORCE_SWITCH` build flag overrides the detection to exercise the
switch path on GCC/Clang hosts; CI uses this flag in a dedicated `test-switch`
matrix entry to keep both paths compiling and passing.

Register values share the `UValue` layout from `src/umodule.h`: 16 bytes
per slot, with a `kind` byte (`UValKind`) discriminating Integer, Float, Bool,
String, or Nil, 7 bytes of alignment padding, and an 8-byte value union
(`int64_t i` for Integer; `double` or `float f` for Float, selected by
`URBI_FLOAT_TYPE` at compile time).

Arithmetic dispatch follows the rules in
[LANG-CONVENTIONS.md §1.3](../LANG-CONVENTIONS.md#13-arithmetic-semantics):

- `OP_ADD`, `OP_SUB`, `OP_MUL`: Integer op Integer yields Integer (wrapping
  on overflow); Integer op Float, or Float op Float, promotes to Float.
- `OP_DIV`: always yields Float, regardless of operand types. `3 / 2` is
  `1.5`, never `1`.
- `OP_NEG`: negates the register value; preserves type (Integer stays Integer,
  Float stays Float).

`OP_RET` terminates dispatch and returns the tagged value from the named
register to the caller.

The module is consumed by reference; the VM does not own it and does not free
it. In-process use (REPL loop) passes the emitter's `UModule` directly — no
serialize/deserialize round-trip is needed. An embedded host loading compiled
bytecode from flash calls `umodule_deserialize` first, then hands the resulting
`UModule` to the VM.

The persistent `UVM` struct supports an `init` / `run` / `destroy` lifecycle
and carries a VM-owned allocator hook (distinct from the `UModule` loader's
allocator). A 128-byte fixed error-message buffer provides
`source:line:`-prefixed diagnostics for `UVM_TYPE_ERROR` and `UVM_OOM`
without depending on `<stdio.h>`, `<string.h>`, or `<stdlib.h>` (the
stdlib-realloc shim is `__STDC_HOSTED__`-gated).

---

## REPL

**Source:** `tools/urbi.c`

The `urbi` CLI binary is the first end-user-visible consumer of the full
pipeline. It drives the pipeline in a loop and supports five modes:

- `-i` — interactive REPL: reads one line at a time via vendored linenoise,
  runs the pipeline, prints `[%08u] value` timestamp frames (wall-clock
  milliseconds via `clock_gettime(CLOCK_MONOTONIC, …)`), and persists
  history to `~/.urbi_history`.
- `-e <expr>` — evaluates a single expression string and exits.
- `[-f] <file>` / positional file argument — reads and evaluates a source
  file; no per-statement print (Unix script convention).
- `--dump-bytecode` — disassembles compiled bytecode via `uemit_disassemble`
  (incompatible with `-i`).
- `--version` / `--help` — print version or usage and exit.

In all evaluation modes the pipeline is: `ulex_init` → `uparse_next_statement`
loop → `uemit_statement` loop → `uemit_finish` → VM dispatch → result print.
The `UVM` is persistent across interactive lines. A fresh `UModule` and
`UArena` is allocated per line; `uarena_reset` reclaims AST memory after each
emitter pass without a `destroy`/`init` cycle. An implicit `|` statement
terminator is appended if the input line is missing one.

Result formatting is handled by `src/uvalue.{c,h}` (the `uvalue_format`
function), which is a separate hosted-only library module — not part of
`tools/urbi.c` itself. This keeps the formatter testable in isolation and
available to future embedding scenarios (e.g. a debugger or a remote REPL
over a byte-stream transport).

The `urbi` binary lives in `tools/` rather than `src/` to preserve the
`cc src/*.c` drop-in invariant. It is never built for cross-compile targets.
The factoring of the per-line eval logic into a reusable `urbi_repl_eval_line`
function in `src/urepl.{c,h}` is a scheduled future refactor (see the
"Embedded REPL over UART / byte-stream transports" backlog entry); at the
v0.1.0-skeleton tag the logic lives inline in `tools/urbi.c`.

---

## Runtime subsystems

The pipeline above produces and runs bytecode for the arithmetic
expression core. Beyond that, several runtime subsystems collaborate to
implement the language features that make urbiscript distinctive —
concurrency, the prototype object model, reactive watchers, garbage
collection, and the realm + module-instance system. Each has a dedicated
deep-dive doc; this section is the orientation map.

### Concurrency

urbiscript's statement separators encode concurrency:

- `;` — sequential with yield (one statement completes before the next
  starts, yielding to the scheduler between them).
- `|` — sequential atomic (no yield between statements).
- `,` — parallel fire-and-forget (both sides spawn immediately; caller does
  not wait).
- `&` — parallel join (both sides spawn; caller waits for both to complete).

The runtime ships a cooperative scheduler (`URBI_SCHED_COOPERATIVE`) as
the `v1.0` baseline. Every running coroutine is a `UStrand` that holds
its own register window, instruction pointer, and trace state; the
scheduler walks a priority-aware ready queue and yields control at
statement-separator boundaries. An ISR-safe SPSC event ring buffers
events from interrupt context for drain at the next safe point. The
scheduler determinism gate runs three configurations × 100 iterations
on every release. See [Scheduler design](scheduler-design.md) for the
full contract.

First-class `Tag` objects group related watchers and coroutines;
`tag.stop()` cancels all activity under the tag. Tags carry `enter`
and `leave` event callbacks for RAII-style cleanup.

### Object model

Objects are prototype-based with hidden-class slot layout. A `UObject`
header (56 B host, 48 B 32-bit embedded, both pinned by
`_Static_assert`) points at a `UShape` describing its slot layout, plus
a tagged-pointer prototype chain (three forms: zero-proto, single-proto,
multi-proto). Slot lookup goes through a 4-entry-per-call-site inline
cache (2 entries on the embedded-footprint preset). Shape transitions
are interned through a per-VM `UShapeMap` so that two objects that have
evolved through the same series of slot adds share identity. See
[Object model](object-model.md) for the layout, IC design, and the nine
atom-family singletons.

### Reactive runtime

`at (cond) body` registers a persistent watcher that fires whenever
`cond` transitions from false to true. `whenever` re-fires while `cond`
remains true. `every(100ms)` fires on a timer. `waituntil` blocks the
current coroutine until a condition holds.

Reactive constructs compile to install opcodes that build watchers on
the heap. Watchers fire from three safe-point families: condition-dirty
re-evaluation, slot-change events, and explicit emit (`E.emit(...)`).
The emit pipeline routes every sync-execution site through a single
primitive — `urbi_run_closure_on_scratch` — that spins up an ephemeral
strand for the body closure and tears it down on completion. See
[Reactive runtime](reactive-runtime.md) for the full lifecycle, the
ownership flags that govern watcher teardown, and the freereg/next_reg
sync rubric.

### Garbage collection

The runtime uses incremental tri-color mark-sweep
(`URBI_GC_INCREMENTAL`) with a no-GC build (`URBI_GC_NONE`) carried
through CI for the smallest embedded footprints. Write barriers fire on
slot stores and other heap-pointer mutations; safe points are
statement-separator boundaries plus explicit `urbi_gc_slice()` calls in
embedded driver loops. The strand-walker traverses live coroutines from
realm hierarchy roots so that a single GC pass sees all reachable
strand state. Pause budget is ≤2.1 µs measured against a 1 ms target.
See [GC](gc.md) for the cell-type inventory, gc_byte bit layout, and
the realm-hierarchy walker contract.

### Realm and module instances

A realm holds the top-level globals plus the (vm, module) → instance
cache. `urbi_run_chunk` and `urbi_vm_run` automatically bind a
`UModuleInstance` for the realm at first invocation, lazily interning
the IC name table and threading `proto_instances` through the call
frame for `UClosure.proto_inst` access. The walk-then-prepend protocol
on the cache is correct under the single-threaded-VM assumption that
defines the `v1.0` baseline. See [Realm and modules](realm-and-modules.md)
for the load contract, the lazy-intern protocol, and the multi-threaded
deferrals.

---

## Source layout

```text
src/
  urbi.h              Public C embedding API (currently: urbi_version())
  urbi.c              Core implementation (minimal at walking-skeleton stage)
  ulex.h              Lexer API: UToken, UTokenType, ULexError, ULexer
  ulex.c              Lexer implementation: ulex_init, ulex_next, ulex_token_name
  uast.h              AST node types: UAstKind, UAstNode, UAstUnaryOp, UAstBinaryOp, UParseError
  uarena.h            Arena allocator API: UArena, UAllocFn, UFreeFn
  uarena.c            Arena implementation: uarena_init, _ex, _static, alloc, reset, destroy
  uparse.h            Parser API: UParser
  uparse.c            Parser implementation: uparse_init, uparse_next_statement, uparse_error_name
  umodule.h           UModule struct, UValue, UOpcode, UValKind, instruction encode/decode helpers
  umodule.c           UModule deserializer, verifier, destroy: umodule_deserialize, umodule_destroy
  uvarint.h           LEB128 varint codec API: UVarintError, size/write/decode for u + zz
  uvarint.c           LEB128 varint implementation: pure byte math, freestanding-clean
  uemit.h             Emitter API: UEmitter, UEmitError; also declares umodule_serialize
  uemit.c             Emitter implementation: uemit_init, uemit_statement, uemit_finish,
                      uemit_disassemble, umodule_serialize
  uvm.h               VM API: UVM, UVMError, UValue, uvm_init, uvm_run, uvm_destroy
  uvm.c               VM implementation: computed-goto / switch dispatch, arithmetic
                      type matrix, TypeError/OOM diagnostics, syncline decoder
  uvalue.h            UValue-to-string formatter API: uvalue_format
  uvalue.c            Formatter implementation (hosted only, __STDC_HOSTED__-gated):
                      Lua-5.4-style number formatting for all 5 UValKinds

tools/
  urbi.c              REPL binary — the first end-user-visible consumer of the full
                      pipeline. Five modes: -i (interactive), -e, -f / positional
                      file, --dump-bytecode, --version / --help. Not part of
                      liburbi.a; never built on cross-compile targets.
  linenoise.h         Vendored line editor header (BSD-2, antirez/linenoise)
  linenoise.c         Vendored line editor implementation; see LINENOISE-UPSTREAM.md

tests/unit/
  utest.h             Header-only test harness — see internals/test-harness.md
  runner.c            main() — calls each suite function in sequence
  test_lexer.c        Lexer test suite
  test_arena.c        Arena allocator test suite
  test_parser.c       Parser test suite
  test_varint.c       Varint codec test suite
  test_module.c       UModule loader / verifier test suite
  test_emit.c         Emitter test suite
  test_vm.c           VM test suite
  test_uvalue.c       UValue formatter test suite

tests/integration/
  repl_smoke.sh       POSIX sh harness covering every CLI mode and error path
```

---

## Multi-VM model

Multiple `UVM` instances may coexist in the same process. Each is fully
independent: no mutable state is shared across VMs.

```text
Process
  ├── UVM (A)
  │     ├── intern_table   (per-VM string interning pool, ustr_intern)
  │     ├── topology_gen   (per-VM IC invalidation counter)
  │     └── UModule.origin_vm → (A)   stamped at compile time
  └── UVM (B)
        ├── intern_table
        ├── topology_gen
        └── UModule.origin_vm → (B)
```

**Per-VM state catalog.** Every mutable datum lives on the `UVM` struct.
At v0.2.0-expressions this includes `intern_table` (the string interning
pool) and `topology_gen` (the inline-cache invalidation generation counter).
As additional subsystems land (GC, scheduler, coroutine stacks, reactive
registry) their state will extend `UVM`, not introduce new file-scope
variables.

**Allowed-immutable globals.** Only compile-time constant tables — opcode
name arrays, version strings, static error messages — may live at file
scope. No mutable file-scope variables are permitted; enforcement is via
the `cppcoreguidelines-avoid-non-const-global-variables` clang-tidy check
(gated under `make lint`) and the `tools/audit-globals.sh` script, which
scans the source tree for non-const file-scope definitions.

**Single-threaded per VM.** Each `UVM` is driven by one thread at a time.
Multiple `UVM` instances may run in separate threads without
synchronization; cross-VM value handoff is not supported in v1.0 and is
undefined behavior. The multi-threaded-per-VM and shared-immutable-bytecode-pool
paths are deferred to v1.x. See [`internals/design-decisions.md` — No global
mutable state](design-decisions.md#no-global-mutable-state) for the rationale
and the 8-case test matrix.
