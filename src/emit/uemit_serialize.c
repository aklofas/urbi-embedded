/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_serialize.c — the chunk writer.
 *
 * One walker, write_proto, does both jobs: with buf == NULL it only
 * advances the offset, so the size query and the write are the same code
 * and cannot drift apart.  uchunk_serialize runs it once to size the
 * chunk and once to fill the caller's buffer. */

#include "uemit_internal.h"
#include "util/uvarint.h"
#include "chunk/uchunk.h"
#include "util/umacros.h"
#include <stddef.h>
#include <stdint.h>

/* Every store goes through these, so a NULL buffer counts and writes
   nothing. */
static inline void put(uint8_t *buf, size_t off, uint8_t v) {
    if (buf) buf[off] = v;
}

static inline size_t put_bytes(uint8_t *buf, size_t off, const void *src, size_t n) {
    if (buf && n > 0U) emit_memcpy(buf + off, src, n);
    return off + n;
}

static inline size_t put_varint_u(uint8_t *buf, size_t off, uint64_t v) {
    return buf ? uvarint_write_u(buf, off, v) : off + uvarint_size_u(v);
}

static inline size_t put_varint_zz(uint8_t *buf, size_t off, int64_t v) {
    return buf ? uvarint_write_zz(buf, off, v) : off + uvarint_size_zz(v);
}

/* The 4-byte instruction-stream alignment depends on the absolute offset,
   which is why the walker carries `off` rather than a proto-local size. */
static inline size_t put_align4(uint8_t *buf, size_t off) {
    while ((off & 3U) != 0U) put(buf, off++, 0U);
    return off;
}

/* Per-proto site names: count + N length-prefixed UTF-8 strings. */
static size_t write_site_names(uint8_t *buf, size_t off, uint16_t count,
                               char *const *names) {
    off = put_varint_u(buf, off, (uint64_t)count);
    for (uint16_t k = 0; k < count; k++) {
        const char *name = (names != NULL) ? names[k] : "";
        size_t nlen = (name != NULL) ? urbi_strlen(name) : 0U;
        off = put_varint_u(buf, off, (uint64_t)nlen);
        off = put_bytes(buf, off, name, nlen);
    }
    return off;
}

/* Write a single UProto's serialized form (recursively, nested protos
   last) starting at buf+off; returns the offset past it. */
static size_t write_proto(uint8_t *buf, size_t off, const UProto *p) {
    size_t i;

    put(buf, off++, p->max_reg);
    put(buf, off++, p->nupvals);
    put(buf, off++, p->nparams);

    off = put_varint_u(buf, off, (uint64_t)p->const_count);
    for (i = 0U; i < p->const_count; i++) {
        put(buf, off++, p->constants[i].kind);
        if (p->constants[i].kind == (uint8_t)UVAL_INT) {
            off = put_varint_zz(buf, off, p->constants[i].v.i);
        } else if (p->constants[i].kind == (uint8_t)UVAL_FLOAT) {
            off = put_bytes(buf, off, &p->constants[i].v.f, 8U);  /* always an 8-byte double */
        } else if (p->constants[i].kind == (uint8_t)UVAL_STR) {
            const char *s = (const char *)p->constants[i].v.p;
            const size_t n = (s != NULL) ? urbi_strlen(s) : 0U;
            off = put_varint_u(buf, off, (uint64_t)n);
            off = put_bytes(buf, off, s, n);
        }
    }

    off = put_varint_u(buf, off, (uint64_t)p->instr_count);
    off = put_align4(buf, off);
    for (i = 0U; i < p->instr_count; i++) {
        const uint32_t ins = p->instructions[i];
        put(buf, off + 0U, (uint8_t)(ins         & 0xFFU));
        put(buf, off + 1U, (uint8_t)((ins >>  8) & 0xFFU));
        put(buf, off + 2U, (uint8_t)((ins >> 16) & 0xFFU));
        put(buf, off + 3U, (uint8_t)((ins >> 24) & 0xFFU));
        off += 4U;
    }

    off = put_varint_u(buf, off, (uint64_t)p->instr_count);   /* n_deltas == n_instr */
    off = put_bytes(buf, off, p->line_deltas, p->instr_count);
    off = put_varint_u(buf, off, (uint64_t)p->abs_line_count);
    for (i = 0U; i < p->abs_line_count; i++) {
        off = put_varint_u(buf, off, (uint64_t)p->abs_lines[i].pc);
        off = put_varint_u(buf, off, (uint64_t)p->abs_lines[i].line);
    }

    off = write_site_names(buf, off, p->site_count, p->site_name_strs);

    off = put_varint_u(buf, off, (uint64_t)p->nested_count);
    for (size_t ni = 0U; ni < p->nested_count; ni++) {
        const UProto *child = p->nested[ni];
        if (child != NULL) {
            off = write_proto(buf, off, child);
        } else {
            /* An empty slot (the loader tolerates one) goes out as a stub
             * proto: max_reg/nupvals/nparams 0 and every count 0. */
            put(buf, off++, 0U); put(buf, off++, 0U); put(buf, off++, 0U);
            off = put_varint_u(buf, off, 0U);  /* const_count */
            off = put_varint_u(buf, off, 0U);  /* instr_count */
            off = put_align4(buf, off);
            off = put_varint_u(buf, off, 0U);  /* n_deltas */
            off = put_varint_u(buf, off, 0U);  /* n_abs_lines */
            off = put_varint_u(buf, off, 0U);  /* site_count */
            off = put_varint_u(buf, off, 0U);  /* nested_count */
        }
    }

    return off;
}

/* The whole chunk: 24-byte header, source name, root proto block. */
static size_t write_chunk(uint8_t *buf, const UProto *root) {
    size_t off = 0U;
    put(buf, off++, 'U'); put(buf, off++, 'R'); put(buf, off++, 'B'); put(buf, off++, 'I');
    put(buf, off++, (uint8_t)URBI_BYTECODE_VERSION_BYTE);
    put(buf, off++, (root->arity_prologue != 0U) ? 0x01U : 0x00U);
    off = put_bytes(buf, off, URBI_BYTECODE_CANARY, URBI_BYTECODE_CANARY_LEN);
    put(buf, off++, (uint8_t)URBI_INT_WIDTH);
    put(buf, off++, 8U);   /* float flavor is fixed at f64/double */
    put(buf, off++, (uint8_t)URBI_INSTR_WIDTH);
    put(buf, off++, (uint8_t)URBI_ENDIANNESS);
    while (off < 24U) put(buf, off++, 0U);   /* reserved */

    const size_t src_len = (root->source_name != NULL) ? urbi_strlen(root->source_name) : 0U;
    off = put_varint_u(buf, off, (uint64_t)src_len);
    off = put_bytes(buf, off, root->source_name, src_len);

    return write_proto(buf, off, root);
}

ptrdiff_t uchunk_serialize(const UProto *root, uint8_t *buf, size_t cap) {
    const size_t need = write_chunk(NULL, root);

    /* Size query: buf == NULL means "how many bytes would you write?" */
    if (buf == NULL) return (ptrdiff_t)need;
    if (cap < need)  return -(ptrdiff_t)UCHUNK_LOAD_TRUNCATED;
    return (ptrdiff_t)write_chunk(buf, root);
}
