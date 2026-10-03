# Opcodes

## Instruction encoding

Every instruction is a `uint32_t`, 4-byte aligned in the instruction stream.
Fields are packed in little-endian byte order on all targets. Two forms exist:

```text
  byte 3   byte 2   byte 1   byte 0
+--------+--------+--------+--------+
|   C    |   B    |   A    |   op   |   ABC form
+--------+--------+--------+--------+
|      Bx (u16)   |   A    |   op   |   ABX form
+--------+--------+--------+--------+
```

- `op` — 8-bit opcode (byte 0).
- `A` — 8-bit field (byte 1); present in both forms. Usually a register, but
  some opcodes use it as a flags byte or a scope depth (see the table).
- `B` — 8-bit field (byte 2, ABC form only).
- `C` — 8-bit field (byte 3, ABC form only).
- `Bx` — 16-bit unsigned field (bytes 2–3, ABx form only); occupies the same
  space as B and C combined.

Decode helpers are `static inline` in `src/chunk/uchunk.h`:
`uinstr_op`, `uinstr_a`, `uinstr_b`, `uinstr_c`, `uinstr_bx`.

Encode helpers: `uinstr_enc_abc(op, a, b, c)`, `uinstr_enc_abx(op, a, bx)`.

The per-operand *kind* for every opcode (register / immediate flags /
immediate mode / site index / depth / unused / upvalue index / pool index /
nested-proto index / jump target / handler PC) is a data table, not a
`switch`: `urbi_opcode_shapes[OP_MAX]` in `src/chunk/uopcode_shape.c`,
declared by `src/chunk/uopcode_shape.h`. The verifier (below, and in
[bytecode-format.md](bytecode-format.md)) walks every instruction against
that table; adding an opcode means adding one row to the `.def` file below
and one entry to the shape table, nothing else.

## Opcode table

The canonical enum is `UOpcode` in `src/chunk/uchunk.h`, generated from
`src/chunk/uopcodes.def` — the single source of truth for the 41-opcode
wire v2.0 set. The live opcode space is contiguous `0..40` with `OP_MAX =
41`. Row order is part of the wire format: reordering, inserting or
removing a row is a bytecode version bump.

| Opcode | Value | Form | Operands | Semantics |
|--------|-------|------|----------|-----------|
| `OP_LOADK`                 |  0 | ABX | A, Bx          | `R[A] := K[Bx]` |
| `OP_MOVE`                  |  1 | ABC | A, B           | `R[A] := R[B]` |
| `OP_ADD`                   |  2 | ABC | A, B, C        | `R[A] := R[B] + R[C]`; falls back to operator-method dispatch on type mismatch |
| `OP_SUB`                   |  3 | ABC | A, B, C        | `R[A] := R[B] - R[C]`; operator-method fallback on mismatch |
| `OP_MUL`                   |  4 | ABC | A, B, C        | `R[A] := R[B] * R[C]`; operator-method fallback on mismatch |
| `OP_DIV`                   |  5 | ABC | A, B, C        | `R[A] := R[B] / R[C]` (always Float); operator-method fallback on mismatch |
| `OP_NEG`                   |  6 | ABC | A, B           | `R[A] := -R[B]`; unary minus; operator-method fallback via the `"-"` slot |
| `OP_RET`                   |  7 | ABC | A              | Return `R[A]`; at frame 0 marks the strand DEAD and copies the result to `s->result`; otherwise triggers the unwind walker |
| `OP_LOADNIL`                |  8 | ABC | A              | `R[A] := nil` |
| `OP_LOADBOOL`                |  9 | ABC | A, B, C        | `R[A] := (B != 0)` (B is an immediate 0/1); if `C != 0`, skip the next instruction |
| `OP_LOADVOID`                | 10 | ABC | A              | `R[A] := void` (the value of a `&` sequence) |
| `OP_GETUPVAL`                | 11 | ABC | A, B           | `R[A] := upvalue[B]` |
| `OP_SETUPVAL`                | 12 | ABC | A, B           | `upvalue[B] := R[A]` |
| `OP_CLOSURE`                 | 13 | ABX | A, Bx          | `R[A] := closure(executing_proto->nested[Bx])`; reads `nupvals` upvalue-descriptor pseudo-instructions immediately following (in\_stack + src\_idx per upvalue) |
| `OP_CLOSE`                   | 14 | ABC | A              | Close (heap-promote) every open upvalue for registers `>= R[A]` |
| `OP_CALL`                    | 15 | ABC | A, B, C        | `R[A], ... := R[A](R[A+1], ...)`. B = nargs+1 (plain) or nargs+2 (method). C low 7 bits = nresults+1. C bit 7 (`UCALL_C_METHOD`, 0x80) = method-call flag: when set, R[A+1] holds the receiver (placed by a preceding `OP_SELF`); when clear, `self` is nil |
| `OP_JMP`                     | 16 | ABX | Bx             | `pc := target`, resolved from a 32768-biased signed `Bx` — see [Jump encoding](#jump-encoding) below |
| `OP_TEST`                    | 17 | ABC | A, C           | If `truthy(R[A]) == C` (C an immediate 0/1), skip the next instruction |
| `OP_TESTSET`                 | 18 | ABC | A, B, C        | If `truthy(R[B]) == C`, skip the next instruction; else `R[A] := R[B]` |
| `OP_EQ`                      | 19 | ABC | A, B, C        | If `(R[B] == R[C]) != A` (A an immediate 0/1), skip the next instruction |
| `OP_LT`                      | 20 | ABC | A, B, C        | If `(R[B] < R[C]) != A`, skip the next instruction |
| `OP_LE`                      | 21 | ABC | A, B, C        | If `(R[B] <= R[C]) != A`, skip the next instruction |
| `OP_YIELD`                   | 22 | ABC | —              | Yield to the scheduler; lets other strands run |
| `OP_FORK`                    | 23 | ABC | A, B, C        | Spawn `R[A]` (a closure) as a new strand. C selects the mode: `UFORK_DETACH` (0) spawns detached, and B must be `0xFF` (no handle); `UFORK_JOIN` (1) spawns a joined child whose handle is written to `R[B]`, which must not be `0xFF`. The emitter forks the right-hand arm only after the left-hand arm has run to completion — a `&` pair's two arms do not interleave (legacy urbiscript runs both at once; see `docs/urbi-embedded-design-risks.md` entry `phase3-D`, workspace root) |
| `OP_JOIN_WAIT`                | 24 | ABC | A              | Block until the child strand handle in `R[A]` is DEAD; already-DEAD children continue without blocking. The verifier requires this instruction to sit immediately after the `OP_FORK` that joined it, with matching handle registers — see [Loader verification](#loader-verification) |
| `OP_GETSLOT`                  | 25 | ABC | A, B, C        | `R[A] := R[B].slot`, keyed by slot-site index C (widened by a preceding `OP_EXTARG`, see below) |
| `OP_SETSLOT`                  | 26 | ABC | A, B, C        | `R[B].slot := R[A]`, keyed by site index C. Used for a declaration (`var x = ...`, `Realm.x = ...`) — the write always succeeds, declaring the slot if it is not already there |
| `OP_SETSLOT_UPDATE`           | 27 | ABC | A, B, C        | Same operands and write as `OP_SETSLOT`, but the name is resolved through the prototype chain first and a miss raises `LookupError`: a bare `x = 1` updates an existing binding, it does not declare one |
| `OP_SELF`                     | 28 | ABC | A, B, C        | Load method and receiver atomically: `R[A+1] := R[B]` (receiver snapshot), `R[A] := lookup_slot(R[B], site C)` |
| `OP_GETSLOT_CHANGE_EVENT`     | 29 | ABC | A, B, C        | `R[A] := R[B].changed?` event handle for the slot at site C (same site-index convention as `OP_GETSLOT`) |
| `OP_EXTARG`                   | 30 | ABX | Bx             | Not an instruction in its own right: widens the **next** instruction's site index. `Bx` supplies that index's high 16 bits (`site := C \| (Bx << 8)`). See [EXTARG](#extarg-wide-site-indices) below |
| `OP_THROW`                    | 31 | ABC | A              | Throw `R[A]`; triggers the unwind walker |
| `OP_SCOPE_TRY`                | 32 | ABX | A, Bx          | Open a try scope. `A` is exactly one of `USCOPE_F_HAS_CATCH` (0x1) or `USCOPE_F_HAS_FINALLY` (0x2) — never both, never neither; `Bx` is the handler PC (the catch or finally entry) |
| `OP_SCOPE_TAG`                | 33 | ABX | A, Bx          | Open a tag scope. `A` is the tag's register, or `USCOPE_NO_REG` (0xFF) for a fresh anonymous scope tag; `Bx` is the `onleave` handler PC, or an unreachable placeholder when there is none |
| `OP_SCOPE_POP`                | 34 | ABC | A              | Close the innermost scope. `A`'s low two bits name the entry kind it expects (`USCOPE_POP_TRY` 0x1 or `USCOPE_POP_TAG` 0x2); for a TRY entry, bit `USCOPE_POP_RUN_FINALLY` (0x4) additionally runs the finally body and resumes after this instruction |
| `OP_UNWIND_TO`                | 35 | ABX | A, Bx          | A structured jump: pop exactly `A` scopes (running any `finally` bodies they own along the way) and land at PC `Bx`. Emitted for `break`/`continue`/`return` that cross one or more open scopes |
| `OP_RESUME`                   | 36 | ABC | —              | Resume the pending unwind that a `finally` body's own completion (falling off the end, or a `return`) had deferred |
| `OP_LOAD_CATCH_VALUE`         | 37 | ABC | A              | `R[A] := the pending thrown value`; emitted as the first instruction of a catch-handler body with a bound variable |
| `OP_INSTALL`                  | 38 | ABC | A, B, C        | Install a reactive watcher. `R[A]` is the source (condition/event closure or slot receiver), `R[A+1]` the body closure (if `UINSTALL_F_HAS_BODY`), `R[A+2]` the alternate — `onleave`, or the `whenever ... else` arm (if `UINSTALL_F_HAS_ALT`). `B` selects the mode (1–7, see [INSTALL modes](#install-modes)); `C` is the flags byte (`UINSTALL_F_HAS_BODY` 0x1, `UINSTALL_F_HAS_ALT` 0x2 — unused bits must be zero, and `UINSTALL_WAITUNTIL` requires `C == 0`) |
| `OP_LOAD_REALM_GLOBAL`        | 39 | ABC | A              | `R[A] := the current realm's globals object` |
| `OP_LOAD_RECV`                | 40 | ABC | A              | `R[A] := the current frame's receiver` (`this`); nil outside a method body |

### Jump encoding

`OP_JMP`'s `Bx` is a signed offset biased by 32768, and the two directions
resolve differently. A forward jump (`Bx - 32768 >= 0`) is relative to the
instruction *after* the jump, landing at `pc + offset + 1`. A backward jump
(`Bx - 32768 < 0`) is relative to the jump itself, landing at `pc + offset`.
The emitter has one encoder per direction for exactly this reason. The
load-time verifier resolves and range-checks the target
(`UCHUNK_LOAD_JMP_OUT_OF_BOUNDS` if it falls outside `[0, instr_count)`,
`UCHUNK_LOAD_BAD_EXTARG` if it lands on the instruction right after an
`OP_EXTARG`).

### EXTARG (wide site indices)

A slot-site index (`OP_GETSLOT`, `OP_SETSLOT`, `OP_SETSLOT_UPDATE`, `OP_SELF`,
`OP_GETSLOT_CHANGE_EVENT`) is one byte in the instruction itself — up to 255
distinct sites per function for free. A function that touches more sites
than that gets an `OP_EXTARG` immediately before the site-bearing
instruction: `OP_EXTARG`'s `Bx` supplies bits 8–23 of the site index, OR'd
with the following instruction's `C` byte for bits 0–7. This widens the
per-function site cap from 256 to 65,535 (`site_count` is a `uint16_t`,
matching a byte widened by `OP_EXTARG` to 16 bits).

The verifier enforces, at load time:

- An `OP_EXTARG` may precede only a site-bearing opcode; anything else is
  `UCHUNK_LOAD_BAD_EXTARG`.
- An `OP_EXTARG` may not be the last instruction in a proto.
- No control transfer — a jump target, or an `OP_SCOPE_TRY` /
  `OP_SCOPE_TAG` / `OP_UNWIND_TO` handler PC — may land on the instruction
  immediately after an `OP_EXTARG`; landing there would run that
  instruction with its high site bits silently dropped.
- The widened site index must still be `< site_count`.

### INSTALL modes

`OP_INSTALL`'s `B` operand selects which reactive construct is being armed;
each value pairs with a fixed reading of `C`'s flag bits (`UINSTALL_F_HAS_BODY`,
`UINSTALL_F_HAS_ALT`) and of what `R[A]` holds:

| Mode | Constant | Construct | `R[A]` holds |
|------|----------|-----------|--------------|
| 1 | `UINSTALL_AT_COND` | `at (cond) body` | the condition closure |
| 2 | `UINSTALL_AT_SYNC_COND` | `at sync (cond) body` | the condition closure |
| 3 | `UINSTALL_WHENEVER_COND` | `whenever (cond) body [else alt]` | the condition closure |
| 4 | `UINSTALL_AT_EVENT` | `at (event?) body` | the event expression |
| 5 | `UINSTALL_AT_SYNC_EVENT` | `at sync (event?) body` | the event expression |
| 6 | `UINSTALL_WHENEVER_EVENT` | `whenever (event?) body` | the event expression |
| 7 | `UINSTALL_WAITUNTIL` | `waituntil (cond\|event?)` | the condition or event expression; no body, no alt (`C` must be 0) |

A reactive construct's body — in every mode above — is exactly one
statement; legacy urbiscript's rule, carried forward unchanged. A `|`
immediately after the construct binds OUTSIDE it, as a sibling statement,
not into the body (see `tests/chk/reactive/body_binds_one_statement.chk`).

## Register file

Registers are 0-based per proto. `UProto.max_reg` records the highest
register index used by that function; the runtime allocates `max_reg + 1`
tagged-value slots. Register values share the `UValue` shape (see
`include/urbi/types.h`): 16 bytes, with a `kind` byte (`UValKind`), padding,
and an 8-byte value union — `int64_t i` for integers, `double` for floats
(the float representation is fixed at `double`; the old per-target `f32`
flavor has been retired — see [bytecode-format.md](bytecode-format.md)).

Register discipline, in one paragraph (the emitter's own description, from
`src/emit/uemit_internal.h`): each function state has one cursor, `freereg`.
Locals — named variables and nameless **pins** — are declared at `freereg`
and own their register until their scope ends; the newest local's register
plus one is `ureg_top`. Everything from `ureg_top` up to `freereg` is the
temporaries of the statement being compiled, and every statement ends with
`freereg == ureg_top`. A value that has to survive a nested statement is
pinned, which makes it a nameless local, so the nested statement's own
resets land above it without any arithmetic. There is exactly one register
cursor per function — not the separate temporary-stack and scratch-frame
machinery earlier emitters used.

## Reserved opcode space

The opcode field is 8 bits. Values `0..OP_MAX-1` (currently `0..40`) are
live; values `OP_MAX..255` are reserved for future releases. The loader
rejects any opcode with value `>= OP_MAX` as `UCHUNK_LOAD_CORRUPT`.

## Loader verification

The verifier walks the root chunk plus every non-NULL nested proto in two
passes (`src/chunk/uchunk_verify.c`): pass 1 checks each instruction's
operand bytes against `urbi_opcode_shapes[]` and the per-opcode cross-byte
rules (`OP_SELF`, `OP_CALL`, `OP_INSTALL`, `OP_FORK`, `OP_JOIN_WAIT`,
`OP_SCOPE_POP`, `OP_SCOPE_TRY`'s flag-exclusivity rule above); pass 2
resolves every jump and handler target and the `OP_CLOSURE` upvalue
prelude. The full enumeration, including the `EXTARG` rules above and the
65,535 site-name cap, is in [bytecode-format.md](bytecode-format.md#loader-verification).

Adding a new opcode requires adding exactly one row to `src/chunk/uopcodes.def`
and one entry to `urbi_opcode_shapes[]`; `OP_MAX` is the enum sentinel and
advances automatically. There is no per-opcode `switch` in the verifier.

**The in-memory compile-and-run path does not run this verifier.**
`ufront_compile` (`src/emit/ufront.c`) hands its freshly emitted `UProto`
straight to `uproto_bind` (`src/rt/uexec.c`), which interns names but never
calls `urbi_chunk_decode_verify` / `urbi_chunk_verify_bounds`. Verification
only runs on `uchunk_deserialize` — loading a serialized `.urb` blob from
outside the process. A chunk compiled by `urbi_compile`/`urbi_run`/the REPL
in the same process is trusted on the strength of the emitter alone; see
`docs/urbi-embedded-design-risks.md` (workspace root), `phase4` section.
