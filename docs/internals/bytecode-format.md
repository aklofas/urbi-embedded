# Bytecode Format

## Overview

`.urb` is the on-disk serialized form of a `UModule` — the interface between the
front end (emitter) and the back end (VM). The format is pinned to the v1.0
flavor descriptor in the header; the loader rejects any field mismatch with a
field-specific diagnostic. No run-time coercion is attempted.

Source: `src/umodule.c` (deserializer + verifier), `src/uemit.c` (serializer),
`src/umodule.h` (structs, enums, error codes).

---

## Header (24 bytes)

```text
Offset  Size  Field         Value at v1.0
------  ----  ----------    -----------------------------------------------
     0     4  magic         0x55 0x52 0x42 0x49  ("URBI")
     4     1  version       16·major + minor;  v1.0 = 0x10
     5     1  flags         0x00 at v1.0; loader ignores for forward-compat
     6     6  canary        0x19 0x93 0x0D 0x0A 0x1A 0x0A
    12     1  int_width     8  (i64 on every v1 target)
    13     1  float_type    4 (f32) or 8 (f64); per-target pin
    14     1  instr_width   4  (uint32 always)
    15     1  endianness    0  (little-endian; v1 ships little-endian only)
    16     8  reserved      zero at write; not validated by loader
```

The canary at offsets 6–11 is the sequence `\x19\x93\r\n\x1a\n`. It detects
FTP text-mode transfer and Windows clipboard paste corruption.

The flavor descriptor fields (offsets 12–15) are checked one at a time. On any
mismatch the loader returns `ULOAD_FLAVOR_MISMATCH` and writes a diagnostic
that names the field (e.g. `"flavor mismatch: float_type expected 8, got 4"`).

---

## v1.0 Supported Flavor Combinations

The loader accepts exactly these combinations:

| Target               | int\_width | float\_type | instr\_width | endianness |
|---|---|---|---|---|
| Linux x86-64         | 8          | 8           | 4            | 0          |
| STM32H7              | 8          | 8           | 4            | 0          |
| ESP32-C3 (RV32IMC)   | 8          | 4           | 4            | 0          |
| ESP32-S3 (Xtensa LX7)| 8          | 4           | 4            | 0          |

Any other combination produces `ULOAD_FLAVOR_MISMATCH`.

The compile-time macros that pin these values are defined in `src/umodule.h`:
`URBI_INT_WIDTH`, `URBI_FLOAT_TYPE`, `URBI_INSTR_WIDTH`, `URBI_ENDIANNESS`.

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
| 0         | `UVAL_NIL`  | none (rejected at v1.0 — `ULOAD_CORRUPT_TAG`)  |
| 1         | `UVAL_INT`  | zigzag-varint i64                              |
| 2         | `UVAL_FLOAT`| raw `float_type` bytes (4 or 8)               |
| 3         | `UVAL_BOOL` | none (rejected at v1.0 — `ULOAD_CORRUPT_TAG`)  |
| 4         | `UVAL_STR`  | none (rejected at v1.0 — `ULOAD_CORRUPT_TAG`)  |

Kind bytes above 4 are rejected with `ULOAD_CORRUPT_TAG`. At v1.0 only
`UVAL_INT` and `UVAL_FLOAT` constants are produced by the emitter.

Float constants are stored as raw bytes in the target's native byte order
(always little-endian at v1.0). 4-byte floats use IEEE 754 single precision;
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
`nested_count` (note: nested protos may themselves reference further
nested protos via `OP_CLOSURE`, but at v1.5 the in-tree emitter does
not emit deeply-nested closures; see backlog "v1.x: deeply-nested
closure verification").

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
- Version byte equals `0x10` (`ULOAD_UNSUPPORTED_VERSION`).
- Bytes 6–11 equal the canary sequence exactly (`ULOAD_BAD_MAGIC`).
- `int_width` (byte 12) equals `URBI_INT_WIDTH` (`ULOAD_FLAVOR_MISMATCH`).
- `float_type` (byte 13) equals `URBI_FLOAT_TYPE` (`ULOAD_FLAVOR_MISMATCH`).
- `instr_width` (byte 14) equals `URBI_INSTR_WIDTH` (`ULOAD_FLAVOR_MISMATCH`).
- `endianness` (byte 15) equals `URBI_ENDIANNESS` (`ULOAD_FLAVOR_MISMATCH`).
- Reserved bytes 16–23 are not validated.

**Section checks:**

- Each varint reads without overflow (`ULOAD_CORRUPT_VARINT`) or truncation
  (`ULOAD_TRUNCATED`).
- Source name length does not exceed remaining buffer (`ULOAD_TRUNCATED`).
- `n_constants` does not exceed `UINT16_MAX + 1` (`ULOAD_CORRUPT`).
- Each constant kind byte is `<= UVAL_STR` (4); unknown kinds produce
  `ULOAD_CORRUPT_TAG`.
- Only `UVAL_INT` and `UVAL_FLOAT` constants decode successfully at v1.0;
  all other kinds produce `ULOAD_CORRUPT_TAG`.
- Float constant payload fits in remaining buffer (`ULOAD_TRUNCATED`).
- Instruction alignment pad bytes are zero (`ULOAD_CORRUPT`).
- `n_deltas` equals `n_instructions` (`ULOAD_CORRUPT`).
- Delta stream fits in remaining buffer (`ULOAD_TRUNCATED`).
- Each absolute-line checkpoint `pc` is within `[0, n_instructions)` (`ULOAD_CORRUPT`).
- Checkpoint PCs are strictly monotonically increasing (`ULOAD_CORRUPT`).
- No trailing bytes remain after the syncline section (`ULOAD_CORRUPT`).

**Verifier sweep** (after deserializing all sections):

- Every opcode is `< OP_MAX` (`ULOAD_CORRUPT`).
- Register A is `<= max_reg` for every instruction (`ULOAD_CORRUPT`).
- For `OP_LOADK`: Bx is `< n_constants` (`ULOAD_CORRUPT`).
- For arithmetic ops (`OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`): registers B
  and C are `<= max_reg` (`ULOAD_CORRUPT`).
- For `OP_MOVE` and `OP_NEG`: register B is `<= max_reg` (`ULOAD_CORRUPT`).
- The last instruction is `OP_RET` (`ULOAD_CORRUPT`).

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
