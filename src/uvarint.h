/* SPDX-License-Identifier: BSD-3-Clause */
/* LEB128 varint encode/decode (unsigned + zigzag-signed).  Freestanding.
   Used by umodule (deserialize) and uemit (serialize).  No allocator, no
   stdlib dependencies — only byte math over caller-supplied buffers. */

#ifndef URBI_UVARINT_H
#define URBI_UVARINT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode outcome. Translate to a caller-specific error domain at the
   boundary (see umodule.c for the UModuleLoadError mapping). */
typedef enum {
    UVARINT_OK = 0,
    UVARINT_TRUNCATED,       /* buffer ended before the varint completed */
    UVARINT_OVERSIZE         /* > 10 continuation bytes (exceeds uint64)  */
} UVarintError;

/* --- Encode side --- */

/* Number of bytes an unsigned varint of `v` would occupy.  Always >= 1. */
size_t uvarint_size_u(uint64_t v);

/* Number of bytes a zigzag-encoded signed varint of `v` would occupy. */
size_t uvarint_size_zz(int64_t v);

/* Write unsigned varint `v` into `buf` starting at `off`.  Caller guarantees
   `buf` has at least `uvarint_size_u(v)` bytes of room starting at `off`.
   Returns the new offset (= old off + bytes written). */
size_t uvarint_write_u(uint8_t *buf, size_t off, uint64_t v);

/* Zigzag-encode `v` then write as unsigned varint.  Caller guarantees room. */
size_t uvarint_write_zz(uint8_t *buf, size_t off, int64_t v);

/* --- Decode side --- */

/* Decode an unsigned varint from `buf[0..size)`.  On success writes the value
   to `*v` and the byte count consumed to `*consumed`.  On failure `*v` and
   `*consumed` are unspecified. */
UVarintError uvarint_decode_u(const uint8_t *buf, size_t size,
                              uint64_t *v, size_t *consumed);

/* Decode a zigzag-encoded signed varint.  Decoding is
   uvarint_decode_u followed by `(u >> 1) ^ -(u & 1)`. */
UVarintError uvarint_decode_zz(const uint8_t *buf, size_t size,
                               int64_t *v, size_t *consumed);

#ifdef __cplusplus
}
#endif

#endif  /* URBI_UVARINT_H */
