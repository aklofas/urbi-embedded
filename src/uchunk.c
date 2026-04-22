/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode Chunk deserializer + verifier + destroy.  Freestanding. */

#include "uchunk.h"
#include "uchunk_internal.h"

#include <stdarg.h>               /* va_list / va_start / va_end — freestanding-ok */

/* Local zero-fill.  Replaces memset so uchunk.c compiles without a hosted
   <string.h>.  volatile prevents GCC/Clang from recognizing the loop and
   lowering it back to a memset libcall under -Os.  Same pattern as uarena.c. */
static void chunk_zero(void *const dst, const size_t n) {
    volatile unsigned char *const p = (volatile unsigned char *)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

/* Local byte-compare.  Replaces memcmp so uchunk.c compiles without
   <string.h> under -ffreestanding. */
static int chunk_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

/* Local byte-copy.  Replaces memcpy so uchunk.c compiles without
   <string.h> under -ffreestanding. */
static void chunk_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *pd = (unsigned char *)dst;
    const unsigned char *ps = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) pd[i] = ps[i];
}

/* Canonical canary bytes — docs/internals/bytecode-format.md §Header. */
static const uint8_t kCanary[6] = { 0x19, 0x93, '\r', '\n', 0x1A, '\n' };

#if __STDC_HOSTED__
#  include <stdio.h>
#  include <stdlib.h>

/* Safe snprintf-style helper. No-op when errmsg==NULL or errcap==0. */
static void set_errmsg(char *errmsg, size_t errcap, const char *fmt, ...) {
    if (errmsg == NULL || errcap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(errmsg, errcap, fmt, ap);
    va_end(ap);
}

/* Default allocator: realloc semantics.  Only compiled in hosted builds. */
static void *stdlib_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;
    if (nbytes == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, nbytes);
}
#else  /* freestanding */

/* No-op: freestanding builds suppress diagnostic messages entirely. */
static void set_errmsg(char *errmsg, size_t errcap, const char *fmt, ...) {
    (void)errmsg;
    (void)errcap;
    (void)fmt;
}
#endif  /* __STDC_HOSTED__ */

/* Resolve the effective allocator for a chunk. */
static UChunkAllocFn chunk_allocator(const Chunk *c) {
#if __STDC_HOSTED__
    return c->alloc_fn != NULL ? c->alloc_fn : stdlib_alloc;
#else
    /* Freestanding: caller MUST supply alloc_fn.  NULL here is a programming
       error and chunk_grow will propagate it as OOM. */
    return c->alloc_fn;
#endif
}

/* Grow *data in-place to hold at least new_cap elements of elem_size.
   Doubling policy.  Returns false on allocation failure.  No-op when
   already large enough. */
static bool chunk_grow(Chunk *c, void **data, size_t *cap,
                       size_t new_cap, size_t elem_size) {
    if (*cap >= new_cap) return true;
    UChunkAllocFn alloc = chunk_allocator(c);
    if (alloc == NULL) return false;
    size_t target = *cap == 0u ? 8u : *cap;
    while (target < new_cap) target *= 2u;
    void *fresh = alloc(*data, target * elem_size, c->alloc_ud);
    if (fresh == NULL) return false;
    *data = fresh;
    *cap  = target;
    return true;
}

/* --- Varint decode helpers --- */

/* LEB128-style unsigned varint decoder.  7 payload bits per byte; top bit
   is the continuation flag.  Max 10 bytes for a uint64. */
UChunkLoadError varint_decode_u(const uint8_t *buf, size_t size,
                                uint64_t *v, size_t *consumed) {
    uint64_t result = 0;
    size_t i = 0;
    unsigned shift = 0;
    for (i = 0; i < size; i++) {
        uint8_t b = buf[i];
        result |= (uint64_t)(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0u) {
            *v = result;
            *consumed = i + 1;
            return ULOAD_OK;
        }
        shift += 7;
        if (shift > 63u) {
            return ULOAD_CORRUPT_VARINT;
        }
    }
    return ULOAD_TRUNCATED;
}

/* Zigzag-signed varint decoder. */
UChunkLoadError varint_decode_zz(const uint8_t *buf, size_t size,
                                 int64_t *v, size_t *consumed) {
    uint64_t u = 0;
    UChunkLoadError rc = varint_decode_u(buf, size, &u, consumed);
    if (rc != ULOAD_OK) return rc;
    /* zigzag decode: (u >> 1) ^ -(u & 1) */
    *v = (int64_t)((u >> 1) ^ (uint64_t)(-(int64_t)(u & 1u)));
    return ULOAD_OK;
}

/* --- Public API --- */

UChunkLoadError uchunk_deserialize(Chunk *chunk, const uint8_t *buf, size_t size,
                                   char *errmsg, size_t errcap) {
    if (chunk == NULL || buf == NULL) {
        set_errmsg(errmsg, errcap, "null chunk or buffer");
        return ULOAD_TRUNCATED;
    }

    /* --- 24-byte header --- */
    if (size < 24u) {
        set_errmsg(errmsg, errcap,
                   "buffer truncated at header (got %zu bytes, need 24)", size);
        return ULOAD_TRUNCATED;
    }

    /* magic "URBI" at bytes 0-3 */
    if (buf[0] != 'U' || buf[1] != 'R' || buf[2] != 'B' || buf[3] != 'I') {
        set_errmsg(errmsg, errcap, "bad magic (expected \"URBI\")");
        return ULOAD_BAD_MAGIC;
    }

    /* version byte: 0x10 = v1.0 (16*major + minor) */
    if (buf[4] != 0x10u) {
        set_errmsg(errmsg, errcap,
                   "unsupported version byte 0x%02x", (unsigned)buf[4]);
        return ULOAD_UNSUPPORTED_VERSION;
    }

    /* buf[5] = flags; no flag bits defined at v1.0, ignored for forward-compat */

    /* canary bytes at offsets 6-11 */
    if (chunk_memcmp(buf + 6, kCanary, sizeof kCanary) != 0) {
        set_errmsg(errmsg, errcap,
                   "corrupt canary bytes (possible FTP/Windows paste translation)");
        return ULOAD_BAD_MAGIC;
    }

    /* format descriptor fields, one at a time for specific diagnostics */
    if (buf[12] != (uint8_t)URBI_INT_WIDTH) {
        set_errmsg(errmsg, errcap,
                   "flavor mismatch: int_width expected %u, got %u",
                   (unsigned)URBI_INT_WIDTH, (unsigned)buf[12]);
        return ULOAD_FLAVOR_MISMATCH;
    }
    if (buf[13] != (uint8_t)URBI_FLOAT_TYPE) {
        set_errmsg(errmsg, errcap,
                   "flavor mismatch: float_type expected %u, got %u",
                   (unsigned)URBI_FLOAT_TYPE, (unsigned)buf[13]);
        return ULOAD_FLAVOR_MISMATCH;
    }
    if (buf[14] != (uint8_t)URBI_INSTR_WIDTH) {
        set_errmsg(errmsg, errcap,
                   "flavor mismatch: instr_width expected %u, got %u",
                   (unsigned)URBI_INSTR_WIDTH, (unsigned)buf[14]);
        return ULOAD_FLAVOR_MISMATCH;
    }
    if (buf[15] != (uint8_t)URBI_ENDIANNESS) {
        set_errmsg(errmsg, errcap,
                   "flavor mismatch: endianness expected %u, got %u",
                   (unsigned)URBI_ENDIANNESS, (unsigned)buf[15]);
        return ULOAD_FLAVOR_MISMATCH;
    }
    /* buf[16..23] reserved — not validated (forward-compat) */

    size_t off = 24;

    /* --- metadata --- */
    if (off + 1u > size) { set_errmsg(errmsg, errcap, "truncated at metadata"); return ULOAD_TRUNCATED; }
    chunk->max_reg = buf[off++];

    uint64_t src_len = 0;
    size_t consumed = 0;
    UChunkLoadError rc = varint_decode_u(buf + off, size - off, &src_len, &consumed);
    if (rc != ULOAD_OK) { set_errmsg(errmsg, errcap, "bad varint at source_name_len"); return rc; }
    off += consumed;
    if (off + src_len > size) { set_errmsg(errmsg, errcap, "truncated at source_name"); return ULOAD_TRUNCATED; }
    if (src_len > 0u) {
        UChunkAllocFn alloc = chunk_allocator(chunk);
        if (alloc == NULL) { set_errmsg(errmsg, errcap, "no allocator for source_name"); return ULOAD_OOM; }
        char *name = (char *)alloc(NULL, src_len + 1u, chunk->alloc_ud);
        if (name == NULL) return ULOAD_OOM;
        chunk_memcpy(name, buf + off, src_len);
        name[src_len] = '\0';
        chunk->source_name = name;
        off += src_len;
    }

    /* --- constants --- */
    uint64_t n_const = 0;
    rc = varint_decode_u(buf + off, size - off, &n_const, &consumed);
    if (rc != ULOAD_OK) { set_errmsg(errmsg, errcap, "bad varint at n_constants"); return rc; }
    off += consumed;
    if (n_const > (uint64_t)UINT16_MAX + 1u) { set_errmsg(errmsg, errcap, "n_constants too large"); return ULOAD_CORRUPT; }
    if (n_const > 0u) {
        if (!chunk_grow(chunk, (void **)&chunk->constants, &chunk->const_cap, (size_t)n_const, sizeof(UConst))) {
            return ULOAD_OOM;
        }
    }
    for (uint64_t i = 0; i < n_const; i++) {
        if (off + 1u > size) { set_errmsg(errmsg, errcap, "truncated at constant kind"); return ULOAD_TRUNCATED; }
        uint8_t kind = buf[off++];
        if (kind > (uint8_t)UVAL_STR) {
            set_errmsg(errmsg, errcap, "constant kind %u not yet decodable at M1", (unsigned)kind);
            return ULOAD_CORRUPT_TAG;
        }
        chunk->constants[chunk->const_count].kind = kind;
        if (kind == (uint8_t)UVAL_INT) {
            int64_t v = 0;
            rc = varint_decode_zz(buf + off, size - off, &v, &consumed);
            if (rc != ULOAD_OK) { set_errmsg(errmsg, errcap, "bad varint in UVAL_INT"); return rc; }
            off += consumed;
            chunk->constants[chunk->const_count].v.i = v;
        } else if (kind == (uint8_t)UVAL_FLOAT) {
#if URBI_FLOAT_TYPE == 8
            if (off + 8u > size) { set_errmsg(errmsg, errcap, "truncated at UVAL_FLOAT"); return ULOAD_TRUNCATED; }
            chunk_memcpy(&chunk->constants[chunk->const_count].v.f, buf + off, 8);
            off += 8;
#else
            if (off + 4u > size) { set_errmsg(errmsg, errcap, "truncated at UVAL_FLOAT"); return ULOAD_TRUNCATED; }
            chunk_memcpy(&chunk->constants[chunk->const_count].v.f, buf + off, 4);
            off += 4;
#endif
        } else {
            /* UVAL_NIL / UVAL_BOOL / UVAL_STR — no payload at M1.
               M1 should not encounter these in produced bytecode, but the
               loader must not crash if hand-crafted. */
            set_errmsg(errmsg, errcap, "constant kind %u not yet decodable at M1", (unsigned)kind);
            return ULOAD_CORRUPT_TAG;
        }
        chunk->const_count++;
    }

    /* --- instructions --- */
    uint64_t n_instr = 0;
    rc = varint_decode_u(buf + off, size - off, &n_instr, &consumed);
    if (rc != ULOAD_OK) { set_errmsg(errmsg, errcap, "bad varint at n_instructions"); return rc; }
    off += consumed;
    /* 4-byte alignment: skip 0..3 padding bytes, all must be zero. */
    while ((off & 3u) != 0u) {
        if (off >= size) { set_errmsg(errmsg, errcap, "truncated at instruction alignment padding"); return ULOAD_TRUNCATED; }
        if (buf[off] != 0u) { set_errmsg(errmsg, errcap, "non-zero instruction-align padding at offset %zu", off); return ULOAD_CORRUPT; }
        off++;
    }
    if (n_instr > 0u) {
        if (!chunk_grow(chunk, (void **)&chunk->instructions, &chunk->instr_cap, (size_t)n_instr, sizeof(uint32_t))) {
            return ULOAD_OOM;
        }
    }
    for (uint64_t i = 0; i < n_instr; i++) {
        if (off + 4u > size) { set_errmsg(errmsg, errcap, "truncated at instruction %llu", (unsigned long long)i); return ULOAD_TRUNCATED; }
        /* Read uint32 little-endian (v1 endianness = LE). */
        chunk->instructions[chunk->instr_count] =
              (uint32_t)buf[off + 0]
            | ((uint32_t)buf[off + 1] << 8)
            | ((uint32_t)buf[off + 2] << 16)
            | ((uint32_t)buf[off + 3] << 24);
        off += 4;
        chunk->instr_count++;
    }

    /* --- synclines: delta array + absolute-line checkpoints --- */
    uint64_t n_deltas = 0;
    rc = varint_decode_u(buf + off, size - off, &n_deltas, &consumed);
    if (rc != ULOAD_OK) { set_errmsg(errmsg, errcap, "bad varint at n_deltas"); return rc; }
    off += consumed;
    if (n_deltas != (uint64_t)chunk->instr_count) {
        set_errmsg(errmsg, errcap,
                   "n_deltas=%llu does not match n_instructions=%zu",
                   (unsigned long long)n_deltas, chunk->instr_count);
        return ULOAD_CORRUPT;
    }
    if (n_deltas > 0u) {
        UChunkAllocFn alloc = chunk_allocator(chunk);
        if (alloc == NULL) return ULOAD_OOM;
        chunk->line_deltas = (int8_t *)alloc(NULL, (size_t)n_deltas, chunk->alloc_ud);
        if (chunk->line_deltas == NULL) return ULOAD_OOM;
        if (off + (size_t)n_deltas > size) {
            set_errmsg(errmsg, errcap, "truncated at line_deltas");
            return ULOAD_TRUNCATED;
        }
        chunk_memcpy(chunk->line_deltas, buf + off, (size_t)n_deltas);
        off += (size_t)n_deltas;
    }

    uint64_t n_abs = 0;
    rc = varint_decode_u(buf + off, size - off, &n_abs, &consumed);
    if (rc != ULOAD_OK) { set_errmsg(errmsg, errcap, "bad varint at n_abs_lines"); return rc; }
    off += consumed;
    if (n_abs > 0u) {
        if (!chunk_grow(chunk, (void **)&chunk->abs_lines, &chunk->abs_line_cap,
                        (size_t)n_abs, sizeof(AbsLine))) {
            return ULOAD_OOM;
        }
    }
    uint32_t prev_pc_checkpoint = 0;
    bool first_checkpoint = true;
    for (uint64_t i = 0; i < n_abs; i++) {
        uint64_t pc64 = 0;
        uint64_t line64 = 0;
        rc = varint_decode_u(buf + off, size - off, &pc64, &consumed);
        if (rc != ULOAD_OK) { set_errmsg(errmsg, errcap, "bad varint at abs_line pc"); return rc; }
        off += consumed;
        rc = varint_decode_u(buf + off, size - off, &line64, &consumed);
        if (rc != ULOAD_OK) { set_errmsg(errmsg, errcap, "bad varint at abs_line line"); return rc; }
        off += consumed;
        if (pc64 >= (uint64_t)chunk->instr_count) {
            set_errmsg(errmsg, errcap,
                       "abs_line pc=%llu out of range (instr_count=%zu)",
                       (unsigned long long)pc64, chunk->instr_count);
            return ULOAD_CORRUPT;
        }
        if (!first_checkpoint && (uint32_t)pc64 <= prev_pc_checkpoint) {
            set_errmsg(errmsg, errcap,
                       "abs_lines not monotonic in pc at %llu",
                       (unsigned long long)pc64);
            return ULOAD_CORRUPT;
        }
        chunk->abs_lines[chunk->abs_line_count].pc   = (uint32_t)pc64;
        chunk->abs_lines[chunk->abs_line_count].line = (uint32_t)line64;
        chunk->abs_line_count++;
        prev_pc_checkpoint = (uint32_t)pc64;
        first_checkpoint = false;
    }

    /* Trailing bytes after syncline section indicate a corrupt or mis-versioned blob. */
    if (off != size) {
        set_errmsg(errmsg, errcap,
                   "trailing %zu bytes after syncline section", size - off);
        return ULOAD_CORRUPT;
    }

    /* --- verifier sweep --- */
    if (chunk->instr_count > 0u) {
        size_t vi;
        for (vi = 0; vi < chunk->instr_count; vi++) {
            uint32_t ins;
            uint8_t  op;
            uint8_t  a;
            ins = chunk->instructions[vi];
            op  = (uint8_t)uinstr_op(ins);
            if (op >= (uint8_t)OP_MAX) {
                set_errmsg(errmsg, errcap,
                           "corrupt opcode %u at pc %zu", (unsigned)op, vi);
                return ULOAD_CORRUPT;
            }
            a = uinstr_a(ins);
            if (a > chunk->max_reg) {
                set_errmsg(errmsg, errcap,
                           "register A=%u > max_reg=%u at pc %zu",
                           (unsigned)a, (unsigned)chunk->max_reg, vi);
                return ULOAD_CORRUPT;
            }
            if (op == (uint8_t)OP_LOADK) {
                uint16_t bx = uinstr_bx(ins);
                if ((size_t)bx >= chunk->const_count) {
                    set_errmsg(errmsg, errcap,
                               "LOADK Bx=%u out of range (pool size %zu) at pc %zu",
                               (unsigned)bx, chunk->const_count, vi);
                    return ULOAD_CORRUPT;
                }
            } else {
                uint8_t b = uinstr_b(ins);
                /* B is unused in OP_RET (only A carries the return register);
                   accept arbitrary B values, same treatment as unused C fields. */
                if (op != (uint8_t)OP_RET && b > chunk->max_reg) {
                    set_errmsg(errmsg, errcap,
                               "register B=%u > max_reg=%u at pc %zu",
                               (unsigned)b, (unsigned)chunk->max_reg, vi);
                    return ULOAD_CORRUPT;
                }
                /* C is only meaningful for ADD/SUB/MUL/DIV; MOVE/NEG/RET leave
                   it unused.  Only range-check C for the opcodes that use it —
                   arbitrary C values in unused fields are intentionally accepted. */
                if (op == (uint8_t)OP_ADD || op == (uint8_t)OP_SUB
                 || op == (uint8_t)OP_MUL || op == (uint8_t)OP_DIV) {
                    uint8_t c = uinstr_c(ins);
                    if (c > chunk->max_reg) {
                        set_errmsg(errmsg, errcap,
                                   "register C=%u > max_reg=%u at pc %zu",
                                   (unsigned)c, (unsigned)chunk->max_reg, vi);
                        return ULOAD_CORRUPT;
                    }
                }
            }
        }
        /* Last instruction must be OP_RET. */
        {
            uint32_t last = chunk->instructions[chunk->instr_count - 1u];
            if (uinstr_op(last) != OP_RET) {
                set_errmsg(errmsg, errcap, "last instruction is not OP_RET");
                return ULOAD_CORRUPT;
            }
        }
    }

    return ULOAD_OK;
}

void uchunk_destroy(Chunk *chunk) {
    if (chunk == NULL) return;
    UChunkAllocFn alloc = chunk_allocator(chunk);
    if (alloc != NULL) {
        if (chunk->instructions != NULL) (void)alloc(chunk->instructions, 0, chunk->alloc_ud);
        if (chunk->constants    != NULL) (void)alloc(chunk->constants,    0, chunk->alloc_ud);
        if (chunk->line_deltas  != NULL) (void)alloc(chunk->line_deltas,  0, chunk->alloc_ud);
        if (chunk->abs_lines    != NULL) (void)alloc(chunk->abs_lines,    0, chunk->alloc_ud);
        if (chunk->source_name  != NULL) (void)alloc(chunk->source_name,  0, chunk->alloc_ud);
    }
    /* Zero the entire struct — preserves no fields (source_name, alloc_fn,
       alloc_ud are all reset; caller must re-init before re-use). */
    chunk_zero(chunk, sizeof(*chunk));
}

const char *uchunk_load_error_name(UChunkLoadError code) {
    switch (code) {
    case ULOAD_OK:                  return "ULOAD_OK";
    case ULOAD_BAD_MAGIC:           return "ULOAD_BAD_MAGIC";
    case ULOAD_UNSUPPORTED_VERSION: return "ULOAD_UNSUPPORTED_VERSION";
    case ULOAD_FLAVOR_MISMATCH:     return "ULOAD_FLAVOR_MISMATCH";
    case ULOAD_TRUNCATED:           return "ULOAD_TRUNCATED";
    case ULOAD_CORRUPT_VARINT:      return "ULOAD_CORRUPT_VARINT";
    case ULOAD_CORRUPT_TAG:         return "ULOAD_CORRUPT_TAG";
    case ULOAD_CORRUPT:             return "ULOAD_CORRUPT";
    case ULOAD_OOM:                 return "ULOAD_OOM";
    }
    return "ULOAD_UNKNOWN";
}
