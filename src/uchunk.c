/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode Chunk deserializer + verifier + destroy.  Freestanding. */

#include "uchunk.h"

/* Public-API stubs — filled in by later tasks. */

UChunkLoadError uchunk_deserialize(Chunk *chunk, const uint8_t *buf, size_t size,
                                   char *errmsg, size_t errcap) {
    (void)chunk;
    (void)buf;
    (void)size;
    (void)errmsg;
    (void)errcap;
    return ULOAD_OK;
}

void uchunk_destroy(Chunk *chunk) {
    (void)chunk;
}

const char *uchunk_load_error_name(UChunkLoadError code) {
    (void)code;
    return "ULOAD_UNKNOWN";
}
