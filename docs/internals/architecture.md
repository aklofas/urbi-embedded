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
     ▼  [ulex.c]     produces  Token stream with line/col synclines
     │
     ▼  [uparse.c]   produces  AstNode statements (UArena-allocated)
     │
     ▼  [uemit.c]    produces  Chunk (bytecode, constants, synclines, max_reg)
     │
     ▼  [uvm.c]      produces  result (UValue tagged value)
     │
     ▼  [urbi CLI]   prints    result to REPL output
```

The key invariant is that the hand-off between stages is a small, typed struct
— `Token`, `AstNode *`, `Chunk *`, `UValue` — not an implicit shared global.
Changing the emitter's register-allocation strategy does not touch the lexer.
Adding a new opcode to the VM does not touch the parser. The boundaries are
the design.

At the time of the v0.1.0-skeleton tag the VM subsystem is shipped; the REPL
is in active development. The architecture described here is the target shape
the v0.1.0-skeleton release implements.

---

## Lexer

**Source:** `src/ulex.c` / `src/ulex.h`

The lexer consumes a caller-owned, null-terminated-or-length-bounded source
buffer and produces a stream of `Token` values via repeated calls to
`ulex_next`. It performs no allocation of any kind: the `Lexer` struct is
stack-allocated by the caller and initialized with `ulex_init`; each `Token`
is returned by value.

Lexemes are zero-copy. When the lexer produces `TOK_IDENT`, the resulting
`Token` carries a `(const char *start, int len)` pair that points directly
into the caller's source buffer. The buffer must outlive the `Lexer` and any
`Token` derived from it. No copy is made.

Every `Token` carries its source position: `line` and `col` are 1-based,
matching the convention used throughout the pipeline. The `len` field records
the span in source bytes. This position information flows downstream into AST
nodes, then into the emitter's synclines, and ultimately into the `.urb`
bytecode as the delta-encoded line table — ensuring error messages from the VM
can name the original source line.

When the lexer encounters malformed input it returns a `TOK_ERROR` token
rather than aborting. The error variant carries a `LexErrorCode` and a static
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

The parser consumes a `Lexer` and a caller-provided `Arena`, and produces one
`AstNode *` per statement via `uparse_next_statement`. It is streaming: each
call parses exactly one statement and returns. The caller processes the tree,
then calls `uarena_reset` on the arena before the next statement. Long
programs never accumulate unbounded AST memory.

The `Parser` struct is stack-allocated by the caller and initialized with
`uparse_init`. It borrows both the `Lexer` and the `Arena`; both must outlive
the `Parser` and any `AstNode` returned from it.

### Expression parsing

Expressions use a Pratt-style precedence climber. Precedence levels are
statically encoded in the parser; at the walking-skeleton stage the hierarchy
is: additive (`+`, `-`) < multiplicative (`*`, `/`). Parentheses group via
standard recursive descent. Unary negation (`-`) is handled as a prefix
operator at the right-associative unary binding power.

The five `AstKind` values in the tagged union are:

| Kind | Active union field | Contents |
|---|---|---|
| `AST_INT` | `u.i` | Parsed `int64_t` value |
| `AST_IDENT` | `u.ident` | Zero-copy `(start, len)` into source buffer |
| `AST_UNARY` | `u.unary` | `UnaryOp` + pointer to operand node |
| `AST_BINARY` | `u.binary` | `BinaryOp` + pointers to left and right operand nodes |
| `AST_ERROR` | `u.err` | `ParseErrorCode` + static message string |

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
allocated `AST_ERROR` with code `PARSE_OOM`), which is a valid `AstNode *`
the caller can pass to the emitter without a null check. The emitter rejects
it with `EMIT_AST_ERROR`.

`uparse_next_statement` returns `NULL` at EOF; further calls are idempotent.

**Public API:** `uparse_init`, `uparse_next_statement`, `uparse_error_name`.

---

## Arena allocator

**Source:** `src/uarena.c` / `src/uarena.h`

The `Arena` is a chunk-list bump allocator used by the parser to allocate
`AstNode` trees and by the emitter's working memory. All allocations from a
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

Pointer stability: once an `AstNode *` is returned from `uarena_alloc`, it
remains valid at the same address until `uarena_reset` or `uarena_destroy`.
Chunk-list growth never moves existing allocations. This allows the emitter to
hold raw pointers into AST trees without any pinning protocol.

`uarena_alloc` zero-fills all returned memory and aligns to 16 bytes —
sufficient for `long double` and SIMD on all v1.0 targets.

---

## Emitter

**Source:** `src/uemit.c` / `src/uemit.h`

The emitter consumes an `AstNode` tree and writes bytecode into a `Chunk`.
It is initialized once per chunk with `uemit_init`, then driven with one
`uemit_statement(e, stmt)` call per top-level statement, and finalized with
`uemit_finish(e)`. After `uemit_finish`, the caller owns a fully populated
`Chunk` ready for the VM or for serialization.

The `Emitter` struct is stack-allocated by the caller. It borrows the `Chunk`
and the `Arena`; both must outlive the `Emitter`.

### Register allocation

The emitter uses a stack-discipline allocator: registers are assigned from
slot 0 upward as expression nodes are recursively compiled; when a subtree
is complete, its destination register slot is available for reuse by the
enclosing expression. This means the register count at any point equals the
depth of the expression tree, not the total number of nodes visited.

A sticky `max_reg_seen` watermark tracks the highest register index used.
After `uemit_finish`, this value is written into `chunk->max_reg`; the VM
allocates exactly `max_reg + 1` tagged-value slots — no waste, no guessing.

### Constant pool

Integer and float constants are stored in the chunk's constant pool, not
inlined into instructions. Before emitting a `LOADK` instruction the emitter
scans the existing pool for a duplicate; if found, it reuses the existing
index. The scan is linear, which is efficient for the constant-pool sizes that
arise in expression compilation. The 16-bit Bx field in `OP_LOADK` supports
up to 65 536 constants per chunk; see [opcodes.md](opcodes.md) for the
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

The `Emitter` maintains a sticky error field. The first error latches;
subsequent `uemit_statement` calls return the same error without touching the
`Chunk`. After `uemit_finish` the accumulated error is returned. Seven error
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
`uemit_error_name`, `uemit_disassemble`, `uchunk_serialize`.

---

## Chunk

**Source:** `src/uchunk.c` / `src/uchunk.h`

The `Chunk` is the interface between the front end (emitter) and the back end
(VM). It is a plain struct that carries five owned arrays:

- `instructions` — array of `uint32_t`, 4-byte aligned.
- `constants` — array of `UValue` (16-byte tagged-value records).
- `line_deltas` — array of `int8_t`, one per instruction.
- `abs_lines` — array of `(pc, line)` checkpoint records.
- `source_name` — null-terminated string, or `NULL` if absent.

Plus two scalar fields: `max_reg` (the highest register index, set at emit
time) and the pluggable allocator pair `(alloc_fn, alloc_ud)`.

The `Chunk` can be populated in two ways: by the emitter (in-process, no
serialize/deserialize round-trip) or by `uchunk_deserialize` (loading a
serialized `.urb` file). Both paths produce the same struct layout with the
same ownership contract — every field, including `source_name`, is allocated
through the chunk's own allocator and freed by `uchunk_destroy`. The
`uemit_init` `source_name` parameter is borrowed and copied into the chunk at
init time; the caller's string does not need to outlive the chunk. The VM
does not distinguish between the two population paths.

### On-disk format

`uchunk_serialize` (declared in `uemit.h`, implemented in `uemit.c`) writes
the `.urb` binary format: a 24-byte header carrying a magic number, a version
byte, a 6-byte FTP/paste canary, and an 8-byte flavor descriptor. The body
contains varint-prefixed sections for metadata, the constant pool, the
instruction stream (4-byte aligned), and the synclines. The complete format is
specified in [bytecode-format.md](bytecode-format.md).

### Loader and verifier

`uchunk_deserialize` both reads and verifies the byte stream. It checks the
header, all structural invariants (varint bounds, section counts, alignment
pad), and then sweeps every instruction to verify opcode range, register
range, `OP_LOADK` Bx bounds, and a terminal `OP_RET`. The full verification
checklist is in [bytecode-format.md](bytecode-format.md#loader-verification).

On any check failure, `uchunk_deserialize` stops, writes a diagnostic string
into the caller-supplied buffer, and returns a `UChunkLoadError` code.
`uchunk_load_error_name` maps codes to static strings for debug output.

### Pluggable allocator

The `Chunk` allocator follows realloc semantics: a single callback
`UChunkAllocFn` handles allocate, reallocate, and free based on whether `ptr`
and `nbytes` are null/zero. The callback and its `ud` cookie are stored on the
struct; `uchunk_destroy` frees all owned arrays through the same callback that
allocated them. This ensures that hosted targets using `stdlib` realloc and
embedded targets using a pool allocator each free through the allocator that
made the allocations. The design rationale is in
[design-decisions.md](design-decisions.md#pluggable-allocator-on-chunk-via-uchunkallocfn).

`uchunk_destroy` zeros the struct after freeing; it is safe to call on a
zero-initialized `Chunk`.

**Public API:** `uchunk_deserialize`, `uchunk_destroy`, `uchunk_load_error_name`.

---

## VM

**Source:** `src/uvm.c` / `src/uvm.h`

The VM is a register-based interpreter. It takes a populated `Chunk`,
allocates a register frame of `max_reg + 1` tagged-value slots, and dispatches
each instruction using a computed-goto table under GCC/Clang (`__GNUC__` /
`__clang__` detected at compile time) or a `switch`-based loop otherwise.
The `URBI_VM_FORCE_SWITCH` build flag overrides the detection to exercise the
switch path on GCC/Clang hosts; CI uses this flag in a dedicated `test-switch`
matrix entry to keep both paths compiling and passing.

Register values share the `UValue` layout from `src/uchunk.h`: 16 bytes
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

The chunk is consumed by reference; the VM does not own it and does not free
it. In-process use (REPL loop) passes the emitter's `Chunk` directly — no
serialize/deserialize round-trip is needed. An embedded host loading compiled
bytecode from flash calls `uchunk_deserialize` first, then hands the resulting
`Chunk` to the VM.

The persistent `UVM` struct supports an `init` / `run` / `destroy` lifecycle
and carries a VM-owned allocator hook (distinct from the `Chunk` loader's
allocator). A 128-byte fixed error-message buffer provides
`source:line:`-prefixed diagnostics for `UVM_TYPE_ERROR` and `UVM_OOM`
without depending on `<stdio.h>`, `<string.h>`, or `<stdlib.h>` (the
stdlib-realloc shim is `__STDC_HOSTED__`-gated).

---

## REPL

The `urbi` CLI binary drives the full pipeline in a loop. It supports three
modes:

- `-i` — interactive REPL: reads one line at a time from stdin, runs the
  pipeline, and prints the result.
- `-e <expr>` — evaluates a single expression string and exits.
- `-f <file>` — reads and evaluates a source file.

In all modes the pipeline is: `ulex_init` → `uparse_next_statement` loop →
`uemit_statement` loop → `uemit_finish` → VM dispatch → result print.
The REPL reuses a single `Arena` per line, calling `uarena_reset` after each
emitter pass to reclaim AST memory without a `destroy`/`init` cycle.

The `urbi` binary does not add any new language-level functionality; it is
purely a driver that wires the existing pipeline stages together with line
reading and result formatting.

At the time of the v0.1.0-skeleton tag the REPL binary is in active
development; it arrives as part of the same release that completes the VM.

---

## Concurrency, scheduler, GC

The walking-skeleton stage covers the arithmetic expression core. The language
features that make urbiscript distinctive — concurrent statement separators,
reactive watchers, and structured cancellation — are deferred to later
releases. See [ROADMAP.md](../ROADMAP.md) for the release sequence.

### Concurrency

urbiscript's statement separators encode concurrency:

- `;` — sequential with yield (one statement completes before the next
  starts, yielding to the scheduler between them).
- `|` — sequential atomic (no yield between statements).
- `,` — parallel fire-and-forget (both sides spawn immediately; caller does
  not wait).
- `&` — parallel join (both sides spawn; caller waits for both to complete).

`at (cond) body` registers a persistent watcher that fires whenever `cond`
transitions from false to true. `whenever` re-fires while `cond` remains
true. `every(100ms)` fires on a timer. `waituntil` blocks the current
coroutine until a condition holds.

First-class `Tag` objects group related watchers and coroutines; `tag.stop()`
cancels all activity under the tag. Tags carry `enter` and `leave` event
callbacks for RAII-style cleanup.

The cooperative coroutine scheduler and the reactive watcher registry both
land in a later release.

### Scheduler

The scheduler is cooperative and priority-aware. Coroutines yield at
statement boundaries; the scheduler picks the highest-priority runnable
coroutine at each yield. Real-time deadlines are expressed through tag
priorities and yield points, not through preemption. The scheduler is
designed to run without OS support; on FreeRTOS and bare-metal targets it is
called from the main loop (or from a timer ISR that pokes the tick counter)
rather than via any threading primitive.

The scheduler implementation lands in a later release.

### GC

The garbage collector is an incremental tri-color mark-and-sweep with
safe-point discipline. GC steps occur at statement boundaries — the same
yield points as the scheduler — so GC pauses are bounded by the cost of
processing one statement, not by the size of the live heap. Emergency GC
inside the allocator hot path is not performed; if a step-based GC cannot
keep pace, the next safe point runs a proportionally larger increment.

The choice of incremental over stop-the-world is motivated by the GC pause
target in [ROADMAP.md](../ROADMAP.md): ≤ 1 ms under a typical reactive
workload on 32-bit embedded hardware. A stop-the-world collector cannot
provide that guarantee without hard limits on live-set size; an incremental
collector provides it structurally by bounding each step.

The GC implementation lands in a later release.

---

## Source layout

```text
src/
  urbi.h              Public C embedding API (currently: urbi_version())
  urbi.c              Core implementation (minimal at walking-skeleton stage)
  ulex.h              Lexer API: Token, TokenType, LexErrorCode, Lexer
  ulex.c              Lexer implementation: ulex_init, ulex_next, ulex_token_name
  uast.h              AST node types: AstKind, AstNode, UnaryOp, BinaryOp, ParseErrorCode
  uarena.h            Arena allocator API: Arena, UAllocFn, UFreeFn
  uarena.c            Arena implementation: uarena_init, _ex, _static, alloc, reset, destroy
  uparse.h            Parser API: Parser
  uparse.c            Parser implementation: uparse_init, uparse_next_statement, uparse_error_name
  uchunk.h            Chunk struct, UValue, UOpcode, UValKind, instruction encode/decode helpers
  uchunk.c            Chunk deserializer, verifier, destroy: uchunk_deserialize, uchunk_destroy
  uvarint.h           LEB128 varint codec API: UVarintError, size/write/decode for u + zz
  uvarint.c           LEB128 varint implementation: pure byte math, freestanding-clean
  uemit.h             Emitter API: Emitter, EmitError; also declares uchunk_serialize
  uemit.c             Emitter implementation: uemit_init, uemit_statement, uemit_finish,
                      uemit_disassemble, uchunk_serialize
  uvm.h               VM API: UVM, UVMError, UValue, uvm_init, uvm_run, uvm_destroy
  uvm.c               VM implementation: computed-goto / switch dispatch, arithmetic
                      type matrix, TypeError/OOM diagnostics, syncline decoder

tests/unit/
  utest.h             Header-only test harness — see internals/test-harness.md
  runner.c            main() — calls each suite function in sequence
  test_lexer.c        Lexer test suite
  test_arena.c        Arena allocator test suite
  test_parser.c       Parser test suite
  test_varint.c       Varint codec test suite
  test_chunk.c        Chunk loader / verifier test suite
  test_emit.c         Emitter test suite
  test_vm.c           VM test suite
```
