/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_serialize.c — module bytecode serialization.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #8). */

#include "uemit_internal.h"
#include "value/uvarint.h"
#include "module/umodule.h"
#include "runtime/umacros.h"
#include <stddef.h>
#include <stdint.h>

/* Compute serialized byte size of a single UProto starting at absolute
 * offset `start_off`.  Returns the number of bytes the proto occupies
 * from start_off onward.  The starting-offset parameter is required
 * because the 4-byte instruction-stream alignment pad depends on the
 * proto's absolute position in the stream — alignment relative to a
 * local 0 would diverge from write_proto's runtime offset. */
static size_t proto_wire_size(const UProto *p, size_t start_off) {
    size_t i;
    size_t off = start_off;

    off += 1U;                                          /* max_reg */
    off += 1U;                                          /* nupvals */
    off += 1U;                                          /* nparams */

    off += uvarint_size_u((uint64_t)p->const_count);
    for (i = 0U; i < p->const_count; i++) {
        off += 1U;
        if (p->constants[i].kind == (uint8_t)UVAL_INT) {
            off += uvarint_size_zz(p->constants[i].v.i);
        } else if (p->constants[i].kind == (uint8_t)UVAL_FLOAT) {
            off += (URBI_FLOAT_TYPE == 8) ? 8U : 4U;
        }
    }

    off += uvarint_size_u((uint64_t)p->instr_count);
    while ((off & 3U) != 0U) off++;                     /* aligns absolute offset */
    off += p->instr_count * 4U;

    off += uvarint_size_u((uint64_t)p->instr_count);    /* n_deltas == n_instr */
    off += p->instr_count;
    off += uvarint_size_u((uint64_t)p->abs_line_count);
    for (i = 0U; i < p->abs_line_count; i++) {
        off += uvarint_size_u((uint64_t)p->abs_lines[i].pc);
        off += uvarint_size_u((uint64_t)p->abs_lines[i].line);
    }

    off += uvarint_size_u((uint64_t)p->ic_count);
    for (uint16_t k = 0; k < p->ic_count; k++) {
        const char *name = (p->ic_name_strs != NULL) ? p->ic_name_strs[k] : "";
        size_t nlen = (name != NULL) ? urbi_strlen(name) : 0U;
        off += uvarint_size_u((uint64_t)nlen);
        off += nlen;
    }

    return off - start_off;
}

/* Write per-proto IC names (count + N length-prefixed UTF-8 strings). */
static size_t write_ic_names(uint8_t *buf, size_t off, uint16_t count,
                             char *const *names) {
    off = uvarint_write_u(buf, off, (uint64_t)count);
    for (uint16_t k = 0; k < count; k++) {
        const char *name = (names != NULL) ? names[k] : "";
        size_t nlen = (name != NULL) ? urbi_strlen(name) : 0U;
        off = uvarint_write_u(buf, off, (uint64_t)nlen);
        if (nlen > 0U) {
            emit_memcpy(buf + off, name, nlen);
            off += nlen;
        }
    }
    return off;
}

/* Write a single UProto's serialized form starting at buf+off. */
static size_t write_proto(uint8_t *buf, size_t off, const UProto *p) {
    size_t i;

    buf[off++] = p->max_reg;
    buf[off++] = p->nupvals;
    buf[off++] = p->nparams;

    off = uvarint_write_u(buf, off, (uint64_t)p->const_count);
    for (i = 0U; i < p->const_count; i++) {
        buf[off++] = p->constants[i].kind;
        if (p->constants[i].kind == (uint8_t)UVAL_INT) {
            off = uvarint_write_zz(buf, off, p->constants[i].v.i);
        } else if (p->constants[i].kind == (uint8_t)UVAL_FLOAT) {
            const size_t fsz = (URBI_FLOAT_TYPE == 8) ? 8U : 4U;
            emit_memcpy(buf + off, &p->constants[i].v.f, fsz);
            off += fsz;
        }
    }

    off = uvarint_write_u(buf, off, (uint64_t)p->instr_count);
    while ((off & 3U) != 0U) buf[off++] = 0U;
    for (i = 0U; i < p->instr_count; i++) {
        const uint32_t ins = p->instructions[i];
        buf[off + 0U] = (uint8_t)(ins         & 0xFFU);
        buf[off + 1U] = (uint8_t)((ins >>  8) & 0xFFU);
        buf[off + 2U] = (uint8_t)((ins >> 16) & 0xFFU);
        buf[off + 3U] = (uint8_t)((ins >> 24) & 0xFFU);
        off += 4U;
    }

    off = uvarint_write_u(buf, off, (uint64_t)p->instr_count);
    if (p->instr_count > 0U) {
        emit_memcpy(buf + off, p->line_deltas, p->instr_count);
        off += p->instr_count;
    }
    off = uvarint_write_u(buf, off, (uint64_t)p->abs_line_count);
    for (i = 0U; i < p->abs_line_count; i++) {
        off = uvarint_write_u(buf, off, (uint64_t)p->abs_lines[i].pc);
        off = uvarint_write_u(buf, off, (uint64_t)p->abs_lines[i].line);
    }

    off = write_ic_names(buf, off, p->ic_count, p->ic_name_strs);

    return off;
}

/* Compute total serialized byte count.  Must match the write path
   in umodule_serialize byte-for-byte. */
static size_t module_wire_size(const UModule *c) {
    size_t i;
    size_t n = 24U;                                   /* fixed header */
    size_t src_len;

    /* metadata */
    n += 1U;                                          /* max_reg */
    src_len = (c->source_name != NULL) ? urbi_strlen(c->source_name) : 0U;
    n += uvarint_size_u((uint64_t)src_len);
    n += src_len;

    /* constants */
    n += uvarint_size_u((uint64_t)c->const_count);
    for (i = 0U; i < c->const_count; i++) {
        n += 1U;                                      /* kind byte */
        if (c->constants[i].kind == (uint8_t)UVAL_INT) {
            n += uvarint_size_zz(c->constants[i].v.i);
        } else if (c->constants[i].kind == (uint8_t)UVAL_FLOAT) {
            n += (URBI_FLOAT_TYPE == 8) ? 8U : 4U;
        }
        /* Other kinds: not produced by M1 emitter; serialize leaves them
           with just the kind byte (payload omitted). */
    }

    /* instructions: varint count + 0-3 alignment pad bytes + raw 4-byte words */
    n += uvarint_size_u((uint64_t)c->instr_count);
    while ((n & 3U) != 0U) n++;                       /* pad to 4-byte boundary */
    n += c->instr_count * 4U;

    /* synclines */
    n += uvarint_size_u((uint64_t)c->instr_count);     /* n_deltas */
    n += c->instr_count;                              /* one int8 per instruction */
    n += uvarint_size_u((uint64_t)c->abs_line_count);
    for (i = 0U; i < c->abs_line_count; i++) {
        n += uvarint_size_u((uint64_t)c->abs_lines[i].pc);
        n += uvarint_size_u((uint64_t)c->abs_lines[i].line);
    }

    /* root-chunk IC name table */
    n += uvarint_size_u((uint64_t)c->ic_count);
    for (uint16_t k = 0; k < c->ic_count; k++) {
        const char *name = (c->ic_name_strs != NULL) ? c->ic_name_strs[k] : "";
        size_t nlen = (name != NULL) ? urbi_strlen(name) : 0U;
        n += uvarint_size_u((uint64_t)nlen);
        n += nlen;
    }

    /* nested[] protos */
    n += uvarint_size_u((uint64_t)c->nested_count);
    for (i = 0U; i < c->nested_count; i++) {
        if (c->nested[i] != NULL) {
            n += proto_wire_size(c->nested[i], n);
        } else {
            /* watcher-detached slot: serialize as max_reg=0, nupvals=0,
             * nparams=0, all counts = 0 (a "stub" proto record).  Loader
             * accepts these as no-op slots; v1.x backlog refines the
             * encoding to a single zero-byte sentinel. */
            n += 3U;            /* three zero bytes for max_reg/nupvals/nparams */
            n += 1U;            /* uvarint zero const_count */
            n += 1U;            /* uvarint zero instr_count */
            /* alignment pad: 0..3 bytes brings stream to 4-aligned; for the
             * stub proto with instr_count=0 the subsequent body has no 4-byte
             * elements so the pad collapses to whatever zeroes the running
             * `n & 3U` check requires. */
            while ((n & 3U) != 0U) n++;
            n += 1U;            /* uvarint zero n_deltas */
            n += 1U;            /* uvarint zero n_abs_lines */
            n += 1U;            /* uvarint zero ic_count */
        }
    }

    return n;
}

ptrdiff_t umodule_serialize(const UModule *module, uint8_t *buf, size_t cap) {
    size_t i;
    size_t off;
    size_t src_len;
    const size_t need = module_wire_size(module);

    /* Size query: buf == NULL means "how many bytes would you write?" */
    if (buf == NULL) return (ptrdiff_t)need;
    if (cap < need)  return -(ptrdiff_t)ULOAD_TRUNCATED;

    /* --- 24-byte header --- */
    buf[0] = 'U'; buf[1] = 'R'; buf[2] = 'B'; buf[3] = 'I';
    buf[4] = (uint8_t)URBI_BYTECODE_VERSION_BYTE;  /* version v1.4 */
    buf[5] = 0x00U;              /* flags: none defined */
    emit_memcpy(buf + 6, URBI_BYTECODE_CANARY, URBI_BYTECODE_CANARY_LEN);
    buf[12] = (uint8_t)URBI_INT_WIDTH;
    buf[13] = (uint8_t)URBI_FLOAT_TYPE;
    buf[14] = (uint8_t)URBI_INSTR_WIDTH;
    buf[15] = (uint8_t)URBI_ENDIANNESS;
    buf[16] = 0U; buf[17] = 0U; buf[18] = 0U; buf[19] = 0U;  /* reserved */
    buf[20] = 0U; buf[21] = 0U; buf[22] = 0U; buf[23] = 0U;  /* reserved */

    off = 24U;

    /* --- metadata --- */
    buf[off++] = module->max_reg;
    src_len = (module->source_name != NULL) ? urbi_strlen(module->source_name) : 0U;
    off = uvarint_write_u(buf, off, (uint64_t)src_len);
    if (src_len > 0U) {
        emit_memcpy(buf + off, module->source_name, src_len);
        off += src_len;
    }

    /* --- constants --- */
    off = uvarint_write_u(buf, off, (uint64_t)module->const_count);
    for (i = 0U; i < module->const_count; i++) {
        buf[off++] = module->constants[i].kind;
        if (module->constants[i].kind == (uint8_t)UVAL_INT) {
            off = uvarint_write_zz(buf, off, module->constants[i].v.i);
        } else if (module->constants[i].kind == (uint8_t)UVAL_FLOAT) {
            const size_t fsz = (URBI_FLOAT_TYPE == 8) ? 8U : 4U;
            emit_memcpy(buf + off, &module->constants[i].v.f, fsz);
            off += fsz;
        }
    }

    /* --- instructions: varint count + align pad + raw LE uint32s --- */
    off = uvarint_write_u(buf, off, (uint64_t)module->instr_count);
    while ((off & 3U) != 0U) buf[off++] = 0U;         /* zero alignment pad */
    for (i = 0U; i < module->instr_count; i++) {
        const uint32_t ins = module->instructions[i];
        buf[off + 0U] = (uint8_t)(ins         & 0xFFU);
        buf[off + 1U] = (uint8_t)((ins >>  8) & 0xFFU);
        buf[off + 2U] = (uint8_t)((ins >> 16) & 0xFFU);
        buf[off + 3U] = (uint8_t)((ins >> 24) & 0xFFU);
        off += 4U;
    }

    /* --- synclines: delta array then abs-line checkpoints --- */
    off = uvarint_write_u(buf, off, (uint64_t)module->instr_count);  /* n_deltas */
    if (module->instr_count > 0U) {
        emit_memcpy(buf + off, module->line_deltas, module->instr_count);
        off += module->instr_count;
    }
    off = uvarint_write_u(buf, off, (uint64_t)module->abs_line_count);
    for (i = 0U; i < module->abs_line_count; i++) {
        off = uvarint_write_u(buf, off, (uint64_t)module->abs_lines[i].pc);
        off = uvarint_write_u(buf, off, (uint64_t)module->abs_lines[i].line);
    }

    /* --- root-chunk IC name table --- */
    off = write_ic_names(buf, off, module->ic_count, module->ic_name_strs);

    /* --- nested[] protos --- */
    off = uvarint_write_u(buf, off, (uint64_t)module->nested_count);
    for (i = 0U; i < module->nested_count; i++) {
        const UProto *p = module->nested[i];
        if (p != NULL) {
            off = write_proto(buf, off, p);
        } else {
            /* stub proto for watcher-detached slots; mirrors module_wire_size. */
            buf[off++] = 0U; buf[off++] = 0U; buf[off++] = 0U;
            off = uvarint_write_u(buf, off, 0U);  /* const_count */
            off = uvarint_write_u(buf, off, 0U);  /* instr_count */
            while ((off & 3U) != 0U) buf[off++] = 0U;
            off = uvarint_write_u(buf, off, 0U);  /* n_deltas */
            off = uvarint_write_u(buf, off, 0U);  /* n_abs_lines */
            off = uvarint_write_u(buf, off, 0U);  /* ic_count */
        }
    }

    return (ptrdiff_t)off;
}
