/* SPDX-License-Identifier: BSD-3-Clause */
/* Internal helpers surfaced for unit testing.  NOT a public interface. */

#ifndef UCHUNK_INTERNAL_H
#define UCHUNK_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "uchunk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Varint decode (LEB128-style).  Returns ULOAD_TRUNCATED if buf is too
   short, ULOAD_CORRUPT_VARINT if more than 10 continuation bytes (exceeds
   uint64 capacity).  On success, writes decoded value to *v and bytes-read
   count to *consumed. */
UChunkLoadError varint_decode_u(const uint8_t *buf, size_t size,
                                uint64_t *v, size_t *consumed);

/* Zigzag-signed variant: decodes via varint_decode_u, then decodes zigzag.
   zigzag: (n >> 1) ^ -(n & 1). */
UChunkLoadError varint_decode_zz(const uint8_t *buf, size_t size,
                                 int64_t *v, size_t *consumed);

#ifdef __cplusplus
}
#endif

#endif
