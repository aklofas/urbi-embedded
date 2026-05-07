/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_serialize.c — module bytecode serialization.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #8). */

#include "uemit_internal.h"
#include "value/uvarint.h"

/* Compute total serialized byte count.  Must match the write path
   in umodule_serialize byte-for-byte. */
static size_t module_wire_size(const UModule *c) {
    size_t i;
    size_t n = 24u;                                   /* fixed header */
    size_t src_len;

    /* metadata */
    n += 1u;                                          /* max_reg */
    src_len = (c->source_name != NULL) ? urbi_strlen(c->source_name) : 0u;
    n += uvarint_size_u((uint64_t)src_len);
    n += src_len;

    /* constants */
    n += uvarint_size_u((uint64_t)c->const_count);
    for (i = 0u; i < c->const_count; i++) {
        n += 1u;                                      /* kind byte */
        if (c->constants[i].kind == (uint8_t)UVAL_INT) {
            n += uvarint_size_zz(c->constants[i].v.i);
        } else if (c->constants[i].kind == (uint8_t)UVAL_FLOAT) {
            n += (URBI_FLOAT_TYPE == 8) ? 8u : 4u;
        }
        /* Other kinds: not produced by M1 emitter; serialize leaves them
           with just the kind byte (payload omitted). */
    }

    /* instructions: varint count + 0-3 alignment pad bytes + raw 4-byte words */
    n += uvarint_size_u((uint64_t)c->instr_count);
    while ((n & 3u) != 0u) n++;                       /* pad to 4-byte boundary */
    n += c->instr_count * 4u;

    /* synclines */
    n += uvarint_size_u((uint64_t)c->instr_count);     /* n_deltas */
    n += c->instr_count;                              /* one int8 per instruction */
    n += uvarint_size_u((uint64_t)c->abs_line_count);
    for (i = 0u; i < c->abs_line_count; i++) {
        n += uvarint_size_u((uint64_t)c->abs_lines[i].pc);
        n += uvarint_size_u((uint64_t)c->abs_lines[i].line);
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
    buf[5] = 0x00u;              /* flags: none defined */
    buf[6]  = 0x19u; buf[7]  = 0x93u;   /* canary bytes 0-1 */
    buf[8]  = '\r';  buf[9]  = '\n';    /* canary bytes 2-3 */
    buf[10] = 0x1Au; buf[11] = '\n';   /* canary bytes 4-5 */
    buf[12] = (uint8_t)URBI_INT_WIDTH;
    buf[13] = (uint8_t)URBI_FLOAT_TYPE;
    buf[14] = (uint8_t)URBI_INSTR_WIDTH;
    buf[15] = (uint8_t)URBI_ENDIANNESS;
    buf[16] = 0u; buf[17] = 0u; buf[18] = 0u; buf[19] = 0u;  /* reserved */
    buf[20] = 0u; buf[21] = 0u; buf[22] = 0u; buf[23] = 0u;  /* reserved */

    off = 24u;

    /* --- metadata --- */
    buf[off++] = module->max_reg;
    src_len = (module->source_name != NULL) ? urbi_strlen(module->source_name) : 0u;
    off = uvarint_write_u(buf, off, (uint64_t)src_len);
    if (src_len > 0u) {
        emit_memcpy(buf + off, module->source_name, src_len);
        off += src_len;
    }

    /* --- constants --- */
    off = uvarint_write_u(buf, off, (uint64_t)module->const_count);
    for (i = 0u; i < module->const_count; i++) {
        buf[off++] = module->constants[i].kind;
        if (module->constants[i].kind == (uint8_t)UVAL_INT) {
            off = uvarint_write_zz(buf, off, module->constants[i].v.i);
        } else if (module->constants[i].kind == (uint8_t)UVAL_FLOAT) {
            const size_t fsz = (URBI_FLOAT_TYPE == 8) ? 8u : 4u;
            emit_memcpy(buf + off, &module->constants[i].v.f, fsz);
            off += fsz;
        }
    }

    /* --- instructions: varint count + align pad + raw LE uint32s --- */
    off = uvarint_write_u(buf, off, (uint64_t)module->instr_count);
    while ((off & 3u) != 0u) buf[off++] = 0u;         /* zero alignment pad */
    for (i = 0u; i < module->instr_count; i++) {
        const uint32_t ins = module->instructions[i];
        buf[off + 0u] = (uint8_t)(ins         & 0xFFu);
        buf[off + 1u] = (uint8_t)((ins >>  8) & 0xFFu);
        buf[off + 2u] = (uint8_t)((ins >> 16) & 0xFFu);
        buf[off + 3u] = (uint8_t)((ins >> 24) & 0xFFu);
        off += 4u;
    }

    /* --- synclines: delta array then abs-line checkpoints --- */
    off = uvarint_write_u(buf, off, (uint64_t)module->instr_count);  /* n_deltas */
    if (module->instr_count > 0u) {
        emit_memcpy(buf + off, module->line_deltas, module->instr_count);
        off += module->instr_count;
    }
    off = uvarint_write_u(buf, off, (uint64_t)module->abs_line_count);
    for (i = 0u; i < module->abs_line_count; i++) {
        off = uvarint_write_u(buf, off, (uint64_t)module->abs_lines[i].pc);
        off = uvarint_write_u(buf, off, (uint64_t)module->abs_lines[i].line);
    }

    return (ptrdiff_t)off;
}
