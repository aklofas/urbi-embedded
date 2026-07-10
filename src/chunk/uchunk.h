/* SPDX-License-Identifier: BSD-3-Clause */
/* UChunk (UModule) — bytecode loader front-end / back-end interface.
 * Freestanding.  Includes uproto.h for UProto + per-proto helpers. */

#ifndef UCHUNK_H
#define UCHUNK_H

#include "uproto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- bytecode format version (loader rejects anything other than VERSION_BYTE) ---
   Encoding: VERSION_BYTE = (major << 4) | minor.  Hard breaks require a minor bump.
   v1.0 = 0x10 (v0.1.0), v1.1 = 0x11 (v0.2.0), v1.2 = 0x12 (v0.3.0 — control transfer),
   v1.3 = 0x13 (v0.4.0 — UProto.ic_count + UProto.ic_names side table),
   v1.4 = 0x14 (v0.5.0 — reactive opcodes 39-46, gc_byte bit 7, 4 new AST node kinds),
   v1.5 = 0x15 (v0.5.6 — wire-format completion: nested protos + per-proto
                + root ic_name_strs, header reserved bytes 16-23 strictly zero,
                opcode-shape table verifier, OP_INVOKE retired, v0.5.0 reactive
                opcodes renumbered 39-46 -> 38-45).
   v1.6 = 0x16 (v0.7.2 S42 — method-call ABI cleanup: new OP_SELF (47) loads
                method + receiver into adjacent registers; OP_CALL gains a
                method-flag bit (C & 0x80) so the receiver is read from
                R[A+1] explicitly instead of the now-deleted vm->last_recv
                global.  Eliminates the silent-elision bug where intervening
                OP_GETSLOTs in argument evaluation clobbered last_recv before
                the outer OP_CALL.  OP_MAX = 48.).
   v1.7 = 0x17 (v0.8.1-uproto-root Phase 3 — UModule body shrinks to header
                + source_name + recursive root_proto block.  Per-field
                duplication of chunk-top fields removed from the UModule
                wire section; root_proto is now serialized as a standard
                UProto block (max_reg, nupvals, nparams, constants,
                instructions, synclines, ic_names, nested_count, nested[]).
                Non-root UProtos write nested_count = 0 (flat-on-root
                emitter per spec §4.2).  v1.6 rejected as
                UCHUNK_LOAD_UNSUPPORTED_VERSION.).
   v1.8 = 0x18 (v0.9.2-uproto-only — Approach C: UModule struct deleted.
                Wire-byte layout unchanged from v1.7 — source_name was
                already at the chunk-body level pre-cliff, the spec §4.1
                description of a separate "UModule-header section" was
                imprecise; the bump is semantic, signaling that the
                emitting runtime treats every chunk as a UProto with no
                separate loader-shell type.  v1.7 rejected as
                UCHUNK_LOAD_UNSUPPORTED_VERSION.).
   v1.9 = 0x19 (v0.10.2-reactive — opcode space extension: new
                OP_WHENEVER_EVENT_INSTALL at slot 48 for whenever (e?)
                event-subscriber installs.  OP_MAX was 48; now 49.
                v1.8 rejected as UCHUNK_LOAD_UNSUPPORTED_VERSION.).

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

#ifndef URBI_FLOAT_TYPE
#define URBI_FLOAT_TYPE 8         /* 8 = f64, 4 = f32; overridden per target */
#endif

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

/* v0.9.2: struct UModule has been deleted.  A "module" is now
 * simply its root UProto.  The root UProto carries all fields that were
 * previously on UModule (source_name, origin_vm, next_proto_serial,
 * total_proto_count, next_in_realm, owning_realm, heap_allocated) in the
 * "root-only meaningful" section added to UProto in uproto.h.
 *
 * The public API function names use the urbi_chunk_* prefix. */

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
    /* --- bytecode F2: per-instruction bounds hardening (v0.10.7 verifier pass) --- */
    UCHUNK_LOAD_TRUNCATED_UPVALUES,     /* OP_CLOSURE upvalue prelude extends past bytecode end */
    UCHUNK_LOAD_MALFORMED_UPVALUE,      /* OP_CLOSURE upvalue pseudo-instr has invalid in_stack or src_idx */
    UCHUNK_LOAD_JMP_OUT_OF_BOUNDS,      /* OP_JMP Bx target pc outside [0, instr_count) */
    UCHUNK_LOAD_CALL_NRESULTS_ZERO,     /* OP_CALL C low-7 == 0 (nresults+1 must be >= 1) */
    UCHUNK_LOAD_RESERVED_OPCODE,        /* opcode is reserved/unimplemented at this wire version */
    /* --- bytecode F3: ic_index DFS pre-order mirror (v0.10.8 verifier pass) --- */
    UCHUNK_LOAD_IC_INDEX_MISMATCH       /* proto->ic_index does not match its DFS pre-order visit index */
} UChunkLoadError;

/* Per-proto cap on instruction count.  Bytecode-encoded as varint;
 * decoded into size_t.  The cap stops a malicious or corrupt module from
 * requesting an n_instr that would either overflow size_t on 32-bit
 * ports or balloon allocation past any plausible per-function budget.
 * 1 MiB instructions is well past any human-authored source. */
#define URBI_MAX_INSTRS_PER_PROTO ((size_t)(1U << 20))

/* --- API --- */

/* v0.9.2: strand-bind release helper.
 * Decrements root->refcount and, when it reaches 0 with a prior
 * uchunk_destroy call pending, fires uchunk_destroy_internal.
 * Pass NULL for root to no-op safely.  vm may be NULL in test contexts.
 * (The UModule* first argument has been removed — root is the module.) */
void uproto_strand_refcount_dec(UProto *root, struct UVM *vm);

/* Allocate a new UProto as parent_proto->nested[nested_count++].
 * Returns pointer to the new proto on success, NULL on OOM.
 * The proto is zero-initialized; alloc_fn/alloc_ud are copied from root.
 *
 * Watcher-detach interaction: condition/body/onleave protos for installed
 * at/whenever/waituntil watchers are created here, then later detached from
 * root_proto->nested[] by strand_closure_unlink.
 * After detach, the corresponding nested[k] slot becomes NULL and ownership
 * transfers to the watcher (freed via pool_free on watcher recycle).
 * uchunk_destroy is robust to NULL slots in nested[].  See also MOD-015. */
/* v0.8.5: parent_proto explicitly selects the nested[] array to grow
 * (root for top-level function literals, the enclosing UProto for nested
 * function literals).  Each call increments root->next_proto_serial and
 * assigns the new proto's ic_index from the post-increment value,
 * matching DFS pre-order. */
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
