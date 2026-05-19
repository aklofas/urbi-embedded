/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode UModule deserializer + verifier + destroy.  Freestanding. */

#include "module/umodule.h"
#include "runtime/umacros.h"
#include "value/uvarint.h"
#include "uopcode_shape.h"
#include "vm/uvm.h"               /* struct UVM access for umodule_destroy rescue path */

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

/* Forward declaration — umodule_destroy_internal is defined below, after
 * umodule_destroy_proto_buffers.  The public umodule_destroy shim (v0.8.0
 * deferred-destroy) calls into this. */
static void umodule_destroy_internal(UModule *module, struct UVM *vm);

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

/* MOD-032: free a single buffer through `alloc`, skipping NULL.
 * Centralizes the `if (p != NULL) (void)alloc(p, 0, ud);` pattern that
 * appears 6× in umodule_destroy (and 5× in umodule_destroy_proto_buffers,
 * which we leave alone for surgical scope).  The pointer is not NULLed
 * because both call sites zero the containing struct via urbi_zero after
 * all frees complete. */
static inline void module_buf_free(UModuleAllocFn alloc, void *alloc_ud,
                                   void *p) {
    if (p != NULL) (void)alloc(p, 0, alloc_ud);
}

/* Free the per-constant module-owned bytes attached to UVAL_STR slots whose
 * _pad[0] marker was set by the deserializer (ownership flag, see umodule.c
 * decode_constants_into UVAL_STR arm).  Emit-time UVAL_STR slots carry an
 * intern-table pointer (VM-owned) and must NOT be freed here; the marker
 * distinguishes the two ownership domains.
 *
 * Idempotent — safe to call on an already-freed slot because zero-init or
 * post-fixup buffers have _pad[0] == 0.  Module-instance create clears the
 * marker after the lazy intern fixup so this helper never double-frees. */
static void free_owned_str_constants(UValue *constants, size_t count,
                                     UModuleAllocFn alloc, void *alloc_ud) {
    if (constants == NULL || alloc == NULL) return;
    for (size_t i = 0U; i < count; i++) {
        if (constants[i].kind == (uint8_t)UVAL_STR
            && constants[i]._pad[0] == 1U
            && constants[i].v.p != NULL) {
            (void)alloc(constants[i].v.p, 0, alloc_ud);
            constants[i].v.p = NULL;
            constants[i]._pad[0] = 0U;
        }
    }
}

void umodule_destroy_proto_buffers(UProto *proto, UModuleAllocFn alloc,
                                   void *alloc_ud) {
    /* MOD-030: every caller guards proto != NULL; the runtime contract is
     * "non-NULL proto" — assert rather than silently no-op. */
    URBI_INTERNAL_ASSERT(proto != NULL);
    if (alloc == NULL) return;
    /* Task 11: root_proto owns nested[] — free sub-protos first.
     * Nested protos have nested_count == 0 so this walk is a no-op for them. */
    if (proto->nested != NULL) {
        size_t i;
        for (i = 0; i < proto->nested_count; i++) {
            UProto *p = proto->nested[i];
            if (p == NULL) continue;  /* MOD-015: watcher-detached slot */
            umodule_destroy_proto_buffers(p, alloc, alloc_ud);
            alloc(p, 0, alloc_ud);
        }
        alloc((void *)proto->nested, 0, alloc_ud);
    }
    if (proto->instructions != NULL) alloc(proto->instructions, 0, alloc_ud);
    free_owned_str_constants(proto->constants, proto->const_count, alloc, alloc_ud);
    if (proto->constants    != NULL) alloc(proto->constants,    0, alloc_ud);
    if (proto->line_deltas  != NULL) alloc(proto->line_deltas,  0, alloc_ud);
    if (proto->abs_lines    != NULL) alloc(proto->abs_lines,    0, alloc_ud);
    /* TIDY-005: explicit (void *) casts on multi-level pointer free paths
     * (USymbol ** / char ** / UProto ** all decay to void * for alloc's
     * inout pointer; the implicit conversion violates strict-aliasing
     * cleanliness even though every modern allocator treats the pointer
     * as an opaque tag). */
    if (proto->ic_names     != NULL) alloc((void *)proto->ic_names,     0, alloc_ud);
    if (proto->ic_name_strs != NULL) {
        /* Each entry is a NUL-terminated string allocated separately. */
        for (uint16_t k = 0; k < proto->ic_count; k++) {
            if (proto->ic_name_strs[k] != NULL) {
                alloc(proto->ic_name_strs[k], 0, alloc_ud);
            }
        }
        alloc((void *)proto->ic_name_strs, 0, alloc_ud);
    }
    /* Zero the proto struct but do not free proto itself (owned by parent). */
    urbi_zero(proto, sizeof(*proto));
}

UProto *umodule_alloc_nested_proto(UModule *module, UProto *parent_proto) {
    UModuleAllocFn alloc = module_allocator(module);
    if (alloc == NULL) return NULL;
    /* v0.8.5: parent_proto is the explicit nested[] target.  For top-level
     * function literals callers pass module->root_proto; for nested
     * function literals callers pass the enclosing UProto. */
    if (parent_proto == NULL) return NULL;

    /* Grow parent_proto->nested[] array if needed. */
    if (parent_proto->nested_count >= parent_proto->nested_cap) {
        size_t new_cap = parent_proto->nested_cap == 0 ? 4 : parent_proto->nested_cap * 2;
        /* TIDY-005: explicit (void *) cast on UProto ** → void * decay. */
        void *fresh = alloc((void *)parent_proto->nested,
                            new_cap * sizeof(UProto *),
                            module->alloc_ud);
        if (fresh == NULL) return NULL;
        parent_proto->nested     = (UProto **)fresh;
        parent_proto->nested_cap = new_cap;
    }

    /* Allocate the UProto struct itself.
     *
     * MOD-003: if this allocation fails AFTER the nested[] grow above
     * succeeded, we leave `root->nested` pointing at the grown (larger)
     * buffer with `nested_cap` bumped but `nested_count` unchanged.  This
     * is "grow-without-commit" — the array is correctly sized for an
     * unused trailing slot range [nested_count..nested_cap), every
     * existing entry [0..nested_count) is intact, and the next caller
     * walks the same grow path with the larger cap already satisfied
     * (skipping the realloc).
     *
     * Rolling back the grow would require freeing the larger buffer and
     * restoring the prior nested pointer.  Since realloc invalidates the
     * prior pointer when it returns a different address, restoring would
     * mean re-allocating yet again — net cost higher than carrying the
     * benign over-cap.  The "benign over-cap" state is observed by:
     *   - umodule_destroy: walks [0..nested_count) only.
     *   - serialize: writes nested_count, not nested_cap.
     *   - subsequent umodule_alloc_nested_proto: enters the grow branch
     *     only when nested_count >= nested_cap, which now skips the
     *     realloc and proceeds to UProto alloc.
     * No code path reads beyond [0..nested_count). */
    UProto *proto = (UProto *)alloc(NULL, sizeof(UProto), module->alloc_ud);
    if (proto == NULL) return NULL;
    urbi_zero(proto, sizeof(*proto));
    proto->alloc_fn = module->alloc_fn;
    proto->alloc_ud = module->alloc_ud;

    /* v0.8.1 Variant B Option (a) per spec §3.5: slot-implicit refcount dropped.
     * The nested[] slot's reachability is structural (root_proto owns nested[]);
     * no independent refcount needed.  Closures bump root_proto.refcount via
     * uproto_root_of() at vm_alloc_closure; that is the only accounting needed. */
    proto->refcount = 0U;

    /* v0.8.5: assign DFS pre-order serial.  Root's ic_index = 0 was set at
     * root-proto allocation (uemit_init / umodule_deserialize); each call
     * here produces the next available serial.  The first nested
     * allocation produces ic_index = 1 because next_proto_serial starts
     * at 0 via the UModule zero-init. */
    proto->ic_index = ++module->next_proto_serial;

    parent_proto->nested[parent_proto->nested_count++] = proto;
    return proto;
}

/* --- Per-section decoder context (file-private) --- */

typedef struct {
    UModule        *module;
    UProto         *rp;      /* root_proto: allocated before decode; receives chunk-top fields */
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
    /* version byte: 0x17 = v1.7 (16*major + minor); all prior versions are
       hard-rejected.  v1.6 → v1.7 is the v0.8.1-uproto-root Phase 3 break
       (UModule body shrinks to header + source_name + recursive root_proto
       block; per-field duplication of chunk-top fields removed).  Loading
       older modules silently would parse the body as the wrong structure. */
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

/* v1.7: metadata section is source_name only.  max_reg moved into root_proto
 * block (read by decode_proto alongside nupvals/nparams). */
static UModuleLoadError decode_metadata(MDecCtx *d) {
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
        } else if (kind == (uint8_t)UVAL_STR) {
            /* M6 (closes v0.5.6 MOD-008 reservation): UVAL_STR carries a
             * uvarint byte-length prefix + raw UTF-8 bytes.  The loader has
             * no UVM in scope and therefore cannot intern; instead we
             * allocate a NUL-terminated buffer via the module allocator and
             * record ownership with the _pad[0] = 1 marker.  Module-instance
             * create (or any future fixup pass) interns the bytes against
             * the runtime VM and clears the marker.  umodule_destroy walks
             * constants and frees any v.p whose owner-flag is still set. */
            uint64_t slen = 0;
            rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                        &slen, &consumed);
            if (rc != ULOAD_OK) {
                set_errmsg(d->errmsg, d->errcap, "bad varint at UVAL_STR length");
                return rc;
            }
            d->off += consumed;
            if (d->off + slen > d->size) {
                set_errmsg(d->errmsg, d->errcap, "truncated at UVAL_STR bytes");
                return ULOAD_TRUNCATED;
            }
            UModuleAllocFn alloc_fn = alloc;
            char *bytes = (char *)alloc_fn(NULL, (size_t)slen + 1U, alloc_ud);
            if (bytes == NULL) {
                return ULOAD_OOM;
            }
            module_memcpy(bytes, d->buf + d->off, (size_t)slen);
            bytes[slen] = '\0';
            d->off += (size_t)slen;
            (*target_buf)[*target_count].v.p = bytes;
            (*target_buf)[*target_count]._pad[0] = 1U;  /* owned-by-module marker */
        } else {
            /* UVAL_NIL / UVAL_BOOL — no payload encoder/decoder.  The emitter
             * never produces these in constant pools (BOOL is OP_LOADBOOL
             * immediate, NIL is OP_LOADNIL).  Hand-crafted bytecode that
             * smuggles them in is rejected via ULOAD_CORRUPT_TAG so the
             * loader does not crash on the missing payload read. */
            set_errmsg(d->errmsg, d->errcap, "constant kind %u not decodable in constant pools",
                       (unsigned)kind);
            return ULOAD_CORRUPT_TAG;
        }
        (*target_count)++;
    }
    return ULOAD_OK;
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
    /* MOD-014: monotonic abs_line invariant — pc values must form a strictly
     * increasing sequence (each later checkpoint references a higher pc than
     * the prior).  Skip the comparison on i==0 since there is no prior to
     * compare against; the first checkpoint may legitimately reference pc=0. */
    uint32_t prev_pc_checkpoint = 0;
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
        if (i > 0 && (uint32_t)pc64 <= prev_pc_checkpoint) {
            set_errmsg(d->errmsg, d->errcap,
                       "abs_lines not monotonic in pc at %llu",
                       (unsigned long long)pc64);
            return ULOAD_CORRUPT;
        }
        (*abs_lines_out)[*abs_line_count_out].pc   = (uint32_t)pc64;
        (*abs_lines_out)[*abs_line_count_out].line = (uint32_t)line64;
        (*abs_line_count_out)++;
        prev_pc_checkpoint = (uint32_t)pc64;
    }
    return ULOAD_OK;
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

/* Forward declaration: decode_proto is recursive (v1.7 nested[] children). */
static UModuleLoadError decode_proto(MDecCtx *d, UProto *p);

/* Decode the nested[] section of a UProto: varint n_nested + N proto records.
 * v1.7: called from decode_proto for both root and nested protos. */
static UModuleLoadError decode_nested_protos_into(MDecCtx *d, UProto *parent) {
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
        /* Allocate child proto under parent's module ownership. */
        UModuleAllocFn alloc = module_allocator(d->module);
        if (alloc == NULL) return ULOAD_OOM;
        /* Grow parent->nested[] array. */
        if (parent->nested_count >= parent->nested_cap) {
            size_t new_cap = parent->nested_cap == 0 ? 4 : parent->nested_cap * 2;
            void *fresh = alloc((void *)parent->nested, new_cap * sizeof(UProto *),
                                d->module->alloc_ud);
            if (fresh == NULL) return ULOAD_OOM;
            parent->nested     = (UProto **)fresh;
            parent->nested_cap = new_cap;
        }
        UProto *child = (UProto *)alloc(NULL, sizeof(UProto), d->module->alloc_ud);
        if (child == NULL) return ULOAD_OOM;
        urbi_zero(child, sizeof(*child));
        child->alloc_fn = d->module->alloc_fn;
        child->alloc_ud = d->module->alloc_ud;
        /* v0.8.5: assign DFS pre-order serial identical to the emit path.
         * Recurse order matches umodule_alloc_nested_proto's DFS pre-order
         * because decode_proto is called per child (depth-first) before
         * moving to the next sibling. */
        child->ic_index = ++d->module->next_proto_serial;
        parent->nested[parent->nested_count++] = child;
        rc = decode_proto(d, child);
        if (rc != ULOAD_OK) return rc;
    }
    return ULOAD_OK;
}

/* Decode a single UProto from the stream into a pre-allocated proto.
 * v1.7: recursive — reads nested_count + nested[] children at end.
 * The proto's alloc_fn/alloc_ud must be set by the caller. */
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
    /* W4 / T79: nupvals + nparams cross-check.  Each occupies one byte
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
        return ULOAD_CORRUPT;
    }

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
    if (rc != ULOAD_OK) return rc;

    /* v1.7: nested_count + recursive nested[] children. */
    rc = decode_nested_protos_into(d, p);
    return rc;
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
/* Return true if `op` is an IC-bearing opcode (carries an ic_idx in C).
 * Mirror at v1.6: OP_GETSLOT, OP_SETSLOT, OP_GETSLOT_CHANGE_EVENT, OP_SELF.
 * Mirror discipline: any new IC-bearing opcode added in a future
 * milestone must be added here AND in uemit_assign_ic_index call sites. */
static bool op_carries_ic_index(uint8_t op) {
    return op == (uint8_t)OP_GETSLOT
        || op == (uint8_t)OP_SETSLOT
        || op == (uint8_t)OP_GETSLOT_CHANGE_EVENT
        || op == (uint8_t)OP_SELF;
}

/* Walk one block of instructions (root chunk OR a nested proto) against
   the opcode-shape table, applying per-block bounds (max_reg /
   const_count / instr_count / nested_count / ic_count).  Callers pass
   the root-level nested_count for both root and per-proto walks since
   the v1.5 emitter allocates all function literals as flat siblings
   under the root UModule's nested[] (an OP_CLOSURE inside a nested
   proto refers to a sibling slot in the same root array). */
static UModuleLoadError verify_walk_block(MDecCtx *d,
                                          uint8_t max_reg,
                                          size_t const_count,
                                          size_t instr_count,
                                          size_t nested_count,
                                          uint16_t ic_count,
                                          const uint32_t *instructions) {
    /* MOD-016 / W4: count IC-bearing opcodes seen during the walk so we
     * can cross-validate ic_count after the loop.  Every ic_idx must be
     * < ic_count (per-instruction); ic_count must be <= ic_seen
     * (count check; rejects modules that lie about ic_count without
     * emitting matching IC sites). */
    size_t ic_seen = 0;
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

        /* Cross-validate IC index for IC-bearing opcodes. */
        if (op_carries_ic_index(op)) {
            uint8_t ic_idx = uinstr_c(ins);
            if ((uint16_t)ic_idx >= ic_count) {
                set_errmsg(d->errmsg, d->errcap,
                           "ic_idx=%u >= ic_count=%u at pc %zu (op=%u)",
                           (unsigned)ic_idx, (unsigned)ic_count, vi, (unsigned)op);
                return ULOAD_CORRUPT;
            }
            ic_seen++;
        }

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
    /* MOD-016 / W4: ic_count must not exceed the count of IC-bearing
     * opcodes in the instruction stream.  Each ic_name (and the
     * corresponding runtime UIC entry) is keyed off an emitted
     * GETSLOT/SETSLOT/GETSLOT_CHANGE_EVENT site; lying about ic_count
     * would either leave UIC entries unused (waste) or — worse — leave
     * ic_name_strs[k>=ic_seen] holding a name that no instruction
     * indexes (eligible for confusion attacks at later milestones when
     * ic_index becomes wider). */
    if ((size_t)ic_count > ic_seen) {
        set_errmsg(d->errmsg, d->errcap,
                   "ic_count=%u exceeds %zu IC-bearing opcodes seen",
                   (unsigned)ic_count, ic_seen);
        return ULOAD_CORRUPT;
    }
    return ULOAD_OK;
}

/* v0.8.5: recursive verifier walk.  Each UProto is verified against its
 * OWN nested_count (per-parent OP_CLOSURE Bx index space), matching the
 * truly-recursive emitter contract.  Pre-v0.8.5 the verifier passed
 * the root-level nested_count for every nested proto because the flat
 * emitter routed every OP_CLOSURE to root's nested[] regardless of
 * lexical scope. */
static UModuleLoadError verify_proto_recursive(MDecCtx *d, const UProto *p) {
    if (p == NULL) return ULOAD_OK;
    UModuleLoadError rc = verify_walk_block(d,
                                            p->max_reg,
                                            p->const_count,
                                            p->instr_count,
                                            p->nested_count,
                                            p->ic_count,
                                            p->instructions);
    if (rc != ULOAD_OK) return rc;
    for (size_t i = 0; i < p->nested_count; i++) {
        rc = verify_proto_recursive(d, p->nested[i]);
        if (rc != ULOAD_OK) return rc;
    }
    return ULOAD_OK;
}

static UModuleLoadError decode_verify(MDecCtx *d) {
    return verify_proto_recursive(d, d->rp);
}

/* v0.8.5: recursively set every UProto's root back-pointer.  The module's
 * root_proto gets root = NULL; every other proto in the tree gets
 * root = rp.  Mirrors set_root_recursive in uemit.c — kept independent
 * to avoid cross-module static-helper coupling. */
static void set_root_backptr_recursive(UProto *node, UProto *root) {
    if (node == NULL) return;
    node->root = (node == root) ? NULL : root;
    for (size_t i = 0U; i < node->nested_count; i++) {
        set_root_backptr_recursive(node->nested[i], root);
    }
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

    /* Task 11: allocate root_proto before decoding so decode functions
     * write chunk-top fields directly into root_proto (no alias-copy).
     * If re-deserializing into the same module struct, free the old
     * root_proto (and its buffers) first via umodule_destroy_proto_buffers. */
    UModuleAllocFn root_alloc = module_allocator(module);
    if (root_alloc == NULL) return ULOAD_OOM;
    if (module->root_proto != NULL) {
        umodule_destroy_proto_buffers(module->root_proto, root_alloc, module->alloc_ud);
        root_alloc(module->root_proto, 0, module->alloc_ud);
        module->root_proto = NULL;
    }
    UProto *rp = (UProto *)root_alloc(NULL, sizeof(UProto), module->alloc_ud);
    if (rp == NULL) return ULOAD_OOM;
    urbi_zero(rp, sizeof(UProto));
    rp->root     = NULL;  /* root's own back-pointer is NULL */
    rp->alloc_fn = module->alloc_fn;
    rp->alloc_ud = module->alloc_ud;
    module->root_proto = rp;

    MDecCtx d;
    d.module = module;
    d.rp     = rp;
    d.buf    = buf;
    d.size   = size;
    d.off    = 0;
    d.errmsg = errmsg;
    d.errcap = errcap;

    UModuleLoadError rc;
    if ((rc = decode_header(&d))   != ULOAD_OK) return rc;
    /* v1.7: body = source_name + root_proto block. */
    if ((rc = decode_metadata(&d)) != ULOAD_OK) return rc;
    if ((rc = decode_proto(&d, rp)) != ULOAD_OK) return rc;
    if ((rc = decode_trailer(&d))  != ULOAD_OK) return rc;
    if ((rc = decode_verify(&d))   != ULOAD_OK) return rc;

    /* Back-pointer walk: every UProto's root field points at rp.
     * v0.8.5 made this recursive (was flat-only): walks the full tree
     * DFS so grandchildren also get root set under recursive emission.
     * For flat trees (pre-v0.8.5 bytecode) the inner recursion is a no-op
     * because nested_count == 0 at depth 1. */
    set_root_backptr_recursive(rp, rp);

    /* v0.8.5: stamp total_proto_count to match the emit path (uemit_finish).
     * next_proto_serial holds the last assigned non-root serial; total
     * includes root (ic_index = 0). */
    module->total_proto_count = (uint16_t)(module->next_proto_serial + 1U);

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
 */

/* --- Module strand-bind release (v0.8.1 Phase 2) ----------------------- */

/* v0.8.1 Phase 2: strand-bind release with deferred-destroy trigger.
 * Called by ustrand_destroy and the fatal-loader early-discharge path
 * (uchunk.c) when we hold the still-valid module pointer.
 * Decrements root_proto->refcount; if it reaches 0 and the self-link
 * sentinel is set (umodule_destroy was called with vm=NULL), fires
 * umodule_destroy_internal immediately.
 *
 * Task 11: UModule.destroy_requested deleted.  The vm=NULL deferred path
 * sets root_proto->next_alloc = root_proto (self-link sentinel) instead.
 * When refcount hits 0 here, the sentinel signals deferred destroy. */
void
umodule_strand_refcount_dec(UModule *m, UProto *root_proto, struct UVM *vm)
{
    if (root_proto == NULL) return;
    umodule_proto_refcount_dec(root_proto);
    /* Deferred-destroy trigger: self-link sentinel means the module shell
     * was already freed (vm=NULL destroy path) but root_proto was left with
     * a non-zero refcount.  Now that the last strand-bind ref is gone and
     * the sentinel is set, perform the actual internal free.  m may be NULL
     * if the module shell has already been freed via that path. */
    if (root_proto->refcount == 0U && root_proto->next_alloc == root_proto) {
        /* Deferred-destroy triggered: the host already called umodule_destroy
         * with vm=NULL (self-link sentinel path) while a strand was alive.
         * Now that the last strand-bind ref is gone, perform the actual free.
         * Clear sentinel first so umodule_destroy_proto_buffers can walk
         * nested[] cleanly (next_alloc is not walked, but zeroing is safe). */
        root_proto->next_alloc = NULL;
        umodule_destroy_proto_buffers(root_proto, root_proto->alloc_fn, root_proto->alloc_ud);
        if (root_proto->alloc_fn != NULL) {
            root_proto->alloc_fn(root_proto, 0, root_proto->alloc_ud);
        }
        (void)m;
        (void)vm;
    }
    /* When refcount hits 0 and no self-link sentinel: the module shell is still
     * owned by the host.  Do not auto-destroy — the host is responsible for
     * calling umodule_destroy explicitly.  If the host never calls it, the
     * module buffers are freed at vm_destroy via vm->rescued_protos or simply
     * left for the host to manage (stack/static storage).  The refcount reaching
     * zero is only a signal that no strands are currently bound; it does not
     * transfer ownership. */
}

/* MOD-015 — nested[k] may be NULL by design:
 *   strand_closure_unlink (src/watcher/uwatcher_install.c) detaches a UProto
 *   from module->nested[] when its UClosure is captured by a watcher
 *   (transferring ownership from the module to the watcher pool).  After
 *   detach, nested[k] reads NULL.  This is the expected steady-state for any
 *   chunk that installed reactive watchers — umodule_destroy must skip NULL
 *   slots without freeing them, since the watcher's pool_free now owns
 *   that proto and will free it on watcher recycle.
 *
 *   v0.7.3 — detach only happens at `s->frame_count == 0` (chunk-top
 *   installs).  Installs inside a callee skip the transfer entirely to
 *   avoid the cascade-wake use-after-free on shared protos, so callee-side
 *   nested[] slots stay populated and are freed normally below.  See
 *   the watcher-ownership design rationale (URBI_WATCHER_OWNS_* flags deleted
 *   at v0.8.4 Step C-3; GC now manages closure lifetime). */

/* v0.8.1 Phase 2 (Variant B fusion): deferred-destroy check reads root_proto->refcount.
 * Strand-bind refs now land on root_proto (not module->refcount), so the "are
 * any strands still alive?" test must check root_proto.  Host's existing pattern
 * (umodule_destroy after urbi_vm_destroy) still works: vm_destroy kills all strands
 * first → root_proto->refcount drops to 0 → immediate free.
 *
 * module->refcount is always 0 after Phase 2 redirect (nothing bumps it);
 * it is retained in the struct until Task 11 deletes it. */
void
umodule_destroy(UModule *module, struct UVM *vm)
{
    if (module == NULL) return;
    /* Variant B coexistence path (Phase 2 Task 9 of v0.8.1-uproto-root):
     * when root_proto->refcount > 0 (a strand is still alive), rescue the
     * whole root_proto to vm->rescued_protos.  The root_proto carries ownership
     * of nested[] and all chunk-top buffers; the module shell fields are
     * NULLed so umodule_destroy_internal does not double-free them.
     *
     * The per-nested rescue path in umodule_destroy_internal (vm->stdlib_protos)
     * still runs for the remaining (NULLed) nested[] and buffers — it is a
     * no-op since all the pointers are now NULL.  Coexistence is safe because
     * the two lists are independent; Task 10 removes the per-nested path once
     * Task 8 (closure-refcount redirect) makes whole-root_proto rescue
     * self-sufficient. */
    if (module->root_proto != NULL && module->root_proto->refcount > 0U) {
        if (vm != NULL) {
            UProto *rp = module->root_proto;
            /* Thread rp onto vm->rescued_protos (reuses UProto.next_alloc). */
            rp->next_alloc    = vm->rescued_protos;
            vm->rescued_protos = rp;
            /* Detach root_proto reference from the module shell.
             * Task 11: all chunk-top data lives on root_proto — no duplicate
             * module fields to NULL out. */
            module->root_proto = NULL;
            /* source_name stays on the module shell (not owned by rp). */
        } else {
            /* No vm available — cannot rescue root_proto onto vm->rescued_protos
             * immediately.  Set self-link sentinel (next_alloc == root_proto)
             * on root_proto so that umodule_strand_refcount_dec can detect
             * the deferred-destroy when refcount hits 0.
             *
             * Task 11: UModule.destroy_requested deleted.  The sentinel is
             * the sole signal.  Self-link is unambiguous: while root_proto is
             * alive inside a UModule, next_alloc is NULL; on rescued_protos,
             * next_alloc points to the next list entry, never to itself. */
            module->root_proto->next_alloc = module->root_proto;
            /* Free the module shell (source_name + struct) — root_proto
             * survives with the self-link sentinel. */
            {
                UModuleAllocFn alloc = module_allocator(module);
                if (alloc != NULL) {
                    module_buf_free(alloc, module->alloc_ud, module->source_name);
                }
            }
            urbi_zero(module, sizeof(*module));
            return;
        }
    }
    umodule_destroy_internal(module, vm);
}

static void umodule_destroy_internal(UModule *module, struct UVM *vm) {
    if (module == NULL) return;
    (void)vm;  /* Task 11: per-nested stdlib_protos rescue path deleted */
    UModuleAllocFn alloc = module_allocator(module);
    if (alloc != NULL) {
        /* Task 11: all chunk-top data (nested[], buffers, ic_names) lives on
         * root_proto.  umodule_destroy_proto_buffers frees everything owned
         * by root_proto; then free the root_proto struct itself.
         * The per-nested rescue walk (vm->stdlib_protos) is deleted — under
         * Variant B Option (a) nested refcounts are always 0 at this point;
         * whole-root_proto rescue via vm->rescued_protos handles surviving
         * closures (see umodule_destroy). */
        if (module->root_proto != NULL) {
            umodule_destroy_proto_buffers(module->root_proto, alloc, module->alloc_ud);
            alloc(module->root_proto, 0, module->alloc_ud);
        }
        module_buf_free(alloc, module->alloc_ud, module->source_name);
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
