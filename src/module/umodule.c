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

/* Resolve the effective allocator for a module. */
static UModuleAllocFn module_allocator(const UModule *c) {
#if __STDC_HOSTED__
    return c->alloc_fn != NULL ? c->alloc_fn : stdlib_alloc;
#else
    /* Freestanding: caller MUST supply alloc_fn.  NULL here is a programming
       error and module_grow will propagate it as OOM. */
    return c->alloc_fn;
#endif
}

/* Grow *data in-place to hold at least new_cap elements of elem_size.
   Doubling policy.  Returns false on allocation failure.  No-op when
   already large enough. */
static bool module_grow(UModule *c, void **data, size_t *cap,
                       size_t new_cap, size_t elem_size) {
    if (*cap >= new_cap) return true;
    UModuleAllocFn alloc = module_allocator(c);
    if (alloc == NULL) return false;
    size_t target = *cap == 0U ? 8U : *cap;
    while (target < new_cap) target *= 2U;
    void *fresh = alloc(*data, target * elem_size, c->alloc_ud);
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
    /* version byte: 0x14 = v1.4 (16*major + minor); all prior versions are
       hard-rejected.  v1.3 → v1.4 is the M5 break (reactive opcodes 39-46,
       gc_byte bit 7, 4 new AST node kinds); loading older modules silently
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
    if (!urbi_memeq(d->buf + 6, kCanary, sizeof kCanary)) {
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
    /* buf[16..23] reserved — not validated (forward-compat) */
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

static UModuleLoadError decode_constants(MDecCtx *d) {
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
        if (!module_grow(d->module, (void **)&d->module->constants, &d->module->const_cap,
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
            set_errmsg(d->errmsg, d->errcap, "constant kind %u not yet decodable at M1",
                       (unsigned)kind);
            return ULOAD_CORRUPT_TAG;
        }
        d->module->constants[d->module->const_count].kind = kind;
        if (kind == (uint8_t)UVAL_INT) {
            int64_t v = 0;
            rc = module_decode_varint_zz(d->buf + d->off, d->size - d->off, &v, &consumed);
            if (rc != ULOAD_OK) {
                set_errmsg(d->errmsg, d->errcap, "bad varint in UVAL_INT");
                return rc;
            }
            d->off += consumed;
            d->module->constants[d->module->const_count].v.i = v;
        } else if (kind == (uint8_t)UVAL_FLOAT) {
#if URBI_FLOAT_TYPE == 8
            if (d->off + 8U > d->size) {
                set_errmsg(d->errmsg, d->errcap, "truncated at UVAL_FLOAT");
                return ULOAD_TRUNCATED;
            }
            module_memcpy(&d->module->constants[d->module->const_count].v.f,
                          d->buf + d->off, 8);
            d->off += 8;
#else
            if (d->off + 4U > d->size) {
                set_errmsg(d->errmsg, d->errcap, "truncated at UVAL_FLOAT");
                return ULOAD_TRUNCATED;
            }
            module_memcpy(&d->module->constants[d->module->const_count].v.f,
                          d->buf + d->off, 4);
            d->off += 4;
#endif
        } else {
            /* UVAL_NIL / UVAL_BOOL / UVAL_STR — no payload encoder/decoder
               implemented in v0.5.5.  The emitter never produces these in
               constant pools (BOOL is OP_LOADBOOL immediate, NIL is
               OP_LOADNIL, STR is M6 stdlib).  Hand-crafted bytecode that
               smuggles them in is rejected via ULOAD_CORRUPT_TAG so the
               loader does not crash on the missing payload read. */
            set_errmsg(d->errmsg, d->errcap, "constant kind %u not decodable in v0.5.5 constant pools",
                       (unsigned)kind);
            return ULOAD_CORRUPT_TAG;
        }
        d->module->const_count++;
    }
    return ULOAD_OK;
}

static UModuleLoadError decode_instructions(MDecCtx *d) {
    uint64_t n_instr = 0;
    size_t consumed = 0;
    UModuleLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                  &n_instr, &consumed);
    if (rc != ULOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_instructions");
        return rc;
    }
    d->off += consumed;
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
        if (!module_grow(d->module, (void **)&d->module->instructions, &d->module->instr_cap,
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
        d->module->instructions[d->module->instr_count] =
              (uint32_t)d->buf[d->off + 0]
            | ((uint32_t)d->buf[d->off + 1] << 8)
            | ((uint32_t)d->buf[d->off + 2] << 16)
            | ((uint32_t)d->buf[d->off + 3] << 24);
        d->off += 4;
        d->module->instr_count++;
    }
    return ULOAD_OK;
}

static UModuleLoadError decode_line_table(MDecCtx *d) {
    uint64_t n_deltas = 0;
    size_t consumed = 0;
    UModuleLoadError rc = module_decode_varint_u(d->buf + d->off, d->size - d->off,
                                                  &n_deltas, &consumed);
    if (rc != ULOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_deltas");
        return rc;
    }
    d->off += consumed;
    if (n_deltas != (uint64_t)d->module->instr_count) {
        set_errmsg(d->errmsg, d->errcap,
                   "n_deltas=%llu does not match n_instructions=%zu",
                   (unsigned long long)n_deltas, d->module->instr_count);
        return ULOAD_CORRUPT;
    }
    if (n_deltas > 0U) {
        UModuleAllocFn alloc = module_allocator(d->module);
        if (alloc == NULL) return ULOAD_OOM;
        d->module->line_deltas = (int8_t *)alloc(NULL, (size_t)n_deltas, d->module->alloc_ud);
        if (d->module->line_deltas == NULL) return ULOAD_OOM;
        if (d->off + (size_t)n_deltas > d->size) {
            set_errmsg(d->errmsg, d->errcap, "truncated at line_deltas");
            return ULOAD_TRUNCATED;
        }
        module_memcpy(d->module->line_deltas, d->buf + d->off, (size_t)n_deltas);
        d->off += (size_t)n_deltas;
    }

    uint64_t n_abs = 0;
    rc = module_decode_varint_u(d->buf + d->off, d->size - d->off, &n_abs, &consumed);
    if (rc != ULOAD_OK) {
        set_errmsg(d->errmsg, d->errcap, "bad varint at n_abs_lines");
        return rc;
    }
    d->off += consumed;
    if (n_abs > 0U) {
        if (!module_grow(d->module, (void **)&d->module->abs_lines, &d->module->abs_line_cap,
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
        if (pc64 >= (uint64_t)d->module->instr_count) {
            set_errmsg(d->errmsg, d->errcap,
                       "abs_line pc=%llu out of range (instr_count=%zu)",
                       (unsigned long long)pc64, d->module->instr_count);
            return ULOAD_CORRUPT;
        }
        if (!first_checkpoint && (uint32_t)pc64 <= prev_pc_checkpoint) {
            set_errmsg(d->errmsg, d->errcap,
                       "abs_lines not monotonic in pc at %llu",
                       (unsigned long long)pc64);
            return ULOAD_CORRUPT;
        }
        d->module->abs_lines[d->module->abs_line_count].pc   = (uint32_t)pc64;
        d->module->abs_lines[d->module->abs_line_count].line = (uint32_t)line64;
        d->module->abs_line_count++;
        prev_pc_checkpoint = (uint32_t)pc64;
        first_checkpoint = false;
    }
    /* Trailing bytes after syncline section indicate a corrupt or mis-versioned blob. */
    if (d->off != d->size) {
        set_errmsg(d->errmsg, d->errcap,
                   "trailing %zu bytes after syncline section", d->size - d->off);
        return ULOAD_CORRUPT;
    }
    return ULOAD_OK;
}

/* Verify a single byte field per its UOperandKind. */
static UModuleLoadError verify_byte_operand(MDecCtx *d, uint8_t op,
                                            uint8_t value, UOperandKind kind,
                                            const char *which, size_t pc) {
    switch (kind) {
        case UOPK_UNUSED:
            return ULOAD_OK;
        case UOPK_REG:
            if (value > d->module->max_reg) {
                set_errmsg(d->errmsg, d->errcap,
                           "register %s=%u > max_reg=%u at pc %zu (op=%u)",
                           which, (unsigned)value,
                           (unsigned)d->module->max_reg, pc, (unsigned)op);
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
            if (reg_idx > d->module->max_reg) {
                set_errmsg(d->errmsg, d->errcap,
                           "%s tag_reg=%u > max_reg=%u at pc %zu (op=%u)",
                           which, (unsigned)reg_idx,
                           (unsigned)d->module->max_reg, pc, (unsigned)op);
                return ULOAD_CORRUPT;
            }
            return ULOAD_OK;
        }
        case UOPK_UPVAL_IDX:
            /* Runtime-checked at OP_GETUPVAL/OP_SETUPVAL dispatch (UClosure
             * carries the upvalue array length).  No static range. */
            return ULOAD_OK;
        case UOPK_NUP_PRELUDE:
            /* OP_CLOSURE B/C carry NUP+upvalue-descriptor encoding handled
             * inline at the OP_CLOSURE arm below.  Treated as UNUSED here
             * because the descriptor walk is per-instruction, not per-byte. */
            return ULOAD_OK;
        case UOPK_FRAME_REG_BASE:
            /* OP_PUSH_FRAME_GUARD A is base register; <= max_reg. */
            if (value > d->module->max_reg) {
                set_errmsg(d->errmsg, d->errcap,
                           "frame guard base=%u > max_reg=%u at pc %zu",
                           (unsigned)value, (unsigned)d->module->max_reg, pc);
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

static UModuleLoadError decode_verify(MDecCtx *d) {
    size_t vi;
    for (vi = 0; vi < d->module->instr_count; vi++) {
        uint32_t ins = d->module->instructions[vi];
        uint8_t  op  = (uint8_t)uinstr_op(ins);
        if (op >= (uint8_t)OP_MAX) {
            set_errmsg(d->errmsg, d->errcap, "corrupt opcode %u at pc %zu",
                       (unsigned)op, vi);
            return ULOAD_CORRUPT;
        }
        const UOpcodeShape *sh = &urbi_opcode_shapes[op];

        uint8_t a = uinstr_a(ins);
        UModuleLoadError rc = verify_byte_operand(d, op, a, sh->a_kind, "A", vi);
        if (rc != ULOAD_OK) return rc;

        if (sh->format == UOPF_ABC) {
            uint8_t b = uinstr_b(ins);
            uint8_t c = uinstr_c(ins);
            rc = verify_byte_operand(d, op, b, sh->b_kind, "B", vi);
            if (rc != ULOAD_OK) return rc;
            rc = verify_byte_operand(d, op, c, sh->c_kind, "C", vi);
            if (rc != ULOAD_OK) return rc;

            /* OP_PUSH_FRAME_GUARD: cross-byte invariant base+count <= max_reg+1. */
            if (op == (uint8_t)OP_PUSH_FRAME_GUARD) {
                if ((unsigned)a + (unsigned)b > (unsigned)d->module->max_reg + 1U) {
                    set_errmsg(d->errmsg, d->errcap,
                               "frame guard base+count=%u exceeds max_reg+1=%u at pc %zu",
                               (unsigned)a + (unsigned)b,
                               (unsigned)d->module->max_reg + 1U, vi);
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
                    if ((size_t)bx >= d->module->const_count) {
                        set_errmsg(d->errmsg, d->errcap,
                                   "Bx=%u >= const_count=%zu at pc %zu (op=%u)",
                                   (unsigned)bx, d->module->const_count, vi, (unsigned)op);
                        return ULOAD_CORRUPT;
                    }
                    break;
                case UBXK_NESTED_INDEX:
                    /* T5 wires this once nested[] is in scope. */
                    if ((size_t)bx >= d->module->nested_count) {
                        set_errmsg(d->errmsg, d->errcap,
                                   "Bx=%u >= nested_count=%zu at pc %zu (op=%u)",
                                   (unsigned)bx, d->module->nested_count, vi, (unsigned)op);
                        return ULOAD_CORRUPT;
                    }
                    break;
                case UBXK_JUMP_SIGNED:
                    /* No static range check; OP_JMP target out-of-range
                     * surfaces at runtime when pc + signed(Bx) - 32768
                     * leaves [0, instr_count). */
                    break;
                case UBXK_HANDLER_PC:
                    if ((size_t)bx >= d->module->instr_count) {
                        set_errmsg(d->errmsg, d->errcap,
                                   "handler-PC Bx=%u >= instr_count=%zu at pc %zu (op=%u)",
                                   (unsigned)bx, d->module->instr_count, vi, (unsigned)op);
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
    if (d->module->instr_count > 0U) {
        uint32_t last = d->module->instructions[d->module->instr_count - 1U];
        if (uinstr_op(last) != OP_RET) {
            set_errmsg(d->errmsg, d->errcap, "last instruction is not OP_RET");
            return ULOAD_CORRUPT;
        }
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
        return ULOAD_TRUNCATED;
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
    if ((rc = decode_verify(&d))       != ULOAD_OK) return rc;
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
    }
    return "ULOAD_UNKNOWN";
}
