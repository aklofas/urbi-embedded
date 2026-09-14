/* SPDX-License-Identifier: BSD-3-Clause */
/* UChunk (UModule) — bytecode loader front-end / back-end interface.
 * Freestanding.  Includes uproto.h for UProto + per-proto helpers. */

#ifndef UCHUNK_H
#define UCHUNK_H

#include "uproto.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
   Version-mismatch policy: exact-match.  Any byte other than VERSION_BYTE is
   a hard UCHUNK_LOAD_UNSUPPORTED_VERSION reject — there is no best-effort or
   forward/backward compatibility.  Older modules silently loading would
   produce unknown opcodes, misread GC state, or wrongly-sized IC tables.
   Re-emit from source to migrate. */

#define URBI_BYTECODE_VERSION_MAJOR  1U
#define URBI_BYTECODE_VERSION_MINOR  9U
#define URBI_BYTECODE_VERSION_BYTE   ((URBI_BYTECODE_VERSION_MAJOR << 4U) | URBI_BYTECODE_VERSION_MINOR)

/* --- Header canary bytes (offsets 6-11) ---
 *
 * The 6-byte sequence detects FTP/Windows-paste corruption on transfer.
 * `\x19\x93` is binary noise; `\r\n` is munged to `\n` by FTP ASCII
 * mode; `\x1A\n` is the DOS EOF + LF.  Any text-mode mangling of the
 * file produces a canary mismatch, returned as UCHUNK_LOAD_BAD_MAGIC.
 *
 * Defined as a static-const-array initializer in the header so both
 * the serializer (uemit_serialize.c) and deserializer (uchunk_io.c)
 * consume the same constant rather than duplicating the byte sequence. */
#define URBI_BYTECODE_CANARY_LEN 6U
static const uint8_t URBI_BYTECODE_CANARY[URBI_BYTECODE_CANARY_LEN] = {
    0x19U, 0x93U, '\r', '\n', 0x1AU, '\n'
};

/* --- bytecode flavor knobs (compile-time-pinned to host or cross target) --- */

#ifndef URBI_INT_WIDTH
#define URBI_INT_WIDTH 8          /* i64 on every v1 target */
#endif

/* Float flavor is fixed at 8 (f64/double) — the old per-target f32 flavor
 * (URBI_FLOAT_TYPE) has been retired; UValue's float arm is always double
 * (include/urbi/types.h). */

#ifndef URBI_INSTR_WIDTH
#define URBI_INSTR_WIDTH 4        /* uint32 always */
#endif

#ifndef URBI_ENDIANNESS
#define URBI_ENDIANNESS 0         /* 0 = little, 1 = big; v1 ships little-only */
#endif

/* --- opcode set — generated from src/chunk/uopcodes.def (49 opcodes, v1.9) ---
 *
 * Row order is wire-format frozen; do NOT reorder.  Operand encoding notes
 * live in docs/internals/opcodes.md and individual comments in uopcodes.def.
 * The computed-goto dispatch in uvm.c is NOT generated (hottest path). */

typedef enum {
#define URBI_OP(n, u, s) OP_##n,
#include "chunk/uopcodes.def"
#undef URBI_OP
    OP_MAX
} UOpcode;

/* --- instruction decode helpers (static inline; byte-aligned fields) --- */

static inline UOpcode  uinstr_op (uint32_t i) { return (UOpcode)(i & 0xFFU); }
static inline uint8_t  uinstr_a  (uint32_t i) { return (uint8_t)((i >> 8)  & 0xFFU); }
static inline uint8_t  uinstr_b  (uint32_t i) { return (uint8_t)((i >> 16) & 0xFFU); }
static inline uint8_t  uinstr_c  (uint32_t i) { return (uint8_t)((i >> 24) & 0xFFU); }
static inline uint16_t uinstr_bx (uint32_t i) { return (uint16_t)((i >> 16) & 0xFFFFU); }

static inline uint32_t uinstr_enc_abc (UOpcode op, uint8_t a, uint8_t b, uint8_t c) {
    return (uint32_t)op
         | ((uint32_t)a << 8)
         | ((uint32_t)b << 16)
         | ((uint32_t)c << 24);
}
static inline uint32_t uinstr_enc_abx (UOpcode op, uint8_t a, uint16_t bx) {
    return (uint32_t)op
         | ((uint32_t)a << 8)
         | ((uint32_t)bx << 16);
}

 /* The public API function names use the urbi_chunk_* prefix. */

/* --- errors --- */

typedef enum {
    UCHUNK_LOAD_OK = 0,
    UCHUNK_LOAD_BAD_MAGIC,              /* magic or canary mismatch */
    UCHUNK_LOAD_UNSUPPORTED_VERSION,
    UCHUNK_LOAD_FLAVOR_MISMATCH,        /* any descriptor field incl. endianness */
    UCHUNK_LOAD_TRUNCATED,
    UCHUNK_LOAD_CORRUPT_VARINT,
    UCHUNK_LOAD_CORRUPT_TAG,
    UCHUNK_LOAD_CORRUPT,                /* bad opcode / out-of-range reg / count mismatch / misaligned */
    UCHUNK_LOAD_OOM,
    UCHUNK_LOAD_INVALID_ARG,            /* NULL module / NULL buf etc.; distinct from TRUNCATED */
    UCHUNK_LOAD_OVERSIZED,              /* count fields exceed compile-time per-proto caps */
    UCHUNK_LOAD_TRUNCATED_UPVALUES,     /* OP_CLOSURE upvalue prelude extends past bytecode end */
    UCHUNK_LOAD_MALFORMED_UPVALUE,      /* OP_CLOSURE upvalue pseudo-instr has invalid in_stack or src_idx */
    UCHUNK_LOAD_JMP_OUT_OF_BOUNDS,      /* OP_JMP Bx target pc outside [0, instr_count) */
    UCHUNK_LOAD_CALL_NRESULTS_ZERO,     /* OP_CALL C low-7 == 0 (nresults+1 must be >= 1) */
    UCHUNK_LOAD_RESERVED_OPCODE,        /* opcode is reserved/unimplemented at this wire version */
    UCHUNK_LOAD_IC_INDEX_MISMATCH       /* proto->ic_index does not match its DFS pre-order visit index */
} UChunkLoadError;

/* Per-proto cap on instruction count.  Bytecode-encoded as varint;
 * decoded into size_t.  The cap stops a malicious or corrupt module from
 * requesting an n_instr that would either overflow size_t on 32-bit
 * ports or balloon allocation past any plausible per-function budget.
 * 1 MiB instructions is well past any human-authored source. */
#define URBI_MAX_INSTRS_PER_PROTO ((size_t)(1U << 20))

/* --- API --- */

void uproto_strand_refcount_dec(UProto *root, struct UVM *vm);

/* Allocate a new UProto as parent_proto->nested[nested_count++].
 * Returns pointer to the new proto on success, NULL on OOM.
 * The proto is zero-initialized; alloc_fn/alloc_ud are copied from root.
 */
UProto *uproto_alloc_nested(UProto *root, UProto *parent_proto);

/* Free a UProto's owned buffers.  Does NOT free the UProto struct itself
 * (it is owned by the module's nested[] array, or by a watcher pool slot
 * after strand_closure_unlink has detached it). */
void uproto_destroy_buffers(UProto *proto, UChunkAllocFn alloc,
                                   void *alloc_ud);

/* uchunk_deserialize — heap-allocate a new root UProto and populate it from
 * `buf`.  On success writes the new root through *out_root.
 *
 * `alloc_fn` / `alloc_ud` provide the allocator for the new root and all
 * sub-allocations.  `alloc_fn` == NULL falls back to stdlib realloc on hosted
 * builds; freestanding callers MUST supply alloc_fn.
 *
 * `errmsg` / `errcap` receive a human-readable diagnostic on failure.
 * Pass `(NULL, 0)` to suppress.
 *
 * Error semantics:
 *   - On success returns UCHUNK_LOAD_OK; *out_root points to the new root.
 *   - NULL out_root or NULL buf returns UCHUNK_LOAD_INVALID_ARG.
 *   - On any other failure returns a non-OK code; *out_root is NULL.
 *     If a root was partially allocated before the failure, it is cleaned
 *     up internally — callers do not need to call uchunk_destroy on error.
 *
 * Coverage at v1.7:
 *   - Header (24 bytes), source_name, root_proto block (recursive UProto:
 *     max_reg, nupvals, nparams, constants, instructions, synclines,
 *     ic_name_strs, nested_count, nested[]).
 *   - Verifier walks every instruction against the opcode-shape table
 *     (urbi_opcode_shapes[]); register operands < max_reg+1, Bx fields
 *     range-checked per UBxKind, last instruction must be OP_RET.
 *   - ic_names interning is deferred to urbi_chunk_instance_create
 *     (see object/uchunk_instance.h); deserialize itself does not need
 *     a VM. */
UChunkLoadError uchunk_deserialize(UProto **out_root,
                                   const uint8_t *buf, size_t size,
                                   UChunkAllocFn alloc_fn, void *alloc_ud,
                                   char *errmsg, size_t errcap);

/* uchunk_destroy — release all owned buffers on a root UProto.
 * If vm is non-NULL and root->refcount > 0, the root is rescued onto
 * vm->rescued_protos (surviving closures still reference it).
 * If root->heap_allocated is true, the root struct itself is freed via
 * its stored alloc_fn after buffers are released.
 *
 * vm-NULL contract (caller must guarantee):
 *   - The root has either never been run, OR
 *   - Every UClosure that pointed at any proto in this root has been freed
 *     BEFORE this call.
 *
 * Live-vm callsites should always pass the vm pointer; reserve NULL for
 * failed-compile cleanup where the root was never bound to any vm. */
void uchunk_destroy(UProto *root, struct UVM *vm);

/* Return a static string such as "UCHUNK_LOAD_BAD_MAGIC" for debug. */
const char *uchunk_load_error_name(UChunkLoadError code);

/* uchunk_verify_ic_index — Walk the UProto tree rooted at `root` in DFS
 * pre-order, verifying that every proto's ic_index matches its expected visit
 * index (root = 0, then children left-to-right, recursing depth-first).
 *
 * Returns UCHUNK_LOAD_OK on success, UCHUNK_LOAD_IC_INDEX_MISMATCH on the
 * first violation, UCHUNK_LOAD_INVALID_ARG if root is NULL.  `errmsg`/`errcap`
 * receive a human-readable diagnostic on failure; pass (NULL,0) to suppress.
 *
 * Called internally by uchunk_deserialize (Pass 3).  Also exposed for unit
 * tests that construct UProto trees via uproto_alloc_nested and patch ic_index
 * directly (the wire-format path cannot produce mismatches since the
 * deserializer assigns ic_index by construction). */
UChunkLoadError uchunk_verify_ic_index(const UProto *root,
                                       char *errmsg, size_t errcap);

#ifdef __cplusplus
}
#endif

#endif /* UCHUNK_H */
