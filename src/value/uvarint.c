/* SPDX-License-Identifier: BSD-3-Clause */
/* LEB128 varint codec.  Freestanding; see uvarint.h. */

#include "value/uvarint.h"
#include <stdint.h>

/* --- Encode --- */

size_t uvarint_size_u(uint64_t v) {
    size_t n = 1U;
    while (v >= 0x80U) { v >>= 7; n++; }
    return n;
}

size_t uvarint_size_zz(int64_t v) {
    /* Portable sign-extended mask: 0 for v >= 0, UINT64_MAX for v < 0.
       Avoids the implementation-defined behavior of (v >> 63) on signed
       negative values. */
    const uint64_t sign = (uint64_t)(-(uint64_t)(v < 0));
    const uint64_t u = ((uint64_t)v << 1) ^ sign;
    return uvarint_size_u(u);
}

size_t uvarint_write_u(uint8_t *buf, size_t off, uint64_t v) {
    while (v >= 0x80U) {
        buf[off++] = (uint8_t)((v & 0x7FU) | 0x80U);
        v >>= 7;
    }
    buf[off++] = (uint8_t)v;
    return off;
}

size_t uvarint_write_zz(uint8_t *buf, size_t off, int64_t v) {
    /* See uvarint_size_zz for the sign-mask rationale. */
    const uint64_t sign = (uint64_t)(-(uint64_t)(v < 0));
    const uint64_t u = ((uint64_t)v << 1) ^ sign;
    return uvarint_write_u(buf, off, u);
}

/* --- Decode --- */

UVarintError uvarint_decode_u(const uint8_t *buf, size_t size,
                              uint64_t *v, size_t *consumed) {
    uint64_t result = 0;
    size_t i = 0;
    unsigned shift = 0;
    for (i = 0; i < size; i++) {
        uint8_t b = buf[i];
        /* At shift == 63 (the 10th byte) only bit 0 of the 7-bit
           payload fits in uint64_t. Payload values 0x02..0x7F at that
           position would silently overflow; reject them as oversize. */
        if (shift == 63U && (b & 0x7EU) != 0U) {
            return UVARINT_OVERSIZE;
        }
        result |= (uint64_t)(b & 0x7FU) << shift;
        if ((b & 0x80U) == 0U) {
            *v = result;
            *consumed = i + 1;
            return UVARINT_OK;
        }
        shift += 7;
        if (shift > 63U) {
            return UVARINT_OVERSIZE;
        }
    }
    return UVARINT_TRUNCATED;
}

UVarintError uvarint_decode_zz(const uint8_t *buf, size_t size,
                               int64_t *v, size_t *consumed) {
    uint64_t u = 0;
    UVarintError rc = uvarint_decode_u(buf, size, &u, consumed);
    if (rc != UVARINT_OK) return rc;
    /* FOUND-015: unsigned-only zigzag decode.  The previous form
     *   (uint64_t)(-(int64_t)(u & 1U))
     * incurs implementation-defined behaviour when (u & 1) is 1: the
     * intermediate (int64_t)1 is negated to -1, then cast through uint64_t,
     * relying on two's-complement representation.  The unsigned form
     *   (0u - (u & 1u))
     * is defined for all inputs (modulo-2^64 arithmetic). */
    *v = (int64_t)((u >> 1) ^ (uint64_t)(0U - (u & 1U)));
    return UVARINT_OK;
}
