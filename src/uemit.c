/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode emitter. */

#include "uemit.h"
#include "uemit_internal.h"
#include "uintern.h"
#include "uvarint.h"
#include "ucleanup.h"   /* FLAG_HAS_CATCH, FLAG_HAS_FINALLY — AST_TRY emit */

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>

/* Local zero-fill.  Replaces memset so uemit.c compiles without a hosted
   <string.h>.  volatile prevents GCC/Clang from recognizing the loop and
   lowering it back to a memset libcall under -Os.  Same pattern as uarena.c. */
static void emit_zero(void *const dst, const size_t n) {
    volatile unsigned char *const p = (volatile unsigned char *)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0u;
}

/* Local byte-copy.  Replaces memcpy so the serializer compiles without
   a hosted <string.h>.  Same pattern as module_memcpy in umodule.c. */
static void emit_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *pd = (unsigned char *)dst;
    const unsigned char *ps = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) pd[i] = ps[i];
}

/* Local strlen replacement (byte-loop).  Freestanding-safe. */
static size_t emit_strlen(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

#if __STDC_HOSTED__
#  include <stdlib.h>

static void *emit_stdlib_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

#endif  /* __STDC_HOSTED__ */

/* Return the allocator to use for module c.  Available in both hosted and
   freestanding builds so that emit_grow (below) can call it unconditionally.
   In freestanding builds the stdlib fallback is absent; the caller must have
   supplied alloc_fn, and emit_grow will return false if it is NULL. */
static UModuleAllocFn emit_alloc_for(const UModule *c) {
#if __STDC_HOSTED__
    return c->alloc_fn != NULL ? c->alloc_fn : emit_stdlib_alloc;
#else
    return c->alloc_fn;   /* freestanding: caller must supply */
#endif
}

/* Resolve which proto to write instructions/constants/synclines into.
 * When the current FuncState has a non-NULL target_proto, we are inside a
 * nested function body — write to the child proto.  Otherwise write to the
 * module root (the classic M1 path). */
static UProto *current_proto(const UEmitter *e) {
    if (e->current_fs != NULL && e->current_fs->target_proto != NULL) {
        return (UProto *)e->current_fs->target_proto;
    }
    return NULL;  /* NULL means: write to module root (legacy path) */
}

#if __STDC_HOSTED__

/* Deep-copy source_name into the module using the module's allocator.
   Sets e->error = EMIT_OOM on allocation failure.  No-op if src is NULL.
   Under __STDC_HOSTED__ emit_alloc_for always returns non-NULL (falls
   back to the stdlib wrapper), so no NULL guard on `alloc` is needed. */
static void emit_copy_source_name(UEmitter *e, const char *src) {
    if (src == NULL) return;
    size_t len = emit_strlen(src);
    UModuleAllocFn alloc = emit_alloc_for(e->module);
    char *copy = (char *)alloc(NULL, len + 1u, e->module->alloc_ud);
    if (copy == NULL) { e->error = EMIT_OOM; return; }
    emit_memcpy(copy, src, len + 1u);
    e->module->source_name = copy;
}

#else  /* freestanding */

/* Freestanding builds: emit is host-side in all real uses, so source_name
   copy is skipped.  source_name remains NULL in this environment. */
static void emit_copy_source_name(UEmitter *e, const char *src) {
    (void)e;
    (void)src;
}

#endif  /* __STDC_HOSTED__ */

/* --- Internal helpers --- */

/* Grow *data to at least new_cap elements of elem_size.  Doubling policy.
   Mirror of module_grow in umodule.c; used by constant-pool and instruction
   array in the emitter. */
static bool emit_grow(UModule *c, void **data, size_t *cap,
                      size_t new_cap, size_t elem_size) {
    if (*cap >= new_cap) return true;
    UModuleAllocFn alloc = emit_alloc_for(c);
    if (alloc == NULL) return false;
    size_t target = *cap == 0u ? 8u : *cap;
    while (target < new_cap) target *= 2u;
    void *fresh = alloc(*data, target * elem_size, c->alloc_ud);
    if (fresh == NULL) return false;
    *data  = fresh;
    *cap   = target;
    return true;
}

/* Grow a buffer owned by either the module root or a nested UProto.
 * When `proto` is NULL, delegates to emit_grow (module root path). */
static bool proto_grow(UModule *module, UProto *proto,
                       void **data, size_t *cap,
                       size_t new_cap, size_t elem_size) {
    if (proto != NULL) {
        UModuleAllocFn alloc = proto->alloc_fn;
        if (alloc == NULL) {
#if __STDC_HOSTED__
            alloc = emit_stdlib_alloc;
#else
            return false;
#endif
        }
        if (*cap >= new_cap) return true;
        size_t target = *cap == 0 ? 8 : *cap;
        while (target < new_cap) target *= 2;
        void *fresh = alloc(*data, target * elem_size, proto->alloc_ud);
        if (fresh == NULL) return false;
        *data = fresh;
        *cap  = target;
        return true;
    }
    return emit_grow(module, data, cap, new_cap, elem_size);
}

/* Bump the register-allocator cursor and track high-water mark.
   Returns the allocated register index.  Sets EMIT_REG_EXHAUSTED if
   all 256 slots are consumed (cursor at 255 before call). */
static uint8_t alloc_reg(UEmitter *e) {
    if (e->next_reg == 255u) { e->error = EMIT_REG_EXHAUSTED; return 0u; }
    uint8_t r = e->next_reg++;
    if (r > e->max_reg_seen) e->max_reg_seen = r;
    return r;
}

/* Release the most-recently-allocated register (stack discipline). */
static void free_reg(UEmitter *e) {
    if (e->next_reg > 0u) e->next_reg--;
}

/* Linear-scan dedup over the integer pool.  Returns existing index if
   a UVAL_INT entry with the same value already exists; otherwise appends
   a new entry and returns its index.  Sets e->error and returns 0 on
   pool-full (> UINT16_MAX entries) or OOM.
   Routes to the nested UProto constant pool when in a nested function. */
static uint16_t add_const_int(UEmitter *e, const int64_t v) {
    UProto *p = current_proto(e);
    UValue **pool;
    size_t  *count;
    size_t  *cap;

    if (p != NULL) {
        pool  = &p->constants;
        count = &p->const_count;
        cap   = &p->const_cap;
    } else {
        pool  = &e->module->constants;
        count = &e->module->const_count;
        cap   = &e->module->const_cap;
    }

    size_t i;
    for (i = 0; i < *count; i++) {
        if ((*pool)[i].kind == (uint8_t)UVAL_INT && (*pool)[i].v.i == v) {
            return (uint16_t)i;
        }
    }
    if (*count > (size_t)UINT16_MAX) {
        e->error = EMIT_CONSTANT_POOL_FULL;
        return 0u;
    }
    if (!proto_grow(e->module, p, (void **)pool, cap, *count + 1u, sizeof(UValue))) {
        e->error = EMIT_OOM;
        return 0u;
    }
    {
        const size_t idx = *count;
        int pad;
        (*pool)[idx].kind = (uint8_t)UVAL_INT;
        for (pad = 0; pad < 7; pad++) (*pool)[idx]._pad[pad] = 0u;
        (*pool)[idx].v.i = v;
        (*count)++;
        return (uint16_t)idx;
    }
}

/* Append one absolute-line checkpoint to abs_lines.  Uses emit_grow or
   proto_grow depending on whether we are in a nested function body. */
static void emit_push_abs_line(UEmitter *e, const uint32_t pc, const uint32_t line) {
    UProto *p = current_proto(e);
    if (p != NULL) {
        if (!proto_grow(e->module, p, (void **)&p->abs_lines, &p->abs_line_cap,
                        p->abs_line_count + 1u, sizeof(UAbsLine))) {
            e->error = EMIT_OOM;
            return;
        }
        p->abs_lines[p->abs_line_count].pc   = pc;
        p->abs_lines[p->abs_line_count].line = line;
        p->abs_line_count++;
        return;
    }
    if (!emit_grow(e->module, (void **)&e->module->abs_lines, &e->module->abs_line_cap,
                   e->module->abs_line_count + 1u, sizeof(UAbsLine))) {
        e->error = EMIT_OOM;
        return;
    }
    e->module->abs_lines[e->module->abs_line_count].pc   = pc;
    e->module->abs_lines[e->module->abs_line_count].line = line;
    e->module->abs_line_count++;
}

/* Append one delta byte to line_deltas.  line_deltas has no cap field —
   it is sized exactly to instr_count.  Called after instr_count has been
   incremented so the new slot is at [instr_count - 1].
   When writing to a nested proto, use the proto's allocator. */
static void emit_push_line_delta(UEmitter *e, const int8_t delta) {
    UProto *p = current_proto(e);
    if (p != NULL) {
        /* Nested proto path. */
        UModuleAllocFn alloc = p->alloc_fn;
        if (alloc == NULL) {
#if __STDC_HOSTED__
            alloc = emit_stdlib_alloc;
#else
            e->error = EMIT_OOM; return;
#endif
        }
        void *fresh = alloc(p->line_deltas,
                            p->instr_count * sizeof(int8_t),
                            p->alloc_ud);
        if (fresh == NULL) { e->error = EMIT_OOM; return; }
        p->line_deltas = (int8_t *)fresh;
        p->line_deltas[p->instr_count - 1u] = delta;
        return;
    }
    UModuleAllocFn alloc = emit_alloc_for(e->module);
    if (alloc == NULL) { e->error = EMIT_OOM; return; }
    void *fresh = alloc(e->module->line_deltas,
                        e->module->instr_count * sizeof(int8_t),
                        e->module->alloc_ud);
    if (fresh == NULL) { e->error = EMIT_OOM; return; }
    e->module->line_deltas = (int8_t *)fresh;
    e->module->line_deltas[e->module->instr_count - 1u] = delta;
}

/* Append one encoded instruction with Lua-5.5-style delta syncline encoding.
   No-op when e->error is already set.
   Routes to the nested UProto when current_proto(e) is non-NULL. */
static void emit_instr(UEmitter *e, const uint32_t ins, const uint32_t line) {
    uint32_t pc;
    int8_t delta;
    bool needs_abs;

    if (e->error != EMIT_OK) return;
    if (line > (uint32_t)INT32_MAX) { e->error = EMIT_LINE_OVERFLOW; return; }

    UProto *p = current_proto(e);
    if (p != NULL) {
        /* Nested proto path: write instruction into the child proto. */
        if (!proto_grow(e->module, p, (void **)&p->instructions,
                        &p->instr_cap, p->instr_count + 1u, sizeof(uint32_t))) {
            e->error = EMIT_OOM;
            return;
        }
        p->instructions[p->instr_count++] = ins;

        pc = (uint32_t)(p->instr_count - 1u);
        delta = 0;
        needs_abs = false;
        if (e->prev_line == 0u) {
            needs_abs = true;
        } else {
            const int64_t d = (int64_t)line - (int64_t)e->prev_line;
            if (d <= (int64_t)INT8_MIN || d > (int64_t)INT8_MAX) {
                needs_abs = true;
            } else {
                delta = (int8_t)d;
            }
        }
        if (needs_abs) {
            delta = (int8_t)-128;
            emit_push_abs_line(e, pc, line);
            if (e->error != EMIT_OK) return;
        }
        emit_push_line_delta(e, delta);
        if (e->error != EMIT_OK) return;
        e->prev_line = line;
        return;
    }

    /* Root module path (existing behavior). */
    if (!emit_grow(e->module, (void **)&e->module->instructions,
                   &e->module->instr_cap,
                   e->module->instr_count + 1u, sizeof(uint32_t))) {
        e->error = EMIT_OOM;
        return;
    }
    e->module->instructions[e->module->instr_count++] = ins;

    /* Delta encoding.  INT8_MIN (-128) is the sentinel; valid range [-127,+127]. */
    pc = (uint32_t)(e->module->instr_count - 1u);
    delta = 0;
    needs_abs = false;
    if (e->prev_line == 0u) {
        /* First instruction ever: bootstrap abs checkpoint regardless of line value. */
        needs_abs = true;
    } else {
        const int64_t d = (int64_t)line - (int64_t)e->prev_line;
        if (d <= (int64_t)INT8_MIN || d > (int64_t)INT8_MAX) {
            needs_abs = true;
        } else {
            delta = (int8_t)d;
        }
    }
    if (needs_abs) {
        delta = (int8_t)-128;
        emit_push_abs_line(e, pc, line);
        if (e->error != EMIT_OK) return;
    }
    emit_push_line_delta(e, delta);
    if (e->error != EMIT_OK) return;
    e->prev_line = line;
}

/* Patch instruction at index `pc` in the current proto (root or nested).
 * Used by JMP back-patching in if/while/function emit. */
static void emit_patch_instr(UEmitter *e, int pc, uint32_t new_instr) {
    UProto *p = current_proto(e);
    if (p != NULL) {
        p->instructions[pc] = new_instr;
    } else {
        e->module->instructions[pc] = new_instr;
    }
}

/* Return the current instruction count in the active proto. */
static size_t emit_instr_count(const UEmitter *e) {
    UProto *p = current_proto(e);
    if (p != NULL) return p->instr_count;
    return e->module->instr_count;
}

/* Map UAstBinaryOp to the corresponding arithmetic opcode. */
static UOpcode binop_to_opcode(const UAstBinaryOp op) {
    switch (op) {
    case BOP_ADD: return OP_ADD;
    case BOP_SUB: return OP_SUB;
    case BOP_MUL: return OP_MUL;
    case BOP_DIV: return OP_DIV;
    }
    /* unreachable — parser produces only these four. */
    return OP_ADD;
}

/* Forward declaration (emit_lazy_thunk calls emit_expr). */
static uint8_t emit_expr(UEmitter *e, UAstNode *n);

/* T16: Compile `expr` as a zero-arg closure (lazy thunk).
 * Builds a synthetic AST_FUNCTION wrapping `expr` in a single-statement
 * body, then recurses into the AST_FUNCTION emit arm.  The result register
 * holds a UVAL_CLOSURE.  Upvalue capture falls out naturally from the
 * recursive AST_FUNCTION emit (the upvalue cascade walks parent FuncStates
 * to find any names `expr` references).
 *
 * Pass-through optimization: if `expr` is AST_IDENT that resolves to a
 * lazy local in the current scope, skip re-wrapping and emit a plain
 * OP_MOVE of that slot.  This keeps thunk identity stable across
 * pass-through layers (avoiding thunk-of-a-thunk double-forcing). */
static uint8_t emit_lazy_thunk(UEmitter *e, UAstNode *expr) {
    /* Pass-through shortcut: lazy local used as lazy arg — pass the closure
     * register directly without re-wrapping.  Check BEFORE setting
     * lazy_arg_context (which would suppress the is_lazy check in
     * emit_expr/AST_IDENT). */
    if (expr->kind == AST_IDENT && e->vm != NULL && e->current_fs != NULL) {
        const char *canonical = ustr_intern(e->vm, expr->u.ident.start,
                                            (size_t)expr->u.ident.len);
        if (canonical != NULL) {
            UFuncState *fs = e->current_fs;
            for (int i = fs->nactvar - 1; i >= 0; i--) {
                if (fs->actvars[i].name == canonical && fs->actvars[i].is_lazy) {
                    /* Found a lazy local — emit OP_MOVE of its slot directly.
                     * No force, no re-wrap. */
                    uint8_t dst = e->next_reg;
                    if (dst >= (uint8_t)(UFS_MAX_REGS - 1)) {
                        e->error = EMIT_REG_EXHAUSTED;
                        return 0u;
                    }
                    emit_instr(e, uinstr_enc_abc(OP_MOVE, dst,
                                                 fs->actvars[i].slot, 0u),
                               (uint32_t)expr->line);
                    e->next_reg++;
                    if (e->next_reg > e->max_reg_seen)
                        e->max_reg_seen = e->next_reg;
                    if (e->next_reg > fs->max_reg_seen)
                        fs->max_reg_seen = e->next_reg;
                    return dst;
                }
                /* Stop if we found the name but it's not lazy (normal local). */
                if (fs->actvars[i].name == canonical) break;
            }
        }
    }

    /* General case: build a synthetic AST_FUNCTION wrapping `expr`.
     * Stack-allocate the synthetic nodes — they are used only within this
     * call frame and must NOT be allocated from e->arena because the arena
     * may have been reset between statements (the UFuncState and other
     * persistent emitter state also live in the arena; overwriting them
     * would corrupt the compiler state). */
    UAstNode *stmts_arr[1];
    stmts_arr[0] = expr;

    UAstNode body_node;
    emit_zero(&body_node, sizeof(body_node));
    body_node.kind             = AST_BLOCK;
    body_node.line             = expr->line;
    body_node.col              = expr->col;
    body_node.u.block.stmts   = stmts_arr;
    body_node.u.block.count   = 1;

    UAstNode fn_node;
    emit_zero(&fn_node, sizeof(fn_node));
    fn_node.kind              = AST_FUNCTION;
    fn_node.line              = expr->line;
    fn_node.col               = expr->col;
    fn_node.u.func.params     = NULL;
    fn_node.u.func.param_count = 0;
    fn_node.u.func.body       = &body_node;

    /* Recurse into AST_FUNCTION emit.  lazy_arg_context must be clear
     * during this call so that any lazy-local reads *inside* the thunk
     * body emit implicit force (they are reads, not arg passes). */
    bool saved_ctx = e->lazy_arg_context;
    e->lazy_arg_context = false;
    uint8_t dst = emit_expr(e, &fn_node);
    e->lazy_arg_context = saved_ctx;
    return dst;
}

/* AST walker — returns the register holding the result of the expression.
   Returns 0 and sets e->error on any failure. */
static uint8_t emit_expr(UEmitter *e, UAstNode *n) {
    if (e->error != EMIT_OK) return 0u;
    /* Default arm returns EMIT_UNSUPPORTED_AST for AST kinds not yet
       emitted by this milestone. Later tasks will add explicit case arms as
       each construct's emit lands; the NOLINT suppresses clang's switch-
       exhaustiveness warning until that work completes. */
    // NOLINTNEXTLINE(clang-diagnostic-switch)
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
    case AST_IDENT: {
        if (e->vm == NULL || e->current_fs == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }
        const char *canonical = ustr_intern(e->vm, n->u.ident.start,
                                            (size_t)n->u.ident.len);
        if (canonical == NULL) {
            e->error = EMIT_OOM;
            return 0u;
        }

        /* Local lookup — scan active locals from innermost to outermost. */
        UFuncState *fs = e->current_fs;
        int slot = -1;
        bool is_lazy_local = false;
        for (int i = fs->nactvar - 1; i >= 0; i--) {
            if (fs->actvars[i].name == canonical) {
                slot = (int)fs->actvars[i].slot;
                is_lazy_local = fs->actvars[i].is_lazy;
                break;
            }
        }
        if (slot >= 0) {
            uint8_t dst = e->next_reg;
            if (dst >= (uint8_t)(UFS_MAX_REGS - 1)) {
                e->error = EMIT_REG_EXHAUSTED;
                return 0u;
            }
            emit_instr(e, uinstr_enc_abc(OP_MOVE, dst, (uint8_t)slot, 0u),
                       (uint32_t)n->line);
            e->next_reg++;
            if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
            if (e->next_reg > fs->max_reg_seen) fs->max_reg_seen = e->next_reg;

            /* T16: implicit force for lazy locals (spec §4.1).
             * Skip when lazy_arg_context is set — caller is passing this
             * value as a call argument (pass-through, spec §4.2). */
            if (is_lazy_local && !e->lazy_arg_context) {
                /* dst currently holds the thunk closure.  Force it:
                 * OP_CALL dst, 1, 2 — zero args, 1 result, result in dst. */
                emit_instr(e, uinstr_enc_abc(OP_CALL, dst, 1u, 2u),
                           (uint32_t)n->line);
            }
            return dst;
        }

        /* Upvalue cascade. */
        int up = find_or_install_upvalue(e, fs, canonical, n->u.ident.len);
        if (up >= 0) {
            uint8_t dst = e->next_reg;
            if (dst >= (uint8_t)(UFS_MAX_REGS - 1)) {
                e->error = EMIT_REG_EXHAUSTED;
                return 0u;
            }
            emit_instr(e, uinstr_enc_abc(OP_GETUPVAL, dst, (uint8_t)up, 0u),
                       (uint32_t)n->line);
            e->next_reg++;
            if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
            if (e->next_reg > fs->max_reg_seen) fs->max_reg_seen = e->next_reg;
            return dst;
        }

        /* No globals at v1.0. */
        e->error = EMIT_UNRESOLVED_NAME;
        return 0u;
    }
    case AST_VAR_DECL: {
        if (e->current_fs == NULL || e->vm == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }
        UFuncState *fs = e->current_fs;

        /* Intern the variable name. */
        const char *canonical = ustr_intern(e->vm, n->u.var_decl.name_start,
                                            (size_t)n->u.var_decl.name_len);
        if (canonical == NULL) { e->error = EMIT_OOM; return 0u; }

        /* Redeclare check within current block (or whole actvar table). */
        int search_from = (fs->nblocks > 0)
            ? fs->blocks[fs->nblocks - 1].first_local_idx
            : 0;
        for (int i = search_from; i < fs->nactvar; i++) {
            if (fs->actvars[i].name == canonical) {
                e->error = EMIT_LOCAL_REDECLARE;
                return 0u;
            }
        }
        if (fs->nactvar >= UFS_MAX_LOCALS) {
            e->error = EMIT_REG_EXHAUSTED;
            return 0u;
        }

        /* Record where the init will land: current top-of-stack register.
           The init expression is emitted at this slot via alloc_reg(). */
        uint8_t reg_before = e->next_reg;

        /* Emit init expression — lands at reg_before (alloc_reg gives it
           the next free slot, which is e->next_reg == reg_before). */
        uint8_t init_reg = emit_expr(e, n->u.var_decl.init);
        if (e->error != EMIT_OK) return 0u;

        /* Sanity: init must have landed at exactly reg_before. */
        if (init_reg != reg_before) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }

        /* Absorb the temp into the local zone: register at reg_before is
           now the local's permanent slot. Do NOT free the register. */
        int local_idx = fs->nactvar;
        ULocalVar *lv = &fs->actvars[fs->nactvar++];
        lv->name       = canonical;
        lv->name_len   = n->u.var_decl.name_len;
        lv->slot       = reg_before;
        lv->is_captured = false;
        lv->is_lazy    = false;

        /* T16: if init is a literal function, record its lazy-param signature
         * so the call-site emit can wrap args correctly (spec §2.2). */
        if (n->u.var_decl.init->kind == AST_FUNCTION) {
            UAstNode *fn = n->u.var_decl.init;
            UFuncSig *sig = &fs->actvar_sigs[local_idx];
            sig->resolved = true;
            sig->nparams = fn->u.func.param_count;
            {
                int pi;
                for (pi = 0; pi < fn->u.func.param_count && pi < 16; pi++) {
                    sig->param_is_lazy[pi] =
                        (fn->u.func.params[pi]->kind == AST_LAZY_PARAM);
                }
            }
        }

        /* Sync freereg: it now equals e->next_reg (one past the local's slot).
           The local occupies [reg_before]; e->next_reg is already reg_before+1. */
        fs->freereg = e->next_reg;
        if (fs->freereg > fs->max_reg_seen) fs->max_reg_seen = fs->freereg;

        /* var-decl "returns" the value in its slot (for use as an expression
           in separator chains). Caller can free_reg as normal; the slot
           remains because it is now a local (tracked by nactvar). */
        return init_reg;
    }
    case AST_ASSIGN: {
        if (e->current_fs == NULL || e->vm == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }
        UFuncState *fs = e->current_fs;

        const char *canonical = ustr_intern(e->vm, n->u.assign.name_start,
                                            (size_t)n->u.assign.name_len);
        if (canonical == NULL) { e->error = EMIT_OOM; return 0u; }

        /* Resolve target: local first. */
        int local_slot = -1;
        for (int i = fs->nactvar - 1; i >= 0; i--) {
            if (fs->actvars[i].name == canonical) {
                /* T16: reject assignment to lazy parameter (spec §4.5). */
                if (fs->actvars[i].is_lazy) {
                    e->error = EMIT_LAZY_PARAM_ASSIGN;
                    return 0u;
                }
                local_slot = (int)fs->actvars[i].slot;
                break;
            }
        }

        int upvalue_idx = -1;
        if (local_slot < 0) {
            upvalue_idx = find_or_install_upvalue(e, fs, canonical,
                                                  n->u.assign.name_len);
            if (upvalue_idx < 0) {
                e->error = EMIT_UNRESOLVED_NAME;
                return 0u;
            }
        }

        /* Emit RHS into top temp. */
        uint8_t reg_before = e->next_reg;
        uint8_t rhs_reg = emit_expr(e, n->u.assign.value);
        if (e->error != EMIT_OK) return 0u;

        /* Move into the target slot. */
        if (local_slot >= 0) {
            emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)local_slot,
                                         rhs_reg, 0u),
                       (uint32_t)n->line);
        } else {
            emit_instr(e, uinstr_enc_abc(OP_SETUPVAL, rhs_reg,
                                         (uint8_t)upvalue_idx, 0u),
                       (uint32_t)n->line);
        }
        /* Free the temp — it was only needed for the RHS. */
        e->next_reg = reg_before;
        /* Assignment expression value is the target slot's value; return it. */
        return (local_slot >= 0) ? (uint8_t)local_slot : reg_before;
    }
    case AST_NARY: {
        /* `,` parallel semantics land at M3; emit-time error at M2. */
        if (n->u.nary.separator == SEP_COMMA) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }
        /* SEP_SEMI: compile each child; OP_YIELD between children (not
           before first, not after last); release temp regs between.
           Last child's result register is the Nary's value.

           Between children, reset next_reg to freereg (first slot above
           all declared locals) rather than blindly decrementing.  The
           decrement-by-one pattern is wrong when a var-decl child has
           promoted a temp into a permanent local (advancing freereg), or
           when a non-var child needed more than one temp: both cases leave
           freereg ahead of where a simple decrement would land. */
        uint8_t r = 0u;
        for (int i = 0; i < n->u.nary.count; i++) {
            if (i > 0) {
                /* Release all temps allocated by the previous child, but
                 * keep locals (tracked by freereg / nactvar). */
                e->next_reg = e->current_fs->freereg;
                emit_instr(e, uinstr_enc_abc(OP_YIELD, 0u, 0u, 0u),
                           e->prev_line);
                if (e->error != EMIT_OK) return 0u;
            }
            r = emit_expr(e, n->u.nary.children[i]);
            if (e->error != EMIT_OK) return 0u;
        }
        return r;
    }
    case AST_BIN_SEP: {
        /* `&` fork/join lands at M3; emit-time error at M2. */
        if (n->u.bin_sep.separator == SEP_AMP) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }
        /* SEP_PIPE: lhs then rhs in sequence, no yield.
           LHS value is discarded; result is rhs. */
        uint8_t lhs_r = emit_expr(e, n->u.bin_sep.lhs);
        if (e->error != EMIT_OK) return 0u;
        /* Release lhs register before rhs so rhs may reuse the slot. */
        (void)lhs_r;
        if (e->next_reg > 0u) e->next_reg--;
        uint8_t rhs_r = emit_expr(e, n->u.bin_sep.rhs);
        return rhs_r;
    }
    case AST_NOOP:
        /* No-op: load nil as the value. */
        {
            uint8_t r = alloc_reg(e);
            if (e->error != EMIT_OK) return 0u;
            emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0u, 0u),
                       (uint32_t)n->line);
            return r;
        }
    case AST_BOOL: {
        uint8_t r = alloc_reg(e);
        if (e->error != EMIT_OK) return 0u;
        emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, r, n->u.b ? 1u : 0u, 0u),
                   (uint32_t)n->line);
        return r;
    }
    case AST_NIL: {
        uint8_t r = alloc_reg(e);
        if (e->error != EMIT_OK) return 0u;
        emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0u, 0u),
                   (uint32_t)n->line);
        return r;
    }
    case AST_COMPARE: {
        /* Compile LHS into rb, RHS into the next register. */
        uint8_t rb = e->next_reg;
        uint8_t lhs_reg = emit_expr(e, n->u.cmp.lhs);
        if (e->error != EMIT_OK) return 0u;
        (void)lhs_reg;  /* rb == lhs_reg; named for clarity */
        uint8_t rc_reg = e->next_reg;
        uint8_t rhs_reg = emit_expr(e, n->u.cmp.rhs);
        if (e->error != EMIT_OK) return 0u;
        (void)rhs_reg;  /* rc_reg == rhs_reg */

        /* Pick opcode + A bit per operator.
           CMP_GT / CMP_GE are emitted as swapped OP_LT / OP_LE.
           The dispatch arm skips the next instruction when (result != A).
           For bool production: "skip" → LOADBOOL true. So we want skip
           when the comparison is TRUE → (result != A) must be true when
           result=true → A=0.  Exception: CMP_NEQ wants skip when eq=false
           → (false != A) true when A=1. */
        UOpcode op;
        uint8_t a_bit;
        uint8_t b_reg, c_reg;
        switch (n->u.cmp.op) {
            case CMP_EQ:  op = OP_EQ;  a_bit = 0u; b_reg = rb;     c_reg = rc_reg; break;
            case CMP_NEQ: op = OP_EQ;  a_bit = 1u; b_reg = rb;     c_reg = rc_reg; break;
            case CMP_LT:  op = OP_LT;  a_bit = 0u; b_reg = rb;     c_reg = rc_reg; break;
            case CMP_LE:  op = OP_LE;  a_bit = 0u; b_reg = rb;     c_reg = rc_reg; break;
            case CMP_GT:  op = OP_LT;  a_bit = 0u; b_reg = rc_reg; c_reg = rb;     break;
            case CMP_GE:  op = OP_LE;  a_bit = 0u; b_reg = rc_reg; c_reg = rb;     break;
            default:      e->error = EMIT_UNSUPPORTED_AST; return 0u;
        }

        /* Free LHS+RHS temps; result goes into rb. */
        e->next_reg = rb + 1u;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->current_fs != NULL) {
            if (e->next_reg > e->current_fs->max_reg_seen)
                e->current_fs->max_reg_seen = e->next_reg;
        }

        /* 4-instruction Lua-style branch idiom:
             op A, b_reg, c_reg    ; skip next if comparison matches a_bit
             JMP +1                ; jump past LOADBOOL true (→ false arm)
             LOADBOOL rb, 1, 1     ; rb = true; pc++ (skip false arm)
             LOADBOOL rb, 0, 0     ; rb = false
        */
        emit_instr(e, uinstr_enc_abc(op, a_bit, b_reg, c_reg), (uint32_t)n->line);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0u, (uint16_t)(32768u + 1u)), (uint32_t)n->line);
        emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, rb, 1u, 1u), (uint32_t)n->line);
        emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, rb, 0u, 0u), (uint32_t)n->line);

        return rb;
    }
    case AST_BLOCK: {
        /* Scoped sequence of statements inside `{ }`.
           Opens a block scope so locals declared inside don't outlive the
           block.  The block's "value" is the last statement's result reg
           (or nil if empty).  Temps are reset between statements. */
        if (e->current_fs == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }
        if (!uemit_open_block(e, false)) return 0u;

        uint8_t r = 0u;
        for (int i = 0; i < n->u.block.count; i++) {
            r = emit_expr(e, n->u.block.stmts[i]);
            if (e->error != EMIT_OK) {
                uemit_close_block(e);
                return 0u;
            }
            if (i < n->u.block.count - 1) {
                /* Release temps between statements; locals stay. */
                e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
                e->next_reg = e->current_fs->freereg;
            }
        }

        if (!uemit_close_block(e)) return 0u;
        return r;
    }
    case AST_IF: {
        /* if (cond) then-block [else else-block]

           With else:
             TEST  rx, 0, 1       ; skip JMP if cond is truthy
             JMP   else_target    ; jump here when falsy
             <then-block>         ; result in rd
             JMP   end_target     ; skip else
             else_target:
             <else-block>         ; result in rd
             end_target:

           Without else:
             TEST  rx, 0, 1       ; skip JMP if cond is truthy
             JMP   nil_target     ; jump here when falsy
             <then-block>         ; result in rd
             JMP   end_target     ; skip nil-load
             nil_target:
             LOADNIL rd
             end_target:

           Both arms are compiled with next_reg = rd so they write
           their result into rd.  The if-expr returns rd.

           OP_TEST polarity: "if (truthy(R[A]) == C) pc++" so C=1 skips
           (falls into then-block) when truthy; C=0 would skip when falsy.
           C=1 is correct for if-then. */
        if (e->current_fs == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }

        /* 1. rd is the result register; cond is compiled into rx >= rd.
              Since cond may use multiple regs (e.g. a comparison), compile
              it first (at current next_reg), record the base as rd, then
              reset next_reg back to rd before each arm. */
        uint8_t rd = e->next_reg;

        /* Compile cond starting at rd. */
        uint8_t rx = rd;
        uint8_t cond_reg = emit_expr(e, n->u.if_stmt.cond);
        if (e->error != EMIT_OK) return 0u;
        (void)cond_reg;  /* rx == cond_reg */

        /* 2. TEST rx, 0, 1 — skip next instr (JMP) when cond is truthy. */
        emit_instr(e, uinstr_enc_abc(OP_TEST, rx, 0u, 1u), (uint32_t)n->line);

        /* 3. JMP placeholder to else/nil target (patched later). */
        int jmp_to_else = (int)emit_instr_count(e);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0u, 32768u), (uint32_t)n->line);

        /* 4. Reset cursor to rd so then-block allocates starting at rd. */
        e->next_reg = rd;
        e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;

        /* 5. Compile then-block. */
        uint8_t then_r = emit_expr(e, n->u.if_stmt.then_block);
        if (e->error != EMIT_OK) return 0u;
        if (then_r != rd) {
            emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, then_r, 0u),
                       (uint32_t)n->line);
        }

        /* 6. JMP past else/nil-load to end (patched later). */
        int jmp_to_end = (int)emit_instr_count(e);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0u, 32768u), (uint32_t)n->line);

        /* 7. Patch jmp_to_else → current pc (start of else/nil arm). */
        {
            int alt_target = (int)emit_instr_count(e);
            int alt_offset = alt_target - (jmp_to_else + 1);
            emit_patch_instr(e, jmp_to_else,
                uinstr_enc_abx(OP_JMP, 0u, (uint16_t)(32768 + alt_offset)));
        }

        /* 8. Reset cursor to rd for else/nil arm. */
        e->next_reg = rd;
        e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;

        /* 9. Compile else-block or emit LOADNIL. */
        if (n->u.if_stmt.else_block != NULL) {
            uint8_t else_r = emit_expr(e, n->u.if_stmt.else_block);
            if (e->error != EMIT_OK) return 0u;
            if (else_r != rd) {
                emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, else_r, 0u),
                           (uint32_t)n->line);
            }
        } else {
            emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0u, 0u),
                       (uint32_t)n->line);
        }

        /* 10. Patch jmp_to_end → current pc. */
        {
            int end_target = (int)emit_instr_count(e);
            int end_offset = end_target - (jmp_to_end + 1);
            emit_patch_instr(e, jmp_to_end,
                uinstr_enc_abx(OP_JMP, 0u, (uint16_t)(32768 + end_offset)));
        }

        /* Advance past rd so callers can free it as a temp if needed. */
        e->next_reg = rd + 1u;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->current_fs != NULL) {
            if (e->next_reg > e->current_fs->max_reg_seen)
                e->current_fs->max_reg_seen = e->next_reg;
            e->current_fs->freereg = e->next_reg;
        }

        return rd;
    }
    case AST_WHILE: {
        /* while (cond) { body }
           Loop structure:
             loop_start:
               <cond>               ; result in rx
               TEST rx, 0, 1        ; skip JMP-to-exit when cond is truthy
               JMP <exit>           ; exit when falsy
               <body stmts>         ; body block opened with is_loop=true
               emit_loop_back_close ; OP_CLOSE if any local captured
               JMP loop_start       ; back-edge
             exit: */
        if (e->current_fs == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }

        int loop_start = (int)emit_instr_count(e);

        /* 1. Compile cond into rx. */
        uint8_t rx = e->next_reg;
        emit_expr(e, n->u.while_stmt.cond);
        if (e->error != EMIT_OK) return 0u;

        /* 2. TEST rx, 0, 1 — skip JMP-to-exit when cond is truthy. */
        emit_instr(e, uinstr_enc_abc(OP_TEST, rx, 0u, 1u), (uint32_t)n->line);

        /* 3. JMP placeholder to exit (patched later). */
        int jmp_to_exit = (int)emit_instr_count(e);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0u, 32768u), (uint32_t)n->line);

        /* Free cond temp; locals beneath rx stay. */
        e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
        e->next_reg = e->current_fs->freereg;

        /* 4. Body — open block as is_loop=true (different from AST_BLOCK
              which opens with is_loop=false). */
        if (n->u.while_stmt.body->kind != AST_BLOCK) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }
        {
            UAstNode *body = n->u.while_stmt.body;
            if (!uemit_open_block(e, /*is_loop=*/true)) return 0u;

            for (int i = 0; i < body->u.block.count; i++) {
                emit_expr(e, body->u.block.stmts[i]);
                if (e->error != EMIT_OK) {
                    uemit_close_block(e);
                    return 0u;
                }
                /* Release temps between body statements; locals stay. */
                e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
                e->next_reg = e->current_fs->freereg;
            }

            /* 5. OP_CLOSE-on-back-edge if any local in the loop block was captured. */
            uemit_emit_loop_back_close(e);

            /* 6. Back-edge JMP to loop_start. */
            {
                int back_offset = loop_start - ((int)emit_instr_count(e) + 1);
                emit_instr(e, uinstr_enc_abx(OP_JMP, 0u,
                                             (uint16_t)(32768 + back_offset)),
                           (uint32_t)n->line);
            }

            /* 7. Close the loop block (emits OP_CLOSE if has_captured, then pops
                  actvars back). */
            if (!uemit_close_block(e)) return 0u;
        }

        /* 8. Patch the exit JMP to current pc. */
        {
            int exit_target = (int)emit_instr_count(e);
            int exit_offset = exit_target - (jmp_to_exit + 1);
            emit_patch_instr(e, jmp_to_exit,
                uinstr_enc_abx(OP_JMP, 0u, (uint16_t)(32768 + exit_offset)));
        }

        /* while-loop is a statement; it doesn't produce a value.
           Return a register that holds nil to give callers a valid reg. */
        {
            uint8_t r = e->next_reg;
            emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0u, 0u),
                       (uint32_t)n->line);
            e->next_reg++;
            if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
            if (e->current_fs->freereg < e->next_reg)
                e->current_fs->freereg = e->next_reg;
            return r;
        }
    }
    case AST_CALL: {
        /* Emit callee into dst register, then args into consecutive registers.
         * OP_CALL A, B, C: R[A] = callee, args at R[A+1..A+B-1], B = nargs+1.
         * Result written to R[A] by OP_RET. */
        if (e->current_fs == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }

        /* T16: Look up callee's function signature when the callee is a
         * statically-visible local declared with a function literal.
         * Used below to decide whether to wrap each arg as a lazy thunk. */
        UFuncSig *call_sig = NULL;
        if (n->u.call.callee->kind == AST_IDENT && e->vm != NULL) {
            const char *cn = ustr_intern(e->vm,
                                         n->u.call.callee->u.ident.start,
                                         (size_t)n->u.call.callee->u.ident.len);
            if (cn != NULL) {
                UFuncState *fs = e->current_fs;
                for (int i = fs->nactvar - 1; i >= 0; i--) {
                    if (fs->actvars[i].name == cn) {
                        if (fs->actvar_sigs[i].resolved) {
                            call_sig = &fs->actvar_sigs[i];
                        }
                        break;
                    }
                }
            }
        }

        uint8_t callee_reg = e->next_reg;
        uint8_t callee_r   = emit_expr(e, n->u.call.callee);
        if (e->error != EMIT_OK) return 0u;
        /* Move callee into callee_reg if emit_expr put it elsewhere
         * (shouldn't happen since next_reg == callee_reg on entry, but be safe). */
        if (callee_r != callee_reg) {
            emit_instr(e, uinstr_enc_abc(OP_MOVE, callee_reg, callee_r, 0u),
                       (uint32_t)n->line);
        }

        /* Sync freereg up to next_reg before the arg loop.  When the callee
         * was a local (OP_MOVE, using next_reg-based allocation), freereg
         * still points at the local zone boundary and is behind next_reg.
         * emit_lazy_thunk → AST_FUNCTION emit uses freereg (not next_reg) as
         * the OP_CLOSURE destination, so without this sync it would clobber
         * the callee's register. */
        if (e->current_fs->freereg < e->next_reg)
            e->current_fs->freereg = e->next_reg;

        /* Emit each argument.  For lazy positions, wrap in a thunk.
         * For all args (lazy or eager), set lazy_arg_context so that
         * any lazy-local reads inside the arg expression use pass-through
         * semantics (spec §4.2). */
        {
            int ai;
            bool saved_ctx = e->lazy_arg_context;
            for (ai = 0; ai < n->u.call.arg_count; ai++) {
                bool param_lazy = (call_sig != NULL &&
                                   ai < call_sig->nparams &&
                                   call_sig->param_is_lazy[ai]);
                /* Pass-through context: suppress implicit force on lazy-local
                 * reads appearing directly as call arguments. */
                e->lazy_arg_context = true;
                uint8_t arg_r;
                if (param_lazy) {
                    /* Sync freereg to next_reg before the thunk emit so
                     * AST_FUNCTION's OP_CLOSURE destination (taken from
                     * freereg) doesn't clobber an already-allocated arg. */
                    if (e->current_fs->freereg < e->next_reg)
                        e->current_fs->freereg = e->next_reg;
                    /* Lazy position: compile arg as zero-arg thunk. */
                    arg_r = emit_lazy_thunk(e, n->u.call.args[ai]);
                } else {
                    arg_r = emit_expr(e, n->u.call.args[ai]);
                }
                e->lazy_arg_context = saved_ctx;
                if (e->error != EMIT_OK) return 0u;
                uint8_t expected = callee_reg + 1u + (uint8_t)ai;
                if (arg_r != expected) {
                    emit_instr(e, uinstr_enc_abc(OP_MOVE, expected, arg_r, 0u),
                               (uint32_t)n->line);
                }
            }
        }

        /* OP_CALL callee_reg, nargs+1, 2 (1 result expected). */
        uint8_t b = (uint8_t)(n->u.call.arg_count + 1);
        emit_instr(e, uinstr_enc_abc(OP_CALL, callee_reg, b, 2u),
                   (uint32_t)n->line);
        /* Result is written to R[callee_reg] by the called function's OP_RET. */
        e->next_reg = callee_reg + 1u;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->current_fs != NULL && e->next_reg > e->current_fs->max_reg_seen)
            e->current_fs->max_reg_seen = e->next_reg;
        return callee_reg;
    }
    case AST_RETURN: {
        /* return [expr]: compile the value (or nil if absent), emit OP_RET.
         * Only valid inside a function body (current_fs must be non-NULL and
         * must have a target_proto — top-level return is not meaningful but
         * is not rejected at emit time; OP_RET at top-level exits uvm_run). */
        if (e->current_fs == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }
        uint8_t ret_reg;
        if (n->u.ret.value != NULL) {
            ret_reg = emit_expr(e, n->u.ret.value);
            if (e->error != EMIT_OK) return 0u;
        } else {
            /* Bare `return`: return nil. */
            ret_reg = alloc_reg(e);
            if (e->error != EMIT_OK) return 0u;
            emit_instr(e, uinstr_enc_abc(OP_LOADNIL, ret_reg, 0u, 0u),
                       (uint32_t)n->line);
        }
        emit_instr(e, uinstr_enc_abc(OP_RET, ret_reg, 0u, 0u),
                   (uint32_t)n->line);
        /* Return the register so the block's last-stmt-reg logic works.
         * Any instructions after OP_RET in the same block are unreachable
         * but that is allowed (e.g., the function body auto-appends OP_RET). */
        return ret_reg;
    }
    case AST_FUNCTION: {
        if (e->current_fs == NULL || e->vm == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }
        UFuncState *parent_fs = e->current_fs;

        /* 1. Allocate a new UProto under the module's nested[] list. */
        UProto *child_proto = umodule_alloc_nested_proto(e->module);
        if (child_proto == NULL) { e->error = EMIT_OOM; return 0u; }
        int proto_idx = (int)(e->module->nested_count - 1);

        /* 2. Open a nested FuncState targeting child_proto. */
        UFuncState *child_fs = uemit_open_function(e, parent_fs);
        if (child_fs == NULL) return 0u;
        child_fs->target_proto = child_proto;

        /* 3. Declare parameters as locals in child_fs. */
        {
            int pi;
            for (pi = 0; pi < n->u.func.param_count; pi++) {
                UAstNode *pn = n->u.func.params[pi];
                const char *cname = ustr_intern(e->vm, pn->u.param.name_start,
                                                (size_t)pn->u.param.name_len);
                if (cname == NULL) { e->error = EMIT_OOM; uemit_close_function(e); return 0u; }
                int slot = uemit_declare_local(e, cname, pn->u.param.name_len);
                if (slot < 0) { uemit_close_function(e); return 0u; }
                if (pn->kind == AST_LAZY_PARAM) {
                    child_fs->actvars[slot].is_lazy = true;
                }
            }
        }
        child_proto->nparams = (uint8_t)n->u.func.param_count;

        /* Sync the flat register cursor to the child's freereg so temps
         * inside the function body are allocated above all param slots.
         * Without this, a lazy-param force that allocates a temp via
         * next_reg could pick a register that overlaps a param (e.g.,
         * MOVE R0, R0 → CALL R0 overwrites the thunk with its result,
         * breaking subsequent reads of the same lazy param). */
        e->next_reg = child_fs->freereg;

        /* 4. Compile body (AST_BLOCK); emit_instr routes to child_proto.
         *    Save the result register the block returns. */
        uint8_t body_reg = emit_expr(e, n->u.func.body);
        if (e->error != EMIT_OK) {
            uemit_close_function(e);
            return 0u;
        }

        /* 5. Final OP_RET in child proto using the block's result register.
         *    AST_BLOCK returns the last statement's result reg (or 0 for empty).
         *    If the block was empty or returned nil, body_reg is still valid. */
        emit_instr(e, uinstr_enc_abc(OP_RET, body_reg, 0u, 0u),
                   (uint32_t)n->line);

        /* 6. Capture upvalue descriptors before closing child_fs. */
        int nup = child_fs->nupvalues;
        UUpvalDesc upvals_copy[UFS_MAX_UPVALUES];
        {
            int ui;
            for (ui = 0; ui < nup; ui++) {
                upvals_copy[ui] = child_fs->upvalues[ui];
            }
        }

        uemit_close_function(e);   /* pops back to parent_fs */

        /* 7. In parent, emit OP_CLOSURE + nup pseudo-instructions. */
        {
            uint8_t dst = e->current_fs->freereg;
            if (dst >= (uint8_t)(UFS_MAX_REGS - 1)) {
                e->error = EMIT_REG_EXHAUSTED;
                return 0u;
            }
            e->current_fs->freereg++;
            if (e->current_fs->freereg > e->current_fs->max_reg_seen)
                e->current_fs->max_reg_seen = e->current_fs->freereg;
            e->next_reg = e->current_fs->freereg;
            if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;

            emit_instr(e, uinstr_enc_abx(OP_CLOSURE, dst, (uint16_t)proto_idx),
                       (uint32_t)n->line);
            {
                int ui;
                for (ui = 0; ui < nup; ui++) {
                    UUpvalDesc *ud = &upvals_copy[ui];
                    /* Pseudo-instruction: B=in_stack, C=src_idx */
                    emit_instr(e,
                        uinstr_enc_abc(OP_MOVE, 0u,
                                       ud->in_stack ? 1u : 0u,
                                       (uint8_t)ud->idx),
                        (uint32_t)n->line);
                }
            }
            return dst;
        }
    }
    case AST_THROW: {
        /* throw expr: eval the expression, emit OP_THROW, set pending_unwind.
         * OP_THROW goes to safepoint; urbi_unwind walks the cleanup stack. */
        if (e->current_fs == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }
        uint8_t val_reg = emit_expr(e, n->u.throw_expr.value);
        if (e->error != EMIT_OK) return 0u;
        uemit_throw(e, val_reg, (uint32_t)n->line);
        /* throw is a statement; return a nil reg for the block's last-stmt logic. */
        uint8_t rd = e->next_reg;
        emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0u, 0u),
                   (uint32_t)n->line);
        e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->current_fs->freereg < e->next_reg)
            e->current_fs->freereg = e->next_reg;
        return rd;
    }
    case AST_TRY: {
        /* try { body } [catch (e) { handler }] [finally { cleanup }]
         *
         * Bytecode layout:
         *   [try_begin_pc]:
         *     OP_TRY_BEGIN flags, handler_pc_placeholder
         *     <body opcodes>
         *     OP_TRY_END
         *     OP_JMP past_handler_placeholder
         *   [handler_pc]:           ← OP_TRY_BEGIN Bx points here
         *     If catch:
         *       OP_LOAD_CATCH_VALUE catch_reg  ; bind e = s->catch_value
         *       <catch body opcodes>
         *     If finally (no catch):
         *       <finally body opcodes>
         *       OP_RESUME 0          ; exit cleanup body; walker restores saved unwind
         *     If catch + finally:
         *       OP_LOAD_CATCH_VALUE catch_reg
         *       <catch body opcodes>
         *     (finally runs as a SEPARATE try-block wrapping the whole thing;
         *      simplified at T10 to: catch-only handler OR finally-only handler)
         *   [past_handler_pc]:
         *     <continuation>
         *
         * T10 simplified: catch and finally are mutually exclusive handler blocks
         * behind a single TRY_FRAME entry.  If both exist, we emit two TRY_FRAME
         * entries: outer finally wraps inner try+catch.
         *
         * For the combined case (catch + finally), layout is:
         *   OP_TRY_BEGIN FLAG_HAS_FINALLY, outer_finally_pc     ; outer TRY_FRAME
         *     OP_TRY_BEGIN FLAG_HAS_CATCH, catch_pc             ; inner TRY_FRAME
         *       <body>
         *     OP_TRY_END
         *     OP_JMP past_catch_pc
         *   [catch_pc]:
         *     OP_LOAD_CATCH_VALUE catch_reg
         *     <catch body>
         *   [past_catch_pc]:
         *   OP_TRY_END    (outer)
         *   OP_JMP past_finally_pc
         *   [outer_finally_pc]:
         *     <finally body>
         *     OP_RESUME 0
         *   [past_finally_pc]:
         *
         * Result register: the try's "value" is nil (try/catch/finally is a statement). */

        if (e->current_fs == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }

        const int has_catch   = (n->u.try_stmt.catch_body != NULL);
        const int has_finally = (n->u.try_stmt.finally_body != NULL);

        /* Result dest — allocate a nil-holding register. */
        uint8_t rd = e->next_reg;

        if (has_catch && has_finally) {
            /* === OUTER TRY_FRAME: finally wrapper === */
            int outer_try_begin_pc = (int)emit_instr_count(e);
            uemit_try_begin(e, FLAG_HAS_FINALLY, 0u, (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* === INNER TRY_FRAME: catch wrapper === */
            int inner_try_begin_pc = (int)emit_instr_count(e);
            uemit_try_begin(e, FLAG_HAS_CATCH, 0u, (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* Body */
            e->next_reg = rd;
            e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
            if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
            emit_expr(e, n->u.try_stmt.body);
            if (e->error != EMIT_OK) return 0u;

            /* OP_TRY_END (inner) */
            uemit_try_end(e, (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* JMP past catch */
            int jmp_past_catch_pc = (int)emit_instr_count(e);
            emit_instr(e, uinstr_enc_abx(OP_JMP, 0u, 32768u), (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* Patch inner_try_begin handler_pc → catch handler */
            {
                int catch_target = (int)emit_instr_count(e);
                emit_patch_instr(e, inner_try_begin_pc,
                    uinstr_enc_abx(OP_TRY_BEGIN, FLAG_HAS_CATCH,
                                   (uint16_t)catch_target));
            }

            /* Catch handler: declare catch var as a local, emit body */
            {
                const char *cv_name = NULL;
                int catch_reg;
                e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
                e->next_reg = e->current_fs->freereg;

                if (n->u.try_stmt.catch_var_start != NULL && e->vm != NULL) {
                    cv_name = ustr_intern(e->vm,
                                          n->u.try_stmt.catch_var_start,
                                          (size_t)n->u.try_stmt.catch_var_len);
                    if (cv_name == NULL) { e->error = EMIT_OOM; return 0u; }
                    int slot = uemit_declare_local(e, cv_name,
                                                   n->u.try_stmt.catch_var_len);
                    if (slot < 0) return 0u;
                    catch_reg = slot;
                    /* OP_LOAD_CATCH_VALUE loads s->catch_value into the local slot. */
                    uemit_load_catch_value(e, (uint8_t)catch_reg,
                                           (uint32_t)n->line);
                    if (e->error != EMIT_OK) return 0u;
                } else {
                    /* No named variable — still load catch value to advance PC. */
                    uint8_t tmp = e->next_reg++;
                    if (e->next_reg > e->max_reg_seen)
                        e->max_reg_seen = e->next_reg;
                    uemit_load_catch_value(e, tmp, (uint32_t)n->line);
                    if (e->error != EMIT_OK) return 0u;
                }

                if (!uemit_open_block(e, false)) return 0u;
                emit_expr(e, n->u.try_stmt.catch_body);
                if (e->error != EMIT_OK) { uemit_close_block(e); return 0u; }
                if (!uemit_close_block(e)) return 0u;

                /* Un-declare the catch variable by restoring nactvar. */
                if (cv_name != NULL && e->current_fs->nactvar > 0) {
                    e->current_fs->nactvar--;
                    e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
                    e->next_reg = e->current_fs->freereg;
                }
            }

            /* Patch jmp_past_catch → here (past_catch_pc) */
            {
                int past_catch_target = (int)emit_instr_count(e);
                int off = past_catch_target - (jmp_past_catch_pc + 1);
                emit_patch_instr(e, jmp_past_catch_pc,
                    uinstr_enc_abx(OP_JMP, 0u, (uint16_t)(32768 + off)));
            }

            /* OP_TRY_END (outer) */
            uemit_try_end(e, (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* JMP past finally */
            int jmp_past_finally_pc = (int)emit_instr_count(e);
            emit_instr(e, uinstr_enc_abx(OP_JMP, 0u, 32768u), (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* Patch outer_try_begin handler_pc → finally handler */
            {
                int finally_target = (int)emit_instr_count(e);
                emit_patch_instr(e, outer_try_begin_pc,
                    uinstr_enc_abx(OP_TRY_BEGIN, FLAG_HAS_FINALLY,
                                   (uint16_t)finally_target));
            }

            /* Finally body */
            e->next_reg = rd;
            e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
            if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
            if (!uemit_open_block(e, false)) return 0u;
            emit_expr(e, n->u.try_stmt.finally_body);
            if (e->error != EMIT_OK) { uemit_close_block(e); return 0u; }
            if (!uemit_close_block(e)) return 0u;
            uemit_resume(e, 0u, (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* Patch jmp_past_finally → here */
            {
                int past_finally_target = (int)emit_instr_count(e);
                int off = past_finally_target - (jmp_past_finally_pc + 1);
                emit_patch_instr(e, jmp_past_finally_pc,
                    uinstr_enc_abx(OP_JMP, 0u, (uint16_t)(32768 + off)));
            }

        } else if (has_catch) {
            /* === Catch-only TRY_FRAME === */
            int try_begin_pc = (int)emit_instr_count(e);
            uemit_try_begin(e, FLAG_HAS_CATCH, 0u, (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* Body */
            e->next_reg = rd;
            e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
            if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
            emit_expr(e, n->u.try_stmt.body);
            if (e->error != EMIT_OK) return 0u;

            uemit_try_end(e, (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* JMP past handler */
            int jmp_past_handler_pc = (int)emit_instr_count(e);
            emit_instr(e, uinstr_enc_abx(OP_JMP, 0u, 32768u), (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* Patch try_begin handler_pc → catch handler */
            {
                int catch_target = (int)emit_instr_count(e);
                emit_patch_instr(e, try_begin_pc,
                    uinstr_enc_abx(OP_TRY_BEGIN, FLAG_HAS_CATCH,
                                   (uint16_t)catch_target));
            }

            /* Catch handler: declare var, emit OP_LOAD_CATCH_VALUE, emit body */
            {
                const char *cv_name = NULL;
                e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
                e->next_reg = e->current_fs->freereg;

                if (n->u.try_stmt.catch_var_start != NULL && e->vm != NULL) {
                    cv_name = ustr_intern(e->vm,
                                          n->u.try_stmt.catch_var_start,
                                          (size_t)n->u.try_stmt.catch_var_len);
                    if (cv_name == NULL) { e->error = EMIT_OOM; return 0u; }
                    int slot = uemit_declare_local(e, cv_name,
                                                   n->u.try_stmt.catch_var_len);
                    if (slot < 0) return 0u;
                    uemit_load_catch_value(e, (uint8_t)slot, (uint32_t)n->line);
                    if (e->error != EMIT_OK) return 0u;
                } else {
                    uint8_t tmp = e->next_reg++;
                    if (e->next_reg > e->max_reg_seen)
                        e->max_reg_seen = e->next_reg;
                    uemit_load_catch_value(e, tmp, (uint32_t)n->line);
                    if (e->error != EMIT_OK) return 0u;
                }

                if (!uemit_open_block(e, false)) return 0u;
                emit_expr(e, n->u.try_stmt.catch_body);
                if (e->error != EMIT_OK) { uemit_close_block(e); return 0u; }
                if (!uemit_close_block(e)) return 0u;

                /* Un-declare catch var */
                if (cv_name != NULL && e->current_fs->nactvar > 0) {
                    e->current_fs->nactvar--;
                    e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
                    e->next_reg = e->current_fs->freereg;
                }
            }

            /* Patch jmp_past_handler → here */
            {
                int past_target = (int)emit_instr_count(e);
                int off = past_target - (jmp_past_handler_pc + 1);
                emit_patch_instr(e, jmp_past_handler_pc,
                    uinstr_enc_abx(OP_JMP, 0u, (uint16_t)(32768 + off)));
            }

        } else {
            /* === Finally-only TRY_FRAME === */
            int try_begin_pc = (int)emit_instr_count(e);
            uemit_try_begin(e, FLAG_HAS_FINALLY, 0u, (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* Body */
            e->next_reg = rd;
            e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
            if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
            emit_expr(e, n->u.try_stmt.body);
            if (e->error != EMIT_OK) return 0u;

            uemit_try_end(e, (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* JMP past finally (normal exit path) */
            int jmp_past_finally_pc = (int)emit_instr_count(e);
            emit_instr(e, uinstr_enc_abx(OP_JMP, 0u, 32768u), (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* Patch try_begin handler_pc → finally handler */
            {
                int finally_target = (int)emit_instr_count(e);
                emit_patch_instr(e, try_begin_pc,
                    uinstr_enc_abx(OP_TRY_BEGIN, FLAG_HAS_FINALLY,
                                   (uint16_t)finally_target));
            }

            /* Finally body */
            e->next_reg = rd;
            e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
            if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
            if (!uemit_open_block(e, false)) return 0u;
            emit_expr(e, n->u.try_stmt.finally_body);
            if (e->error != EMIT_OK) { uemit_close_block(e); return 0u; }
            if (!uemit_close_block(e)) return 0u;
            uemit_resume(e, 0u, (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;

            /* Patch jmp_past_finally → here */
            {
                int past_target = (int)emit_instr_count(e);
                int off = past_target - (jmp_past_finally_pc + 1);
                emit_patch_instr(e, jmp_past_finally_pc,
                    uinstr_enc_abx(OP_JMP, 0u, (uint16_t)(32768 + off)));
            }
        }

        /* Emit nil into rd for the "value" of the try expression. */
        e->next_reg = rd;
        e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
        emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0u, 0u),
                   (uint32_t)n->line);
        e->next_reg = rd + 1u;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        e->current_fs->freereg = e->next_reg;
        return rd;
    }
    case AST_TAG_PREFIX: {
        /* mytag: { body }
         *
         * Bytecode layout:
         *   [push_tag_pc]:
         *     OP_PUSH_TAG packed_A, onleave_pc_placeholder  ; onleave_pc=0 at M3 (no onleave)
         *     <body opcodes>
         *     OP_POP_TAG  tag_reg
         *     OP_JMP      past_handler_placeholder
         *   [onleave_pc]:                 ← OP_PUSH_TAG Bx points here (0 at M3)
         *     (empty — onleave body deferred to M5)
         *   [past_handler_pc]:
         *     <continuation>
         *
         * At M3: onleave is always NULL, so:
         *   - OP_PUSH_TAG emits flags=0 (no FLAG_HAS_ONLEAVE), handler_pc = PC-after-JMP.
         *   - OP_POP_TAG: pop entry; since flags=0 the FLAG_HAS_ONLEAVE branch is dead.
         *   - OP_JMP past-handler: jumps over the (empty) onleave block.
         *   - Handler block: empty; no instructions emitted.
         *
         * tag_reg: evaluate tag_expr to a register.  At M3 UTag doesn't exist, so
         * tag_expr (an identifier) resolves to nil.  The cleanup entry stores
         * owning_tag=NULL (T29 wires the real UTag pointer). tag_reg is limited to
         * [0,15] by the 4-bit nibble encoding of OP_PUSH_TAG.A[3:0].
         *
         * Result: the body's result value (tag-prefix is an expression at M3). */

        if (e->current_fs == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0u;
        }

        /* Evaluate tag_expr to get a register (will be nil at M3). */
        uint8_t tag_reg = emit_expr(e, n->u.tag_prefix.tag_expr);
        if (e->error != EMIT_OK) return 0u;

        /* tag_reg must fit in 4 bits for OP_PUSH_TAG encoding. */
        if (tag_reg > 15u) {
            /* Spill into a lower register by moving (shouldn't happen in practice
             * since tag-prefix appears near top of scope, but defensive). */
            uint8_t spill = e->next_reg++;
            if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
            if (e->current_fs->freereg < e->next_reg)
                e->current_fs->freereg = e->next_reg;
            emit_instr(e, uinstr_enc_abc(OP_MOVE, spill, tag_reg, 0u),
                       (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0u;
            tag_reg = spill;
        }

        /* Emit OP_PUSH_TAG with placeholder onleave_pc (will be patched). */
        uint8_t flags_m3 = 0u;  /* no FLAG_HAS_ONLEAVE at M3 */
        int push_tag_pc = (int)emit_instr_count(e);
        uemit_push_tag(e, tag_reg, flags_m3, 0u /* placeholder */, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0u;

        /* Emit body. */
        uint8_t rd = e->next_reg;
        e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
        if (!uemit_open_block(e, false)) return 0u;
        uint8_t body_result = emit_expr(e, n->u.tag_prefix.body);
        if (e->error != EMIT_OK) { uemit_close_block(e); return 0u; }
        (void)body_result;
        if (!uemit_close_block(e)) return 0u;

        /* Emit OP_POP_TAG. */
        uemit_pop_tag(e, tag_reg, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0u;

        /* Emit OP_JMP past the (empty) onleave handler block. */
        int jmp_past_handler_pc = (int)emit_instr_count(e);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0u, 32768u), (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0u;

        /* Onleave handler block starts here.
         * At M3, onleave is always NULL — emit nothing; just record the PC. */
        int onleave_target = (int)emit_instr_count(e);

        /* Patch OP_PUSH_TAG Bx to point to onleave handler PC. */
        emit_patch_instr(e, push_tag_pc,
            uinstr_enc_abx(OP_PUSH_TAG,
                           (uint8_t)(((flags_m3 & 0xFu) << 4) | (tag_reg & 0xFu)),
                           (uint16_t)onleave_target));

        /* Past-handler: JMP lands here. */
        {
            int past_handler_target = (int)emit_instr_count(e);
            int off = past_handler_target - (jmp_past_handler_pc + 1);
            emit_patch_instr(e, jmp_past_handler_pc,
                uinstr_enc_abx(OP_JMP, 0u, (uint16_t)(32768 + off)));
        }

        /* Return a nil register as the tag-prefix's value. */
        e->next_reg = rd;
        e->current_fs->freereg = (uint8_t)e->current_fs->nactvar;
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
        emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0u, 0u), (uint32_t)n->line);
        e->next_reg = rd + 1u;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        e->current_fs->freereg = e->next_reg;
        return rd;
    }
    case AST_LOCAL_REF:
    case AST_PARAM:
    case AST_LAZY_PARAM:
        /* These nodes are produced by the parser/emitter internally; they
         * are consumed before emit_expr is called (AST_PARAM/AST_LAZY_PARAM
         * are visited in the AST_FUNCTION arm; AST_LOCAL_REF is handled as
         * an optimised AST_IDENT).  Reaching this arm means a malformed
         * AST — treat as unsupported. */
        e->error = EMIT_UNSUPPORTED_AST;
        return 0u;
    case AST_ERROR:
        e->error = EMIT_AST_ERROR;
        return 0u;
    }
    e->error = EMIT_UNSUPPORTED_AST;
    return 0u;
}

/* --- Public API --- */

void uemit_init(UEmitter *e, UModule *module, UArena *arena,
                struct UVM *vm, const char *source_name) {
    emit_zero(e, sizeof(*e));
    e->module = module;
    e->arena = arena;
    e->vm = vm;
    if (vm != NULL) {
        module->origin_vm = vm;
    }
    emit_copy_source_name(e, source_name);
}

UEmitError uemit_statement(UEmitter *e, UAstNode *stmt) {
    uint8_t result;
    if (e->finished) return EMIT_FINISHED;
    if (e->error != EMIT_OK) return e->error;

    /* Lazy-open a top-level FuncState on first statement when the caller
     * has not already opened one.  This lets existing tests that manage
     * their own open/close continue to work unchanged. */
    if (e->current_fs == NULL) {
        if (uemit_open_function(e, NULL) == NULL) return e->error;
    }

    /* Sync the flat register cursor to the FuncState freereg so temps
     * are allocated above any declared locals. */
    e->next_reg = e->current_fs->freereg;

    result = emit_expr(e, stmt);
    if (e->error != EMIT_OK) return e->error;
    e->last_result_reg = result;
    e->any_stmt_emitted = true;

    /* Release the result temp (only if it is genuinely a temp — i.e., above
     * the local zone).  Locals keep their registers across statements. */
    if (e->next_reg > e->current_fs->freereg) {
        e->next_reg--;
    }

    return EMIT_OK;
}

UEmitError uemit_finish(UEmitter *e) {
    if (e->finished) return e->error;
    if (e->error == EMIT_OK && e->any_stmt_emitted) {
        emit_instr(e, uinstr_enc_abc(OP_RET, e->last_result_reg, 0u, 0u),
                   e->prev_line);
    }
    /* Close any lazily-opened top-level FuncState. */
    if (e->current_fs != NULL && e->current_fs->parent == NULL) {
        uemit_close_function(e);
    }
    e->finished = true;
    e->module->max_reg = e->max_reg_seen;
    return e->error;
}

const char *uemit_error_name(UEmitError code) {
    switch (code) {
    case EMIT_OK:                 return "EMIT_OK";
    case EMIT_OOM:                return "EMIT_OOM";
    case EMIT_AST_ERROR:          return "EMIT_AST_ERROR";
    case EMIT_UNSUPPORTED_AST:    return "EMIT_UNSUPPORTED_AST";
    case EMIT_REG_EXHAUSTED:      return "EMIT_REG_EXHAUSTED";
    case EMIT_CONSTANT_POOL_FULL: return "EMIT_CONSTANT_POOL_FULL";
    case EMIT_LINE_OVERFLOW:      return "EMIT_LINE_OVERFLOW";
    case EMIT_FINISHED:           return "EMIT_FINISHED";
    case EMIT_UPVAL_EXHAUSTED:    return "EMIT_UPVAL_EXHAUSTED";
    case EMIT_LOCAL_REDECLARE:    return "EMIT_LOCAL_REDECLARE";
    case EMIT_UNRESOLVED_NAME:    return "EMIT_UNRESOLVED_NAME";
    case EMIT_NESTING_TOO_DEEP:   return "EMIT_NESTING_TOO_DEEP";
    case EMIT_BARE_LAZY_FUNCTION: return "EMIT_BARE_LAZY_FUNCTION";
    case EMIT_CLOSURE_KEYWORD:    return "EMIT_CLOSURE_KEYWORD";
    case EMIT_LAZY_ON_METHOD:     return "EMIT_LAZY_ON_METHOD";
    case EMIT_LAZY_PARAM_ASSIGN:  return "EMIT_LAZY_PARAM_ASSIGN";
    }
    return "EMIT_UNKNOWN";
}

#if __STDC_HOSTED__
#  include <stdio.h>
#  include <inttypes.h>

static const char *opname(const UOpcode op) {
    switch (op) {
    case OP_LOADK:        return "LOADK";
    case OP_MOVE:         return "MOVE";
    case OP_ADD:          return "ADD";
    case OP_SUB:          return "SUB";
    case OP_MUL:          return "MUL";
    case OP_DIV:          return "DIV";
    case OP_NEG:          return "NEG";
    case OP_RET:          return "RET";
    case OP_LOADNIL:      return "LOADNIL";
    case OP_LOADBOOL:     return "LOADBOOL";
    case OP_LOADVOID:     return "LOADVOID";
    case OP_GETUPVAL:     return "GETUPVAL";
    case OP_SETUPVAL:     return "SETUPVAL";
    case OP_CLOSURE:      return "CLOSURE";
    case OP_CLOSE:        return "CLOSE";
    case OP_CALL:         return "CALL";
    case OP_JMP:          return "JMP";
    case OP_TEST:         return "TEST";
    case OP_TESTSET:      return "TESTSET";
    case OP_EQ:           return "EQ";
    case OP_NEQ:          return "NEQ";
    case OP_LT:           return "LT";
    case OP_LE:           return "LE";
    case OP_YIELD:        return "YIELD";
    case OP_FORK_DETACH:  return "FORK_DETACH";
    case OP_FORK_JOIN:    return "FORK_JOIN";
    case OP_JOIN_WAIT:    return "JOIN_WAIT";
    case OP_GETSLOT:      return "GETSLOT";
    case OP_SETSLOT:      return "SETSLOT";
    case OP_THROW:        return "THROW";
    case OP_TAG_STOP:     return "TAG_STOP";
    case OP_TRY_BEGIN:    return "TRY_BEGIN";
    case OP_TRY_END:      return "TRY_END";
    case OP_PUSH_TAG:     return "PUSH_TAG";
    case OP_POP_TAG:      return "POP_TAG";
    case OP_PUSH_FRAME_GUARD:     return "PUSH_FRAME_GUARD";
    case OP_RESUME:               return "RESUME";
    case OP_LOAD_CATCH_VALUE:     return "LOAD_CATCH_VALUE";
    case OP_MAX:                  break;
    }
    return "OP?";
}

/* snprintf into (buf+off, cap-off), advancing *off.  Returns false when
   capacity is exhausted; always null-terminates buf when cap > 0. */
static bool dis_printf(char *buf, const size_t cap, size_t *off,
                       const char *fmt, ...) {
    va_list ap;
    int n;
    if (*off >= cap) return false;
    va_start(ap, fmt);
    n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0) return false;
    if ((size_t)n >= cap - *off) {
        *off = cap - 1u;
        buf[*off] = '\0';
        return false;
    }
    *off += (size_t)n;
    return true;
}

size_t uemit_disassemble(const UModule *module, char *buf, const size_t cap) {
    size_t off;
    size_t i;
    if (cap == 0 || buf == NULL) return 0;
    buf[0] = '\0';
    off = 0;
    if (module->instr_count == 0) {
        dis_printf(buf, cap, &off, "(empty)\n");
        return off;
    }
    for (i = 0; i < module->instr_count; i++) {
        const uint32_t ins = module->instructions[i];
        const UOpcode  op  = uinstr_op(ins);
        const uint8_t  a   = uinstr_a(ins);
        bool ok;
        switch (op) {
        case OP_LOADK:
            ok = dis_printf(buf, cap, &off, "%04zu  LOADK R%u, K%u\n",
                            i, (unsigned)a, (unsigned)uinstr_bx(ins));
            break;
        case OP_RET:
            ok = dis_printf(buf, cap, &off, "%04zu  RET R%u\n",
                            i, (unsigned)a);
            break;
        case OP_NEG:
            ok = dis_printf(buf, cap, &off, "%04zu  NEG R%u, R%u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins));
            break;
        case OP_CLOSURE: {
            const uint16_t bx = uinstr_bx(ins);
            ok = dis_printf(buf, cap, &off, "%04zu  CLOSURE R%u, P%u\n",
                            i, (unsigned)a, (unsigned)bx);
            if (!ok) return off;
            if (bx < module->nested_count && module->nested[bx] != NULL) {
                const UProto *child = module->nested[bx];
                uint8_t u;
                for (u = 0; u < child->nupvals &&
                     (i + 1u + (size_t)u) < module->instr_count; u++) {
                    uint32_t pi = module->instructions[i + 1u + u];
                    ok = dis_printf(buf, cap, &off,
                        "    upval[%u]: %s parent_idx=%u\n",
                        (unsigned)u,
                        uinstr_b(pi) ? "in_stack" : "from_upval",
                        (unsigned)uinstr_c(pi));
                    if (!ok) return off;
                }
                i += child->nupvals;  /* skip upvalue prelude instructions */
            }
            break;
        }
        case OP_JMP:
            ok = dis_printf(buf, cap, &off, "%04zu  JMP %d\n",
                            i, (int)uinstr_bx(ins) - 32768);
            break;
        case OP_LOADNIL:
            ok = dis_printf(buf, cap, &off, "%04zu  LOADNIL R%u\n",
                            i, (unsigned)a);
            break;
        case OP_LOADBOOL:
            ok = dis_printf(buf, cap, &off, "%04zu  LOADBOOL R%u, %s%s\n",
                            i, (unsigned)a,
                            uinstr_b(ins) ? "true" : "false",
                            uinstr_c(ins) ? " (skip)" : "");
            break;
        case OP_LOADVOID:
            ok = dis_printf(buf, cap, &off, "%04zu  LOADVOID R%u\n",
                            i, (unsigned)a);
            break;
        case OP_GETUPVAL:
            ok = dis_printf(buf, cap, &off, "%04zu  GETUPVAL R%u, U%u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins));
            break;
        case OP_SETUPVAL:
            ok = dis_printf(buf, cap, &off, "%04zu  SETUPVAL U%u, R%u\n",
                            i, (unsigned)uinstr_b(ins), (unsigned)a);
            break;
        case OP_CLOSE:
            ok = dis_printf(buf, cap, &off, "%04zu  CLOSE R%u..\n",
                            i, (unsigned)a);
            break;
        case OP_CALL:
            ok = dis_printf(buf, cap, &off, "%04zu  CALL R%u, %d args, %d results\n",
                            i, (unsigned)a,
                            (int)uinstr_b(ins) - 1,
                            (int)uinstr_c(ins) - 1);
            break;
        case OP_TEST:
            ok = dis_printf(buf, cap, &off, "%04zu  TEST R%u, %s\n",
                            i, (unsigned)a,
                            uinstr_c(ins) ? "skip-if-truthy" : "skip-if-falsy");
            break;
        case OP_TESTSET:
            ok = dis_printf(buf, cap, &off, "%04zu  TESTSET R%u, R%u, %u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
            break;
        case OP_EQ:
            ok = dis_printf(buf, cap, &off, "%04zu  EQ %s R%u, R%u\n",
                            i, a ? "==" : "!=",
                            (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
            break;
        case OP_NEQ:
            ok = dis_printf(buf, cap, &off, "%04zu  NEQ R%u, R%u\n",
                            i, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
            break;
        case OP_LT:
            ok = dis_printf(buf, cap, &off, "%04zu  LT R%u, R%u (%s)\n",
                            i, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins),
                            a ? "<" : ">=");
            break;
        case OP_LE:
            ok = dis_printf(buf, cap, &off, "%04zu  LE R%u, R%u (%s)\n",
                            i, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins),
                            a ? "<=" : ">");
            break;
        case OP_YIELD:
            ok = dis_printf(buf, cap, &off, "%04zu  YIELD\n", i);
            break;
        case OP_FORK_DETACH:
            ok = dis_printf(buf, cap, &off, "%04zu  FORK_DETACH (reserved)\n", i);
            break;
        case OP_FORK_JOIN:
            ok = dis_printf(buf, cap, &off, "%04zu  FORK_JOIN (reserved)\n", i);
            break;
        case OP_JOIN_WAIT:
            ok = dis_printf(buf, cap, &off, "%04zu  JOIN_WAIT (reserved)\n", i);
            break;
        case OP_GETSLOT:
            ok = dis_printf(buf, cap, &off, "%04zu  GETSLOT (reserved M4)\n", i);
            break;
        case OP_SETSLOT:
            ok = dis_printf(buf, cap, &off, "%04zu  SETSLOT (reserved M4)\n", i);
            break;
        default:
            ok = dis_printf(buf, cap, &off, "%04zu  %s R%u, R%u, R%u\n",
                            i, opname(op), (unsigned)a,
                            (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
            break;
        }
        if (!ok) return off;
    }
    if (!dis_printf(buf, cap, &off, "; constants:\n")) return off;
    for (i = 0; i < module->const_count; i++) {
        bool ok;
        if (module->constants[i].kind == (uint8_t)UVAL_INT) {
            ok = dis_printf(buf, cap, &off, ";   K%zu = INT %" PRId64 "\n",
                            i, module->constants[i].v.i);
        } else {
            ok = dis_printf(buf, cap, &off, ";   K%zu = ?\n", i);
        }
        if (!ok) return off;
    }
    return off;
}

#else  /* freestanding */

size_t uemit_disassemble(const UModule *module, char *buf, const size_t cap) {
    (void)module;
    if (cap > 0 && buf != NULL) buf[0] = '\0';
    return 0;
}

#endif  /* __STDC_HOSTED__ */

/* Compute total serialized byte count.  Must match the write path
   in umodule_serialize byte-for-byte. */
static size_t module_wire_size(const UModule *c) {
    size_t i;
    size_t n = 24u;                                   /* fixed header */
    size_t src_len;

    /* metadata */
    n += 1u;                                          /* max_reg */
    src_len = (c->source_name != NULL) ? emit_strlen(c->source_name) : 0u;
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
    buf[4] = (uint8_t)URBI_BYTECODE_VERSION_BYTE;  /* version v1.2 */
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
    src_len = (module->source_name != NULL) ? emit_strlen(module->source_name) : 0u;
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

/* --- M2 upvalue cascade --- */

/* Find `name` (interned) as a local in fs's actvars. Returns slot or -1. */
static int local_lookup_for_upvalue(UFuncState *fs, const char *name) {
    for (int i = fs->nactvar - 1; i >= 0; i--) {
        if (fs->actvars[i].name == name) return fs->actvars[i].slot;
    }
    return -1;
}

/* Find `name` already installed in fs's upvalue table. Returns idx or -1. */
static int upvalue_lookup(const UFuncState *fs, const char *name) {
    for (int i = 0; i < fs->nupvalues; i++) {
        if (fs->upvalues[i].name == name) return i;
    }
    return -1;
}

/* Install a new upvalue descriptor on fs. Returns the new index, or -1
 * (sets EMIT_UPVAL_EXHAUSTED) on overflow. */
static int upvalue_install(UEmitter *e, UFuncState *fs,
                           const char *name, int name_len,
                           uint8_t parent_idx, bool in_stack) {
    if (fs->nupvalues >= UFS_MAX_UPVALUES) {
        e->error = EMIT_UPVAL_EXHAUSTED;
        return -1;
    }
    int idx = fs->nupvalues++;
    fs->upvalues[idx].name     = name;
    fs->upvalues[idx].name_len = name_len;
    fs->upvalues[idx].idx      = parent_idx;
    fs->upvalues[idx].in_stack = in_stack;
    return idx;
}

/* Recursive upvalue cascade. Returns the upvalue index in fs's table, or -1
 * if name is not found in any enclosing scope. Marks the parent's actvar and
 * enclosing block as captured so OP_CLOSE fires on block exit. */
int find_or_install_upvalue(UEmitter *e, UFuncState *fs,
                            const char *name, int name_len) {
    if (fs->parent == NULL) return -1;       /* no enclosing scope to capture from */

    /* Short-circuit: already installed in this function's upvalue table. */
    int existing = upvalue_lookup(fs, name);
    if (existing >= 0) return existing;

    /* Check immediate parent's locals. */
    int parent_local = local_lookup_for_upvalue(fs->parent, name);
    if (parent_local >= 0) {
        /* Mark the parent actvar as captured and flag the enclosing block. */
        for (int i = 0; i < fs->parent->nactvar; i++) {
            if (fs->parent->actvars[i].name == name) {
                fs->parent->actvars[i].is_captured = true;
                /* Find the innermost block in parent that contains this local. */
                for (int b = fs->parent->nblocks - 1; b >= 0; b--) {
                    if (fs->parent->blocks[b].first_local_idx <= i) {
                        fs->parent->blocks[b].has_captured = true;
                        break;
                    }
                }
                break;
            }
        }
        return upvalue_install(e, fs, name, name_len,
                               (uint8_t)parent_local, true);
    }

    /* Recurse into grandparent — intermediate frame captures via upvalue
     * (in_stack=false). */
    int grand_idx = find_or_install_upvalue(e, fs->parent, name, name_len);
    if (grand_idx < 0) return -1;
    return upvalue_install(e, fs, name, name_len,
                           (uint8_t)grand_idx, false);
}

/* --- M2 UFuncState lifecycle --- */

UFuncState *uemit_open_function(UEmitter *e, UFuncState *parent) {
    if (e->error != EMIT_OK) return NULL;

    UFuncState *fs = uarena_alloc(e->arena, sizeof(UFuncState));
    if (fs == NULL) {
        e->error = EMIT_OOM;
        return NULL;
    }
    /* zero-init via byte-loop — UFuncState is POD */
    emit_zero(fs, sizeof(UFuncState));
    fs->parent = parent;
    fs->target_proto = NULL;            /* T14 wires nested-proto bufs */
    e->current_fs = fs;
    return fs;
}

UFuncState *uemit_close_function(UEmitter *e) {
    UFuncState *fs = e->current_fs;
    if (fs == NULL) return NULL;
    /* Roll max_reg_seen into target_proto when closing a nested function. */
    if (fs->target_proto != NULL) {
        UProto *p = (UProto *)fs->target_proto;
        p->max_reg  = fs->max_reg_seen;
        p->nupvals  = (uint8_t)fs->nupvalues;
    }
    /* Restore prev_line to a sentinel so the parent proto's next instruction
     * bootstraps a fresh abs checkpoint after re-entering. This is a subtle
     * correctness point: without this reset, the first instruction of the
     * parent emitted after the CLOSURE prelude would compute a delta against
     * the last line of the child body, producing a wrong delta. */
    if (fs->parent != NULL && fs->target_proto != NULL) {
        e->prev_line = 0u;
    }
    e->current_fs = fs->parent;
    return fs;
}

int uemit_declare_local(UEmitter *e, const char *name, int name_len) {
    UFuncState *fs = e->current_fs;
    if (fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return -1;
    }
    /* Search current block (or whole actvars table at no-block scope) for
     * duplicate. T7 will use blocks[].first_local_idx; for T6 we just
     * scan from 0. */
    int search_from = (fs->nblocks > 0)
                    ? fs->blocks[fs->nblocks - 1].first_local_idx
                    : 0;
    for (int i = search_from; i < fs->nactvar; i++) {
        if (fs->actvars[i].name == name) {
            e->error = EMIT_LOCAL_REDECLARE;
            return -1;
        }
    }
    if (fs->nactvar >= UFS_MAX_LOCALS) {
        e->error = EMIT_REG_EXHAUSTED;
        return -1;
    }
    ULocalVar *lv = &fs->actvars[fs->nactvar];
    lv->name = name;
    lv->name_len = name_len;
    lv->slot = fs->freereg;
    lv->is_captured = false;
    lv->is_lazy = false;
    fs->nactvar++;
    fs->freereg++;
    if (fs->freereg > fs->max_reg_seen) fs->max_reg_seen = fs->freereg;
    return lv->slot;
}

bool uemit_open_block(UEmitter *e, bool is_loop) {
    UFuncState *fs = e->current_fs;
    if (fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return false;
    }
    if (fs->nblocks >= UFS_MAX_BLOCKS) {
        e->error = EMIT_NESTING_TOO_DEEP;
        return false;
    }
    UBlockCtx *blk = &fs->blocks[fs->nblocks++];
    blk->nactvar_on_enter = fs->nactvar;
    blk->first_local_idx = fs->nactvar;
    blk->is_loop = is_loop;
    blk->has_captured = false;
    blk->break_chain = -1;
    blk->continue_chain = -1;
    return true;
}

bool uemit_close_block(UEmitter *e) {
    UFuncState *fs = e->current_fs;
    if (fs == NULL || fs->nblocks == 0) {
        e->error = EMIT_UNSUPPORTED_AST;
        return false;
    }

    const UBlockCtx *blk = &fs->blocks[fs->nblocks - 1];

    /* Per allocator spec: emit OP_CLOSE if any local in this block was
     * captured. base = first slot to close = first_local_idx. */
    if (blk->has_captured) {
        uint32_t i = uinstr_enc_abc(OP_CLOSE, (uint8_t)blk->first_local_idx, 0u, 0u);
        emit_instr(e, i, e->prev_line);
    }

    /* Pop actvars back to entry snapshot; restore freereg.
     * After block exit, freereg falls back to first_local_idx (locals
     * are gone; temps above had already been freed at statement
     * boundaries). */
    fs->nactvar = blk->nactvar_on_enter;
    fs->freereg = (uint8_t)fs->nactvar;
    fs->nblocks--;
    return true;
}

void uemit_emit_loop_back_close(UEmitter *e) {
    UFuncState *fs = e->current_fs;
    if (fs == NULL || fs->nblocks == 0) return;
    const UBlockCtx *blk = &fs->blocks[fs->nblocks - 1];
    if (blk->is_loop && blk->has_captured) {
        uint32_t i = uinstr_enc_abc(OP_CLOSE, (uint8_t)blk->first_local_idx, 0u, 0u);
        emit_instr(e, i, e->prev_line);
    }
}

/* =========================================================================
 * M3 row 7 control-transfer opcode encoder helpers.
 * Each function encodes exactly one instruction word and calls emit_instr.
 * See umodule.h §M3 row 7 for the field layout of each opcode.
 * ========================================================================= */

/* OP_THROW ABx: A = reg_value, Bx = 0 (unused). */
void uemit_throw(UEmitter *e, uint8_t reg_value, uint32_t line) {
    emit_instr(e, uinstr_enc_abx(OP_THROW, reg_value, 0u), line);
}

/* OP_TAG_STOP ABC: A = reg_tag, B = reg_value, C = 0. */
void uemit_tag_stop(UEmitter *e, uint8_t reg_tag, uint8_t reg_value, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_TAG_STOP, reg_tag, reg_value, 0u), line);
}

/* OP_TRY_BEGIN ABx: A = flags byte, Bx = handler PC (16-bit, range 0-65535).
 * flags bits: bit 0 = has_catch, bit 1 = has_finally (defined by T9/T10). */
void uemit_try_begin(UEmitter *e, uint8_t flags, uint16_t handler_pc, uint32_t line) {
    emit_instr(e, uinstr_enc_abx(OP_TRY_BEGIN, flags, handler_pc), line);
}

/* OP_TRY_END ABC: no operands (all zero). Pops top cleanup entry. */
void uemit_try_end(UEmitter *e, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_TRY_END, 0u, 0u, 0u), line);
}

/* OP_PUSH_TAG ABx: A packs flags nibble and tag_reg nibble.
 *   A[7:4] = flags (4 bits, values 0-15)
 *   A[3:0] = tag_reg (4 bits, values 0-15)
 *   Bx     = onleave PC (16-bit, range 0-65535)
 * tag_reg must be in [0,15]; flags must be in [0,15]. T30 revisits
 * if wider operand ranges become necessary. */
void uemit_push_tag(UEmitter *e, uint8_t reg_tag, uint8_t flags,
                    uint16_t onleave_pc, uint32_t line) {
    uint8_t a = (uint8_t)(((flags & 0xFu) << 4) | (reg_tag & 0xFu));
    emit_instr(e, uinstr_enc_abx(OP_PUSH_TAG, a, onleave_pc), line);
}

/* OP_POP_TAG ABC: A = reg_tag, B = C = 0. */
void uemit_pop_tag(UEmitter *e, uint8_t reg_tag, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_POP_TAG, reg_tag, 0u, 0u), line);
}

/* OP_PUSH_FRAME_GUARD ABC: A = register_base, B = register_count, C = 0. */
void uemit_push_frame_guard(UEmitter *e, uint8_t register_base,
                             uint8_t register_count, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_PUSH_FRAME_GUARD, register_base,
                                  register_count, 0u), line);
}

/* OP_RESUME ABC: A = reg_state, B = C = 0. */
void uemit_resume(UEmitter *e, uint8_t reg_state, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_RESUME, reg_state, 0u, 0u), line);
}

/* OP_LOAD_CATCH_VALUE ABC: A = destination register, B = C = 0.
 * T10 empirical addition — loads s->catch_value into R[A] at handler entry. */
void uemit_load_catch_value(UEmitter *e, uint8_t reg, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_LOAD_CATCH_VALUE, reg, 0u, 0u), line);
}
