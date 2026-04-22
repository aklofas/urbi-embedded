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

#endif  /* __STDC_HOSTED__ */

/* Return the allocator to use for chunk c.  Available in both hosted and
   freestanding builds so that emit_grow (below) can call it unconditionally.
   In freestanding builds the stdlib fallback is absent; the caller must have
   supplied alloc_fn, and emit_grow will return false if it is NULL. */
static UChunkAllocFn emit_alloc_for(const Chunk *c) {
#if __STDC_HOSTED__
    return c->alloc_fn != NULL ? c->alloc_fn : emit_stdlib_alloc;
#else
    return c->alloc_fn;   /* freestanding: caller must supply */
#endif
}

#if __STDC_HOSTED__

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

/* --- Internal helpers --- */

/* Grow *data to at least new_cap elements of elem_size.  Doubling policy.
   Mirror of chunk_grow in uchunk.c; used by constant-pool and instruction
   array in the emitter. */
static bool emit_grow(Chunk *c, void **data, size_t *cap,
                      size_t new_cap, size_t elem_size) {
    if (*cap >= new_cap) return true;
    UChunkAllocFn alloc = emit_alloc_for(c);
    if (alloc == NULL) return false;
    size_t target = *cap == 0u ? 8u : *cap;
    while (target < new_cap) target *= 2u;
    void *fresh = alloc(*data, target * elem_size, c->alloc_ud);
    if (fresh == NULL) return false;
    *data  = fresh;
    *cap   = target;
    return true;
}

/* Bump the register-allocator cursor and track high-water mark.
   Returns the allocated register index.  Sets EMIT_REG_EXHAUSTED if
   all 256 slots are consumed (cursor at 255 before call). */
static uint8_t alloc_reg(Emitter *e) {
    if (e->next_reg == 255u) { e->error = EMIT_REG_EXHAUSTED; return 0u; }
    uint8_t r = e->next_reg++;
    if (r > e->max_reg_seen) e->max_reg_seen = r;
    return r;
}

/* Release the most-recently-allocated register (stack discipline). */
static void free_reg(Emitter *e) {
    if (e->next_reg > 0u) e->next_reg--;
}

/* Linear-scan dedup over the integer pool.  Returns existing index if
   a UVAL_INT entry with the same value already exists; otherwise appends
   a new entry and returns its index.  Sets e->error and returns 0 on
   pool-full (> UINT16_MAX entries) or OOM. */
static uint16_t add_const_int(Emitter *e, const int64_t v) {
    size_t i;
    for (i = 0; i < e->chunk->const_count; i++) {
        if (e->chunk->constants[i].kind == (uint8_t)UVAL_INT
         && e->chunk->constants[i].v.i == v) {
            return (uint16_t)i;
        }
    }
    if (e->chunk->const_count > (size_t)UINT16_MAX) {
        e->error = EMIT_CONSTANT_POOL_FULL;
        return 0u;
    }
    if (!emit_grow(e->chunk, (void **)&e->chunk->constants, &e->chunk->const_cap,
                   e->chunk->const_count + 1u, sizeof(UConst))) {
        e->error = EMIT_OOM;
        return 0u;
    }
    {
        const size_t idx = e->chunk->const_count;
        int p;
        e->chunk->constants[idx].kind = (uint8_t)UVAL_INT;
        /* Clear pad bytes for deterministic serialization. */
        for (p = 0; p < 7; p++) e->chunk->constants[idx]._pad[p] = 0u;
        e->chunk->constants[idx].v.i = v;
        e->chunk->const_count++;
        return (uint16_t)idx;
    }
}

/* Append one encoded instruction.  Stores line in prev_line for Task 13
   (syncline delta encoding).  No-op when e->error is already set. */
static void emit_instr(Emitter *e, const uint32_t ins, const uint32_t line) {
    if (e->error != EMIT_OK) return;
    if (!emit_grow(e->chunk, (void **)&e->chunk->instructions,
                   &e->chunk->instr_cap,
                   e->chunk->instr_count + 1u, sizeof(uint32_t))) {
        e->error = EMIT_OOM;
        return;
    }
    e->chunk->instructions[e->chunk->instr_count++] = ins;
    e->prev_line = line;
}

/* Map BinaryOp to the corresponding arithmetic opcode. */
static UOpcode binop_to_opcode(const BinaryOp op) {
    switch (op) {
    case BOP_ADD: return OP_ADD;
    case BOP_SUB: return OP_SUB;
    case BOP_MUL: return OP_MUL;
    case BOP_DIV: return OP_DIV;
    }
    /* unreachable — parser produces only these four. */
    return OP_ADD;
}

/* AST walker — returns the register holding the result of the expression.
   Returns 0 and sets e->error on any failure. */
static uint8_t emit_expr(Emitter *e, AstNode *n) {
    if (e->error != EMIT_OK) return 0u;
    switch (n->kind) {
    case AST_INT: {
        const uint8_t r = alloc_reg(e);
        if (e->error != EMIT_OK) return 0u;
        const uint16_t k = add_const_int(e, n->u.i);
        if (e->error != EMIT_OK) return 0u;
        emit_instr(e, uinstr_enc_abx(OP_LOADK, r, k), (uint32_t)n->line);
        return r;
    }
    case AST_BINARY: {
        const uint8_t lhs_reg = emit_expr(e, n->u.binary.lhs);
        if (e->error != EMIT_OK) return 0u;
        const uint8_t rhs_reg = emit_expr(e, n->u.binary.rhs);
        if (e->error != EMIT_OK) return 0u;
        emit_instr(e,
                   uinstr_enc_abc(binop_to_opcode(n->u.binary.op),
                                  lhs_reg, lhs_reg, rhs_reg),
                   (uint32_t)n->line);
        free_reg(e);              /* rhs released; lhs holds result in place */
        return lhs_reg;
    }
    case AST_UNARY: {
        /* M1: parser strips UOP_PLUS at parse time, so AST_UNARY is always
           negation.  Emit the operand into src_reg, then NEG in-place. */
        const uint8_t src_reg = emit_expr(e, n->u.unary.operand);
        if (e->error != EMIT_OK) return 0u;
        emit_instr(e, uinstr_enc_abc(OP_NEG, src_reg, src_reg, 0u),
                   (uint32_t)n->line);
        return src_reg;   /* dest reuses src; no free_reg */
    }
    case AST_IDENT:
    case AST_ERROR:
        e->error = EMIT_UNSUPPORTED_AST;
        return 0u;
    }
    e->error = EMIT_UNSUPPORTED_AST;
    return 0u;
}

/* --- Public API --- */

void uemit_init(Emitter *e, Chunk *chunk, Arena *arena, const char *source_name) {
    emit_zero(e, sizeof(*e));
    e->chunk = chunk;
    e->arena = arena;
    emit_copy_source_name(e, source_name);
}

EmitError uemit_statement(Emitter *e, AstNode *stmt) {
    uint8_t result;
    if (e->finished) return EMIT_FINISHED;
    if (e->error != EMIT_OK) return e->error;
    /* Fresh register-allocator cursor per statement at M1
       (no locals persist across statement boundaries). */
    e->next_reg = 0u;
    result = emit_expr(e, stmt);
    if (e->error != EMIT_OK) return e->error;
    e->last_result_reg = result;
    e->any_stmt_emitted = true;
    free_reg(e);                    /* release result slot (stack discipline) */
    return EMIT_OK;
}

EmitError uemit_finish(Emitter *e) {
    if (e->finished) return e->error;
    if (e->error == EMIT_OK && e->any_stmt_emitted) {
        emit_instr(e, uinstr_enc_abc(OP_RET, e->last_result_reg, 0u, 0u),
                   e->prev_line);
    }
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
