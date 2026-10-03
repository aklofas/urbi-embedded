# Bytecode Format

## Wire Format Version

The current wire format version byte is **`0x20`** (v2.0), defined as:

```c
/* src/chunk/uchunk.h */
#define URBI_BYTECODE_VERSION_MAJOR  2U
#define URBI_BYTECODE_VERSION_MINOR  0U
#define URBI_BYTECODE_VERSION_BYTE   ((URBI_BYTECODE_VERSION_MAJOR << 4U) | URBI_BYTECODE_VERSION_MINOR)
```

The loader rejects any byte other than `URBI_BYTECODE_VERSION_BYTE` with
`UCHUNK_LOAD_UNSUPPORTED_VERSION`. There is no forward- or backward-compatibility
tolerance: every version change is a hard break. Re-emit from source to migrate.

## Overview

`.urb` is the on-disk serialized form of a chunk — the interface between the
front end (the parser + emitter under `src/parse/` and `src/emit/`) and the
back end (the runtime under `src/rt/`). A chunk IS its root `UProto`; there
is no separate module struct.

Source files:

- `src/chunk/uchunk_io.c` — deserializer (header, metadata, every UProto section)
- `src/chunk/uchunk_verify.c` — the two load-time verifier passes
- `src/emit/uemit_serialize.c` — serializer
- `src/chunk/uchunk.h` — structs, enums, error codes, the opcode set
- `src/chunk/uproto.h` — `UProto` definition
- `src/chunk/uopcode_shape.{h,c}` — verifier shape table

---

## Header (24 bytes)

```text
Offset  Size  Field         Value at v2.0
------  ----  ----------    -----------------------------------------------
     0     4  magic         0x55 0x52 0x42 0x49  ("URBI")
     4     1  version       16·major + minor;  v2.0 = 0x20
     5     1  flags         bit 0 = arity self-check discipline (below);
                            bits 1-7 undefined (0 at write); loader
                            ignores unknown bits for forward-compat
     6     6  canary        0x19 0x93 0x0D 0x0A 0x1A 0x0A
    12     1  int_width     8  (i64 on every target)
    13     1  float_type    8  (f64/double — the only value the loader
                            accepts; see "Float flavor" below)
    14     1  instr_width   4  (uint32 always)
    15     1  endianness    0  (little-endian; little-endian only)
    16     8  reserved      zero at write; loader strictly enforces all-zero
                            (any non-zero byte returns UCHUNK_LOAD_CORRUPT)
```

The canary at offsets 6–11 is the sequence `\x19\x93\r\n\x1a\n`. It detects
FTP text-mode transfer and Windows clipboard paste corruption. Defined as
`URBI_BYTECODE_CANARY` in `src/chunk/uchunk.h`.

The flavor descriptor fields (offsets 12–15) are checked one at a time. On any
mismatch the loader returns `UCHUNK_LOAD_FLAVOR_MISMATCH` and writes a diagnostic
that names the field (e.g. `"flavor mismatch: float_type expected 8, got 4"`).

**Float flavor.** The per-target `f32` float flavor (the old `URBI_FLOAT_TYPE`
knob) has been retired: `UValue`'s float arm is always `double`, on every
target including 32-bit MCUs, and byte 13 is checked against a hardcoded 8
rather than a build-time macro. A chunk built for a pre-retirement target
with `float_type == 4` is rejected by this build, as any flavor mismatch is.

### Flags bit 0 — arity self-check discipline

Chunks produced by the emitter carry flag bit 0 set. It declares that every
proto with `nparams >= 1` in the chunk plants a bytecode prologue that
throws a catchable error when fewer than its minimum arity of arguments
were passed, and fills omitted defaulted parameters at call time. Such
protos reserve one synthetic local at register index `nparams`; `OP_CALL`
seeds it with the actual passed count, and the VM-side arity check relaxes
to `nargs <= nparams` (too many is still a VM-side TypeError).

The loader propagates the bit to every decoded proto (`UProto.arity_prologue`,
a runtime-only field). Chunks with bit 0 clear keep the historic exact-match
arity check.

### Wire version history

Each release that changes wire format bumps the version byte; loading an
older chunk is a hard error (`UCHUNK_LOAD_UNSUPPORTED_VERSION`).

| Byte | Version | Wire-format change |
|------|---------|---------------------|
| 0x1A | v1.10   | Last byte of the pre-refoundation wire format (50 opcodes, `OP_SETSLOT_UPDATE` at slot 49, per-target float flavor). |
| 0x20 | v2.0    | The frontend rewrite. Opcode set replaced wholesale: 41 opcodes (down from 50), the control-transfer family collapsed to `SCOPE_TRY` / `SCOPE_TAG` / `SCOPE_POP` / `UNWIND_TO` (replacing `TRY_BEGIN` / `TRY_END` / `PUSH_TAG` / `POP_TAG` / `PUSH_FRAME_GUARD`), the six reactive install opcodes collapsed to one `OP_INSTALL` with a mode operand, a new `OP_EXTARG` prefix widens slot-site indices past 255, the IC name table is renamed `site_names` (same wire shape: count + length-prefixed UTF-8 strings, cap raised from 256 to 65,535), and the float flavor byte is pinned to 8 (double) on every target. Header layout, varint encoding, the constant pool, the instruction/syncline section shapes, and the nested-proto recursion are byte-for-byte unchanged from v1.10. |

The wire-format history before v1.10 — the eight bumps from the walking
skeleton through the reactive-opcode renumbering, `OP_SELF`, the UModule
removal, and the whenever-event fix — is preserved in the git history of
this file; it is not repeated here because none of those shapes exist any
more to compare against.

---

## Supported Flavor Combinations

The loader accepts exactly one combination, on every target:

| int\_width | float\_type | instr\_width | endianness |
|-----------|-------------|--------------|------------|
| 8         | 8           | 4            | 0          |

Any other combination produces `UCHUNK_LOAD_FLAVOR_MISMATCH`. Before the
float-flavor retirement, 32-bit MCU targets without an FPU shipped
`float_type = 4`; that distinction is gone — every target now carries
`double` arithmetic in `UValue`, compiled in software where there is no FPU.

The compile-time macros that pin int_width/instr_width/endianness are
defined in `src/chunk/uchunk.h`: `URBI_INT_WIDTH`, `URBI_INSTR_WIDTH`,
`URBI_ENDIANNESS`. There is no `URBI_FLOAT_TYPE` macro any more.

---

## On-Disk Layout

After the 24-byte header the chunk body is a fixed sequence:

1. Metadata (`source_name`)
2. Root UProto block (recursive, includes nested protos)

All counts and length prefixes use unsigned LEB128 varints except where
noted. All signed integers use zigzag LEB128 (see [Appendix](#appendix-varint-encoding)).

### Metadata

| Field            | Encoding           | Notes                              |
|------------------|--------------------|-------------------------------------|
| `source_name_len`| uvarint            | byte length of source name string  |
| `source_name`    | raw bytes          | UTF-8, not NUL-terminated on wire; loader appends NUL when allocating |

When `source_name_len` is 0, no bytes follow and `source_name` is NULL in
the loaded root `UProto`. `source_name` lives directly on the root `UProto`;
on non-root nested protos the field is zero-initialized and never written.

### UProto Block

Each UProto (root and nested) is encoded with the same structure, in this
order:

| Sub-section       | Description                                   |
|-------------------|-----------------------------------------------|
| Proto header      | 3 bytes: max_reg, nupvals, nparams            |
| Constants         | n_constants + records                         |
| Instructions      | n_instructions + alignment + stream           |
| Synclines         | n_deltas + delta stream + abs_lines           |
| Site names        | n_site_names + name records                   |
| Nested protos     | n_nested + recursive UProto blocks            |

#### Proto Header (3 bytes, raw)

| Field     | Encoding     | Notes                                       |
|-----------|--------------|---------------------------------------------|
| `max_reg` | 1 byte raw   | VM allocates `max_reg + 1` register slots  |
| `nupvals` | 1 byte raw   | count of upvalues captured                 |
| `nparams` | 1 byte raw   | count of formal parameters                 |

The loader verifies `nupvals + nparams <= max_reg + 1`.

#### Constants

| Field        | Encoding       | Notes                        |
|--------------|----------------|------------------------------|
| `n_constants`| uvarint        | count of `UValue` records; must not exceed `UINT16_MAX + 1` |
| records      | see below      | one record per constant      |

Each record starts with a 1-byte kind tag (`UValKind`):

| Kind byte | Enum        | Payload                                             |
|-----------|-------------|------------------------------------------------------|
| 0         | `UVAL_NIL`  | rejected (`UCHUNK_LOAD_CORRUPT_TAG`) — NIL is `OP_LOADNIL`, never a constant |
| 1         | `UVAL_INT`  | zigzag-varint i64                                   |
| 2         | `UVAL_FLOAT`| raw 8-byte `double`, little-endian                  |
| 3         | `UVAL_BOOL` | rejected (`UCHUNK_LOAD_CORRUPT_TAG`) — BOOL is an `OP_LOADBOOL` immediate |
| 4         | `UVAL_STR`  | uvarint length + raw UTF-8 bytes                    |

Kind bytes above 4 (`UVAL_CLOSURE`, `UVAL_VOID`, `UVAL_STRAND`, `UVAL_OBJECT`,
`UVAL_EVENT`, `UVAL_HOST_FN`, and anything undefined) are runtime-only or
nonexistent and rejected at load with `UCHUNK_LOAD_CORRUPT_TAG`.

**UVAL_STR at load:** the loader allocates a NUL-terminated buffer through
the module allocator; `uchunk_destroy` frees it.

#### Instructions

| Field              | Encoding   | Notes                                           |
|--------------------|------------|-------------------------------------------------|
| `n_instructions`   | uvarint    | count of uint32 instructions; capped at `URBI_MAX_INSTRS_PER_PROTO` (`1 << 20`, 1,048,576) |
| alignment pad      | 0–3 bytes  | zero bytes to align the stream to a 4-byte boundary |
| instruction stream | raw        | `n_instructions × 4` bytes, little-endian uint32 each |

Exceeding the instruction cap returns `UCHUNK_LOAD_OVERSIZED`. Pad bytes
must be zero; a non-zero pad byte is `UCHUNK_LOAD_CORRUPT`.

Instruction encoding is described in [opcodes.md](opcodes.md). `OP_MAX` is
**41** (opcodes 0–40).

An `OP_CLOSURE` instruction is followed in the stream by `nupvals`
upvalue-descriptor pseudo-instructions (one per upvalue the child closure
captures) before the next real instruction; these are not separately
counted or length-prefixed — the loader and the verifier both derive the
count from the referenced child proto's own `nupvals` field. Each pseudo-instruction
packs `in_stack` (B: 0 or 1) and `src_idx` (C): `in_stack == 1` captures a
local register of the enclosing function (`src_idx <= max_reg`); `in_stack
== 0` re-captures one of the enclosing function's own upvalues (`src_idx <
nupvals` of the *enclosing* function).

#### Synclines

| Field         | Encoding          | Notes                                      |
|---------------|-------------------|--------------------------------------------|
| `n_deltas`    | uvarint           | must equal `n_instructions`               |
| delta stream  | raw int8 bytes    | one byte per instruction                   |
| `n_abs_lines` | uvarint           | count of absolute-line checkpoint records; capped at `n_instructions` |
| checkpoints   | pairs of uvarints | `(pc, line)` per record, strictly increasing in `pc` |

See [Synclines: Delta Encoding](#synclines-delta-encoding) below.

#### Site names

Encodes the per-proto slot-site names. Mirrors `UProto.site_count` +
`UProto.site_name_strs`. At load the deserializer stores raw strings in a
`site_name_strs` array; binding the chunk to a VM (`uproto_bind`, `src/rt/uexec.c`)
interns each one.

Site-bearing opcodes at v2.0: `OP_GETSLOT`, `OP_SETSLOT`,
`OP_SETSLOT_UPDATE`, `OP_GETSLOT_CHANGE_EVENT`, `OP_SELF`. The C operand of
each carries the low byte of the site index; a preceding `OP_EXTARG`
supplies the high bits for a site index above 255 (see
[opcodes.md](opcodes.md#extarg-wide-site-indices)).

| Field           | Encoding | Notes                                |
|-----------------|----------|---------------------------------------|
| `n_site_names`  | uvarint  | count of sites; capped at **65,535** — a site index is a byte widened by `OP_EXTARG` to 16 bits, and `site_count` itself is a `uint16_t` |
| name records    | see below | one per site |

Before allocating the name-pointer array, the loader additionally rejects a
count the remaining buffer cannot possibly hold: each name costs at least
its one-byte length prefix, so `n_site_names` greater than `size - off`
remaining bytes is `UCHUNK_LOAD_CORRUPT` — a few crafted input bytes cannot
request a 512 KB pointer array.

Each name record:

| Field        | Encoding | Notes                                      |
|--------------|----------|--------------------------------------------|
| `name_len`   | uvarint  | UTF-8 byte length; capped at 256          |
| `name_bytes` | raw      | UTF-8, not NUL-terminated on wire         |

The verifier additionally requires `site_count` not to exceed the number of
site-bearing instructions actually observed in the proto's own instruction
stream (`UCHUNK_LOAD_CORRUPT` otherwise) — a table claiming more sites than
the stream uses carries names no instruction indexes.

#### Nested Protos (recursive)

After the site-name table, each UProto encodes its direct children:

| Field           | Encoding | Notes                                |
|-----------------|----------|---------------------------------------|
| `n_nested`      | uvarint  | count of direct child protos; capped at 1024 |
| proto records   | recursive | one UProto block per child (depth-first pre-order) |

Each child proto is a full UProto block (the same structure: header,
constants, instructions, synclines, site names, and its own nested
protos). Nesting depth is capped at 64 (`UCHUNK_MAX_PROTO_DEPTH`); a chunk
deeper than that is `UCHUNK_LOAD_CORRUPT`.

Function literals are allocated under their enclosing proto's `nested[]`
array — truly recursive, not a flat per-module table. An `OP_CLOSURE Bx`
inside a proto indexes into *that proto's own* `nested[]`, not the root's.
Both the emitter and the deserializer walk in DFS pre-order.

**Root-proto-only fields:** the root `UProto` carries additional fields
that are zero-initialized and meaningless on non-root protos: `source_name`,
`origin_vm`, `next_proto_serial`, `total_proto_count`, `next_in_realm`,
`owning_realm`, `heap_allocated`. None of these are written to the wire —
they are runtime state populated after deserialization (or after binding,
for the in-process compile path).

---

## Synclines: Delta Encoding

Each instruction position has a corresponding int8 delta byte in the delta
stream. To recover the source line for instruction `pc`, sum all deltas from
the last absolute-line checkpoint up to and including `pc`.

Delta value `INT8_MIN` (`-128`, `0x80`) is a sentinel: the source line for
this instruction is stored in the next absolute-line checkpoint record, not
recoverable by delta accumulation alone. The emitter emits a checkpoint
whenever the line offset from the previous checkpoint would overflow an int8.

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

`uchunk_deserialize` decodes the header and every section; on the first
failed structural check (truncation, bad varint, a count past its cap) it
stops, sets the diagnostic string, and returns the indicated error code —
see the per-section notes above for each cap.

After every section decodes, two separate verifier passes
(`src/chunk/uchunk_verify.c`) walk the root UProto and every nested proto,
recursively, DFS pre-order:

**Pass 1 — per-instruction shape** (`urbi_chunk_decode_verify` →
`verify_walk_block`): for every instruction, the opcode must be `< OP_MAX`;
each operand byte is checked against `urbi_opcode_shapes[op]`'s
`UOperandKind` for that field (a register must be `<= max_reg`; `UOPK_REG_OR_NONE`
additionally accepts `0xFF`; an immediate bool must be 0 or 1; an `INSTALL`
mode must be 1–7); an ABX opcode's `Bx` is checked per its `UBxKind` against
the matching section count (`OP_LOADK` against `const_count`, `OP_CLOSURE`
against `nested_count`, the scope-handler opcodes against `instr_count`).
Beyond the per-field table, pass 1 enforces the rules a shape row cannot
express:

- `OP_SELF` requires `A + 1 <= max_reg`.
- `OP_CALL` requires `A + B <= max_reg + 1`.
- `OP_INSTALL` requires `A + 2 <= max_reg`, and that `C`'s flag bits are a
  subset of `UINSTALL_F_HAS_BODY | UINSTALL_F_HAS_ALT` (and `C == 0` when
  `B == UINSTALL_WAITUNTIL`).
- `OP_FORK` requires a detached mode (`UFORK_DETACH`) to carry no handle
  (`B == 0xFF`) and a joined mode (`UFORK_JOIN`) to carry one (`B != 0xFF`).
- `OP_JOIN_WAIT` requires the immediately preceding instruction to be the
  `OP_FORK` that joined it, with `FORK.C == UFORK_JOIN` and `FORK.B ==
  JOIN_WAIT.A` — the adjacency a hand-built chunk could otherwise use to
  read a strand handle eager reaping already freed.
- `OP_SCOPE_TRY`'s `A` must be exactly one of `USCOPE_F_HAS_CATCH` or
  `USCOPE_F_HAS_FINALLY` — never both (which would reach the walker's
  private `RUNNING` bit straight from bytecode) and never neither.
- `OP_SCOPE_POP`'s `A` must name exactly one valid entry kind, with
  `USCOPE_POP_RUN_FINALLY` legal only on a TRY entry.
- `OP_EXTARG` may only precede a site-bearing opcode, may not be the last
  instruction, and a widened site index must be `< site_count`.
- An `OP_CLOSURE`'s upvalue prelude (`nupvals` pseudo-instructions) must lie
  within the instruction array; pass 1 skips over the prelude words rather
  than treating them as real instructions (so they do not spuriously
  satisfy the "instruction after EXTARG" or site-counting rules).
- The last instruction of the block must be `OP_RET`.
- `site_count` must not exceed the number of site-bearing instructions seen.

**Pass 2 — per-sequence bounds** (`urbi_chunk_verify_bounds` →
`verify_bounds_proto`): checks that need a sequence of instructions or
context pass 1's single-instruction table cannot express:

- Each `OP_CLOSURE` upvalue pseudo-instruction's `in_stack` is 0 or 1; if 1,
  `src_idx <= max_reg`; if 0, `src_idx < nupvals` of the *enclosing* proto
  (re-capturing a parent upvalue that must itself exist).
- `OP_JMP`'s resolved target (see [opcodes.md](opcodes.md#jump-encoding))
  lies in `[0, instr_count)`, and does not land on the instruction right
  after an `OP_EXTARG`.
- `OP_CALL`'s `C` low 7 bits (`nresults+1`) must not be 0.
- Every `OP_SCOPE_TRY` / `OP_SCOPE_TAG` / `OP_UNWIND_TO` handler/target PC
  (already range-checked by pass 1) does not land right after an `OP_EXTARG`
  either.

On success both passes return `UCHUNK_LOAD_OK`.

**The verifier runs only on `uchunk_deserialize`** — loading a `.urb` blob
from outside the process. The in-memory compile path (`ufront_compile` →
`uproto_bind`, used by `urbi_compile`/`urbi_run`/the REPL/the CLI's `-e`
and `-f`) never calls either pass: a chunk compiled in the same process is
trusted on the strength of the emitter alone. See
`docs/urbi-embedded-design-risks.md` (workspace root), `phase4` section.

---

## Error Codes

All error codes are returned by `uchunk_deserialize` and named by
`uchunk_load_error_name(UChunkLoadError)`.

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
| `UCHUNK_LOAD_OVERSIZED`            | Count field exceeds a compile-time cap               |
| `UCHUNK_LOAD_TRUNCATED_UPVALUES`   | `OP_CLOSURE` upvalue prelude extends past bytecode end |
| `UCHUNK_LOAD_MALFORMED_UPVALUE`    | An upvalue pseudo-instruction's `in_stack` or `src_idx` is invalid |
| `UCHUNK_LOAD_JMP_OUT_OF_BOUNDS`    | `OP_JMP`'s resolved target falls outside `[0, instr_count)` |
| `UCHUNK_LOAD_CALL_NRESULTS_ZERO`   | `OP_CALL`'s `C` low 7 bits are 0 |
| `UCHUNK_LOAD_RESERVED_OPCODE`      | Opcode reserved/unimplemented at this wire version |
| `UCHUNK_LOAD_BAD_EXTARG`           | `OP_EXTARG` misplaced: last, doubled, before a non-site opcode, or a jump/handler lands right after it |

---

## Migration Policy

Loading an older chunk byte is a hard error (`UCHUNK_LOAD_UNSUPPORTED_VERSION`).
No live-system upgrade tooling exists; rebuild from source to migrate. Live-system
bytecode upgrade tooling remains on the post-v1.0 roadmap.

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
