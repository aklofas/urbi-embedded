/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode Chunk — the front-end / back-end interface.  Freestanding. */

#ifndef UCHUNK_H
#define UCHUNK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- bytecode flavor knobs (compile-time-pinned to host or cross target) --- */

#ifndef URBI_INT_WIDTH
#define URBI_INT_WIDTH 8          /* i64 on every v1 target */
#endif

#ifndef URBI_FLOAT_TYPE
#define URBI_FLOAT_TYPE 8         /* 8 = f64, 4 = f32; overridden per target at M6 */
#endif

#ifndef URBI_INSTR_WIDTH
#define URBI_INSTR_WIDTH 4        /* uint32 always */
#endif

#ifndef URBI_ENDIANNESS
#define URBI_ENDIANNESS 0         /* 0 = little, 1 = big; v1 ships little-only */
#endif

/* --- tagged value shape shared between pool and runtime registers --- */

typedef enum {
    UVAL_NIL   = 0,
    UVAL_INT   = 1,
    UVAL_FLOAT = 2,
    UVAL_BOOL  = 3,
    UVAL_STR   = 4
    /* 5-15 reserved; loader rejects > UVAL_STR at v1.0 */
} UValKind;

typedef struct {
    uint8_t  kind;                /* UValKind */
    uint8_t  _pad[7];             /* 8-byte align of .v */
    union {
        int64_t i;
#if URBI_FLOAT_TYPE == 8
        double  f;
#else
        float   f;
#endif
    } v;
} UValue;                         /* 16 bytes */

/* --- opcode set (M1 reserves slots 0-7; 8-255 reserved for M2+) --- */

typedef enum {
    OP_LOADK = 0,                 /* ABx:  R[A] := K[Bx]           */
    OP_MOVE  = 1,                 /* ABC:  R[A] := R[B]            */
    OP_ADD   = 2,                 /* ABC:  R[A] := R[B] + R[C]     */
    OP_SUB   = 3,                 /* ABC:  R[A] := R[B] - R[C]     */
    OP_MUL   = 4,                 /* ABC:  R[A] := R[B] * R[C]     */
    OP_DIV   = 5,                 /* ABC:  R[A] := R[B] / R[C]     */
    OP_NEG   = 6,                 /* ABC:  R[A] := -R[B]  (C=0)    */
    OP_RET   = 7,                 /* ABC:  return R[A]    (B=C=0)  */
    OP_MAX
} UOpcode;

/* --- instruction decode helpers (static inline; byte-aligned fields) --- */

static inline UOpcode  uinstr_op (uint32_t i) { return (UOpcode)(i & 0xFFu); }
static inline uint8_t  uinstr_a  (uint32_t i) { return (uint8_t)((i >> 8)  & 0xFFu); }
static inline uint8_t  uinstr_b  (uint32_t i) { return (uint8_t)((i >> 16) & 0xFFu); }
static inline uint8_t  uinstr_c  (uint32_t i) { return (uint8_t)((i >> 24) & 0xFFu); }
static inline uint16_t uinstr_bx (uint32_t i) { return (uint16_t)((i >> 16) & 0xFFFFu); }

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

/* --- absolute-line checkpoint record --- */

typedef struct {
    uint32_t pc;
    uint32_t line;
} AbsLine;

/* --- pluggable allocator (matches uarena pattern) --- */

typedef void *(*UChunkAllocFn)(void *ptr, size_t nbytes, void *ud);
/* Standard realloc semantics:
 *   ptr == NULL && nbytes > 0  : allocate fresh buffer; return non-NULL or NULL on OOM.
 *   ptr != NULL && nbytes == 0 : free ptr; return NULL.
 *   ptr != NULL && nbytes > 0  : reallocate ptr to nbytes (may move); return non-NULL or NULL on OOM.
 *   ptr == NULL && nbytes == 0 : no-op; return NULL.
 * ud is an opaque caller-supplied cookie passed through unchanged (same pattern as uarena). */

/* --- Chunk struct --- */

typedef struct Chunk {
    uint32_t  *instructions;
    size_t     instr_count;
    size_t     instr_cap;

    UValue    *constants;
    size_t     const_count;
    size_t     const_cap;

    /* Lua-5.5-style delta encoding; one byte per instruction (size == instr_count).
     * INT8_MIN (-128) is a sentinel: the source line at this pc is in abs_lines,
     * not recoverable by summing deltas from the previous checkpoint. */
    int8_t    *line_deltas;

    AbsLine   *abs_lines;
    size_t     abs_line_count;
    size_t     abs_line_cap;

    uint8_t    max_reg;           /* VM allocates (max_reg + 1) register slots */
    char       *source_name;      /* owned (allocator-allocated, null-terminated); NULL if absent */

    /* allocator hook; NULL -> use stdlib realloc (hosted builds only) */
    UChunkAllocFn alloc_fn;
    void         *alloc_ud;
} Chunk;

/* --- errors --- */

typedef enum {
    ULOAD_OK = 0,
    ULOAD_BAD_MAGIC,              /* magic or canary mismatch */
    ULOAD_UNSUPPORTED_VERSION,
    ULOAD_FLAVOR_MISMATCH,        /* any descriptor field incl. endianness */
    ULOAD_TRUNCATED,
    ULOAD_CORRUPT_VARINT,
    ULOAD_CORRUPT_TAG,
    ULOAD_CORRUPT,                /* bad opcode / out-of-range reg / count mismatch / misaligned */
    ULOAD_OOM
} UChunkLoadError;

/* --- API --- */

/* Populate chunk from buf.  chunk must be zero-initialized before call;
   if chunk->alloc_fn is NULL on entry, the stdlib realloc is used
   (hosted builds only).  errmsg/errcap receive a human-readable
   diagnostic on failure; pass (NULL, 0) to suppress.
   On error the chunk is left empty (destroy is safe but a no-op). */
UChunkLoadError uchunk_deserialize(Chunk *chunk, const uint8_t *buf, size_t size,
                                   char *errmsg, size_t errcap);

/* Free all owned buffers and zero the struct (preserving nothing).
   Safe to call on a zero-initialized Chunk. */
void uchunk_destroy(Chunk *chunk);

/* Return a static string such as "ULOAD_BAD_MAGIC" for debug. */
const char *uchunk_load_error_name(UChunkLoadError code);

#ifdef __cplusplus
}
#endif

#endif
