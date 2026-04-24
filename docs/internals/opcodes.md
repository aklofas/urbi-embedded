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

Decode helpers are `static inline` in `src/umodule.h`:
`uinstr_op`, `uinstr_a`, `uinstr_b`, `uinstr_c`, `uinstr_bx`.

Encode helpers: `uinstr_enc_abc(op, a, b, c)`, `uinstr_enc_abx(op, a, bx)`.

## Opcode table

| Opcode | Value | Form | Operands | Semantics | Notes |
|--------|-------|------|----------|-----------|-------|
| `OP_LOADK` | 0 | ABx | A, Bx | `R[A] := K[Bx]` | Bx is the constants-pool index; 16 bits supports up to 65 536 constants per module |
| `OP_MOVE`  | 1 | ABC | A, B   | `R[A] := R[B]`          | C unused; loader does not enforce `C == 0` |
| `OP_ADD`   | 2 | ABC | A, B, C | `R[A] := R[B] + R[C]`  | Type-dispatched at VM runtime (Int+Int→Int wrap, Int+Float→Float promotion) |
| `OP_SUB`   | 3 | ABC | A, B, C | `R[A] := R[B] - R[C]`  | Same type dispatch as `OP_ADD` |
| `OP_MUL`   | 4 | ABC | A, B, C | `R[A] := R[B] * R[C]`  | Same type dispatch |
| `OP_DIV`   | 5 | ABC | A, B, C | `R[A] := R[B] / R[C]`  | Always produces Float per `../LANG-CONVENTIONS.md` §1.3 |
| `OP_NEG`   | 6 | ABC | A, B   | `R[A] := -R[B]`         | C unused; loader does not enforce `C == 0` |
| `OP_RET`   | 7 | ABC | A      | `return R[A]`           | B and C unused; loader does not enforce `B == 0` or `C == 0` |

## Register file

Registers are 0-based per module. `module->max_reg` records the highest register
index the module uses. At VM setup the runtime allocates `max_reg + 1`
tagged-value slots. Register values share the `UValue` shape from
`src/umodule.h`: 16 bytes, with a `kind` byte (see `UValKind`), 7 bytes of
padding, and an 8-byte value union — `int64_t i` for integer values, `f` for
float values whose type (`double` or `float`) is selected by `URBI_FLOAT_TYPE`
at compile time (8 = `double`, 4 = `float`).

## Reserved opcode space

The opcode field is 8 bits. Values 0–7 are M1; 8–255 are reserved for M2+
(locals, control flow, calls, reactive primitives). The enum sentinel is
`OP_MAX` (= 8 at M1). The loader (`src/umodule.c`) rejects any opcode with
value `>= OP_MAX`, returning `ULOAD_CORRUPT`.

## Loader verification

The loader (`umodule_deserialize` in `src/umodule.c`) enforces the following
constraints on a deserialized module:

- Opcode is in range `[0, OP_MAX)` — out-of-range opcode returns `ULOAD_CORRUPT`.
- Register `A` is in range `[0, max_reg]` for every instruction.
- Register `B` is in range `[0, max_reg]` for all opcodes except `OP_LOADK`
  (which uses Bx) and `OP_RET` (where B is unused and unchecked).
- Register `C` is in range `[0, max_reg]` only for `OP_ADD`, `OP_SUB`,
  `OP_MUL`, and `OP_DIV`. C is unused and unchecked for `OP_MOVE`, `OP_NEG`,
  and `OP_RET`.
- For `OP_LOADK`, `Bx` is in range `[0, n_constants)`.
- The last instruction is `OP_RET` — modules with no instructions skip this
  check; a non-`OP_RET` terminal returns `ULOAD_CORRUPT`.
- Instruction alignment padding bytes (0–3 bytes before the instruction stream
  begin) must all be zero — a non-zero padding byte returns `ULOAD_CORRUPT`.
- Absolute-line checkpoint PCs are strictly monotonically increasing (each
  subsequent checkpoint PC is greater than the previous one). Details in
  `bytecode-format.md`.

**Not enforced:** unused fields `OP_RET.B`, `OP_RET.C`, `OP_MOVE.C`,
`OP_NEG.C`. The loader tolerates any value in these bytes.
