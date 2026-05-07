/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_serialize.c — module bytecode serialization.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #8). */

#include "uemit_internal.h"
#include "value/uvarint.h"
#include "module/umodule.h"
#include "runtime/umacros.h"
#include <stddef.h>
#include <stdint.h>

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
    buf[6]  = 0x19U; buf[7]  = 0x93U;   /* canary bytes 0-1 */
    buf[8]  = '\r';  buf[9]  = '\n';    /* canary bytes 2-3 */
    buf[10] = 0x1AU; buf[11] = '\n';   /* canary bytes 4-5 */
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

    return (ptrdiff_t)off;
}
