/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode UModule deserializer + verifier + destroy.  Freestanding. */

#include "module/umodule.h"
#include "runtime/umacros.h"
#include "value/uvarint.h"
#include "uopcode_shape.h"

#include <stdarg.h>               /* va_list / va_start / va_end — freestanding-ok */
#include <stdint.h>

/* Local byte-copy.  Replaces memcpy so umodule.c compiles without
   <string.h> under -ffreestanding. */
static void module_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *pd = (unsigned char *)dst;
    const unsigned char *ps = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) pd[i] = ps[i];
}

/* Canary constant lives in module/umodule.h as URBI_BYTECODE_CANARY (MOD-029). */

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

/* Resolve the effective allocator for a module. */
static UModuleAllocFn module_allocator(const UModule *c) {
#if __STDC_HOSTED__
    return c->alloc_fn != NULL ? c->alloc_fn : stdlib_alloc;
#else
    /* Freestanding: caller MUST supply alloc_fn.  NULL here is a programming
       error and module_grow_with_alloc will propagate it as OOM. */
    return c->alloc_fn;
#endif
}

/* Grow *data in-place using an explicit allocator.  Used by both the
   top-level (module-target) decoders and the per-proto decoders called
   from decode_proto. */
static bool module_grow_with_alloc(UModuleAllocFn alloc, void *alloc_ud,
                                   void **data, size_t *cap,
                                   size_t new_cap, size_t elem_size) {
    if (*cap >= new_cap) return true;
    if (alloc == NULL) return false;
    /* MOD-004: defend against new_cap * elem_size overflow at the helper
     * boundary, so wire-format count fields that slip past per-section
     * caps cannot reach the allocator with a wrap-truncated byte count.
     * Caller-side caps (URBI_MAX_INSTRS_PER_PROTO, n_const cap, etc.)
     * are the primary line of defense; this is belt-and-braces. */
    if (elem_size != 0U && new_cap > SIZE_MAX / elem_size) return false;
    size_t target = *cap == 0U ? 8U : *cap;
    while (target < new_cap) {
        /* Doubling-loop overflow guard: if target would wrap, snap to
         * new_cap (the smallest cap that satisfies the request). */
        if (target > SIZE_MAX / 2U) { target = new_cap; break; }
        target *= 2U;
    }
    /* Re-verify target * elem_size after the doubling loop. */
    if (elem_size != 0U && target > SIZE_MAX / elem_size) return false;
    void *fresh = alloc(*data, target * elem_size, alloc_ud);
    if (fresh == NULL) return false;
    *data = fresh;
    *cap  = target;
    return true;
}

/* --- Varint decode wrappers ---
   Delegate to uvarint.{c,h} and translate UVarintError into UModuleLoadError so
   existing call sites continue to return/compare against ULOAD_* values. */

static UModuleLoadError varint_error_to_module_error(UVarintError ve) {
    switch (ve) {
        case UVARINT_OK:        return ULOAD_OK;
        case UVARINT_TRUNCATED: return ULOAD_TRUNCATED;
        case UVARINT_OVERSIZE:  return ULOAD_CORRUPT_VARINT;
    }
    return ULOAD_CORRUPT;  /* unreachable under -Wswitch-enum */
}

static UModuleLoadError module_decode_varint_u(const uint8_t *buf, size_t size,
                                             uint64_t *v, size_t *consumed) {
    return varint_error_to_module_error(uvarint_decode_u(buf, size, v, consumed));
}

static UModuleLoadError module_decode_varint_zz(const uint8_t *buf, size_t size,
                                              int64_t *v, size_t *consumed) {
    return varint_error_to_module_error(uvarint_decode_zz(buf, size, v, consumed));
}

/* --- Proto helpers --- */

void umodule_destroy_proto_buffers(UProto *proto, UModuleAllocFn alloc,
                                   void *alloc_ud) {
    if (proto == NULL || alloc == NULL) return;
    if (proto->instructions != NULL) alloc(proto->instructions, 0, alloc_ud);
    if (proto->constants    != NULL) alloc(proto->constants,    0, alloc_ud);
    if (proto->line_deltas  != NULL) alloc(proto->line_deltas,  0, alloc_ud);
    if (proto->abs_lines    != NULL) alloc(proto->abs_lines,    0, alloc_ud);
    if (proto->ic_names     != NULL) alloc(proto->ic_names,     0, alloc_ud);
    if (proto->ic_name_strs != NULL) {
        /* Each entry is a NUL-terminated string allocated separately. */
        for (uint16_t k = 0; k < proto->ic_count; k++) {
            if (proto->ic_name_strs[k] != NULL) {
                alloc(proto->ic_name_strs[k], 0, alloc_ud);
            }
        }
        alloc(proto->ic_name_strs, 0, alloc_ud);
    }
    /* Zero the proto struct but do not free proto itself (owned by nested[]). */
    urbi_zero(proto, sizeof(*proto));
}

UProto *umodule_alloc_nested_proto(UModule *module) {
    UModuleAllocFn alloc = module_allocator(module);
    if (alloc == NULL) return NULL;

    /* Grow nested[] array if needed. */
    if (module->nested_count >= module->nested_cap) {
        size_t new_cap = module->nested_cap == 0 ? 4 : module->nested_cap * 2;
        void *fresh = alloc(module->nested, new_cap * sizeof(UProto *),
                            module->alloc_ud);
        if (fresh == NULL) return NULL;
        module->nested     = (UProto **)fresh;
        module->nested_cap = new_cap;
    }

    /* Allocate the UProto struct itself. */
    UProto *proto = (UProto *)alloc(NULL, sizeof(UProto), module->alloc_ud);
    if (proto == NULL) return NULL;
    urbi_zero(proto, sizeof(*proto));
    proto->alloc_fn = module->alloc_fn;
    proto->alloc_ud = module->alloc_ud;

    module->nested[module->nested_count++] = proto;
    return proto;
}

/* --- Per-section decoder context (file-private) --- */

typedef struct {
    UModule        *module;
    const uint8_t  *buf;
    size_t          size;
    size_t          off;
    char           *errmsg;
    size_t          errcap;
} MDecCtx;

/* --- Per-section decode helpers (each <40 LOC) --- */

static UModuleLoadError decode_header(MDecCtx *d) {
    if (d->size < 24U) {
        set_errmsg(d->errmsg, d->errcap,
                   "buffer truncated at header (got %zu bytes, need 24)", d->size);
        return ULOAD_TRUNCATED;
    }
    /* magic "URBI" at bytes 0-3 */
    if (d->buf[0] != 'U' || d->buf[1] != 'R' || d->buf[2] != 'B' || d->buf[3] != 'I') {
        set_errmsg(d->errmsg, d->errcap, "bad magic (expected \"URBI\")");
        return ULOAD_BAD_MAGIC;
    }
    /* version byte: 0x15 = v1.5 (16*major + minor); all prior versions are
       hard-rejected.  v1.4 → v1.5 is the v0.5.6 Wave 4 break (wire-format
       completion at T10-T15: nested protos + per-proto + root ic_name_strs,
       header reserved bytes 16-23 strictly enforced as zero, opcode-shape
       table replaces the M1 verifier, OP_INVOKE retired and M5 reactive
       opcodes renumbered 39-46 -> 38-45).  Loading older modules silently
       would produce unknown opcodes or misread GC state. */
    if (d->buf[4] != URBI_BYTECODE_VERSION_BYTE) {
        set_errmsg(d->errmsg, d->errcap,
                   "unsupported version byte 0x%02x (v%u.%u); this build expects 0x%02x (v%u.%u)",
                   (unsigned)d->buf[4],
                   (unsigned)(d->buf[4] >> 4), (unsigned)(d->buf[4] & 0x0FU),
                   (unsigned)URBI_BYTECODE_VERSION_BYTE,
                   (unsigned)URBI_BYTECODE_VERSION_MAJOR, (unsigned)URBI_BYTECODE_VERSION_MINOR);
        return ULOAD_UNSUPPORTED_VERSION;
    }
    /* buf[5] = flags; no flag bits defined at v1.0, ignored for forward-compat */
    /* canary bytes at offsets 6-11 */
    if (!urbi_memeq(d->buf + 6, URBI_BYTECODE_CANARY, URBI_BYTECODE_CANARY_LEN)) {
        set_errmsg(d->errmsg, d->errcap,
                   "corrupt canary bytes (possible FTP/Windows paste translation)");
        return ULOAD_BAD_MAGIC;
    }
    /* format descriptor fields, one at a time for specific diagnostics */
    if (d->buf[12] != (uint8_t)URBI_INT_WIDTH) {
        set_errmsg(d->errmsg, d->errcap, "flavor mismatch: int_width expected %u, got %u",
                   (unsigned)URBI_INT_WIDTH, (unsigned)d->buf[12]);
        return ULOAD_FLAVOR_MISMATCH;
    }
    if (d->buf[13] != (uint8_t)URBI_FLOAT_TYPE) {
        set_errmsg(d->errmsg, d->errcap, "flavor mismatch: float_type expected %u, got %u",
                   (unsigned)URBI_FLOAT_TYPE, (unsigned)d->buf[13]);
        return ULOAD_FLAVOR_MISMATCH;
    }
    if (d->buf[14] != (uint8_t)URBI_INSTR_WIDTH) {
        set_errmsg(d->errmsg, d->errcap, "flavor mismatch: instr_width expected %u, got %u",
                   (unsigned)URBI_INSTR_WIDTH, (unsigned)d->buf[14]);
        return ULOAD_FLAVOR_MISMATCH;
    }
    if (d->buf[15] != (uint8_t)URBI_ENDIANNESS) {
        set_errmsg(d->errmsg, d->errcap, "flavor mismatch: endianness expected %u, got %u",
                   (unsigned)URBI_ENDIANNESS, (unsigned)d->buf[15]);
        return ULOAD_FLAVOR_MISMATCH;
    }
    /* Strict enforcement of header bytes 16-23 (MOD-038):
     *
     * v1.0 defines no flag bits in this region.  Forward-compat tolerance
     * silently dropped flags that older builds didn't recognize, which is
     * the wrong policy when the runtime does not promise bytecode stability
     * before v1.0.  We reject any non-zero reserved byte. */
    {
        size_t i;
        for (i = 16; i < 24; i++) {
            if (d->buf[i] != 0U) {
                set_errmsg(d->errmsg, d->errcap,
                           "non-zero reserved byte 0x%02x at offset %zu",
                           (unsigned)d->buf[i], i);
                return ULOAD_CORRUPT;
            }
        }
    }
    d->off = 24;
    return ULOAD_OK;
}

static UModuleLoadError decode_metadata(MDecCtx *d) {
    if (d->off + 1U > d->size) {
        set_errmsg(d->errmsg, d->errcap, "truncated at metadata");
        return ULOAD_TRUNCATED;
    }
    d->module->max_reg = d->buf[d->off++];

    uint64_t src_len = 0;
    size_t consumed = 0;
    UModuleLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                 &src_len, &consumed);
    if (rc != ULOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at source_name_len");
        return rc;
    }
    d->off += consumed;
    if (d->off + src_len > d->size) {
        set_errmsg(d->errmsg, d->errcap, "truncated at source_name");
        return ULOAD_TRUNCATED;
    }
    if (src_len > 0U) {
        UModuleAllocFn alloc = module_allocator(d->module);
        if (alloc == NULL) {
            set_errmsg(d->errmsg, d->errcap, "no allocator for source_name");
            return ULOAD_OOM;
        }
        char *name = (char *)alloc(NULL, src_len + 1U, d->module->alloc_ud);
        if (name == NULL) return ULOAD_OOM;
        module_memcpy(name, d->buf + d->off, src_len);
        name[src_len] = '\0';
        d->module->source_name = name;
        d->off += src_len;
    }
    return ULOAD_OK;
}

/* Decode the constants section into (target_buf, target_count, target_cap).
   Used both for the root chunk (writes to module->...) and per-proto
   (writes to p->...). */
static UModuleLoadError decode_constants_into(MDecCtx *d,
                                              UValue **target_buf,
                                              size_t *target_count,
                                              size_t *target_cap,
                                              UModuleAllocFn alloc,
                                              void *alloc_ud) {
    uint64_t n_const = 0;
    size_t consumed = 0;
    UModuleLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                  &n_const, &consumed);
    if (rc != ULOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_constants");
        return rc;
    }
    d->off += consumed;
    if (n_const > (uint64_t)UINT16_MAX + 1U) {
        set_errmsg(d->errmsg, d->errcap, "n_constants too large");
        return ULOAD_CORRUPT;
    }
    if (n_const > 0U) {
        if (!module_grow_with_alloc(alloc, alloc_ud,
                                    (void **)target_buf, target_cap,
                                    (size_t)n_const, sizeof(UValue))) {
            return ULOAD_OOM;
        }
    }
    for (uint64_t i = 0; i < n_const; i++) {
        if (d->off + 1U > d->size) {
            set_errmsg(d->errmsg, d->errcap, "truncated at constant kind");
            return ULOAD_TRUNCATED;
        }
        uint8_t kind = d->buf[d->off++];
        if (kind > (uint8_t)UVAL_STR) {
            set_errmsg(d->errmsg, d->errcap, "constant kind %u out of range (max %u)",
                       (unsigned)kind, (unsigned)UVAL_STR);
            return ULOAD_CORRUPT_TAG;
        }
        (*target_buf)[*target_count].kind = kind;
        if (kind == (uint8_t)UVAL_INT) {
            int64_t v = 0;
            rc = module_decode_varint_zz(d->buf + d->off, d->size - d->off, &v, &consumed);
            if (rc != ULOAD_OK) {
                set_errmsg(d->errmsg, d->errcap, "bad varint in UVAL_INT");
                return rc;
            }
            d->off += consumed;
            (*target_buf)[*target_count].v.i = v;
        } else if (kind == (uint8_t)UVAL_FLOAT) {
#if URBI_FLOAT_TYPE == 8
            if (d->off + 8U > d->size) {
                set_errmsg(d->errmsg, d->errcap, "truncated at UVAL_FLOAT");
                return ULOAD_TRUNCATED;
            }
            module_memcpy(&(*target_buf)[*target_count].v.f,
                          d->buf + d->off, 8);
            d->off += 8;
#else
            if (d->off + 4U > d->size) {
                set_errmsg(d->errmsg, d->errcap, "truncated at UVAL_FLOAT");
                return ULOAD_TRUNCATED;
            }
            module_memcpy(&(*target_buf)[*target_count].v.f,
                          d->buf + d->off, 4);
            d->off += 4;
#endif
        } else {
            /* UVAL_NIL / UVAL_BOOL / UVAL_STR — no payload encoder/decoder
               implemented in v0.5.6.  The emitter never produces these in
               constant pools (BOOL is OP_LOADBOOL immediate, NIL is
               OP_LOADNIL, STR is M6 stdlib).  Hand-crafted bytecode that
               smuggles them in is rejected via ULOAD_CORRUPT_TAG so the
               loader does not crash on the missing payload read. */
            set_errmsg(d->errmsg, d->errcap, "constant kind %u not decodable in v0.5.6 constant pools",
                       (unsigned)kind);
            return ULOAD_CORRUPT_TAG;
        }
        (*target_count)++;
    }
    return ULOAD_OK;
}

static UModuleLoadError decode_constants(MDecCtx *d) {
    return decode_constants_into(d, &d->module->constants,
                                 &d->module->const_count,
                                 &d->module->const_cap,
                                 module_allocator(d->module),
                                 d->module->alloc_ud);
}

/* Decode the instructions section into (target_buf, target_count, target_cap). */
static UModuleLoadError decode_instructions_into(MDecCtx *d,
                                                 uint32_t **target_buf,
                                                 size_t *target_count,
                                                 size_t *target_cap,
                                                 UModuleAllocFn alloc,
                                                 void *alloc_ud) {
    uint64_t n_instr = 0;
    size_t consumed = 0;
    UModuleLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                  &n_instr, &consumed);
    if (rc != ULOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_instructions");
        return rc;
    }
    d->off += consumed;
    /* MOD-017: cap instr_count BEFORE the (size_t)n_instr demotion below.
     * On 32-bit ports a uint64_t > SIZE_MAX silently truncates; also
     * defends against unbounded allocation request.  URBI_MAX_INSTRS_PER_PROTO
     * is the documented cap. */
    if (n_instr > (uint64_t)URBI_MAX_INSTRS_PER_PROTO) {
        set_errmsg(d->errmsg, d->errcap,
                   "n_instructions=%llu exceeds URBI_MAX_INSTRS_PER_PROTO=%zu",
                   (unsigned long long)n_instr, URBI_MAX_INSTRS_PER_PROTO);
        return ULOAD_OVERSIZED;
    }
    /* 4-byte alignment: skip 0..3 padding bytes, all must be zero. */
    while ((d->off & 3U) != 0U) {
        if (d->off >= d->size) {
            set_errmsg(d->errmsg, d->errcap, "truncated at instruction alignment padding");
            return ULOAD_TRUNCATED;
        }
        if (d->buf[d->off] != 0U) {
            set_errmsg(d->errmsg, d->errcap, "non-zero instruction-align padding at offset %zu",
                       d->off);
            return ULOAD_CORRUPT;
        }
        d->off++;
    }
    if (n_instr > 0U) {
        if (!module_grow_with_alloc(alloc, alloc_ud,
                                    (void **)target_buf, target_cap,
                                    (size_t)n_instr, sizeof(uint32_t))) {
            return ULOAD_OOM;
        }
    }
    for (uint64_t i = 0; i < n_instr; i++) {
        if (d->off + 4U > d->size) {
            set_errmsg(d->errmsg, d->errcap, "truncated at instruction %llu",
                       (unsigned long long)i);
            return ULOAD_TRUNCATED;
        }
        /* Read uint32 little-endian (v1 endianness = LE). */
        (*target_buf)[*target_count] =
              (uint32_t)d->buf[d->off + 0]
            | ((uint32_t)d->buf[d->off + 1] << 8)
            | ((uint32_t)d->buf[d->off + 2] << 16)
            | ((uint32_t)d->buf[d->off + 3] << 24);
        d->off += 4;
        (*target_count)++;
    }
    return ULOAD_OK;
}

static UModuleLoadError decode_instructions(MDecCtx *d) {
    return decode_instructions_into(d, &d->module->instructions,
                                    &d->module->instr_count,
                                    &d->module->instr_cap,
                                    module_allocator(d->module),
                                    d->module->alloc_ud);
}

/* Decode the syncline (line_deltas + abs_lines) section into the target
   buffers and counts.  instr_count is the expected n_deltas; the target
   buffers/cap are written via *line_deltas_out / *abs_lines_out etc.
   The trailing-bytes check moved to decode_trailer (T13). */
static UModuleLoadError decode_line_table_into(MDecCtx *d,
                                               int8_t **line_deltas_out,
                                               UAbsLine **abs_lines_out,
                                               size_t instr_count,
                                               size_t *abs_line_count_out,
                                               size_t *abs_line_cap_out,
                                               UModuleAllocFn alloc,
                                               void *alloc_ud) {
    uint64_t n_deltas = 0;
    size_t consumed = 0;
    UModuleLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                  &n_deltas, &consumed);
    if (rc != ULOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_deltas");
        return rc;
    }
    d->off += consumed;
    if (n_deltas != (uint64_t)instr_count) {
        set_errmsg(d->errmsg, d->errcap,
                   "n_deltas=%llu does not match n_instructions=%zu",
                   (unsigned long long)n_deltas, instr_count);
        return ULOAD_CORRUPT;
    }
    if (n_deltas > 0U) {
        if (alloc == NULL) return ULOAD_OOM;
        *line_deltas_out = (int8_t *)alloc(NULL, (size_t)n_deltas, alloc_ud);
        if (*line_deltas_out == NULL) return ULOAD_OOM;
        if (d->off + (size_t)n_deltas > d->size) {
            set_errmsg(d->errmsg, d->errcap, "truncated at line_deltas");
            return ULOAD_TRUNCATED;
        }
        module_memcpy(*line_deltas_out, d->buf + d->off, (size_t)n_deltas);
        d->off += (size_t)n_deltas;
    }

    uint64_t n_abs = 0;
    rc = module_decode_varint_u(d->buf + d->off, d->size - d->off, &n_abs, &consumed);
    if (rc != ULOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_abs_lines");
        return rc;
    }
    d->off += consumed;
    /* MOD-018: n_abs is bounded by instr_count — every checkpoint
     * references a unique pc < instr_count, and the existing monotonic
     * check rejects duplicates.  Without this cap a corrupt module
     * could request an arbitrarily large abs_lines allocation. */
    if (n_abs > (uint64_t)instr_count) {
        set_errmsg(d->errmsg, d->errcap,
                   "n_abs_lines=%llu exceeds instr_count=%zu",
                   (unsigned long long)n_abs, instr_count);
        return ULOAD_CORRUPT;
    }
    if (n_abs > 0U) {
        if (!module_grow_with_alloc(alloc, alloc_ud,
                                    (void **)abs_lines_out, abs_line_cap_out,
                                    (size_t)n_abs, sizeof(UAbsLine))) {
            return ULOAD_OOM;
        }
    }
    uint32_t prev_pc_checkpoint = 0;
    bool first_checkpoint = true;
    for (uint64_t i = 0; i < n_abs; i++) {
        uint64_t pc64 = 0;
        uint64_t line64 = 0;
        rc = module_decode_varint_u(d->buf + d->off, d->size - d->off, &pc64, &consumed);
        if (rc != ULOAD_OK) {
            set_errmsg(d->errmsg, d->errcap, "bad varint at abs_line pc");
            return rc;
        }
        d->off += consumed;
        rc = module_decode_varint_u(d->buf + d->off, d->size - d->off, &line64, &consumed);
        if (rc != ULOAD_OK) {
            set_errmsg(d->errmsg, d->errcap, "bad varint at abs_line line");
            return rc;
        }
        d->off += consumed;
        if (pc64 >= (uint64_t)instr_count) {
            set_errmsg(d->errmsg, d->errcap,
                       "abs_line pc=%llu out of range (instr_count=%zu)",
                       (unsigned long long)pc64, instr_count);
            return ULOAD_CORRUPT;
        }
        if (!first_checkpoint && (uint32_t)pc64 <= prev_pc_checkpoint) {
            set_errmsg(d->errmsg, d->errcap,
                       "abs_lines not monotonic in pc at %llu",
                       (unsigned long long)pc64);
            return ULOAD_CORRUPT;
        }
        (*abs_lines_out)[*abs_line_count_out].pc   = (uint32_t)pc64;
        (*abs_lines_out)[*abs_line_count_out].line = (uint32_t)line64;
        (*abs_line_count_out)++;
        prev_pc_checkpoint = (uint32_t)pc64;
        first_checkpoint = false;
    }
    return ULOAD_OK;
}

static UModuleLoadError decode_line_table(MDecCtx *d) {
    return decode_line_table_into(d, &d->module->line_deltas,
                                  &d->module->abs_lines,
                                  d->module->instr_count,
                                  &d->module->abs_line_count,
                                  &d->module->abs_line_cap,
                                  module_allocator(d->module),
                                  d->module->alloc_ud);
}

/* Decode an IC name table: count + N length-prefixed UTF-8 strings.
 * Stores into *out_count + *out_strs (caller-owned).
 * Used for both the root chunk (writes to module->...) and per-proto. */
static UModuleLoadError decode_ic_names_into(MDecCtx *d,
                                              uint16_t *out_count,
                                              char ***out_strs,
                                              UModuleAllocFn alloc,
                                              void *alloc_ud) {
    uint64_t count = 0;
    size_t consumed = 0;
    UModuleLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                  &count, &consumed);
    if (rc != ULOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_ic_names");
        return rc;
    }
    d->off += consumed;
    if (count > 256U) {
        set_errmsg(d->errmsg, d->errcap, "n_ic_names=%llu exceeds cap (256)",
                   (unsigned long long)count);
        return ULOAD_CORRUPT;
    }
    *out_count = (uint16_t)count;
    if (count == 0U) {
        *out_strs = NULL;
        return ULOAD_OK;
    }
    if (alloc == NULL) return ULOAD_OOM;
    char **strs = (char **)alloc(NULL, (size_t)count * sizeof(char *), alloc_ud);
    if (strs == NULL) return ULOAD_OOM;
    /* Zero-init so partial-fail cleanup (umodule_destroy /
       umodule_destroy_proto_buffers) walks well-defined NULL slots. */
    for (uint64_t k = 0; k < count; k++) strs[k] = NULL;
    *out_strs = strs;
    for (uint64_t k = 0; k < count; k++) {
        uint64_t nlen = 0;
        rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                     &nlen, &consumed);
        if (rc != ULOAD_OK) {
            set_errmsg(d->errmsg, d->errcap, "bad varint at ic_name[%llu] length",
                       (unsigned long long)k);
            return rc;
        }
        d->off += consumed;
        if (nlen > 256U) {
            set_errmsg(d->errmsg, d->errcap, "ic_name[%llu] length=%llu exceeds cap (256)",
                       (unsigned long long)k, (unsigned long long)nlen);
            return ULOAD_CORRUPT;
        }
        if (d->off + nlen > d->size) {
            set_errmsg(d->errmsg, d->errcap, "truncated at ic_name[%llu] body",
                       (unsigned long long)k);
            return ULOAD_TRUNCATED;
        }
        char *dup = (char *)alloc(NULL, (size_t)nlen + 1U, alloc_ud);
        if (dup == NULL) return ULOAD_OOM;
        if (nlen > 0U) module_memcpy(dup, d->buf + d->off, (size_t)nlen);
        dup[nlen] = '\0';
        strs[k] = dup;
        d->off += nlen;
    }
    return ULOAD_OK;
}

/* Decode a single UProto from the stream into a pre-allocated proto.
 * The proto's alloc_fn/alloc_ud have already been set by
 * umodule_alloc_nested_proto inheriting from the owning module. */
static UModuleLoadError decode_proto(MDecCtx *d, UProto *p) {
    UModuleAllocFn alloc = p->alloc_fn;
    if (alloc == NULL) {
        /* Hosted-build fallback: caller did not supply an allocator and
           the proto inherits from the module which uses stdlib_alloc. */
        alloc = module_allocator(d->module);
    }
    void *alloc_ud = p->alloc_ud;

    if (d->off + 3U > d->size) {
        set_errmsg(d->errmsg, d->errcap, "truncated at proto header (max_reg/nupvals/nparams)");
        return ULOAD_TRUNCATED;
    }
    p->max_reg = d->buf[d->off++];
    p->nupvals = d->buf[d->off++];
    p->nparams = d->buf[d->off++];

    UModuleLoadError rc;
    rc = decode_constants_into(d, &p->constants, &p->const_count, &p->const_cap,
                               alloc, alloc_ud);
    if (rc != ULOAD_OK) return rc;
    rc = decode_instructions_into(d, &p->instructions, &p->instr_count, &p->instr_cap,
                                  alloc, alloc_ud);
    if (rc != ULOAD_OK) return rc;
    rc = decode_line_table_into(d, &p->line_deltas, &p->abs_lines,
                                p->instr_count, &p->abs_line_count, &p->abs_line_cap,
                                alloc, alloc_ud);
    if (rc != ULOAD_OK) return rc;
    rc = decode_ic_names_into(d, &p->ic_count, &p->ic_name_strs, alloc, alloc_ud);
    return rc;
}

/* Decode the nested[] section: varint n_nested + N proto records. */
static UModuleLoadError decode_nested_protos(MDecCtx *d) {
    uint64_t n_nested = 0;
    size_t consumed = 0;
    UModuleLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                  &n_nested, &consumed);
    if (rc != ULOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_nested");
        return rc;
    }
    d->off += consumed;
    if (n_nested > 1024U) {
        set_errmsg(d->errmsg, d->errcap, "n_nested=%llu exceeds cap (1024)",
                   (unsigned long long)n_nested);
        return ULOAD_CORRUPT;
    }
    for (uint64_t i = 0; i < n_nested; i++) {
        UProto *p = umodule_alloc_nested_proto(d->module);
        if (p == NULL) return ULOAD_OOM;
        rc = decode_proto(d, p);
        if (rc != ULOAD_OK) return rc;
    }
    return ULOAD_OK;
}

/* Final byte check: stream must end exactly at the last decoded section. */
static UModuleLoadError decode_trailer(MDecCtx *d) {
    if (d->off != d->size) {
        set_errmsg(d->errmsg, d->errcap,
                   "trailing %zu bytes after nested-protos section", d->size - d->off);
        return ULOAD_CORRUPT;
    }
    return ULOAD_OK;
}

/* Verify a single byte field per its UOperandKind.  max_reg is the
   per-block bound (root chunk uses module->max_reg; nested protos use
   p->max_reg). */
static UModuleLoadError verify_byte_operand(MDecCtx *d, uint8_t op,
                                            uint8_t value, UOperandKind kind,
                                            const char *which, size_t pc,
                                            uint8_t max_reg) {
    switch (kind) {
        case UOPK_UNUSED:
            return ULOAD_OK;
        case UOPK_REG:
            if (value > max_reg) {
                set_errmsg(d->errmsg, d->errcap,
                           "register %s=%u > max_reg=%u at pc %zu (op=%u)",
                           which, (unsigned)value,
                           (unsigned)max_reg, pc, (unsigned)op);
                return ULOAD_CORRUPT;
            }
            return ULOAD_OK;
        case UOPK_IMM_BOOL:
            if (value > 1U) {
                set_errmsg(d->errmsg, d->errcap,
                           "%s=%u not a 0/1 immediate at pc %zu (op=%u)",
                           which, (unsigned)value, pc, (unsigned)op);
                return ULOAD_CORRUPT;
            }
            return ULOAD_OK;
        case UOPK_IMM_FLAGS:
            return ULOAD_OK;  /* full byte accepted; flag bits unconstrained */
        case UOPK_IMM_REG_NIBBLE: {
            /* The byte packs flags (high nibble) + reg_idx (low nibble).
             * tag_reg is constrained to [0,15] AND <= max_reg. */
            uint8_t reg_idx = value & 0x0FU;
            if (reg_idx > max_reg) {
                set_errmsg(d->errmsg, d->errcap,
                           "%s tag_reg=%u > max_reg=%u at pc %zu (op=%u)",
                           which, (unsigned)reg_idx,
                           (unsigned)max_reg, pc, (unsigned)op);
                return ULOAD_CORRUPT;
            }
            return ULOAD_OK;
        }
        case UOPK_UPVAL_IDX:
            /* Runtime-checked at OP_GETUPVAL/OP_SETUPVAL dispatch (UClosure
             * carries the upvalue array length).  No static range. */
            return ULOAD_OK;
        case UOPK_FRAME_REG_BASE:
            /* OP_PUSH_FRAME_GUARD A is base register; <= max_reg. */
            if (value > max_reg) {
                set_errmsg(d->errmsg, d->errcap,
                           "%s frame guard base=%u > max_reg=%u at pc %zu (op=%u)",
                           which, (unsigned)value, (unsigned)max_reg,
                           pc, (unsigned)op);
                return ULOAD_CORRUPT;
            }
            return ULOAD_OK;
        case UOPK_FRAME_REG_COUNT:
            /* No standalone range check — OP_PUSH_FRAME_GUARD A+B
             * boundary check happens at the per-instruction arm below
             * because we need both bytes simultaneously. */
            return ULOAD_OK;
    }
    return ULOAD_OK;
}

/* OP_JMP Bx range note:
 *   Bx is a 16-bit unsigned field treated as signed with bias 32768
 *   (effective range -32768..+32767).  The verifier intentionally does
 *   NOT range-check Bx because the legitimate range depends on pc — a
 *   target pc' = pc + signed(Bx) - 32768 must satisfy
 *   0 <= pc' <= instr_count.  Per-instruction bounds checks would force
 *   the verifier to know absolute PC; we defer to runtime dispatch
 *   which surfaces an out-of-range jump as URBI_ERR_RUNTIME_FATAL. */
/* Walk one block of instructions (root chunk OR a nested proto) against
   the opcode-shape table, applying per-block bounds (max_reg /
   const_count / instr_count / nested_count).  Callers pass the
   root-level nested_count for both root and per-proto walks since the
   v1.5 emitter allocates all function literals as flat siblings under
   the root UModule's nested[] (an OP_CLOSURE inside a nested proto
   refers to a sibling slot in the same root array). */
static UModuleLoadError verify_walk_block(MDecCtx *d,
                                          uint8_t max_reg,
                                          size_t const_count,
                                          size_t instr_count,
                                          size_t nested_count,
                                          const uint32_t *instructions) {
    size_t vi;
    for (vi = 0; vi < instr_count; vi++) {
        uint32_t ins = instructions[vi];
        uint8_t  op  = (uint8_t)uinstr_op(ins);
        if (op >= (uint8_t)OP_MAX) {
            set_errmsg(d->errmsg, d->errcap, "corrupt opcode %u at pc %zu",
                       (unsigned)op, vi);
            return ULOAD_CORRUPT;
        }
        const UOpcodeShape *sh = &urbi_opcode_shapes[op];

        uint8_t a = uinstr_a(ins);
        UModuleLoadError rc = verify_byte_operand(d, op, a, sh->a_kind, "A", vi, max_reg);
        if (rc != ULOAD_OK) return rc;

        if (sh->format == UOPF_ABC) {
            uint8_t b = uinstr_b(ins);
            uint8_t c = uinstr_c(ins);
            rc = verify_byte_operand(d, op, b, sh->b_kind, "B", vi, max_reg);
            if (rc != ULOAD_OK) return rc;
            rc = verify_byte_operand(d, op, c, sh->c_kind, "C", vi, max_reg);
            if (rc != ULOAD_OK) return rc;

            /* OP_PUSH_FRAME_GUARD: cross-byte invariant base+count <= max_reg+1. */
            if (op == (uint8_t)OP_PUSH_FRAME_GUARD) {
                if ((unsigned)a + (unsigned)b > (unsigned)max_reg + 1U) {
                    set_errmsg(d->errmsg, d->errcap,
                               "frame guard base+count=%u exceeds max_reg+1=%u at pc %zu",
                               (unsigned)a + (unsigned)b,
                               (unsigned)max_reg + 1U, vi);
                    return ULOAD_CORRUPT;
                }
            }
        } else {
            /* UOPF_ABX — Bx range check per shape table. */
            uint16_t bx = uinstr_bx(ins);
            switch (sh->bx_kind) {
                case UBXK_UNUSED:
                    break;
                case UBXK_POOL_INDEX:
                    if ((size_t)bx >= const_count) {
                        set_errmsg(d->errmsg, d->errcap,
                                   "Bx=%u >= const_count=%zu at pc %zu (op=%u)",
                                   (unsigned)bx, const_count, vi, (unsigned)op);
                        return ULOAD_CORRUPT;
                    }
                    break;
                case UBXK_NESTED_INDEX:
                    if ((size_t)bx >= nested_count) {
                        set_errmsg(d->errmsg, d->errcap,
                                   "Bx=%u >= nested_count=%zu at pc %zu (op=%u)",
                                   (unsigned)bx, nested_count, vi, (unsigned)op);
                        return ULOAD_CORRUPT;
                    }
                    break;
                case UBXK_JUMP_SIGNED:
                    /* No static range check; OP_JMP target out-of-range
                     * surfaces at runtime when pc + signed(Bx) - 32768
                     * leaves [0, instr_count). */
                    break;
                case UBXK_HANDLER_PC:
                    if ((size_t)bx >= instr_count) {
                        set_errmsg(d->errmsg, d->errcap,
                                   "handler-PC Bx=%u >= instr_count=%zu at pc %zu (op=%u)",
                                   (unsigned)bx, instr_count, vi, (unsigned)op);
                        return ULOAD_CORRUPT;
                    }
                    break;
                case UBXK_SYMBOL_ID:
                    /* At v1.5 the verifier accepts the full 0..65535
                     * symbol-id range; runtime resolves at dispatch. */
                    break;
            }
        }
    }
    /* Last instruction must be OP_RET (preserved from pre-T4 behavior).
     *
     * v1.x relaxation note: this strict trailing-OP_RET requirement
     * assumes the emitter always closes a chunk with an explicit return.
     * If a future bytecode revision allows fall-through-to-end semantics
     * (e.g. an implicit RET, or a tail-call that elides RET), this check
     * will need to widen.  At v0.5.6 every chunk uemit produces ends in
     * OP_RET, so the strict form catches truncated/corrupt bytecode
     * early. */
    if (instr_count > 0U) {
        uint32_t last = instructions[instr_count - 1U];
        if (uinstr_op(last) != OP_RET) {
            set_errmsg(d->errmsg, d->errcap, "last instruction is not OP_RET");
            return ULOAD_CORRUPT;
        }
    }
    return ULOAD_OK;
}

static UModuleLoadError decode_verify(MDecCtx *d) {
    /* Verify the root chunk. */
    UModuleLoadError rc = verify_walk_block(d,
                                            d->module->max_reg,
                                            d->module->const_count,
                                            d->module->instr_count,
                                            d->module->nested_count,
                                            d->module->instructions);
    if (rc != ULOAD_OK) return rc;

    /* Verify each nested proto's instruction stream against its own
       bounds.  v1.5 in-tree emitter allocates all function literals as
       flat siblings under the root UModule's nested[]; an OP_CLOSURE
       inside a nested proto refers to a sibling slot in the same
       root nested[] array.  Per-proto nested_count for verify purposes
       is therefore the root-level nested_count.  v1.x deeply-nested
       closures may need a per-proto nested_count if/when the emitter
       starts allocating child arrays. */
    for (size_t pi = 0; pi < d->module->nested_count; pi++) {
        UProto *p = d->module->nested[pi];
        if (p == NULL) continue;  /* watcher-detached slot or stub */
        rc = verify_walk_block(d,
                               p->max_reg,
                               p->const_count,
                               p->instr_count,
                               d->module->nested_count,
                               p->instructions);
        if (rc != ULOAD_OK) return rc;
    }
    return ULOAD_OK;
}

/* --- Public API --- */

UModuleLoadError umodule_deserialize(UModule *module, const uint8_t *buf, size_t size,
                                   char *errmsg, size_t errcap) {
    /* errmsg/errcap contract: the (NULL, 0) pair suppresses diagnostics; any
     * other shape — including (non-NULL, 0) — is silently accepted as
     * "diagnostics off".  set_errmsg internally no-ops on errcap == 0 so
     * passing a non-NULL buffer with zero capacity is harmless rather than
     * a contract violation.  Callers that require a populated errmsg must
     * supply errcap >= 1. */
    if (module == NULL || buf == NULL) {
        set_errmsg(errmsg, errcap, "null module or buffer");
        return ULOAD_INVALID_ARG;
    }

    /* Zero origin_vm for deserialized modules. */
    module->origin_vm = NULL;

    MDecCtx d;
    d.module = module;
    d.buf    = buf;
    d.size   = size;
    d.off    = 0;
    d.errmsg = errmsg;
    d.errcap = errcap;

    UModuleLoadError rc;
    if ((rc = decode_header(&d))       != ULOAD_OK) return rc;
    if ((rc = decode_metadata(&d))     != ULOAD_OK) return rc;
    if ((rc = decode_constants(&d))    != ULOAD_OK) return rc;
    if ((rc = decode_instructions(&d)) != ULOAD_OK) return rc;
    if ((rc = decode_line_table(&d))   != ULOAD_OK) return rc;
    if ((rc = decode_ic_names_into(&d, &module->ic_count, &module->ic_name_strs,
                                   module_allocator(module), module->alloc_ud))
        != ULOAD_OK) return rc;
    if ((rc = decode_nested_protos(&d)) != ULOAD_OK) return rc;
    if ((rc = decode_trailer(&d))       != ULOAD_OK) return rc;
    if ((rc = decode_verify(&d))        != ULOAD_OK) return rc;
    return ULOAD_OK;
}

/* Destroy ordering (MOD-005):
 *   1. Resolve allocator BEFORE any frees — alloc_fn/alloc_ud are still
 *      live in the struct at this point and must remain readable for the
 *      entire free walk below.
 *   2. Walk nested[] and free each non-NULL UProto's sub-buffers + the
 *      UProto struct itself.  NULL slots in nested[] are by design (see
 *      MOD-015 below); skip them silently.
 *   3. Free the nested[] array, root-chunk buffers, source_name, and
 *      ic_names — all read directly from `module->...` because nothing
 *      has been zeroed yet.
 *   4. ONLY THEN zero the struct.  After step 4 the struct is fully wiped:
 *      source_name, alloc_fn, alloc_ud are all reset; the caller must
 *      re-init before reuse.
 *
 * MOD-015 — nested[k] may be NULL by design:
 *   strand_closure_unlink (src/watcher/uwatcher_install.c) detaches a UProto
 *   from module->nested[] when its UClosure is captured by a watcher
 *   (transferring ownership from the module to the watcher pool).  After
 *   detach, nested[k] reads NULL.  This is the expected steady-state for any
 *   chunk that installed reactive watchers — umodule_destroy must skip NULL
 *   slots without freeing them, since the watcher's pool_free now owns
 *   that proto and will free it on watcher recycle. */
void umodule_destroy(UModule *module) {
    if (module == NULL) return;
    UModuleAllocFn alloc = module_allocator(module);
    if (alloc != NULL) {
        /* Free nested proto buffers and the proto structs themselves.
         * NULL entries (watcher-detached, see MOD-015) are skipped. */
        if (module->nested != NULL) {
            size_t i;
            for (i = 0; i < module->nested_count; i++) {
                UProto *p = module->nested[i];
                if (p != NULL) {
                    umodule_destroy_proto_buffers(p, alloc, module->alloc_ud);
                    alloc(p, 0, module->alloc_ud);
                }
            }
            alloc(module->nested, 0, module->alloc_ud);
        }
        if (module->instructions != NULL) (void)alloc(module->instructions, 0, module->alloc_ud);
        if (module->constants    != NULL) (void)alloc(module->constants,    0, module->alloc_ud);
        if (module->line_deltas  != NULL) (void)alloc(module->line_deltas,  0, module->alloc_ud);
        if (module->abs_lines    != NULL) (void)alloc(module->abs_lines,    0, module->alloc_ud);
        if (module->source_name  != NULL) (void)alloc(module->source_name,  0, module->alloc_ud);
        if (module->ic_names     != NULL) (void)alloc(module->ic_names,     0, module->alloc_ud);
        if (module->ic_name_strs != NULL) {
            /* Each entry is a NUL-terminated string allocated separately. */
            for (uint16_t k = 0; k < module->ic_count; k++) {
                if (module->ic_name_strs[k] != NULL) {
                    (void)alloc(module->ic_name_strs[k], 0, module->alloc_ud);
                }
            }
            (void)alloc(module->ic_name_strs, 0, module->alloc_ud);
        }
    }
    /* Zero the entire struct AFTER all frees complete.  No field is read
     * after this point. */
    urbi_zero(module, sizeof(*module));
}

const char *umodule_load_error_name(UModuleLoadError code) {
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
    case ULOAD_INVALID_ARG:         return "ULOAD_INVALID_ARG";
    case ULOAD_OVERSIZED:           return "ULOAD_OVERSIZED";
    }
    return "ULOAD_UNKNOWN";
}
