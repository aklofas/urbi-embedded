# Opcodes

## Instruction encoding

Every instruction is a `uint32_t`, 4-byte aligned in the instruction stream.
Fields are packed in little-endian byte order on all v1 targets. Two forms exist:

```text
  byte 3   byte 2   byte 1   byte 0
+--------+--------+--------+--------+
|   C    |   B    |   A    |   op   |   ABC form
+--------+--------+--------+--------+
|      Bx (u16)   |   A    |   op   |   ABx form
+--------+--------+--------+--------+
```

- `op` — 8-bit opcode (byte 0).
- `A` — 8-bit register index (byte 1); present in both forms.
- `B` — 8-bit register index (byte 2, ABC form only).
- `C` — 8-bit register index (byte 3, ABC form only).
- `Bx` — 16-bit unsigned index (bytes 2–3, ABx form only); occupies the same
  space as B and C combined.

Decode helpers are `static inline` in `src/chunk/uchunk.h`:
`uinstr_op`, `uinstr_a`, `uinstr_b`, `uinstr_c`, `uinstr_bx`.

Encode helpers: `uinstr_enc_abc(op, a, b, c)`, `uinstr_enc_abx(op, a, bx)`.

## Opcode table

The canonical enum is `UOpcode` in `src/chunk/uchunk.h`; the verifier shape
table is `urbi_opcode_shapes[]` in `src/chunk/uopcode_shape.{h,c}`. The live
opcode space is contiguous `0..47` with `OP_MAX = 48` (as of wire format v1.6,
introduced at `v0.7.2-esp32`).

The **Since** column records the tag at which the bytecode opcode was first
live; wire-format version bumps that introduced structural layout changes are
noted in the Semantics column.

| Opcode | Value | Form | Operands | Semantics | Since |
|--------|-------|------|----------|-----------|-------|
| `OP_LOADK`              |  0 | ABx | A, Bx          | `R[A] := K[Bx]`                                                                                                               | `v0.1.0-skeleton` |
| `OP_MOVE`               |  1 | ABC | A, B           | `R[A] := R[B]`                                                                                                                | `v0.1.0-skeleton` |
| `OP_ADD`                |  2 | ABC | A, B, C        | `R[A] := R[B] + R[C]`; falls back to operator-method dispatch on type mismatch                                               | `v0.1.0-skeleton` |
| `OP_SUB`                |  3 | ABC | A, B, C        | `R[A] := R[B] - R[C]`; operator-method fallback on mismatch                                                                  | `v0.1.0-skeleton` |
| `OP_MUL`                |  4 | ABC | A, B, C        | `R[A] := R[B] * R[C]`; operator-method fallback on mismatch                                                                  | `v0.1.0-skeleton` |
| `OP_DIV`                |  5 | ABC | A, B, C        | `R[A] := R[B] / R[C]` (always Float per `LANG-CONVENTIONS.md` §1.3); operator-method fallback on mismatch                    | `v0.1.0-skeleton` |
| `OP_NEG`                |  6 | ABC | A, B           | `R[A] := -R[B]`; unary minus; operator-method fallback via `"-"` slot                                                        | `v0.1.0-skeleton` |
| `OP_RET`                |  7 | ABC | A              | Return `R[A]`; at frame 0 marks strand DEAD and copies result to `out_slot`; otherwise triggers unwind walker                 | `v0.1.0-skeleton` |
| `OP_LOADNIL`            |  8 | ABC | A              | `R[A] := nil`                                                                                                                 | `v0.2.0-expressions` |
| `OP_LOADBOOL`           |  9 | ABC | A, B, C        | `R[A] := (B != 0)`; if `C != 0`, skip next instruction                                                                       | `v0.2.0-expressions` |
| `OP_LOADVOID`           | 10 | ABC | A              | `R[A] := void` (used by `&` separator expression)                                                                            | `v0.2.0-expressions` |
| `OP_GETUPVAL`           | 11 | ABC | A, B           | `R[A] := upvalue[B]`; reads from current frame's closure or `s->entry_closure` at frame 0                                    | `v0.2.0-expressions` |
| `OP_SETUPVAL`           | 12 | ABC | A, B           | `upvalue[B] := R[A]`; GC write barrier fires before the store                                                                | `v0.2.0-expressions` |
| `OP_CLOSURE`            | 13 | ABx | A, Bx          | `R[A] := closure(executing_proto->nested[Bx])`; reads `nupvals` upvalue-descriptor pseudo-instructions immediately following (Lua-5.5 prelude pattern); `executing_proto` is the proto whose bytecode contains this instruction (truly-recursive emitter, `v0.8.5`) | `v0.2.0-expressions` |
| `OP_CLOSE`              | 14 | ABC | A              | Close (heap-promote) all upvalues for registers `>= R[A]`                                                                    | `v0.2.0-expressions` |
| `OP_CALL`               | 15 | ABC | A, B, C        | `R[A], ..., R[A+(C&0x7F)-2] := R[A](R[A+1], ..., R[A+B-1])`. B = nargs+1 (plain) or nargs+2 (method). C low 7 bits = nresults+1. C bit 7 (0x80) = method-call flag (wire v1.6): when set, R[A+1] holds the receiver (placed by a preceding `OP_SELF`) and `self` is passed to the callee; when clear, `self` is nil. | `v0.2.0-expressions` |
| `OP_JMP`                | 16 | ABx | Bx             | `pc += signed(Bx) - 32768` (biased signed offset; not range-checked at load time). **Landing is direction-dependent:** a forward jump (offset ≥ 0) applies the offset and then falls through `NEXT()`'s `pc++`, so it lands at `from + offset + 1`; a backward jump (offset < 0) routes through the safepoint path, which dispatches the instruction at `pc + offset` directly (no `pc++`), landing at `from + offset`. The emitter has one encoder per direction: `uemit_jmp_offset` (forward, encodes `target - from - 1`) and `uemit_jmp_offset_backward` (back-edges, encodes `target - from`) — see `src/emit/uemit_internal.h` (refactor-3 FE-01) | `v0.2.0-expressions` |
| `OP_TEST`               | 17 | ABC | A, C           | If `truthy(R[A]) == C`, skip next instruction                                                                                 | `v0.2.0-expressions` |
| `OP_TESTSET`            | 18 | ABC | A, B, C        | If `truthy(R[B]) == C`, skip next; else `R[A] := R[B]`                                                                       | `v0.2.0-expressions` |
| `OP_EQ`                 | 19 | ABC | A, B, C        | If `(R[B] == R[C]) != A`, skip next                                                                                           | `v0.2.0-expressions` |
| `OP_NEQ`                | 20 | ABC | A, B, C        | If `(R[B] != R[C]) != A`, skip next                                                                                           | `v0.2.0-expressions` |
| `OP_LT`                 | 21 | ABC | A, B, C        | If `(R[B] <  R[C]) != A`, skip next                                                                                           | `v0.2.0-expressions` |
| `OP_LE`                 | 22 | ABC | A, B, C        | If `(R[B] <= R[C]) != A`, skip next                                                                                           | `v0.2.0-expressions` |
| `OP_YIELD`              | 23 | ABC | —              | Yield to scheduler; allows other strands to run                                                                               | `v0.2.0-expressions` / `v0.3.0-concurrency` |
| `OP_FORK_DETACH`        | 24 | ABC | A              | Spawn `R[A]` (closure) as a detached strand (`,` separator); parent continues immediately; rejected in `urbi_vm_run` transient strands | `v0.3.0-concurrency` |
| `OP_FORK_JOIN`          | 25 | ABC | A, B           | Spawn `R[A]` (closure) as a joined child; store strand handle in `R[B]` (`&` separator LHS); rejected in transient strands    | `v0.3.0-concurrency` |
| `OP_JOIN_WAIT`          | 26 | ABC | A              | Block until child strand `R[A]` is DEAD (`&` separator join-point); already-DEAD children continue without blocking           | `v0.3.0-concurrency` |
| `OP_GETSLOT`            | 27 | ABC | A, B, C        | `R[A] := R[B].slot` keyed by IC site index C; 4-entry inline cache per site; atom receivers routed via `urbi_atom_proto_for_value` | `v0.4.0-objects` |
| `OP_SETSLOT`            | 28 | ABC | A, B, C        | `R[A].slot := R[B]` keyed by IC site index C; IC invalidation on shape change                                                | `v0.4.0-objects` |
| `OP_THROW`              | 29 | ABC | A              | Throw `R[A]`; triggers the unwind walker                                                                                      | `v0.3.0-concurrency` |
| `OP_TAG_STOP`           | 30 | ABC | A, B           | **Reserved stub** — returns `UVM_TYPE_ERROR` at runtime; see [Reserved stub opcodes](#reserved-stub-opcodes) below             | `v0.3.0-concurrency` |
| `OP_TRY_BEGIN`          | 31 | ABx | A, Bx          | Push cleanup entry onto the strand's cleanup stack; A=flags byte, Bx=handler PC                                              | `v0.3.0-concurrency` |
| `OP_TRY_END`            | 32 | ABC | —              | Pop top cleanup entry                                                                                                         | `v0.3.0-concurrency` |
| `OP_PUSH_TAG`           | 33 | ABx | A, Bx          | Push tag onto strand tag stack; A[7:4]=flags nibble, A[3:0]=tag_reg nibble (range 0–15); Bx=onleave PC                       | `v0.3.0-concurrency` |
| `OP_POP_TAG`            | 34 | ABC | A              | Pop top tag (A=tag_reg)                                                                                                       | `v0.3.0-concurrency` |
| `OP_PUSH_FRAME_GUARD`   | 35 | ABC | A, B           | Install frame guard covering registers [A, A+B); verifier enforces `A + B <= max_reg + 1`                                    | `v0.3.0-concurrency` |
| `OP_RESUME`             | 36 | ABC | A              | Restore unwind state from `R[A]`                                                                                              | `v0.3.0-concurrency` |
| `OP_LOAD_CATCH_VALUE`   | 37 | ABC | A              | `R[A] := s->catch_value`; emitted as the first instruction of every catch-handler body so the catch variable receives the thrown value | `v0.3.0-concurrency` |
| `OP_AT_INSTALL`         | 38 | ABC | A, B, C        | Install `at (cond) body`; A=cond_reg (closure), B=body_reg (closure), C=onleave_reg or `0xFF` (absent)                       | `v0.5.0-reactive` |
| `OP_AT_SYNC_INSTALL`    | 39 | ABC | A, B, C        | Install sync-flavored `at`; same operand shape as `OP_AT_INSTALL`                                                            | `v0.5.0-reactive` |
| `OP_WHENEVER_INSTALL`   | 40 | ABC | A, B, C        | Install `whenever (cond) body`; re-fires body each evaluation cycle while cond is true; C=onleave_reg or `0xFF`               | `v0.5.0-reactive` |
| `OP_WAITUNTIL_INSTALL`  | 41 | ABC | A              | Install `waituntil (cond)`; blocks installing strand until cond becomes truthy                                                | `v0.5.0-reactive` |
| `OP_AT_EVENT_INSTALL`   | 42 | ABC | A, B, C        | Install `at (event?) body`; A=event_reg, B=body_reg, C=onleave_reg or `0xFF`                                                 | `v0.5.0-reactive` |
| `OP_AT_EVENT_SYNC_INSTALL` | 43 | ABC | A, B, C     | Install sync-flavored `at (event?)`; same shape as `OP_AT_EVENT_INSTALL`                                                     | `v0.5.0-reactive` |
| `OP_GETSLOT_CHANGE_EVENT`  | 44 | ABC | A, B, C     | `R[A] := R[B].changed?` event handle; C=IC site index (same convention as `OP_GETSLOT`)                                     | `v0.5.0-reactive` |
| `OP_LOAD_REALM_GLOBAL`     | 45 | ABC | A           | `R[A] := realm->global_object`; B and C are reserved/zero; sym_id extension deferred to v1.x                                | `v0.5.0-reactive` |
| `OP_LOAD_RECV`             | 46 | ABC | A           | `R[A] := current frame's receiver` (`self`); nil when called from top-level or non-method frame; introduced with `this` keyword support (wire v1.6 context) | `v0.6.2-language-completion` |
| `OP_SELF`                  | 47 | ABC | A, B, C     | Load method and receiver atomically: `R[A+1] := R[B]` (receiver snapshot), `R[A] := lookup_slot(R[B], IC[C])`; atom receivers routed via `urbi_atom_proto_for_value`; eliminates the `vm->last_recv` side-channel clobbering bug; wire v1.6 | `v0.7.2-esp32` |

## Reserved stub opcodes

### `OP_TAG_STOP` (value 30)

The bytecode opcode `OP_TAG_STOP` is a **reserved stub**. The runtime path for
stopping a tag is the host-callable `urbi_tag_stop()` API; no part of the
emitter produces `OP_TAG_STOP` bytecode today. The dispatch entry returns
`UVM_TYPE_ERROR` with the message `"OP_TAG_STOP: bytecode emit path is
reserved; use urbi_tag_stop host call"` rather than executing undefined
behaviour. The operand shape (A=tag_reg, B=value_reg) is defined in the shape
table and will be honoured when the emit path is activated in a future release.

## Retired opcodes

- **`OP_INVOKE` (slot 38)** — reserved in `v0.4.0-objects` for a
  collapsed `OP_GETSLOT` + `OP_CALL` fast-path that was never wired
  into the emitter. Retired at `v0.5.6-bytecode`; the gap was
  collapsed by renumbering the reactive opcodes from 39–46 down to
  38–45 (wire format v1.5, `v0.5.6-bytecode`).

## Register file

Registers are 0-based per proto. `UProto.max_reg` records the highest register
index used by that block; the runtime allocates `max_reg + 1` tagged-value
slots. Register values share the `UValue` shape (see `src/value/`): 16 bytes,
with a `kind` byte (`UValKind`), 7 bytes of padding, and an 8-byte value union
— `int64_t i` for integer values, `double` or `float` for float values
(selected by `URBI_FLOAT_TYPE` at compile time: 8 = `double`, 4 = `float`).

## Reserved opcode space

The opcode field is 8 bits. Values `0..OP_MAX-1` (currently `0..47`) are live;
values `OP_MAX..255` are reserved for future releases. The loader rejects any
opcode with value `>= OP_MAX` via the shape-table walk (`UCHUNK_LOAD_CORRUPT`).

## Loader verification

The verifier walks the root chunk plus every non-NULL nested proto via
`decode_verify` (`src/chunk/uchunk_io.c`), consulting the
`urbi_opcode_shapes[]` table for per-operand expectations. See
[`bytecode-format.md`](bytecode-format.md#loader-verification) for the
full enumeration of cross-byte invariants the verifier enforces.

Adding a new opcode requires adding exactly one entry to `urbi_opcode_shapes[]`
and bumping `OP_MAX` (which is the enum sentinel and therefore advances
automatically). No per-opcode `switch` lives in the verifier any more.
