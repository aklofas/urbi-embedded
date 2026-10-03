# Architecture

## Pipeline overview

The urbi-embedded compiler and runtime are organized as a linear pipeline: a
source buffer enters at one end; a tagged-value result exits at the other.
Each stage is a small cluster of C translation units under its own
`src/<stage>/` directory. Each stage produces an owned data structure that
the next stage consumes. Ownership boundaries are explicit — no stage calls
directly into another stage's internals — which makes every stage
independently testable. `src/emit/ufront.c` is the one function
(`ufront_compile`) the runtime core calls to run all three front-end stages
without touching any of their internals; see [Emitter](#emitter) below.

```text
source (const char *)
     │
     ▼  [src/lex/]    produces  UToken stream with line/col synclines
     │
     ▼  [src/parse/]  produces  UAstNode statements (UArena-allocated), every
     │                 desugar already applied — 32 AST kinds, none of which
     │                 need a second lowering pass in the emitter
     │
     ▼  [src/emit/]   produces  compiled chunk (root UProto: bytecode, constants,
     │                 synclines, site names, max_reg) — wire v2.0, 41 opcodes
     │
     ▼  [rt/uexec_ops.c] produces  result (UValue tagged value)
     │
     ▼  [urbi CLI]   prints    result to REPL output
```

The key invariant is that the hand-off between stages is a small, typed struct
— `UToken`, `UAstNode *`, `UProto *` (the root chunk), `UValue` — not an implicit shared global.
Changing the emitter's register-allocation strategy does not touch the lexer.
Adding a new opcode to the VM does not touch the parser. The boundaries are
the design.

This document covers the front half — everything that turns text into a
chunk. The back half is [runtime.md](runtime.md).

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

**Source:** `src/parse/uparse.c` (+ `uparse_stmt.c`, `uparse_expr.c`,
`uparse_react.c`, `uparse_separators.c`, `uparse_top.c`, `uparse_desugar.c`)
/ `src/parse/uparse.h` / `src/parse/uast.h`

The parser consumes a `ULexer` and a caller-provided `UArena`, and produces one
`UAstNode *` per statement via `uparse_next_statement`. It is streaming: each
call parses exactly one statement and returns. The caller processes the tree,
then calls `uarena_reset` on the arena before the next statement. Long
programs never accumulate unbounded AST memory.

The `UParser` struct is stack-allocated by the caller and initialized with
`uparse_init`. It borrows both the `ULexer` and the `UArena`; both must outlive
the `UParser` and any `UAstNode` returned from it.

**The parser does every desugar.** A class declaration, a `detach`/`disown`
form, a list or dict literal, a subscript, a `for`/`switch`/logical-operator
construct — every one of these lowers to a simpler `UAstNode` shape (a
`BLOCK` with a hidden local, a chained stdlib call, a chain of `if`s, and so
on) before the emitter ever sees it. The emitter therefore never desugars:
it is a mechanical AST-to-bytecode pass over a fixed, already-simplified set
of 32 live `UAstKind` values (down from 49 before this split). The full enum,
with each kind's payload and the watcher-mode/separator discriminators it
carries, is the comment block in `src/parse/uast.h`; it is not duplicated
here because it is exactly as authoritative there and changes with every
language feature.

### Expression parsing

Expressions use a Pratt-style precedence climber; precedence levels are
statically encoded in the parser. Parentheses group via standard recursive
descent. Unary negation (`-`) is handled as a prefix operator at the
right-associative unary binding power.

Position fields `line` and `col` are 1-based on every node, matching the
lexer. For a binary-operator node the position points at the operator
token; for `AST_ERROR` it points at the detection site.

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
Arena growth never moves existing allocations. This allows the emitter to
hold raw pointers into AST trees without any pinning protocol.

`uarena_alloc` zero-fills all returned memory and aligns to 16 bytes —
sufficient for `long double` and SIMD on all v1.0 targets.

---

## Emitter

**Source:** `src/emit/uemit.c` (+ `uemit_ctrl.c`, `uemit_diag.c`,
`uemit_disasm.c`, `uemit_expr.c`, `uemit_react.c`, `uemit_reg.c`,
`uemit_scope.c`, `uemit_serialize.c`) / `src/emit/uemit.h` /
`src/emit/uemit_internal.h` / `src/emit/ufront.c` + `ufront.h`

The runtime core never touches `ULexer`, `UParser` or `UEmitter` directly.
`ufront_compile` (`src/emit/ufront.h`) is the single entry point: lex, parse
and emit behind one function, so the frontend can be swapped or stripped
(`URBI_BYTECODE_ONLY`) without the core noticing. Underneath, a driver
creates a `UEmitter` with `uemit_new`, feeds it one `uemit_statement(e,
stmt)` call per top-level statement — AST already fully desugared by the
parser, so emit is purely mechanical AST-to-bytecode — and finishes it with
`uemit_finish(e)`, which returns a fully populated chunk ready for the VM or
for serialization. The `UEmitter` struct is heap-allocated through the
arena's allocator and is opaque outside `src/emit/`.

### Register allocation: one cursor, pins

Every function has exactly one register cursor, `freereg` — not a separate
temporary-stack and scratch-frame pair, which is what every emitter before
this rewrite used. Locals (named variables, and nameless **pins**) are
declared at `freereg` and own their register until their scope ends; the
newest local's register plus one is `ureg_top`. Everything from `ureg_top`
up to `freereg` is the temporaries of the statement currently compiling, and
every statement finishes with `freereg == ureg_top`.

A **pin** is how a value survives a nested statement without becoming a
named local: pinning turns a temporary into a nameless local, so a nested
statement's own register resets land above it with no arithmetic needed to
protect it. This is what replaced the old "adopt every live temporary as a
hidden declared local at block entry" workaround — the earlier emitters
needed that trick (and its own bugs) because they had no single cursor to
pin against.

`max_reg` tracks the highest register index any statement has used. After
`uemit_finish`, this value is written into the chunk's `max_reg`; the VM
allocates exactly `max_reg + 1` tagged-value slots.

### Constant pool

Integer, float and string constants are stored in the proto's constant pool,
not inlined into instructions. Before emitting a `LOADK` instruction the
emitter scans the existing pool for a duplicate and reuses the index if
found. The 16-bit `Bx` field in `OP_LOADK` supports up to 65,536 constants
per proto; see [opcodes.md](opcodes.md) for the encoding.

### Synclines

The emitter tracks source line numbers via a delta encoding: one signed byte
per instruction, a delta from the previous instruction's source line. When
the delta would overflow an `int8_t`, the value `INT8_MIN` is emitted as a
sentinel and an absolute-line checkpoint is written into a parallel table.
See [bytecode-format.md](bytecode-format.md#synclines-delta-encoding) for
the full encoding specification.

### Emit error handling

The `UEmitter` maintains a sticky error field. The first error latches;
subsequent `uemit_statement` calls return the same error without touching
the chunk. 17 error codes (`EMIT_OOM` through `EMIT_JUMP_TOO_FAR`) cover the
observable failure modes, including the language-level ones the parser
leaves for the emitter to catch: `lazy` used on a method or assigned to a
lazy parameter (`EMIT_LAZY_ON_METHOD` / `EMIT_LAZY_PARAM_ASSIGN`), a
function needing more than 65,535 slot sites (`EMIT_TOO_MANY_SITES`), `this`
outside a method (`EMIT_NO_THIS_OUTSIDE_METHOD`). The full list, with one
comment per code, is the `UEmitError` enum in `src/emit/uemit.h`.

### Opcode set and control transfer

Wire v2.0 has 41 opcodes (down from 50): `at`/`whenever`/`waituntil`'s six
separate install opcodes collapsed into one `OP_INSTALL` with a mode
operand, and the control-transfer family (`try`/`catch`/`finally`, tag
scopes, `break`/`continue`/`return` across open scopes) collapsed into
`OP_SCOPE_TRY` / `OP_SCOPE_TAG` / `OP_SCOPE_POP` / `OP_UNWIND_TO`, walked
by the runtime's unwinder rather than by a frame-guard primitive the
emitter used to install separately. `break`/`continue` compile through a
per-loop patch list of pending jumps, patched once the loop's exit (or
back-edge) address is known — not a dedicated opcode. The full set, with
every operand and the `EXTARG` wide-site-index mechanism, is
[opcodes.md](opcodes.md).

**Public API (`src/emit/ufront.h`):** `ufront_compile`, `ufront_serialize`,
`ufront_disassemble`. The lower-level driver (`uemit_new`, `uemit_statement`,
`uemit_finish`, `uemit_disassemble`, `uchunk_serialize`, the diagnostic
calls) lives in `src/emit/uemit.h` and is private to `src/emit/`; the only
caller outside the directory that reaches for it is
`tests/unit/test_emit_bytecode.c`, documented in
[test-harness.md](test-harness.md).

---

## Chunk (compiled module)

**Source:** `src/chunk/uchunk_io.c` / `src/chunk/uchunk.h`

The compiled chunk is the interface between the front end (emitter) and the
back end. There is no standalone module struct: the root
`UProto` carries the owned arrays directly, plus the metadata that was absorbed
that a module type used to carry. The owned arrays are:

- `instructions` — array of `uint32_t`, 4-byte aligned.
- `constants` — array of `UValue` (16-byte tagged-value records).
- `line_deltas` — array of `int8_t`, one per instruction.
- `abs_lines` — array of `(pc, line)` checkpoint records.
- `source_name` — null-terminated string, or `NULL` if absent (root only).

Scalar fields include `max_reg` (the highest register index, set at emit time)
and the pluggable allocator pair `(alloc_fn, alloc_ud)`. The absorbed root-only
metadata includes `origin_vm`, `next_proto_serial`, `total_proto_count`,
`next_in_realm`, and `owning_realm`.

The root `UProto` can be populated in two ways: by the emitter via
`ufront_compile` (in-process, no serialize/deserialize round-trip) or by
`uchunk_deserialize` (loading a serialized `.urb` file). Both paths produce
the same layout with the same ownership contract — every array, including
`source_name`, is allocated through the chunk's own allocator and freed by
`uchunk_destroy`. `ufront_compile`'s `name` parameter is borrowed and copied
into the root `UProto`; the caller's string does not need to outlive the
chunk. The VM does not distinguish between the two population paths for
layout or lifetime — but only the deserialize path runs the load-time
verifier (see [Loader and verifier](#loader-and-verifier) below and
[opcodes.md](opcodes.md#loader-verification)); an in-process compile is
trusted on the strength of the emitter alone.

### On-disk format

`uchunk_serialize` (declared in `uemit.h`, implemented in the emitter) writes
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

The chunk allocator follows realloc semantics: a single callback
`UChunkAllocFn` handles allocate, reallocate, and free based on whether `ptr`
and `nbytes` are null/zero. The callback and its `ud` cookie are stored on the
root `UProto`; `uchunk_destroy` frees all owned arrays through the same callback
that allocated them. This ensures that hosted targets using `stdlib` realloc and
embedded targets using a pool allocator each free through the allocator that
made the allocations. The design rationale is in
[design-decisions.md](design-decisions.md#pluggable-allocator-on-umodule-via-umoduleallocfn).

`uchunk_destroy` releases all owned buffers; it is safe to call on a
zero-initialized root `UProto`.

**Public API:** `uchunk_deserialize`, `uchunk_destroy`, `uchunk_load_error_name`.

---

## The runtime

The pipeline above ends where the runtime begins. Everything past the
chunk — the collector, objects and slots, strands, the scheduler, the
unwinder, watchers, realms, the boot table and the C API — is
[runtime.md](runtime.md). The watcher chapter in full is
[reactive-runtime.md](reactive-runtime.md).

---

## Source layout

```text
src/
  lex/                Lexer: UToken, UTokenType, ULexError, ULexer, synclines
  parse/              Parser and AST: UAstNode, UArena-allocated, recursive descent
  emit/               Emitter, disassembler, serializer, and ufront.c — the one
                      lex -> parse -> emit entry point the runtime calls
  chunk/              The bytecode container: writer, loader, verifier, opcode
                      shape table, UProto
  util/               Shared by the frontend and nobody else: the AST arena, the
                      varint codec, the freestanding string helpers, URBI_REQUIRE
  rt/                 The runtime — see runtime.md
  stdlib/             Built-in methods (C) plus stdlib.u (urbiscript), baked to a
                      tracked bytecode blob by tools/urbi-compile-stdlib
  host/               Public API whose implementation is inherently hosted: the
                      value formatter needs snprintf, which src/rt may not use
  repl/               The cooperative NDJSON eval service (four files built);
                      the networked server and its transports are parked
  ros/  urobotics/    Parked for Phase 5

tools/
  urbi.c              The CLI: -i (interactive), -e, -f / positional file,
                      --dump-bytecode, --version, --help.  Not part of
                      liburbi.a; never built for a cross target
  linenoise.{c,h}     Vendored line editor (BSD-2, antirez/linenoise)
  urbi-compile-stdlib.c  Bakes stdlib.u into the tracked blob

tests/
  unit/               The frontend runner: lexer, parser, arena, emitter, chunk,
                      varint, intern, and the public header's inline values
  rt/                 The runtime runner: one suite per src/rt subsystem
  chk/                The conformance corpus — .chk fixtures, one REPL (or host,
                      or NDJSON) session each
  probes/             Footprint and performance probes; see runtime.md
  integration/        The .chk runners and the REPL smoke harness
  fuzz/               libFuzzer harnesses for the lexer, parser, VM and loader
```

---

## Multi-VM model

Multiple `UVM` instances may coexist in one process, fully independent:
no mutable state is shared. Every mutable datum lives on the `UVM`
struct — the collector, the symbol table, the scheduler, the watcher
state, the prototype table, the realm list, all by value.

Only compile-time constant tables (opcode names, version strings, static
error messages) may live at file scope. The
`cppcoreguidelines-avoid-non-const-global-variables` clang-tidy check
gates it under `make lint`.

Each `UVM` is driven by one thread at a time. Two of them may run in
separate threads without synchronization; handing a value from one to the
other is not supported and is undefined behaviour.
