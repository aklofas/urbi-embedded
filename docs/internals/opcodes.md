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

Decode helpers are `static inline` in `src/module/umodule.h`:
`uinstr_op`, `uinstr_a`, `uinstr_b`, `uinstr_c`, `uinstr_bx`.

Encode helpers: `uinstr_enc_abc(op, a, b, c)`, `uinstr_enc_abx(op, a, bx)`.

## Opcode table

The canonical enum is `UOpcode` in `src/module/umodule.h`; the verifier shape
table is `urbi_opcode_shapes[]` in `src/module/uopcode_shape.{h,c}`. At v1.5
the live opcode space is contiguous `0..45` with `OP_MAX = 46`.

| Opcode | Value | Form | Operands | Semantics | Milestone |
|--------|-------|------|----------|-----------|-----------|
| `OP_LOADK`              |  0 | ABx | A, Bx          | `R[A] := K[Bx]`                                                        | M1 |
| `OP_MOVE`               |  1 | ABC | A, B           | `R[A] := R[B]`                                                         | M1 |
| `OP_ADD`                |  2 | ABC | A, B, C        | `R[A] := R[B] + R[C]`                                                  | M1 |
| `OP_SUB`                |  3 | ABC | A, B, C        | `R[A] := R[B] - R[C]`                                                  | M1 |
| `OP_MUL`                |  4 | ABC | A, B, C        | `R[A] := R[B] * R[C]`                                                  | M1 |
| `OP_DIV`                |  5 | ABC | A, B, C        | `R[A] := R[B] / R[C]` (always Float per `LANG-CONVENTIONS.md` §1.3)    | M1 |
| `OP_NEG`                |  6 | ABC | A, B           | `R[A] := -R[B]`                                                        | M1 |
| `OP_RET`                |  7 | ABC | A              | `return R[A]` (terminator of root chunk)                               | M1 |
| `OP_LOADNIL`            |  8 | ABC | A              | `R[A] := nil`                                                          | M2 |
| `OP_LOADBOOL`           |  9 | ABC | A, B, C        | `R[A] := (B != 0)`; if `C != 0`, skip next instruction                 | M2 |
| `OP_LOADVOID`           | 10 | ABC | A              | `R[A] := void` (used by `&` separator)                                 | M2 |
| `OP_GETUPVAL`           | 11 | ABC | A, B           | `R[A] := upvalue[B]`                                                   | M2 |
| `OP_SETUPVAL`           | 12 | ABC | A, B           | `upvalue[B] := R[A]`                                                   | M2 |
| `OP_CLOSURE`            | 13 | ABx | A, Bx          | `R[A] := closure(nested[Bx])`; reads `nupvals` upvalue prelude entries | M2 |
| `OP_CLOSE`              | 14 | ABC | A              | Close upvalues for `R >= R[A]`                                         | M2 |
| `OP_CALL`               | 15 | ABC | A, B, C        | `R[A], ..., R[A+C-2] := R[A](R[A+1], ..., R[A+B-1])`                   | M2 |
| `OP_JMP`                | 16 | ABx | Bx             | `pc += signed(Bx) - 32768` (intentionally not load-time range-checked) | M2 |
| `OP_TEST`               | 17 | ABC | A, C           | If `truthy(R[A]) == C`, skip next instruction                          | M2 |
| `OP_TESTSET`            | 18 | ABC | A, B, C        | If `truthy(R[B]) == C`, skip; else `R[A] := R[B]`                      | M2 |
| `OP_EQ`                 | 19 | ABC | A, B, C        | If `(R[B] == R[C]) != A`, skip next                                    | M2 |
| `OP_NEQ`                | 20 | ABC | A, B, C        | If `(R[B] != R[C]) != A`, skip next                                    | M2 |
| `OP_LT`                 | 21 | ABC | A, B, C        | If `(R[B] <  R[C]) != A`, skip next                                    | M2 |
| `OP_LE`                 | 22 | ABC | A, B, C        | If `(R[B] <= R[C]) != A`, skip next                                    | M2 |
| `OP_YIELD`              | 23 | ABC | —              | Yield to scheduler                                                     | M2/M3 |
| `OP_FORK_DETACH`        | 24 | ABx | A, Bx          | Spawn nested[Bx] closure detached (`,` separator)                      | M3 |
| `OP_FORK_JOIN`          | 25 | ABx | A, Bx          | Spawn nested[Bx] closure joined (`&` separator)                        | M3 |
| `OP_JOIN_WAIT`          | 26 | ABC | A              | Wait at `&` join point                                                 | M3 |
| `OP_GETSLOT`            | 27 | ABC | A, B, C        | `R[A] := R[B].slot(symbol_id_C)` via 4-entry IC                        | M4 |
| `OP_SETSLOT`            | 28 | ABC | A, B, C        | `R[A].slot(symbol_id_C) := R[B]` via 4-entry IC                        | M4 |
| `OP_THROW`              | 29 | ABx | A              | Throw `R[A]` (Bx unused / 0)                                           | M3 |
| `OP_TAG_STOP`           | 30 | ABC | A, B           | Stop tag `R[A]` with value `R[B]`                                      | M3 |
| `OP_TRY_BEGIN`          | 31 | ABx | A, Bx          | Push cleanup entry; A=flags, Bx=handler PC                             | M3 |
| `OP_TRY_END`            | 32 | ABC | —              | Pop top cleanup entry                                                  | M3 |
| `OP_PUSH_TAG`           | 33 | ABx | A, Bx          | A[7:4]=flags nibble, A[3:0]=tag_reg nibble; Bx=onleave PC              | M3 |
| `OP_POP_TAG`            | 34 | ABC | A              | Pop top tag (A=tag_reg)                                                | M3 |
| `OP_PUSH_FRAME_GUARD`   | 35 | ABC | A, B           | Frame guard at base A, count B (`A + B <= max_reg + 1`)                | M3 |
| `OP_RESUME`             | 36 | ABC | A              | Restore unwind state from `R[A]`                                       | M3 |
| `OP_LOAD_CATCH_VALUE`   | 37 | ABC | A              | `R[A] := s->catch_value` (catch-handler prelude)                       | M3 |
| `OP_AT_INSTALL`         | 38 | ABC | A, B           | Install `at (cond) body`; A=cond_reg, B=body_reg                       | M5 |
| `OP_AT_SYNC_INSTALL`    | 39 | ABC | A, B           | Install sync-flavored `at`; same shape as `OP_AT_INSTALL`              | M5 |
| `OP_WHENEVER_INSTALL`   | 40 | ABC | A, B           | Install `whenever (cond) body`                                         | M5 |
| `OP_WAITUNTIL_INSTALL`  | 41 | ABC | A              | Install `waituntil (cond)`                                             | M5 |
| `OP_AT_EVENT_INSTALL`   | 42 | ABC | A, B           | Install `at (event?) body`; A=event_reg, B=body_reg                    | M5 |
| `OP_AT_EVENT_SYNC_INSTALL` | 43 | ABC | A, B        | Install sync-flavored `at (event?)`                                    | M5 |
| `OP_GETSLOT_CHANGE_EVENT`  | 44 | ABC | A, B, C     | `R[A] := R[B].changed?` event handle (C = IC site index)               | M5 |
| `OP_LOAD_REALM_GLOBAL`     | 45 | ABC | A           | `R[A] := realm_global(symbol_id)` (B,C reserved)                       | M5 |

## Retired opcodes

- **`OP_INVOKE` (slot 38)** — reserved at M4 for a collapsed
  `OP_GETSLOT` + `OP_CALL` fast-path that was never wired into the
  M4-onward emitter. Retired at v0.5.6 Wave 4 (`v0.5.6-bytecode`); the
  gap was collapsed by renumbering the M5 reactive opcodes from 39–46
  down to 38–45. Opcode space is now contiguous `0..45` with
  `OP_MAX = 46`.

## Register file

Registers are 0-based per chunk. `module->max_reg` (and each `UProto.max_reg`)
records the highest register index used by that block. At VM setup the runtime
allocates `max_reg + 1` tagged-value slots. Register values share the `UValue`
shape from `src/module/umodule.h`: 16 bytes, with a `kind` byte (see
`UValKind`), 7 bytes of padding, and an 8-byte value union — `int64_t i` for
integer values, `f` for float values whose type (`double` or `float`) is
selected by `URBI_FLOAT_TYPE` at compile time (8 = `double`, 4 = `float`).

## Reserved opcode space

The opcode field is 8 bits. Values `0..OP_MAX-1` are live; values
`OP_MAX..255` are reserved for future milestones (M6 stdlib will likely
introduce new arithmetic / string-manipulation opcodes; M7 C-API may
introduce host-call shortcuts). The loader rejects any opcode with value
`>= OP_MAX` via the shape-table walk (`ULOAD_CORRUPT`).

## Loader verification

The verifier walks the root chunk plus every non-NULL nested proto via
`decode_verify` (`src/module/umodule.c`), consulting the
`urbi_opcode_shapes[]` table for per-operand expectations. See
[`bytecode-format.md`](bytecode-format.md#loader-verification) for the
full enumeration of cross-byte invariants the verifier enforces.

Adding a new opcode requires adding exactly one entry to `urbi_opcode_shapes[]`
and bumping `OP_MAX` (which is the enum sentinel and therefore advances
automatically). No per-opcode `switch` lives in the verifier any more.
