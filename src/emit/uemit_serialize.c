/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_serialize.c — module bytecode serialization.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #8). */

#include "uemit_internal.h"
#include "value/uvarint.h"
#include "chunk/uchunk.h"
#include "runtime/umacros.h"
#include <stddef.h>
#include <stdint.h>

/* Compute serialized byte size of a single UProto starting at absolute
 * offset `start_off`.  Returns the number of bytes the proto occupies
 * from start_off onward.  The starting-offset parameter is required
 * because the 4-byte instruction-stream alignment pad depends on the
 * proto's absolute position in the stream — alignment relative to a
 * local 0 would diverge from write_proto's runtime offset.
 *
 * v1.7: includes nested_count varint + recursive nested[] children at end. */
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
        } else if (p->constants[i].kind == (uint8_t)UVAL_STR) {
            const char *s = (const char *)p->constants[i].v.p;
            const size_t n = (s != NULL) ? urbi_strlen(s) : 0U;
            off += uvarint_size_u((uint64_t)n);
            off += n;
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

    /* v1.7: nested_count varint + recursive nested[] children.
     * For the v0.8.1 flat-on-root emitter, only root_proto.nested[] is
     * populated; non-root UProtos write nested_count = 0 (one varint byte). */
    off += uvarint_size_u((uint64_t)p->nested_count);
    for (i = 0U; i < p->nested_count; i++) {
        if (p->nested[i] != NULL) {
            off += proto_wire_size(p->nested[i], off);
        } else {
            /* watcher-detached slot: stub proto (max_reg=0, nupvals=0,
             * nparams=0, all counts = 0, nested_count = 0). */
            off += 3U;            /* three zero bytes for max_reg/nupvals/nparams */
            off += 1U;            /* uvarint zero const_count */
            off += 1U;            /* uvarint zero instr_count */
            while ((off & 3U) != 0U) off++;
            off += 1U;            /* uvarint zero n_deltas */
            off += 1U;            /* uvarint zero n_abs_lines */
            off += 1U;            /* uvarint zero ic_count */
            off += 1U;            /* uvarint zero nested_count (v1.7) */
        }
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
        } else if (p->constants[i].kind == (uint8_t)UVAL_STR) {
            const char *s = (const char *)p->constants[i].v.p;
            const size_t n = (s != NULL) ? urbi_strlen(s) : 0U;
            off = uvarint_write_u(buf, off, (uint64_t)n);
            if (n > 0U) {
                emit_memcpy(buf + off, s, n);
                off += n;
            }
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

    /* v1.7: nested_count varint + recursive nested[] children.
     * For the v0.8.1 flat-on-root emitter, only root_proto.nested[] is
     * populated; non-root UProtos write nested_count = 0 (one varint byte). */
    off = uvarint_write_u(buf, off, (uint64_t)p->nested_count);
    for (size_t ni = 0U; ni < p->nested_count; ni++) {
        const UProto *child = p->nested[ni];
        if (child != NULL) {
            off = write_proto(buf, off, child);
        } else {
            /* stub proto for watcher-detached slots; mirrors proto_wire_size. */
            buf[off++] = 0U; buf[off++] = 0U; buf[off++] = 0U;
            off = uvarint_write_u(buf, off, 0U);  /* const_count */
            off = uvarint_write_u(buf, off, 0U);  /* instr_count */
            while ((off & 3U) != 0U) buf[off++] = 0U;
            off = uvarint_write_u(buf, off, 0U);  /* n_deltas */
            off = uvarint_write_u(buf, off, 0U);  /* n_abs_lines */
            off = uvarint_write_u(buf, off, 0U);  /* ic_count */
            off = uvarint_write_u(buf, off, 0U);  /* nested_count (v1.7) */
        }
    }

    return off;
}

/* Compute total serialized byte count.  Must match the write path
   in uchunk_serialize byte-for-byte.
   v1.7: UModule body = header + source_name + root_proto block. */
static size_t module_wire_size(const UModule *c) {
    size_t n = 24U;                                   /* fixed header */
    size_t src_len;

    src_len = (c->source_name != NULL) ? urbi_strlen(c->source_name) : 0U;
    n += uvarint_size_u((uint64_t)src_len);
    n += src_len;

    if (c->root_proto == NULL) return n;  /* empty module (pre-finish) */

    /* root_proto block: recursive UProto serialization.
     * proto_wire_size includes nested_count + recursive nested[]. */
    n += proto_wire_size(c->root_proto, n);

    return n;
}

/* v1.7: UModule body = header + source_name + root_proto block. */
ptrdiff_t uchunk_serialize(const UModule *module, uint8_t *buf, size_t cap) {
    size_t off;
    size_t src_len;
    const size_t need = module_wire_size(module);

    /* Size query: buf == NULL means "how many bytes would you write?" */
    if (buf == NULL) return (ptrdiff_t)need;
    if (cap < need)  return -(ptrdiff_t)UCHUNK_LOAD_TRUNCATED;

    /* --- 24-byte header --- */
    buf[0] = 'U'; buf[1] = 'R'; buf[2] = 'B'; buf[3] = 'I';
    buf[4] = (uint8_t)URBI_BYTECODE_VERSION_BYTE;  /* version v1.7 */
    buf[5] = 0x00U;              /* flags: none defined */
    emit_memcpy(buf + 6, URBI_BYTECODE_CANARY, URBI_BYTECODE_CANARY_LEN);
    buf[12] = (uint8_t)URBI_INT_WIDTH;
    buf[13] = (uint8_t)URBI_FLOAT_TYPE;
    buf[14] = (uint8_t)URBI_INSTR_WIDTH;
    buf[15] = (uint8_t)URBI_ENDIANNESS;
    buf[16] = 0U; buf[17] = 0U; buf[18] = 0U; buf[19] = 0U;  /* reserved */
    buf[20] = 0U; buf[21] = 0U; buf[22] = 0U; buf[23] = 0U;  /* reserved */

    off = 24U;

    /* --- source_name --- */
    src_len = (module->source_name != NULL) ? urbi_strlen(module->source_name) : 0U;
    off = uvarint_write_u(buf, off, (uint64_t)src_len);
    if (src_len > 0U) {
        emit_memcpy(buf + off, module->source_name, src_len);
        off += src_len;
    }

    if (module->root_proto == NULL) return (ptrdiff_t)off;  /* empty module */

    /* --- root_proto block (recursive UProto serialization) --- */
    off = write_proto(buf, off, module->root_proto);

    return (ptrdiff_t)off;
}
