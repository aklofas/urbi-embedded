# Bytecode Format

## Wire Format Version

The current wire format version byte is **`0x19`** (v1.9), defined as:

```c
/* src/chunk/uchunk.h */
#define URBI_BYTECODE_VERSION_MAJOR  1U
#define URBI_BYTECODE_VERSION_MINOR  9U
#define URBI_BYTECODE_VERSION_BYTE   ((URBI_BYTECODE_VERSION_MAJOR << 4U) | URBI_BYTECODE_VERSION_MINOR)
```

The loader rejects any byte other than `URBI_BYTECODE_VERSION_BYTE` with
`UCHUNK_LOAD_UNSUPPORTED_VERSION`. There is no forward- or backward-compatibility
tolerance: all v1.x changes are hard breaks.

## Overview

`.urb` is the on-disk serialized form of a chunk — the interface between the
front end (emitter) and the back end (VM). At v0.9.2 the `UModule` struct was
deleted; a chunk IS its root `UProto`. The format is pinned to the v1.9
version byte in the header; the loader rejects any version mismatch with a
specific diagnostic. No run-time coercion is attempted.

Source files:

- `src/chunk/uchunk_io.c` — deserializer + verifier
- `src/chunk/uemit_serialize.c` — serializer (via the emit layer)
- `src/chunk/uchunk.h` — structs, enums, error codes, opcode set
- `src/chunk/uproto.h` — UProto definition
- `src/chunk/uopcode_shape.c` — verifier shape table data

---

## Header (24 bytes)

```text
Offset  Size  Field         Value at v1.9
------  ----  ----------    -----------------------------------------------
     0     4  magic         0x55 0x52 0x42 0x49  ("URBI")
     4     1  version       16·major + minor;  v1.9 = 0x19
     5     1  flags         bit 0 = arity self-check discipline (below);
                            bits 1-7 undefined (0 at write); loader
                            ignores unknown bits for forward-compat
     6     6  canary        0x19 0x93 0x0D 0x0A 0x1A 0x0A
    12     1  int_width     8  (i64 on every v1 target)
    13     1  float_type    4 (f32) or 8 (f64); per-target pin
    14     1  instr_width   4  (uint32 always)
    15     1  endianness    0  (little-endian; v1 ships little-endian only)
    16     8  reserved      zero at write; loader strictly enforces all-zero
                            (any non-zero byte returns UCHUNK_LOAD_CORRUPT)
```

The canary at offsets 6–11 is the sequence `\x19\x93\r\n\x1a\n`. It detects
FTP text-mode transfer and Windows clipboard paste corruption. Defined as
`URBI_BYTECODE_CANARY` in `src/chunk/uchunk.h`.

The flavor descriptor fields (offsets 12–15) are checked one at a time. On any
mismatch the loader returns `UCHUNK_LOAD_FLAVOR_MISMATCH` and writes a diagnostic
that names the field (e.g. `"flavor mismatch: float_type expected 8, got 4"`).

### Flags bit 0 — arity self-check discipline

Chunks produced by the emitter carry flag bit 0 set. It declares that every
proto with `nparams >= 1` in the chunk plants a bytecode prologue that (a)
throws a catchable error when fewer than its minimum arity of arguments were
passed and (b) fills omitted defaulted parameters at call time (default
parameter values, `function (a, b = expr)`). Such protos reserve one
synthetic local at register index `nparams`; the VM's `OP_CALL` (and the
internal strand-arm paths) seed it with the actual passed count as an
integer, and relax the VM-side arity check to `nargs <= nparams` (too-many
stays a VM-side typed TypeError).

The loader propagates the bit to every decoded proto
(`UProto.arity_prologue`, a runtime-only field — the per-proto record is
unchanged). Chunks with bit 0 clear (all pre-v0.13.5 blobs) keep the
historic exact-match `OP_CALL` arity check, so old bytecode running on a
new VM sees no semantic shift. The reverse direction — a flagged chunk on
a loader that predates the flag — loads (the flags byte was always
reader-ignored) but its >=1-param functions fail at call time because
nothing seeds the count register; pre-v1.0 there is no cross-version
bytecode stability promise (see the reserved-bytes note above).

### Wire version history

Each release that changes wire format bumps the version byte; loading an
older chunk is a hard error (`UCHUNK_LOAD_UNSUPPORTED_VERSION`) — there is no
forward- or backward-compatibility tolerance at v1.x.

| Byte | Version | Release              | Wire-format additions                  |
|------|---------|----------------------|----------------------------------------|
| 0x10 | v1.0    | v0.1.0-skeleton      | Initial 8-opcode walking-skeleton      |
| 0x11 | v1.1    | v0.2.0-expressions   | Closures, locals, control flow         |
| 0x12 | v1.2    | v0.3.0-concurrency   | Control transfer + chunk lifecycle     |
| 0x13 | v1.3    | v0.4.0-objects       | Objects: UProto + ic_count/ic_names    |
| 0x14 | v1.4    | v0.5.0-reactive      | Reactive: 8 new opcodes, UEvent etc.   |
| 0x15 | v1.5    | v0.5.6-bytecode      | Wire-format completion: nested protos  |
|      |         |                      | + per-proto ic_name_strs +             |
|      |         |                      | opcode renumber (OP_INVOKE retired;    |
|      |         |                      | reactive opcodes 38-45; OP_MAX shrinks |
|      |         |                      | 47 → 46) + verifier shape table.       |
| 0x16 | v1.6    | v0.7.2-esp32         | OP_SELF (slot 47) + OP_CALL method-    |
|      |         |                      | flag bit; OP_MAX = 48.                 |
| 0x17 | v1.7    | v0.8.1-uproto-root   | Body shrinks to header + source_name + |
|      |         |                      | recursive root UProto block. Separate  |
|      |         |                      | UModule wire section removed; root     |
|      |         |                      | now serialized as a standard UProto.   |
| 0x18 | v1.8    | v0.9.2-uproto-only   | Semantic bump only — UModule struct    |
|      |         |                      | deleted; chunk IS its root UProto.     |
|      |         |                      | Byte layout unchanged from v1.7.       |
| 0x19 | v1.9    | v0.10.2-reactive     | Semantic bump only — whenever-event    |
|      |         |                      | + OP_CLOSURE-in-body reactive fixes;   |
|      |         |                      | byte layout unchanged from v1.8.       |

---

## Supported Flavor Combinations

The loader accepts exactly these combinations:

| Target                | int\_width | float\_type | instr\_width | endianness |
|-----------------------|-----------|-------------|--------------|------------|
| Linux x86-64          | 8         | 8           | 4            | 0          |
| STM32H7               | 8         | 8           | 4            | 0          |
| RP2040 (Cortex-M0+)   | 8         | 4           | 4            | 0          |
| ESP32-C3 (RV32IMC)    | 8         | 4           | 4            | 0          |
| ESP32-S3 (Xtensa LX7) | 8         | 4           | 4            | 0          |

Any other combination produces `UCHUNK_LOAD_FLAVOR_MISMATCH`.

The compile-time macros that pin these values are defined in
`src/chunk/uchunk.h`: `URBI_INT_WIDTH`, `URBI_FLOAT_TYPE`,
`URBI_INSTR_WIDTH`, `URBI_ENDIANNESS`.

---

## On-Disk Layout

After the 24-byte header the chunk body is a fixed sequence:

1. Metadata (source_name)
2. Root UProto block (recursive, includes nested protos)

All counts and length prefixes use unsigned LEB128 varints except where
noted. All signed integers use zigzag LEB128 (see [Appendix](#appendix-varint-encoding)).

### Metadata

| Field            | Encoding           | Notes                              |
|------------------|--------------------|-------------------------------------|
| `source_name_len`| uvarint            | byte length of source name string  |
| `source_name`    | raw bytes          | UTF-8, not NUL-terminated on wire; loader appends NUL when allocating |

When `source_name_len` is 0, no bytes follow and `source_name` is NULL in
the loaded root `UProto`.

**v0.9.2 note:** `source_name` was previously a field in `UModule`; with
`UModule` deleted it now lives directly on the root `UProto`. On non-root
nested protos this field is zero-initialized and never written.

### UProto Block

Each UProto (root and nested) is encoded with the same structure:

| Sub-section       | Description                                   |
|-------------------|-----------------------------------------------|
| Proto header      | 3 bytes: max_reg, nupvals, nparams            |
| Constants         | n_constants + records                         |
| Instructions      | n_instructions + alignment + stream           |
| Synclines         | n_deltas + delta stream + abs_lines           |
| IC name table     | n_ic_names + name records                     |
| Nested protos     | n_nested + recursive UProto blocks (v1.7+)   |

#### Proto Header (3 bytes, raw)

| Field     | Encoding     | Notes                                       |
|-----------|--------------|---------------------------------------------|
| `max_reg` | 1 byte raw   | VM allocates `max_reg + 1` register slots  |
| `nupvals` | 1 byte raw   | count of upvalues captured                 |
| `nparams` | 1 byte raw   | count of formal parameters                 |

The loader verifies `nupvals + nparams <= max_reg + 1`. All three fields
fit in uint8 (0–255); the sum cap ensures the register frame can hold both
captured upvalues and parameters.

#### Constants

| Field        | Encoding       | Notes                        |
|--------------|----------------|------------------------------|
| `n_constants`| uvarint        | count of `UValue` records    |
| records      | see below      | one record per constant      |

Each record starts with a 1-byte kind tag (`UValKind`):

| Kind byte | Enum        | Payload                                             |
|-----------|-------------|------------------------------------------------------|
| 0         | `UVAL_NIL`  | rejected (`UCHUNK_LOAD_CORRUPT_TAG`)                 |
| 1         | `UVAL_INT`  | zigzag-varint i64                                   |
| 2         | `UVAL_FLOAT`| raw `float_type` bytes (4 or 8), little-endian      |
| 3         | `UVAL_BOOL` | rejected (`UCHUNK_LOAD_CORRUPT_TAG`)                 |
| 4         | `UVAL_STR`  | uvarint length + raw UTF-8 bytes (accepted at load) |

Kind bytes 5–10 (`UVAL_CLOSURE`, `UVAL_VOID`, `UVAL_STRAND`, `UVAL_OBJECT`,
`UVAL_EVENT`, `UVAL_HOST_FN`) are runtime-only and rejected at load with
`UCHUNK_LOAD_CORRUPT_TAG`. Kind bytes above 10 are also rejected.

**UVAL_STR at load (v0.6.x+):** The loader allocates a NUL-terminated buffer
via the module allocator and marks the slot with `_pad[0] = 1` (owned-by-module
flag). Module-instance create interns the bytes against the runtime VM and
clears the marker. `uchunk_destroy` frees any `v.p` whose owner-flag is still
set.

**UVAL_NIL and UVAL_BOOL** are never produced by the emitter in constant
pools (NIL is `OP_LOADNIL`, BOOL is `OP_LOADBOOL` immediate). Hand-crafted
bytecode smuggling these kinds is rejected.

`n_constants` must not exceed `UINT16_MAX + 1` (`UCHUNK_LOAD_CORRUPT`).

#### Instructions

| Field              | Encoding   | Notes                                           |
|--------------------|------------|-------------------------------------------------|
| `n_instructions`   | uvarint    | count of uint32 instructions                   |
| alignment pad      | 0–3 bytes  | zero bytes to align stream to 4-byte boundary  |
| instruction stream | raw        | `n_instructions × 4` bytes, little-endian uint32 each |

`n_instructions` is capped at `URBI_MAX_INSTRS_PER_PROTO` = `1 << 20`
(1 048 576 instructions per proto). Exceeding this returns
`UCHUNK_LOAD_OVERSIZED`.

The alignment pad brings the first instruction to a 4-byte boundary from
the start of the buffer. Pad bytes must be zero;
the loader returns `UCHUNK_LOAD_CORRUPT` on any non-zero pad byte.

Instruction encoding is described in [internals/opcodes.md](opcodes.md).
`OP_MAX` is currently **48** (opcodes 0–47, after the addition of `OP_SELF`
at slot 47 in v1.6).

#### Synclines

| Field         | Encoding          | Notes                                      |
|---------------|-------------------|--------------------------------------------|
| `n_deltas`    | uvarint           | must equal `n_instructions`               |
| delta stream  | raw int8 bytes    | one byte per instruction                   |
| `n_abs_lines` | uvarint           | count of absolute-line checkpoint records |
| checkpoints   | pairs of uvarints | `(pc, line)` per record                   |

`n_abs_lines` is additionally capped at `n_instructions` — every checkpoint
references a unique pc, so more checkpoints than instructions is impossible.

#### IC Name Table

Encodes the per-proto IC site names. Mirrors `UProto.ic_count` +
`UProto.ic_name_strs`. At load the deserializer stores raw strings in a
`ic_name_strs` array; module-instance create interns each into a `USymbol`
via the receiving VM and populates `ic_names`.

IC-bearing opcodes at v1.9: `OP_GETSLOT`, `OP_SETSLOT`,
`OP_GETSLOT_CHANGE_EVENT`, `OP_SELF`. The C operand of each carries the
`ic_idx` (0-based index into `ic_names`).

| Field           | Encoding | Notes                                |
|-----------------|----------|---------------------------------------|
| `n_ic_names`    | uvarint  | count of IC sites; capped at 256     |
| name records    | see below | one per IC site, in OP_GETSLOT order |

Each name record:

| Field        | Encoding | Notes                                      |
|--------------|----------|--------------------------------------------|
| `name_len`   | uvarint  | UTF-8 byte length; capped at 256          |
| `name_bytes` | raw      | UTF-8, not NUL-terminated on wire         |

The verifier cross-checks: `ic_count` must not exceed the count of
IC-bearing opcodes observed in the instruction stream (`UCHUNK_LOAD_CORRUPT`
if the declared count exceeds the observed count).

#### Nested Protos (recursive, v1.7+)

After the IC name table, each UProto encodes its direct children:

| Field           | Encoding | Notes                                |
|-----------------|----------|---------------------------------------|
| `n_nested`      | uvarint  | count of direct child protos; capped at 1024 |
| proto records   | recursive | one UProto block per child (depth-first pre-order) |

Each child proto is a full UProto block (same structure: header + constants +
instructions + synclines + IC names + its own nested protos).

**v0.8.5 truly-recursive emitter:** function literals are allocated under their
enclosing proto's `nested[]` array. An `OP_CLOSURE Bx` inside a proto uses
Bx as an index into *that proto's* own `nested[]`, not the root's flat array.
Both the emitter and the deserializer walk in DFS pre-order, so the `ic_index`
(DFS serial) assignment is identical regardless of load source.

**Root-proto absorbed fields (v0.9.2):** The root `UProto` carries additional
fields that are zero-initialized and meaningless on non-root protos:
`source_name`, `origin_vm`, `next_proto_serial`, `total_proto_count`,
`next_in_realm`, `owning_realm`, `heap_allocated`. None of these are written
to the wire — they are runtime state populated after deserialization.

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

`uchunk_deserialize` checks the following. On the first failed check it
stops, sets the diagnostic string, and returns the indicated error code.

**Header checks:**

- Buffer is at least 24 bytes (`UCHUNK_LOAD_TRUNCATED`).
- Bytes 0–3 equal `"URBI"` (`UCHUNK_LOAD_BAD_MAGIC`).
- Version byte equals `URBI_BYTECODE_VERSION_BYTE` = `0x19` (`UCHUNK_LOAD_UNSUPPORTED_VERSION`).
- Bytes 6–11 equal `URBI_BYTECODE_CANARY` exactly (`UCHUNK_LOAD_BAD_MAGIC`).
- `int_width` (byte 12) equals `URBI_INT_WIDTH` (`UCHUNK_LOAD_FLAVOR_MISMATCH`).
- `float_type` (byte 13) equals `URBI_FLOAT_TYPE` (`UCHUNK_LOAD_FLAVOR_MISMATCH`).
- `instr_width` (byte 14) equals `URBI_INSTR_WIDTH` (`UCHUNK_LOAD_FLAVOR_MISMATCH`).
- `endianness` (byte 15) equals `URBI_ENDIANNESS` (`UCHUNK_LOAD_FLAVOR_MISMATCH`).
- Reserved bytes 16–23 are strictly all-zero (`UCHUNK_LOAD_CORRUPT`).

**Per-proto section checks (applied recursively to root + every nested proto):**

- Proto header truncation: `d->off + 3 <= size` (`UCHUNK_LOAD_TRUNCATED`).
- `nupvals + nparams <= max_reg + 1` (`UCHUNK_LOAD_CORRUPT`).
- Each varint reads without overflow (`UCHUNK_LOAD_CORRUPT_VARINT`) or truncation (`UCHUNK_LOAD_TRUNCATED`).
- `n_constants` does not exceed `UINT16_MAX + 1` (`UCHUNK_LOAD_CORRUPT`).
- Each constant kind byte is `<= UVAL_STR` (4); unknown kinds produce `UCHUNK_LOAD_CORRUPT_TAG`.
- Only `UVAL_INT`, `UVAL_FLOAT`, and `UVAL_STR` decode successfully; `UVAL_NIL` and `UVAL_BOOL` produce `UCHUNK_LOAD_CORRUPT_TAG`.
- Float constant payload fits in remaining buffer (`UCHUNK_LOAD_TRUNCATED`).
- `n_instructions` does not exceed `URBI_MAX_INSTRS_PER_PROTO` (`UCHUNK_LOAD_OVERSIZED`).
- Instruction alignment pad bytes are zero (`UCHUNK_LOAD_CORRUPT`).
- `n_deltas` equals `n_instructions` (`UCHUNK_LOAD_CORRUPT`).
- `n_abs_lines` does not exceed `n_instructions` (`UCHUNK_LOAD_CORRUPT`).
- Each absolute-line checkpoint `pc` is within `[0, n_instructions)` (`UCHUNK_LOAD_CORRUPT`).
- Checkpoint PCs are strictly monotonically increasing (`UCHUNK_LOAD_CORRUPT`).
- `n_ic_names` does not exceed 256 (`UCHUNK_LOAD_CORRUPT`).
- Each IC name length does not exceed 256 (`UCHUNK_LOAD_CORRUPT`).
- `n_nested` does not exceed 1024 (`UCHUNK_LOAD_CORRUPT`).
- No trailing bytes remain after the final section (`UCHUNK_LOAD_CORRUPT`).

**Current gaps (Wave 2 will tighten):**

The loader does not currently check that `n_ic_names` on a nested proto
equals the `ic_count` derived from its instruction stream during the verifier
sweep — instead it relies on the verifier cross-check described below.
No check requires that IC name strings are valid UTF-8 (only byte-length
limits are enforced).

**Verifier sweep** (after deserializing all sections):

The verifier walks every instruction in the root UProto plus all nested
protos recursively (`verify_proto_recursive` in `src/chunk/uchunk_io.c`),
consulting the `urbi_opcode_shapes[]` table declared in
`src/chunk/uopcode_shape.c`. Each operand byte is interpreted per its
`UOperandKind` (register / immediate / packed-nibble / unused / upvalue
index / frame-guard base or count) and range-checked accordingly. The Bx
field of ABx-format opcodes is interpreted per `UBxKind` (constant pool
index / nested-proto index / signed jump / handler PC / symbol id) and
validated against the matching section count.

At v1.9, the verifier passes **each proto's own `nested_count`** for
`OP_CLOSURE Bx` range checks (per-parent index space). This matches the
truly-recursive emitter contract added in v0.8.5.

Specific cross-byte invariants enforced outside the shape walk:

- The last instruction of each proto block must be `OP_RET` (v1.x backlog
  may relax this).
- `OP_PUSH_FRAME_GUARD` requires `A + B <= max_reg + 1` (base + count
  must not exceed the register window).
- `ic_count` must not exceed the count of IC-bearing opcodes
  (`OP_GETSLOT`, `OP_SETSLOT`, `OP_GETSLOT_CHANGE_EVENT`, `OP_SELF`) in
  the instruction stream.

`OP_JMP` Bx is intentionally NOT range-checked at load time: the
legitimate range depends on absolute PC, and runtime dispatch surfaces
out-of-range jumps as `URBI_ERR_RUNTIME_FATAL`.

On success the function returns `UCHUNK_LOAD_OK`.

---

## Error Codes

All error codes are returned by `uchunk_deserialize` and named by
`uchunk_load_error_name(UChunkLoadError)` (returns e.g.
`"UCHUNK_LOAD_BAD_MAGIC"`).

| Code                               | Meaning                                              |
|------------------------------------|------------------------------------------------------|
| `UCHUNK_LOAD_OK`                   | Success                                              |
| `UCHUNK_LOAD_BAD_MAGIC`            | Magic bytes or canary mismatch                       |
| `UCHUNK_LOAD_UNSUPPORTED_VERSION`  | Version byte is not `URBI_BYTECODE_VERSION_BYTE`     |
| `UCHUNK_LOAD_FLAVOR_MISMATCH`      | Any flavor descriptor field mismatch                 |
| `UCHUNK_LOAD_TRUNCATED`            | Buffer ended before a field could be read            |
| `UCHUNK_LOAD_CORRUPT_VARINT`       | LEB128 varint overflowed uint64 (> 10 bytes used)   |
| `UCHUNK_LOAD_CORRUPT_TAG`          | Constant kind byte out of range or not decodable    |
| `UCHUNK_LOAD_CORRUPT`              | Any other structural invariant violation             |
| `UCHUNK_LOAD_OOM`                  | Allocation failure during decode                     |
| `UCHUNK_LOAD_INVALID_ARG`          | NULL `out_root` or NULL `buf` passed to deserializer |
| `UCHUNK_LOAD_OVERSIZED`            | Count field exceeds compile-time cap                 |

---

## Migration Policy

Loading an older chunk byte is a hard error (`UCHUNK_LOAD_UNSUPPORTED_VERSION`).
No live-system upgrade tooling exists; rebuild from source to migrate. A future
v1.x revision may relax this; live-system bytecode upgrade tooling is on the
post-v1.0 roadmap.

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

---

## Post-freeze policy

> Status: wire format pinned at v1.9 / 0x19 by v0.10.6-stabilization.
> See `docs/api-stability.md` for the C-API analogue.

To break the freeze after v0.10.6:

1. **CHANGELOG.md entry** under the new tag's section, explaining the bump
   and the loader's strict-rejection contract.
2. **Static-assert bump** in `src/chunk/uchunk_io.c`.  The build will not
   compile until updated.
3. **Macros bump** in `src/chunk/uchunk.h` (`URBI_BYTECODE_VERSION_MINOR`
   and the derived `_BYTE`).
4. **This document update** — table at the top + any byte-layout changes
   below.

The loader does NOT silently coerce wire formats.  A v1.x consumer
loading a v1.(x+1) blob receives `UCHUNK_LOAD_UNSUPPORTED_VERSION`.  Live
upgrade tooling is a v1.x deferral (see `docs/urbi-embedded-design-risks.md`).
