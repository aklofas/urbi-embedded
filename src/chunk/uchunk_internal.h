/* SPDX-License-Identifier: BSD-3-Clause */
/* uchunk_internal.h — chunk-domain-private decode/verify contract.
 *
 * Shared between the deserializer (uchunk_io.c) and the bytecode verifier
 * (uchunk_verify.c): the per-section decode context and the two verifier
 * entry points the deserializer drives.  Not for embedder use — the public
 * chunk surface is uchunk_deserialize / uchunk_destroy in chunk/uchunk.h. */

#ifndef UCHUNK_INTERNAL_H
#define UCHUNK_INTERNAL_H

#include "chunk/uchunk.h"   /* UChunkLoadError, UProto */

#include <stddef.h>
#include <stdint.h>

/* --- Per-section decoder context (chunk-domain-private) --- */

typedef struct {
    UProto         *root_proto;  /* root UProto; v0.9.2: UModule deleted, root IS the decode target */
    UProto         *rp;          /* same as root_proto (root); kept for decode_verify compatibility */
    const uint8_t  *buf;
    size_t          size;
    size_t          off;
    char           *errmsg;
    size_t          errcap;
    int             depth;       /* current nesting depth inside decode_proto recursion */
    uint8_t         arity_flag;  /* v0.13.5: header flag byte bit 0 — arity
                                    self-check discipline; propagated to
                                    UProto.arity_prologue on every decoded
                                    proto (see uproto.h field comment) */
} MDecCtx;

/* --- Verifier entry points (uchunk_verify.c), driven by uchunk_deserialize ---
 *
 * urbi_chunk_decode_verify — Pass 1: opcode-shape table, per-block register
 *   bounds, ic_count cross-check (renamed from the file-static decode_verify).
 * urbi_chunk_verify_bounds  — Pass 2: per-instruction sequence bounds
 *   (renamed from the file-static verify_chunk_bounds).
 * Pass 3 (ic_index DFS mirror) is the already-public uchunk_verify_ic_index
 * in chunk/uchunk.h. */
UChunkLoadError urbi_chunk_decode_verify(MDecCtx *d);
UChunkLoadError urbi_chunk_verify_bounds(MDecCtx *d);

#endif /* UCHUNK_INTERNAL_H */
