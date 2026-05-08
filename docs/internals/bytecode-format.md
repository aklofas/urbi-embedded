# Bytecode Format

## Overview

`.urb` is the on-disk serialized form of a `UModule` — the interface between the
front end (emitter) and the back end (VM). The format is pinned to the v1.5
flavor descriptor in the header; the loader rejects any field mismatch with a
field-specific diagnostic. No run-time coercion is attempted.

Source: `src/module/umodule.c` (deserializer + verifier),
`src/emit/uemit_serialize.c` (serializer), `src/module/umodule.h` (structs,
enums, error codes), `src/module/uopcode_shape.{h,c}` (verifier shape table).

---

## Header (24 bytes)

```text
Offset  Size  Field         Value at v1.5
------  ----  ----------    -----------------------------------------------
     0     4  magic         0x55 0x52 0x42 0x49  ("URBI")
     4     1  version       16·major + minor;  v1.5 = 0x15
     5     1  flags         0x00 at v1.5; loader ignores for forward-compat
     6     6  canary        0x19 0x93 0x0D 0x0A 0x1A 0x0A
    12     1  int_width     8  (i64 on every v1 target)
    13     1  float_type    4 (f32) or 8 (f64); per-target pin
    14     1  instr_width   4  (uint32 always)
    15     1  endianness    0  (little-endian; v1 ships little-endian only)
    16     8  reserved      zero at write; loader strictly enforces all-zero
                            (any non-zero byte returns ULOAD_CORRUPT)
```

The canary at offsets 6–11 is the sequence `\x19\x93\r\n\x1a\n`. It detects
FTP text-mode transfer and Windows clipboard paste corruption.

The flavor descriptor fields (offsets 12–15) are checked one at a time. On any
mismatch the loader returns `ULOAD_FLAVOR_MISMATCH` and writes a diagnostic
that names the field (e.g. `"flavor mismatch: float_type expected 8, got 4"`).

### Bytecode version history

Each release that changes wire format bumps the version byte; loading an
older module is a hard error (`ULOAD_UNSUPPORTED_VERSION`) — there is no
forward- or backward-compatibility tolerance at v1.x.

| Byte | Version | Release           | Wire-format additions                  |
|------|---------|-------------------|-----------------------------------------|
| 0x10 | v1.0    | v0.1.0-skeleton   | Initial M1 8-opcode walking-skeleton   |
| 0x11 | v1.1    | v0.2.0-expressions| M2 closures, locals, control flow      |
| 0x12 | v1.2    | v0.3.0-concurrency| M3 control transfer + chunk lifecycle  |
| 0x13 | v1.3    | v0.4.0-objects    | M4 objects: UProto + ic_count/ic_names |
| 0x14 | v1.4    | v0.5.0-reactive   | M5 reactive: 8 new opcodes, UEvent etc.|
| 0x15 | v1.5    | v0.5.6-bytecode   | Wave 4 wire-format completion: nested  |
|      |         |                   | protos + per-proto + root ic_name_strs |
|      |         |                   | + opcode renumber (OP_INVOKE retired;  |
|      |         |                   | M5 reactive 38-45; OP_MAX shrinks 47   |
|      |         |                   | → 46) + verifier shape table.          |

---

## v1.5 Supported Flavor Combinations

The loader accepts exactly these combinations:

| Target               | int\_width | float\_type | instr\_width | endianness |
|---|---|---|---|---|
| Linux x86-64         | 8          | 8           | 4            | 0          |
| STM32H7              | 8          | 8           | 4            | 0          |
| ESP32-C3 (RV32IMC)   | 8          | 4           | 4            | 0          |
| ESP32-S3 (Xtensa LX7)| 8          | 4           | 4            | 0          |

Any other combination produces `ULOAD_FLAVOR_MISMATCH`.

The compile-time macros that pin these values are defined in
`src/module/umodule.h`: `URBI_INT_WIDTH`, `URBI_FLOAT_TYPE`,
`URBI_INSTR_WIDTH`, `URBI_ENDIANNESS`.

---

## Sections

After the 24-byte header the module body is a sequence of sections in fixed
order. All counts and length prefixes use unsigned LEB128 varints except where
noted. All signed integers use zigzag LEB128 (see [Appendix](#appendix-varint-encoding)).

### Metadata

| Field            | Encoding           | Notes                              |
|---|---|---|
| `max_reg`        | 1 byte (raw uint8) | VM allocates `max_reg + 1` register slots |
| `source_name_len`| uvarint            | byte length of source name string  |
| `source_name`    | raw bytes          | UTF-8, not NUL-terminated on wire; loader appends NUL when allocating |

When `source_name_len` is 0, no bytes follow and `source_name` is NULL in
the loaded `UModule`.

### Constants

| Field        | Encoding       | Notes                        |
|---|---|---|
| `n_constants`| uvarint        | count of `UValue` records    |
| records      | see below      | one record per constant       |

Each record starts with a 1-byte kind tag (`UValKind`):

| Kind byte | Enum        | Payload                                        |
|---|---|---|
| 0         | `UVAL_NIL`  | none (rejected at v1.5 — `ULOAD_CORRUPT_TAG`)  |
| 1         | `UVAL_INT`  | zigzag-varint i64                              |
| 2         | `UVAL_FLOAT`| raw `float_type` bytes (4 or 8)               |
| 3         | `UVAL_BOOL` | none (rejected at v1.5 — `ULOAD_CORRUPT_TAG`)  |
| 4         | `UVAL_STR`  | none (rejected at v1.5 — `ULOAD_CORRUPT_TAG`)  |

Kind bytes above 4 are rejected with `ULOAD_CORRUPT_TAG`. At v1.5 only
`UVAL_INT` and `UVAL_FLOAT` constants are produced by the emitter; the
remaining tags are reserved for M6 stdlib (`UVAL_STR` will become live
when string literals land) and have no payload encoding yet.

Float constants are stored as raw bytes in the target's native byte order
(always little-endian at v1.5). 4-byte floats use IEEE 754 single precision;
8-byte floats use IEEE 754 double precision.

### Instructions

| Field          | Encoding   | Notes                                     |
|---|---|---|
| `n_instructions`| uvarint   | count of uint32 instructions              |
| alignment pad  | 0–3 bytes  | zero bytes to align stream to 4-byte boundary from current offset |
| instruction stream | raw    | `n_instructions × 4` bytes, little-endian uint32 each |

The alignment pad brings the first instruction to a 4-byte boundary in the
file. Pad bytes must be zero; the loader returns `ULOAD_CORRUPT` on any
non-zero pad byte.

Instruction encoding is described in [internals/opcodes.md](opcodes.md).

### Synclines

| Field         | Encoding          | Notes                                      |
|---|---|---|
| `n_deltas`    | uvarint           | must equal `n_instructions`               |
| delta stream  | raw int8 bytes    | one byte per instruction                   |
| `n_abs_lines` | uvarint           | count of absolute-line checkpoint records |
| checkpoints   | pairs of uvarints | `(pc, line)` per record                   |

### IC name table (root chunk)

Mirrors UModule.ic_count + ic_names.  At v1.5 the loader stores raw
strings in a transient `ic_name_strs` array; module-instance create
interns each into a USymbol via the receiving VM.

| Field           | Encoding | Notes                                |
|---|---|---|
| `n_ic_names`    | uvarint  | count of root-chunk IC sites        |
| name records    | see below | one per IC site, in OP_GETSLOT order |

Each name record:

| Field        | Encoding | Notes                                       |
|---|---|---|
| `name_len`   | uvarint  | UTF-8 byte length of the IC name           |
| `name_bytes` | raw      | UTF-8, not NUL-terminated on wire           |

A v1.5 module emitted by the in-tree emitter has
`n_ic_names == ic_count` of the root chunk; loader rejects mismatch
with `ULOAD_CORRUPT`.

### Nested protos

After the root chunk's IC name table, the wire format encodes the
`nested[]` UProto array.  Each nested proto is a function definition
referenced by `OP_CLOSURE Bx`.

| Field           | Encoding | Notes                                |
|---|---|---|
| `n_nested`      | uvarint  | count of nested protos              |
| proto records   | see below | one per proto, in nested[] order   |

Each proto record:

| Field            | Encoding   | Notes                                  |
|---|---|---|
| `max_reg`        | 1 byte     | proto's register window                |
| `nupvals`        | 1 byte     | count of upvalues captured             |
| `nparams`        | 1 byte     | count of formal parameters             |
| `n_constants`    | uvarint    | per-proto constant count               |
| constant records | see Constants section above | same encoding |
| `n_instructions` | uvarint    | per-proto instruction count            |
| alignment pad    | 0-3 bytes  | zero bytes to 4-byte boundary          |
| instruction stream | raw      | `n_instructions × 4` bytes             |
| `n_deltas`       | uvarint    | must equal `n_instructions`            |
| delta stream     | raw int8   | one byte per instruction               |
| `n_abs_lines`    | uvarint    | absolute-line checkpoint count         |
| checkpoints      | pairs of uvarints | `(pc, line)` per record         |
| `n_ic_names`     | uvarint    | per-proto IC site count               |
| ic name records  | see IC name table | same encoding                  |

The proto's `instructions` stream is verified the same way as the root
chunk: the opcode-shape walk at `decode_verify` runs once per nested
proto with that proto's `max_reg`, `const_count`, `instr_count`, and
the **root-level** `nested_count` for `OP_CLOSURE Bx` range checks. The
v1.5 in-tree emitter allocates all function literals as flat siblings
under the root `UModule`'s `nested[]` array; an `OP_CLOSURE` inside a
nested proto refers to a sibling slot in the same root array, not into
a per-proto child array. v1.x backlog "deeply-nested closure
verification" tracks switching to per-proto child arrays if a future
emitter shape requires them.

### Migration from v1.4

A v1.4 module loaded into a v1.5 build is rejected with
`ULOAD_UNSUPPORTED_VERSION` (exact-match policy retained from v1.0).
No live-system upgrade tooling exists at v0.5.6; rebuild from source.

A future v1.x revision may relax this; see
`docs/urbi-embedded-design-risks.md` row "v1.x: live-system bytecode
upgrade tooling" for the planning sketch.

---

## Synclines: Delta Encoding

Each instruction position has a corresponding int8 delta byte in the delta
stream. To recover the source line for instruction `pc`, sum all deltas from
the last absolute-line checkpoint up to and including `pc`.

Delta value `INT8_MIN` (`-128`, `0x80`) is a sentinel: the source line for
this instruction is stored in the next absolute-line checkpoint record, not
recoverable by delta accumulation alone. The emitter emits a checkpoint
whenever the line offset from the previous checkpoint would overflow an int8.

Absolute-line checkpoint records are `(pc, line)` pairs. PCs must be strictly
monotonically increasing across all checkpoint records in the stream.

**Example.** Four instructions at source lines 10, 10, 10, 11:

```text
Initial line cursor: 10 (from first checkpoint at pc=0, line=10)
Delta stream: [0, 0, 0, 1]

  pc 0 — delta  0 → line 10
  pc 1 — delta  0 → line 10
  pc 2 — delta  0 → line 10
  pc 3 — delta +1 → line 11
```

Absolute-line checkpoints: `n_abs_lines=1`, record `(pc=0, line=10)`.

---

## Loader Verification

`umodule_deserialize` checks the following. On the first failed check it
stops, sets the diagnostic string, and returns the indicated error code.

**Header checks** (error code `ULOAD_BAD_MAGIC` or `ULOAD_UNSUPPORTED_VERSION`
or `ULOAD_FLAVOR_MISMATCH`):

- Buffer is at least 24 bytes (`ULOAD_TRUNCATED`).
- Bytes 0–3 equal `"URBI"` (`ULOAD_BAD_MAGIC`).
- Version byte equals `0x15` (`ULOAD_UNSUPPORTED_VERSION`).
- Bytes 6–11 equal the canary sequence exactly (`ULOAD_BAD_MAGIC`).
- `int_width` (byte 12) equals `URBI_INT_WIDTH` (`ULOAD_FLAVOR_MISMATCH`).
- `float_type` (byte 13) equals `URBI_FLOAT_TYPE` (`ULOAD_FLAVOR_MISMATCH`).
- `instr_width` (byte 14) equals `URBI_INSTR_WIDTH` (`ULOAD_FLAVOR_MISMATCH`).
- `endianness` (byte 15) equals `URBI_ENDIANNESS` (`ULOAD_FLAVOR_MISMATCH`).
- Reserved bytes 16–23 are strictly all-zero (`ULOAD_CORRUPT`).

**Section checks:**

- Each varint reads without overflow (`ULOAD_CORRUPT_VARINT`) or truncation
  (`ULOAD_TRUNCATED`).
- Source name length does not exceed remaining buffer (`ULOAD_TRUNCATED`).
- `n_constants` does not exceed `UINT16_MAX + 1` (`ULOAD_CORRUPT`).
- Each constant kind byte is `<= UVAL_STR` (4); unknown kinds produce
  `ULOAD_CORRUPT_TAG`.
- Only `UVAL_INT` and `UVAL_FLOAT` constants decode successfully at v1.5;
  all other kinds produce `ULOAD_CORRUPT_TAG`.
- Float constant payload fits in remaining buffer (`ULOAD_TRUNCATED`).
- Instruction alignment pad bytes are zero (`ULOAD_CORRUPT`).
- `n_deltas` equals `n_instructions` (`ULOAD_CORRUPT`).
- Delta stream fits in remaining buffer (`ULOAD_TRUNCATED`).
- Each absolute-line checkpoint `pc` is within `[0, n_instructions)` (`ULOAD_CORRUPT`).
- Checkpoint PCs are strictly monotonically increasing (`ULOAD_CORRUPT`).
- The IC name table count `n_ic_names` equals the root chunk `ic_count`
  (`ULOAD_CORRUPT`).
- Each per-proto record's `n_ic_names` equals that proto's `ic_count`
  (`ULOAD_CORRUPT`).
- No trailing bytes remain after the final section (`ULOAD_CORRUPT`).

**Verifier sweep** (after deserializing all sections):

The verifier walks every instruction in the root chunk plus each non-NULL
nested proto, consulting the file-private `urbi_opcode_shapes[]` table
declared in `src/module/uopcode_shape.h`.  Each operand byte is
interpreted per its `UOperandKind` (register / immediate / packed-nibble /
unused / upvalue index / frame-guard base or count) and range-checked
accordingly.  The Bx field of ABx-format opcodes is interpreted per
`UBxKind` (constant pool index / nested-proto index / signed jump /
handler PC / symbol id) and validated against the matching section count.

The walk lives in `decode_verify` (`src/module/umodule.c`); per-block work
is delegated to `verify_walk_block` so the same code path covers the
root chunk and every nested proto.  Adding a new opcode requires adding
exactly one row to `urbi_opcode_shapes[]` — no per-opcode `switch` exists
in the verifier any more.

Specific cross-byte invariants enforced outside the shape walk:

- The last instruction of the root chunk must be `OP_RET` (no other
  terminator is currently produced by the in-tree emitter; v1.x backlog
  may relax this).
- `OP_PUSH_FRAME_GUARD` requires `A + B <= max_reg + 1` (base + count
  must not exceed the register window).

`OP_JMP` Bx is intentionally NOT range-checked at load time: the
legitimate range depends on absolute PC, and runtime dispatch surfaces
out-of-range jumps as `URBI_ERR_RUNTIME_FATAL`.

On success the function returns `ULOAD_OK`.

Error code names are returned by `umodule_load_error_name(UModuleLoadError)`.

---

## Appendix: Varint Encoding

**Unsigned varints** use LEB128 encoding: 7 payload bits per byte, MSB = 1
means more bytes follow, MSB = 0 means this is the last byte. Maximum 10
bytes for a uint64. Same as Protocol Buffers `uint64`.

```text
Value 300 (0x12C):
  byte 0: 0xAC  (0x2C | 0x80 — low 7 bits, continuation set)
  byte 1: 0x02  (0x02 — high bits, stop)
```

**Signed varints** use ZigZag pre-encoding followed by LEB128. The ZigZag
mapping is:

```c
uint64_t u = ((uint64_t)v << 1) ^ (uint64_t)(v >> 63);  /* encode */
int64_t  v = (int64_t)((u >> 1) ^ -(int64_t)(u & 1));   /* decode */
```

This maps 0 → 0, -1 → 1, 1 → 2, -2 → 3, etc., making small negative
integers compact. Same as Protocol Buffers `sint64`.
