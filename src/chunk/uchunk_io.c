/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode UModule deserializer + verifier + destroy.  Freestanding. */

#include "chunk/uchunk.h"
#include "chunk/uchunk_internal.h"  /* MDecCtx + verifier entry-point decls */
#include "util/umacros.h"
#include "util/uvarint.h"
/* refound/core: the destroy path no longer reaches into the VM, the
 * module-instance list, or the realm's loaded-chunk list.  The new core
 * owns a bound chunk through one GC cell (rt/uexec.h UProtoCell) whose
 * finaliser calls uchunk_destroy, so there is nothing left to unlink and
 * no refcount rescue to perform. */

#include <stdarg.h>               /* va_list / va_start / va_end — freestanding-ok */
#include <stdint.h>

/* Local byte-copy.  Replaces memcpy so uchunk_io.c compiles without
   <string.h> under -ffreestanding. */
static void module_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *pd = (unsigned char *)dst;
    const unsigned char *ps = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) pd[i] = ps[i];
}

#if __STDC_HOSTED__
#  include <stdio.h>
#  include <stdlib.h>

/* Safe snprintf-style helper. No-op when errmsg==NULL or errcap==0.
 * Byte-identical mirror lives in uchunk_verify.c; keep the two in sync. */
static void set_errmsg(char *errmsg, size_t errcap, const char *fmt, ...) {
    if (errmsg == NULL || errcap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    /* False positive: ap is initialized by va_start, consumed by vsnprintf,
     * then cleared by va_end.  Analyzer cannot see through the va_list
     * contract on the vsnprintf prototype. */
    (void)vsnprintf(errmsg, errcap, fmt, ap);  /* NOLINT(clang-analyzer-valist.Uninitialized) — ap initialized by va_start above */
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

static void uchunk_destroy_internal(UProto *root);

/* Resolve the effective allocator for a root UProto. */
static UChunkAllocFn module_allocator(const UProto *c) {
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
static bool module_grow_with_alloc(UChunkAllocFn alloc, void *alloc_ud,
                                   void **data, size_t *cap,
                                   size_t new_cap, size_t elem_size) {
    if (*cap >= new_cap) return true;
    if (alloc == NULL) return false;
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
   Delegate to uvarint.{c,h} and translate UVarintError into UChunkLoadError so
   existing call sites continue to return/compare against UCHUNK_LOAD_* values. */

static UChunkLoadError varint_error_to_module_error(UVarintError ve) {
    switch (ve) {
        case UVARINT_OK:        return UCHUNK_LOAD_OK;
        case UVARINT_TRUNCATED: return UCHUNK_LOAD_TRUNCATED;
        case UVARINT_OVERSIZE:  return UCHUNK_LOAD_CORRUPT_VARINT;
    }
    return UCHUNK_LOAD_CORRUPT;  /* unreachable under -Wswitch-enum */
}

static UChunkLoadError module_decode_varint_u(const uint8_t *buf, size_t size,
                                             uint64_t *v, size_t *consumed) {
    return varint_error_to_module_error(uvarint_decode_u(buf, size, v, consumed));
}

static UChunkLoadError module_decode_varint_zz(const uint8_t *buf, size_t size,
                                              int64_t *v, size_t *consumed) {
    return varint_error_to_module_error(uvarint_decode_zz(buf, size, v, consumed));
}

/* --- Proto helpers --- */

static inline void module_buf_free(UChunkAllocFn alloc, void *alloc_ud,
                                   void *p) {
    if (p != NULL) (void)alloc(p, 0, alloc_ud);
}

/* Free the per-constant module-owned bytes attached to UVAL_STR slots, when
 * `owned` says this constants[] array was populated by the deserializer
 * (decode_constants_into's UVAL_STR arm mallocs a fresh NUL-terminated
 * buffer per string).  Emit-time UVAL_STR slots carry an intern-table
 * pointer (VM-owned) and must NOT be freed here; `owned` (proto->
 * constants_owned) distinguishes the two ownership domains — a single
 * UProto's constants[] is populated exclusively by one path or the other,
 * never a mix, so one flag per proto is enough (no per-value marker
 * needed). */
static void free_owned_str_constants(UValue *constants, size_t count,
                                     bool owned,
                                     UChunkAllocFn alloc, void *alloc_ud) {
    if (!owned || constants == NULL || alloc == NULL) return;
    for (size_t i = 0U; i < count; i++) {
        if (constants[i].kind == (uint8_t)UVAL_STR && constants[i].v.p != NULL) {
            (void)alloc(constants[i].v.p, 0, alloc_ud);
            constants[i].v.p = NULL;
        }
    }
}

void uproto_destroy_buffers(UProto *proto, UChunkAllocFn alloc,
                                   void *alloc_ud) {
    URBI_INTERNAL_ASSERT(proto != NULL);
#if __STDC_HOSTED__
    /* Hosted fallback: if no custom allocator, use stdlib_alloc so that
     * test modules initialised with {0} (alloc_fn=NULL) can still have their
     * emit-stdlib-allocated buffers freed.  Freestanding: caller must supply. */
    if (alloc == NULL) alloc = stdlib_alloc;
#else
    if (alloc == NULL) return;
#endif
    /* root_proto owns nested[] — free sub-protos first.
     * Nested protos have nested_count == 0 so this walk is a no-op for them. */
    if (proto->nested != NULL) {
        size_t i;
        for (i = 0; i < proto->nested_count; i++) {
            UProto *p = proto->nested[i];
            if (p == NULL) continue;  /* MOD-015: watcher-detached slot */
            uproto_destroy_buffers(p, alloc, alloc_ud);
            alloc(p, 0, alloc_ud);
        }
        alloc((void *)proto->nested, 0, alloc_ud);
    }
    if (proto->instructions != NULL) alloc(proto->instructions, 0, alloc_ud);
    free_owned_str_constants(proto->constants, proto->const_count,
                             proto->constants_owned, alloc, alloc_ud);
    if (proto->constants    != NULL) alloc(proto->constants,    0, alloc_ud);
    if (proto->line_deltas  != NULL) alloc(proto->line_deltas,  0, alloc_ud);
    if (proto->abs_lines    != NULL) alloc(proto->abs_lines,    0, alloc_ud);
    if (proto->site_names     != NULL) alloc((void *)proto->site_names,     0, alloc_ud);
    if (proto->site_name_strs != NULL) {
        /* Each entry is a NUL-terminated string allocated separately. */
        for (uint16_t k = 0; k < proto->site_count; k++) {
            if (proto->site_name_strs[k] != NULL) {
                alloc(proto->site_name_strs[k], 0, alloc_ud);
            }
        }
        alloc((void *)proto->site_name_strs, 0, alloc_ud);
    }
    /* Zero the proto struct but do not free proto itself (owned by parent). */
    urbi_zero(proto, sizeof(*proto));
}

UProto *uproto_alloc_nested(UProto *root, UProto *parent_proto) {
    UChunkAllocFn alloc = module_allocator(root);
    if (alloc == NULL) return NULL;
    if (parent_proto == NULL) return NULL;

    /* Grow parent_proto->nested[] array if needed. */
    if (parent_proto->nested_count >= parent_proto->nested_cap) {
        size_t new_cap = parent_proto->nested_cap == 0 ? 4 : parent_proto->nested_cap * 2;
        void *fresh = alloc((void *)parent_proto->nested,
                            new_cap * sizeof(UProto *),
                            root->alloc_ud);
        if (fresh == NULL) return NULL;
        parent_proto->nested     = (UProto **)fresh;
        parent_proto->nested_cap = new_cap;
    }

    /* Allocate the UProto struct itself.
     *
     * Rolling back the grow would require freeing the larger buffer and
     * restoring the prior nested pointer.  Since realloc invalidates the
     * prior pointer when it returns a different address, restoring would
     * mean re-allocating yet again — net cost higher than carrying the
     * benign over-cap.  The "benign over-cap" state is observed by:
     *   - uchunk_destroy: walks [0..nested_count) only.
     *   - serialize: writes nested_count, not nested_cap.
     *   - subsequent uproto_alloc_nested: enters the grow branch
     *     only when nested_count >= nested_cap, which now skips the
     *     realloc and proceeds to UProto alloc.
     * No code path reads beyond [0..nested_count). */
    UProto *proto = (UProto *)alloc(NULL, sizeof(UProto), root->alloc_ud);
    if (proto == NULL) return NULL;
    urbi_zero(proto, sizeof(*proto));
    proto->alloc_fn = root->alloc_fn;
    proto->alloc_ud = root->alloc_ud;

    proto->refcount = 0U;

    proto->proto_index = ++root->next_proto_serial;

    parent_proto->nested[parent_proto->nested_count++] = proto;
    return proto;
}

/* --- Per-section decoder context --- */

/* Maximum nested-proto depth accepted by the decoder.  Real programs nest
 * ~10 levels deep (stdlib measured at 1).  Capping at 64 bounds the C call
 * stack consumed by decode_proto ↔ decode_nested_protos_into mutual recursion
 * to well under the smallest embedded stack budget (64 KB MCU stacks).
 * Exceeding the cap returns UCHUNK_LOAD_CORRUPT without crashing. */
#define UCHUNK_MAX_PROTO_DEPTH 64

/* MDecCtx (the per-section decode context) lives in chunk/uchunk_internal.h,
 * shared with the verifier passes in uchunk_verify.c. */

/* --- Per-section decode helpers (each <40 LOC) --- */

static UChunkLoadError decode_header(MDecCtx *d) {
    if (d->size < 24U) {
        set_errmsg(d->errmsg, d->errcap,
                   "buffer truncated at header (got %zu bytes, need 24)", d->size);
        return UCHUNK_LOAD_TRUNCATED;
    }
    /* magic "URBI" at bytes 0-3 */
    if (d->buf[0] != 'U' || d->buf[1] != 'R' || d->buf[2] != 'B' || d->buf[3] != 'I') {
        set_errmsg(d->errmsg, d->errcap, "bad magic (expected \"URBI\")");
        return UCHUNK_LOAD_BAD_MAGIC;
    }
    if (d->buf[4] != URBI_BYTECODE_VERSION_BYTE) {
        set_errmsg(d->errmsg, d->errcap,
                   "unsupported version byte 0x%02x (v%u.%u); this build expects 0x%02x (v%u.%u)",
                   (unsigned)d->buf[4],
                   (unsigned)(d->buf[4] >> 4), (unsigned)(d->buf[4] & 0x0FU),
                   (unsigned)URBI_BYTECODE_VERSION_BYTE,
                   (unsigned)URBI_BYTECODE_VERSION_MAJOR, (unsigned)URBI_BYTECODE_VERSION_MINOR);
        return UCHUNK_LOAD_UNSUPPORTED_VERSION;
    }
    d->arity_flag = (uint8_t)(d->buf[5] & 0x01U);
    /* canary bytes at offsets 6-11 */
    if (!urbi_memeq(d->buf + 6, URBI_BYTECODE_CANARY, URBI_BYTECODE_CANARY_LEN)) {
        set_errmsg(d->errmsg, d->errcap,
                   "corrupt canary bytes (possible FTP/Windows paste translation)");
        return UCHUNK_LOAD_BAD_MAGIC;
    }
    /* format descriptor fields, one at a time for specific diagnostics */
    if (d->buf[12] != (uint8_t)URBI_INT_WIDTH) {
        set_errmsg(d->errmsg, d->errcap, "flavor mismatch: int_width expected %u, got %u",
                   (unsigned)URBI_INT_WIDTH, (unsigned)d->buf[12]);
        return UCHUNK_LOAD_FLAVOR_MISMATCH;
    }
    if (d->buf[13] != 8U) {
        set_errmsg(d->errmsg, d->errcap, "flavor mismatch: float_type expected %u, got %u",
                   8U, (unsigned)d->buf[13]);
        return UCHUNK_LOAD_FLAVOR_MISMATCH;
    }
    if (d->buf[14] != (uint8_t)URBI_INSTR_WIDTH) {
        set_errmsg(d->errmsg, d->errcap, "flavor mismatch: instr_width expected %u, got %u",
                   (unsigned)URBI_INSTR_WIDTH, (unsigned)d->buf[14]);
        return UCHUNK_LOAD_FLAVOR_MISMATCH;
    }
    if (d->buf[15] != (uint8_t)URBI_ENDIANNESS) {
        set_errmsg(d->errmsg, d->errcap, "flavor mismatch: endianness expected %u, got %u",
                   (unsigned)URBI_ENDIANNESS, (unsigned)d->buf[15]);
        return UCHUNK_LOAD_FLAVOR_MISMATCH;
    }
    /* v1.0 defines no flag bits in this region.  Forward-compat tolerance
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
                return UCHUNK_LOAD_CORRUPT;
            }
        }
    }
    d->off = 24;
    return UCHUNK_LOAD_OK;
}

/* v1.7: metadata section is source_name only.  max_reg moved into root_proto
 * block (read by decode_proto alongside nupvals/nparams). */
static UChunkLoadError decode_metadata(MDecCtx *d) {
    uint64_t src_len = 0;
    size_t consumed = 0;
    UChunkLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                 &src_len, &consumed);
    if (rc != UCHUNK_LOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at source_name_len");
        return rc;
    }
    d->off += consumed;
    if (d->off + src_len > d->size) {
        set_errmsg(d->errmsg, d->errcap, "truncated at source_name");
        return UCHUNK_LOAD_TRUNCATED;
    }
    if (src_len > 0U) {
        UChunkAllocFn alloc = module_allocator(d->root_proto);
        if (alloc == NULL) {
            set_errmsg(d->errmsg, d->errcap, "no allocator for source_name");
            return UCHUNK_LOAD_OOM;
        }
        char *name = (char *)alloc(NULL, src_len + 1U, d->root_proto->alloc_ud);
        if (name == NULL) return UCHUNK_LOAD_OOM;
        module_memcpy(name, d->buf + d->off, src_len);
        name[src_len] = '\0';
        d->root_proto->source_name = name;
        d->off += src_len;
    }
    return UCHUNK_LOAD_OK;
}

/* Decode the constants section into (target_buf, target_count, target_cap).
   Used both for the root chunk (writes to module->...) and per-proto
   (writes to p->...). */
static UChunkLoadError decode_constants_into(MDecCtx *d,
                                              UValue **target_buf,
                                              size_t *target_count,
                                              size_t *target_cap,
                                              UChunkAllocFn alloc,
                                              void *alloc_ud) {
    uint64_t n_const = 0;
    size_t consumed = 0;
    UChunkLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                  &n_const, &consumed);
    if (rc != UCHUNK_LOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_constants");
        return rc;
    }
    d->off += consumed;
    if (n_const > (uint64_t)UINT16_MAX + 1U) {
        set_errmsg(d->errmsg, d->errcap, "n_constants too large");
        return UCHUNK_LOAD_CORRUPT;
    }
    if (n_const > 0U) {
        if (!module_grow_with_alloc(alloc, alloc_ud,
                                    (void **)target_buf, target_cap,
                                    (size_t)n_const, sizeof(UValue))) {
            return UCHUNK_LOAD_OOM;
        }
    }
    for (uint64_t i = 0; i < n_const; i++) {
        if (d->off + 1U > d->size) {
            set_errmsg(d->errmsg, d->errcap, "truncated at constant kind");
            return UCHUNK_LOAD_TRUNCATED;
        }
        uint8_t kind = d->buf[d->off++];
        if (kind > (uint8_t)UVAL_STR) {
            set_errmsg(d->errmsg, d->errcap, "constant kind %u out of range (max %u)",
                       (unsigned)kind, (unsigned)UVAL_STR);
            return UCHUNK_LOAD_CORRUPT_TAG;
        }
        (*target_buf)[*target_count].kind = kind;
        if (kind == (uint8_t)UVAL_INT) {
            int64_t v = 0;
            rc = module_decode_varint_zz(d->buf + d->off, d->size - d->off, &v, &consumed);
            if (rc != UCHUNK_LOAD_OK) {
                set_errmsg(d->errmsg, d->errcap, "bad varint in UVAL_INT");
                return rc;
            }
            d->off += consumed;
            (*target_buf)[*target_count].v.i = v;
        } else if (kind == (uint8_t)UVAL_FLOAT) {
            if (d->off + 8U > d->size) {
                set_errmsg(d->errmsg, d->errcap, "truncated at UVAL_FLOAT");
                return UCHUNK_LOAD_TRUNCATED;
            }
            module_memcpy(&(*target_buf)[*target_count].v.f,
                          d->buf + d->off, 8);
            d->off += 8;
        } else if (kind == (uint8_t)UVAL_STR) {
            uint64_t slen = 0;
            rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                        &slen, &consumed);
            if (rc != UCHUNK_LOAD_OK) {
                set_errmsg(d->errmsg, d->errcap, "bad varint at UVAL_STR length");
                return rc;
            }
            d->off += consumed;
            if (d->off + slen > d->size) {
                set_errmsg(d->errmsg, d->errcap, "truncated at UVAL_STR bytes");
                return UCHUNK_LOAD_TRUNCATED;
            }
            UChunkAllocFn alloc_fn = alloc;
            char *bytes = (char *)alloc_fn(NULL, (size_t)slen + 1U, alloc_ud);
            if (bytes == NULL) {
                return UCHUNK_LOAD_OOM;
            }
            module_memcpy(bytes, d->buf + d->off, (size_t)slen);
            bytes[slen] = '\0';
            d->off += (size_t)slen;
            (*target_buf)[*target_count].v.p = bytes;
        } else {
            /* UVAL_NIL / UVAL_BOOL — no payload encoder/decoder.  The emitter
             * never produces these in constant pools (BOOL is OP_LOADBOOL
             * immediate, NIL is OP_LOADNIL).  Hand-crafted bytecode that
             * smuggles them in is rejected via UCHUNK_LOAD_CORRUPT_TAG so the
             * loader does not crash on the missing payload read. */
            set_errmsg(d->errmsg, d->errcap, "constant kind %u not decodable in constant pools",
                       (unsigned)kind);
            return UCHUNK_LOAD_CORRUPT_TAG;
        }
        (*target_count)++;
    }
    return UCHUNK_LOAD_OK;
}

/* Decode the instructions section into (target_buf, target_count, target_cap). */
static UChunkLoadError decode_instructions_into(MDecCtx *d,
                                                 uint32_t **target_buf,
                                                 size_t *target_count,
                                                 size_t *target_cap,
                                                 UChunkAllocFn alloc,
                                                 void *alloc_ud) {
    uint64_t n_instr = 0;
    size_t consumed = 0;
    UChunkLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                  &n_instr, &consumed);
    if (rc != UCHUNK_LOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_instructions");
        return rc;
    }
    d->off += consumed;
    if (n_instr > (uint64_t)URBI_MAX_INSTRS_PER_PROTO) {
        set_errmsg(d->errmsg, d->errcap,
                   "n_instructions=%llu exceeds URBI_MAX_INSTRS_PER_PROTO=%zu",
                   (unsigned long long)n_instr, URBI_MAX_INSTRS_PER_PROTO);
        return UCHUNK_LOAD_OVERSIZED;
    }
    /* 4-byte alignment: skip 0..3 padding bytes, all must be zero. */
    while ((d->off & 3U) != 0U) {
        if (d->off >= d->size) {
            set_errmsg(d->errmsg, d->errcap, "truncated at instruction alignment padding");
            return UCHUNK_LOAD_TRUNCATED;
        }
        if (d->buf[d->off] != 0U) {
            set_errmsg(d->errmsg, d->errcap, "non-zero instruction-align padding at offset %zu",
                       d->off);
            return UCHUNK_LOAD_CORRUPT;
        }
        d->off++;
    }
    if (n_instr > 0U) {
        if (!module_grow_with_alloc(alloc, alloc_ud,
                                    (void **)target_buf, target_cap,
                                    (size_t)n_instr, sizeof(uint32_t))) {
            return UCHUNK_LOAD_OOM;
        }
    }
    for (uint64_t i = 0; i < n_instr; i++) {
        if (d->off + 4U > d->size) {
            set_errmsg(d->errmsg, d->errcap, "truncated at instruction %llu",
                       (unsigned long long)i);
            return UCHUNK_LOAD_TRUNCATED;
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
    return UCHUNK_LOAD_OK;
}

/* Decode the syncline (line_deltas + abs_lines) section into the target
   buffers and counts.  instr_count is the expected n_deltas; the target
   buffers/cap are written via *line_deltas_out / *abs_lines_out etc.
   The trailing-bytes check moved to decode_trailer. */
static UChunkLoadError decode_line_table_into(MDecCtx *d,
                                               int8_t **line_deltas_out,
                                               UAbsLine **abs_lines_out,
                                               size_t instr_count,
                                               size_t *abs_line_count_out,
                                               size_t *abs_line_cap_out,
                                               UChunkAllocFn alloc,
                                               void *alloc_ud) {
    uint64_t n_deltas = 0;
    size_t consumed = 0;
    UChunkLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                  &n_deltas, &consumed);
    if (rc != UCHUNK_LOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_deltas");
        return rc;
    }
    d->off += consumed;
    if (n_deltas != (uint64_t)instr_count) {
        set_errmsg(d->errmsg, d->errcap,
                   "n_deltas=%llu does not match n_instructions=%zu",
                   (unsigned long long)n_deltas, instr_count);
        return UCHUNK_LOAD_CORRUPT;
    }
    if (n_deltas > 0U) {
        if (alloc == NULL) return UCHUNK_LOAD_OOM;
        *line_deltas_out = (int8_t *)alloc(NULL, (size_t)n_deltas, alloc_ud);
        if (*line_deltas_out == NULL) return UCHUNK_LOAD_OOM;
        if (d->off + (size_t)n_deltas > d->size) {
            set_errmsg(d->errmsg, d->errcap, "truncated at line_deltas");
            return UCHUNK_LOAD_TRUNCATED;
        }
        module_memcpy(*line_deltas_out, d->buf + d->off, (size_t)n_deltas);
        d->off += (size_t)n_deltas;
    }

    uint64_t n_abs = 0;
    rc = module_decode_varint_u(d->buf + d->off, d->size - d->off, &n_abs, &consumed);
    if (rc != UCHUNK_LOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_abs_lines");
        return rc;
    }
    d->off += consumed;
    if (n_abs > (uint64_t)instr_count) {
        set_errmsg(d->errmsg, d->errcap,
                   "n_abs_lines=%llu exceeds instr_count=%zu",
                   (unsigned long long)n_abs, instr_count);
        return UCHUNK_LOAD_CORRUPT;
    }
    if (n_abs > 0U) {
        if (!module_grow_with_alloc(alloc, alloc_ud,
                                    (void **)abs_lines_out, abs_line_cap_out,
                                    (size_t)n_abs, sizeof(UAbsLine))) {
            return UCHUNK_LOAD_OOM;
        }
    }
    uint32_t prev_pc_checkpoint = 0;
    for (uint64_t i = 0; i < n_abs; i++) {
        uint64_t pc64 = 0;
        uint64_t line64 = 0;
        rc = module_decode_varint_u(d->buf + d->off, d->size - d->off, &pc64, &consumed);
        if (rc != UCHUNK_LOAD_OK) {
            set_errmsg(d->errmsg, d->errcap, "bad varint at abs_line pc");
            return rc;
        }
        d->off += consumed;
        rc = module_decode_varint_u(d->buf + d->off, d->size - d->off, &line64, &consumed);
        if (rc != UCHUNK_LOAD_OK) {
            set_errmsg(d->errmsg, d->errcap, "bad varint at abs_line line");
            return rc;
        }
        d->off += consumed;
        if (pc64 >= (uint64_t)instr_count) {
            set_errmsg(d->errmsg, d->errcap,
                       "abs_line pc=%llu out of range (instr_count=%zu)",
                       (unsigned long long)pc64, instr_count);
            return UCHUNK_LOAD_CORRUPT;
        }
        if (i > 0 && (uint32_t)pc64 <= prev_pc_checkpoint) {
            set_errmsg(d->errmsg, d->errcap,
                       "abs_lines not monotonic in pc at %llu",
                       (unsigned long long)pc64);
            return UCHUNK_LOAD_CORRUPT;
        }
        (*abs_lines_out)[*abs_line_count_out].pc   = (uint32_t)pc64;
        (*abs_lines_out)[*abs_line_count_out].line = (uint32_t)line64;
        (*abs_line_count_out)++;
        prev_pc_checkpoint = (uint32_t)pc64;
    }
    return UCHUNK_LOAD_OK;
}

/* Decode a site name table: count + N length-prefixed UTF-8 strings.
 * Stores into *out_count + *out_strs (caller-owned).
 * Used for both the root chunk and every nested proto.  The count is
 * capped at 65,535: a site index is a C byte widened by OP_EXTARG to
 * 16 bits, and site_count itself is a uint16_t. */
static UChunkLoadError decode_site_names_into(MDecCtx *d,
                                              uint16_t *out_count,
                                              char ***out_strs,
                                              UChunkAllocFn alloc,
                                              void *alloc_ud) {
    uint64_t count = 0;
    size_t consumed = 0;
    UChunkLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                  &count, &consumed);
    if (rc != UCHUNK_LOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_site_names");
        return rc;
    }
    d->off += consumed;
    if (count > 65535U) {
        set_errmsg(d->errmsg, d->errcap, "n_site_names=%llu exceeds cap (65535)",
                   (unsigned long long)count);
        return UCHUNK_LOAD_CORRUPT;
    }
    /* Each name costs at least its length byte, so a count the rest of
     * the buffer cannot hold is refused before the pointer array is
     * allocated rather than after a few input bytes asked for 512 KB. */
    if (count > (uint64_t)(d->size - d->off)) {
        set_errmsg(d->errmsg, d->errcap, "n_site_names=%llu exceeds the %zu bytes remaining",
                   (unsigned long long)count, d->size - d->off);
        return UCHUNK_LOAD_CORRUPT;
    }
    *out_count = (uint16_t)count;
    if (count == 0U) {
        *out_strs = NULL;
        return UCHUNK_LOAD_OK;
    }
    if (alloc == NULL) return UCHUNK_LOAD_OOM;
    char **strs = (char **)alloc(NULL, (size_t)count * sizeof(char *), alloc_ud);
    if (strs == NULL) return UCHUNK_LOAD_OOM;
    /* Zero-init so partial-fail cleanup (uchunk_destroy /
       uproto_destroy_buffers) walks well-defined NULL slots. */
    for (uint64_t k = 0; k < count; k++) strs[k] = NULL;
    *out_strs = strs;
    for (uint64_t k = 0; k < count; k++) {
        uint64_t nlen = 0;
        rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                     &nlen, &consumed);
        if (rc != UCHUNK_LOAD_OK) {
            set_errmsg(d->errmsg, d->errcap, "bad varint at site_name[%llu] length",
                       (unsigned long long)k);
            return rc;
        }
        d->off += consumed;
        if (nlen > 256U) {
            set_errmsg(d->errmsg, d->errcap, "site_name[%llu] length=%llu exceeds cap (256)",
                       (unsigned long long)k, (unsigned long long)nlen);
            return UCHUNK_LOAD_CORRUPT;
        }
        if (d->off + nlen > d->size) {
            set_errmsg(d->errmsg, d->errcap, "truncated at site_name[%llu] body",
                       (unsigned long long)k);
            return UCHUNK_LOAD_TRUNCATED;
        }
        char *dup = (char *)alloc(NULL, (size_t)nlen + 1U, alloc_ud);
        if (dup == NULL) return UCHUNK_LOAD_OOM;
        if (nlen > 0U) module_memcpy(dup, d->buf + d->off, (size_t)nlen);
        dup[nlen] = '\0';
        strs[k] = dup;
        d->off += nlen;
    }
    return UCHUNK_LOAD_OK;
}

/* Forward declaration: decode_proto is recursive (v1.7 nested[] children). */
static UChunkLoadError decode_proto(MDecCtx *d, UProto *p);

/* Decode the nested[] section of a UProto: varint n_nested + N proto records.
 * v1.7: called from decode_proto for both root and nested protos. */
static UChunkLoadError decode_nested_protos_into(MDecCtx *d, UProto *parent) {
    uint64_t n_nested = 0;
    size_t consumed = 0;
    UChunkLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                  &n_nested, &consumed);
    if (rc != UCHUNK_LOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_nested");
        return rc;
    }
    d->off += consumed;
    if (n_nested > 1024U) {
        set_errmsg(d->errmsg, d->errcap, "n_nested=%llu exceeds cap (1024)",
                   (unsigned long long)n_nested);
        return UCHUNK_LOAD_CORRUPT;
    }
    for (uint64_t i = 0; i < n_nested; i++) {
        if (++d->depth > UCHUNK_MAX_PROTO_DEPTH) {
            set_errmsg(d->errmsg, d->errcap,
                       "nested proto depth exceeds cap (%d) at child %llu",
                       UCHUNK_MAX_PROTO_DEPTH, (unsigned long long)i);
            --d->depth;
            return UCHUNK_LOAD_CORRUPT;
        }
        /* Allocate child proto under parent's module ownership. */
        UChunkAllocFn alloc = module_allocator(d->root_proto);
        if (alloc == NULL) { --d->depth; return UCHUNK_LOAD_OOM; }
        /* Grow parent->nested[] array. */
        if (parent->nested_count >= parent->nested_cap) {
            size_t new_cap = parent->nested_cap == 0 ? 4 : parent->nested_cap * 2;
            void *fresh = alloc((void *)parent->nested, new_cap * sizeof(UProto *),
                                d->root_proto->alloc_ud);
            if (fresh == NULL) { --d->depth; return UCHUNK_LOAD_OOM; }
            parent->nested     = (UProto **)fresh;
            parent->nested_cap = new_cap;
        }
        UProto *child = (UProto *)alloc(NULL, sizeof(UProto), d->root_proto->alloc_ud);
        if (child == NULL) { --d->depth; return UCHUNK_LOAD_OOM; }
        urbi_zero(child, sizeof(*child));
        child->alloc_fn = d->root_proto->alloc_fn;
        child->alloc_ud = d->root_proto->alloc_ud;
        child->proto_index = ++d->root_proto->next_proto_serial;
        parent->nested[parent->nested_count++] = child;
        rc = decode_proto(d, child);
        --d->depth;
        if (rc != UCHUNK_LOAD_OK) return rc;
    }
    return UCHUNK_LOAD_OK;
}

/* Decode a single UProto from the stream into a pre-allocated proto.
 * v1.7: recursive — reads nested_count + nested[] children at end.
 * The proto's alloc_fn/alloc_ud must be set by the caller. */
static UChunkLoadError decode_proto(MDecCtx *d, UProto *p) {
    UChunkAllocFn alloc = p->alloc_fn;
    if (alloc == NULL) {
        /* Hosted-build fallback: caller did not supply an allocator and
           the proto inherits from the module which uses stdlib_alloc. */
        alloc = module_allocator(d->root_proto);
    }
    void *alloc_ud = p->alloc_ud;

    if (d->off + 3U > d->size) {
        set_errmsg(d->errmsg, d->errcap, "truncated at proto header (max_reg/nupvals/nparams)");
        return UCHUNK_LOAD_TRUNCATED;
    }
    p->max_reg = d->buf[d->off++];
    p->nupvals = d->buf[d->off++];
    p->nparams = d->buf[d->off++];
    p->arity_prologue = d->arity_flag;
    /* nupvals + nparams cross-check.  Each occupies one byte
     * (capped at 255 by the wire format) but the sum must fit in the
     * register frame so the runtime can address every captured upvalue
     * and parameter via a register slot.  emit_init_funcstate guarantees
     * this; the check guards against hand-crafted bytecode that
     * overflows R[0..max_reg].  Forward-looking: if either field is
     * widened to varint at a future bytecode break, the byte-width cap
     * goes away and an explicit `<= 256` check is needed. */
    if ((unsigned)p->nupvals + (unsigned)p->nparams > (unsigned)p->max_reg + 1U) {
        set_errmsg(d->errmsg, d->errcap,
                   "proto header: nupvals=%u + nparams=%u exceeds max_reg+1=%u",
                   (unsigned)p->nupvals, (unsigned)p->nparams,
                   (unsigned)p->max_reg + 1U);
        return UCHUNK_LOAD_CORRUPT;
    }

    UChunkLoadError rc;
    /* Every UVAL_STR entry decode_constants_into writes is a fresh malloc'd
     * buffer (see its UVAL_STR arm), so the pool is module-owned and
     * uproto_destroy_buffers frees them.  The flag is set BEFORE the call,
     * not after: a truncated pool returns partway through, and the entries
     * already decoded (const_count only counts completed ones) would
     * otherwise be skipped by the cleanup walk and leak. */
    p->constants_owned = true;
    rc = decode_constants_into(d, &p->constants, &p->const_count, &p->const_cap,
                               alloc, alloc_ud);
    if (rc != UCHUNK_LOAD_OK) return rc;
    rc = decode_instructions_into(d, &p->instructions, &p->instr_count, &p->instr_cap,
                                  alloc, alloc_ud);
    if (rc != UCHUNK_LOAD_OK) return rc;
    rc = decode_line_table_into(d, &p->line_deltas, &p->abs_lines,
                                p->instr_count, &p->abs_line_count, &p->abs_line_cap,
                                alloc, alloc_ud);
    if (rc != UCHUNK_LOAD_OK) return rc;
    rc = decode_site_names_into(d, &p->site_count, &p->site_name_strs, alloc, alloc_ud);
    if (rc != UCHUNK_LOAD_OK) return rc;

    /* v1.7: nested_count + recursive nested[] children. */
    rc = decode_nested_protos_into(d, p);
    return rc;
}

/* Final byte check: stream must end exactly at the last decoded section. */
static UChunkLoadError decode_trailer(MDecCtx *d) {
    if (d->off != d->size) {
        set_errmsg(d->errmsg, d->errcap,
                   "trailing %zu bytes after nested-protos section", d->size - d->off);
        return UCHUNK_LOAD_CORRUPT;
    }
    return UCHUNK_LOAD_OK;
}

static void set_root_backptr_recursive(UProto *node, UProto *root) {
    if (node == NULL) return;
    node->root = (node == root) ? NULL : root;
    for (size_t i = 0U; i < node->nested_count; i++) {
        set_root_backptr_recursive(node->nested[i], root);
    }
}

/* --- Public API --- */

UChunkLoadError uchunk_deserialize(UProto **out_root, const uint8_t *buf, size_t size,
                                   UChunkAllocFn alloc_fn, void *alloc_ud,
                                   char *errmsg, size_t errcap) {
    /* errmsg/errcap contract: the (NULL, 0) pair suppresses diagnostics; any
     * other shape — including (non-NULL, 0) — is silently accepted as
     * "diagnostics off".  set_errmsg internally no-ops on errcap == 0 so
     * passing a non-NULL buffer with zero capacity is harmless rather than
     * a contract violation.  Callers that require a populated errmsg must
     * supply errcap >= 1. */
    if (out_root == NULL || buf == NULL) {
        set_errmsg(errmsg, errcap, "null out_root or buffer");
        return UCHUNK_LOAD_INVALID_ARG;
    }
    *out_root = NULL;

#if __STDC_HOSTED__
    UChunkAllocFn effective_alloc = (alloc_fn != NULL) ? alloc_fn : stdlib_alloc;
#else
    UChunkAllocFn effective_alloc = alloc_fn;
    if (effective_alloc == NULL) return UCHUNK_LOAD_OOM;
#endif

    /* Allocate the root UProto struct via alloc_fn. */
    UProto *rp = (UProto *)effective_alloc(NULL, sizeof(UProto), alloc_ud);
    if (rp == NULL) return UCHUNK_LOAD_OOM;
    urbi_zero(rp, sizeof(UProto));
    rp->root     = NULL;          /* root's own back-pointer is NULL */
    rp->alloc_fn = effective_alloc;
    rp->alloc_ud = alloc_ud;
    rp->origin_vm = NULL;         /* zero for deserialized chunks */
    rp->heap_allocated = true;    /* caller frees via uchunk_destroy */

    MDecCtx d;
    d.root_proto = rp;   /* v0.9.2: UModule deleted, root IS the decode target */
    d.rp         = rp;
    d.buf    = buf;
    d.size   = size;
    d.off    = 0;
    d.errmsg = errmsg;
    d.errcap = errcap;
    d.depth  = 0;
    d.arity_flag = 0U;   /* set by decode_header from header flag bit 0 */

    UChunkLoadError rc;
    if ((rc = decode_header(&d))         != UCHUNK_LOAD_OK) goto fail;
    /* v1.7: body = source_name + root UProto block. */
    if ((rc = decode_metadata(&d))       != UCHUNK_LOAD_OK) goto fail;
    if ((rc = decode_proto(&d, rp))      != UCHUNK_LOAD_OK) goto fail;
    if ((rc = decode_trailer(&d))        != UCHUNK_LOAD_OK) goto fail;
    /* Pass 1: opcode-shape table, register bounds, site indices, EXTARG. */
    if ((rc = urbi_chunk_decode_verify(&d)) != UCHUNK_LOAD_OK) goto fail;
    /* Pass 2: per-sequence bounds (closure preludes, jump and handler
     * targets). */
    if ((rc = urbi_chunk_verify_bounds(&d)) != UCHUNK_LOAD_OK) goto fail;

    set_root_backptr_recursive(rp, rp);

    rp->total_proto_count = (uint16_t)(rp->next_proto_serial + 1U);

    *out_root = rp;
    return UCHUNK_LOAD_OK;

fail:
    /* Partial allocations: free source_name (may have been set by decode_metadata
     * before a later stage failed), then all other buffers, then the struct. */
    if (rp->source_name != NULL) {
        module_buf_free(effective_alloc, alloc_ud, rp->source_name);
        rp->source_name = NULL;
    }
    uproto_destroy_buffers(rp, effective_alloc, alloc_ud);
    effective_alloc(rp, 0, alloc_ud);
    return rc;
}

/* nested[k] may be NULL by design:
 *   the old core's watcher installer detached a UProto from module->nested[]
 *   when its UClosure was captured, transferring ownership from the module to
 *   the watcher pool, and nested[k] then read NULL.  The re-founded runtime
 *   does not do that — a captured closure keeps its proto and the collector
 *   owns both — but the loader still tolerates a NULL slot, because a chunk
 *   that arrives off disk is not required to have been produced by this
 *   build.  uchunk_destroy must skip NULL slots without freeing them, since
 *   whoever nulled one now owns
 *   that proto and will free it on watcher recycle.
 *
 *   Detach only happens at `s->frame_count == 0` (chunk-top installs).
 *   Installs inside a callee skip the transfer entirely to avoid the
 *   cascade-wake use-after-free on shared protos, so callee-side
 *   nested[] slots stay populated and are freed normally below. */

void
uchunk_destroy(UProto *root, struct UVM *vm)
{
    (void)vm;   /* the rescue path is retired; the caller's cell owns the chunk */
    if (root == NULL) return;
    uchunk_destroy_internal(root);
}

static void uchunk_destroy_internal(UProto *root) {
    if (root == NULL) return;

    UChunkAllocFn alloc = module_allocator(root);
    void *alloc_ud = root->alloc_ud;   /* capture before uproto_destroy_buffers zeroes root */
    bool heap = root->heap_allocated;
    if (alloc != NULL) {
        /* Free source_name string. */
        module_buf_free(alloc, alloc_ud, root->source_name);
        root->source_name = NULL;
        /* Free all UProto-owned buffers (nested[], instructions, constants, etc.).
         * uproto_destroy_buffers zeroes the struct at the end, so alloc_ud must
         * be captured before this call. */
        uproto_destroy_buffers(root, alloc, alloc_ud);
        /* If root was heap-allocated (via uchunk_deserialize), free the struct. */
        if (heap) {
            alloc(root, 0, alloc_ud);
            return;
        }
    }
    /* Stack/static root: zero the struct but don't free it. */
    if (!heap) {
        urbi_zero(root, sizeof(*root));
    }
}

const char *uchunk_load_error_name(UChunkLoadError code) {
    switch (code) {
    case UCHUNK_LOAD_OK:                  return "UCHUNK_LOAD_OK";
    case UCHUNK_LOAD_BAD_MAGIC:           return "UCHUNK_LOAD_BAD_MAGIC";
    case UCHUNK_LOAD_UNSUPPORTED_VERSION: return "UCHUNK_LOAD_UNSUPPORTED_VERSION";
    case UCHUNK_LOAD_FLAVOR_MISMATCH:     return "UCHUNK_LOAD_FLAVOR_MISMATCH";
    case UCHUNK_LOAD_TRUNCATED:           return "UCHUNK_LOAD_TRUNCATED";
    case UCHUNK_LOAD_CORRUPT_VARINT:      return "UCHUNK_LOAD_CORRUPT_VARINT";
    case UCHUNK_LOAD_CORRUPT_TAG:         return "UCHUNK_LOAD_CORRUPT_TAG";
    case UCHUNK_LOAD_CORRUPT:             return "UCHUNK_LOAD_CORRUPT";
    case UCHUNK_LOAD_OOM:                 return "UCHUNK_LOAD_OOM";
    case UCHUNK_LOAD_INVALID_ARG:         return "UCHUNK_LOAD_INVALID_ARG";
    case UCHUNK_LOAD_OVERSIZED:           return "UCHUNK_LOAD_OVERSIZED";
    case UCHUNK_LOAD_TRUNCATED_UPVALUES:  return "UCHUNK_LOAD_TRUNCATED_UPVALUES";
    case UCHUNK_LOAD_MALFORMED_UPVALUE:   return "UCHUNK_LOAD_MALFORMED_UPVALUE";
    case UCHUNK_LOAD_JMP_OUT_OF_BOUNDS:   return "UCHUNK_LOAD_JMP_OUT_OF_BOUNDS";
    case UCHUNK_LOAD_CALL_NRESULTS_ZERO:  return "UCHUNK_LOAD_CALL_NRESULTS_ZERO";
    case UCHUNK_LOAD_RESERVED_OPCODE:     return "UCHUNK_LOAD_RESERVED_OPCODE";
    case UCHUNK_LOAD_BAD_EXTARG:          return "UCHUNK_LOAD_BAD_EXTARG";
    case UCHUNK_LOAD_BAD_TARGET:          return "UCHUNK_LOAD_BAD_TARGET";
    }
    return "UCHUNK_LOAD_UNKNOWN";
}
