/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode emitter. */

#include "uemit.h"

#include <stddef.h>

/* Local zero-fill.  Replaces memset so uemit.c compiles without a hosted
   <string.h>.  volatile prevents GCC/Clang from recognizing the loop and
   lowering it back to a memset libcall under -Os.  Same pattern as uarena.c. */
static void emit_zero(void *const dst, const size_t n) {
    volatile unsigned char *const p = (volatile unsigned char *)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0u;
}

#if __STDC_HOSTED__
#  include <stdlib.h>
#  include <string.h>

static void *emit_stdlib_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

static UChunkAllocFn emit_alloc_for(const Chunk *c) {
    return c->alloc_fn != NULL ? c->alloc_fn : emit_stdlib_alloc;
}

/* Deep-copy source_name into the chunk using the chunk's allocator.
   Sets e->error = EMIT_OOM on allocation failure.  No-op if src is NULL. */
static void emit_copy_source_name(Emitter *e, const char *src) {
    if (src == NULL) return;
    size_t len = strlen(src);
    UChunkAllocFn alloc = emit_alloc_for(e->chunk);
    if (alloc == NULL) { e->error = EMIT_OOM; return; }
    char *copy = (char *)alloc(NULL, len + 1u, e->chunk->alloc_ud);
    if (copy == NULL) { e->error = EMIT_OOM; return; }
    memcpy(copy, src, len + 1u);
    e->chunk->source_name = copy;
}

#else  /* freestanding */

/* Freestanding builds: emit is host-side in all real uses, so source_name
   copy is skipped.  source_name remains NULL in this environment. */
static void emit_copy_source_name(Emitter *e, const char *src) {
    (void)e;
    (void)src;
}

#endif  /* __STDC_HOSTED__ */

/* --- Public API --- */

void uemit_init(Emitter *e, Chunk *chunk, Arena *arena, const char *source_name) {
    emit_zero(e, sizeof(*e));
    e->chunk = chunk;
    e->arena = arena;
    emit_copy_source_name(e, source_name);
}

EmitError uemit_statement(Emitter *e, AstNode *stmt) {
    if (e->finished) return EMIT_FINISHED;
    if (e->error != EMIT_OK) return e->error;
    (void)stmt;
    /* Task 9 fills in. */
    return EMIT_OK;
}

EmitError uemit_finish(Emitter *e) {
    if (e->finished) return e->error;
    /* Task 11 will emit OP_RET here when any_stmt_emitted is true.
       For this task: just mark finished and copy max_reg to the chunk. */
    e->finished = true;
    e->chunk->max_reg = e->max_reg_seen;
    return e->error;
}

const char *uemit_error_name(EmitError code) {
    switch (code) {
    case EMIT_OK:                 return "EMIT_OK";
    case EMIT_OOM:                return "EMIT_OOM";
    case EMIT_AST_ERROR:          return "EMIT_AST_ERROR";
    case EMIT_UNSUPPORTED_AST:    return "EMIT_UNSUPPORTED_AST";
    case EMIT_REG_EXHAUSTED:      return "EMIT_REG_EXHAUSTED";
    case EMIT_CONSTANT_POOL_FULL: return "EMIT_CONSTANT_POOL_FULL";
    case EMIT_LINE_OVERFLOW:      return "EMIT_LINE_OVERFLOW";
    case EMIT_FINISHED:           return "EMIT_FINISHED";
    }
    return "EMIT_UNKNOWN";
}

size_t uemit_disassemble(const Chunk *chunk, char *buf, size_t cap) {
    /* Task 16. */
    (void)chunk;
    if (cap > 0 && buf != NULL) buf[0] = '\0';
    return 0;
}

ptrdiff_t uchunk_serialize(const Chunk *chunk, uint8_t *buf, size_t cap) {
    /* Task 14. */
    (void)chunk;
    (void)buf;
    (void)cap;
    return 0;
}
