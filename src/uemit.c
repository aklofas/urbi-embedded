/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode emitter. */

#include "uemit.h"
#include "uemit_internal.h"
#include "uintern.h"
#include "uvarint.h"

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
   pool-full (> UINT16_MAX entries) or OOM. */
static uint16_t add_const_int(UEmitter *e, const int64_t v) {
    size_t i;
    for (i = 0; i < e->module->const_count; i++) {
        if (e->module->constants[i].kind == (uint8_t)UVAL_INT
         && e->module->constants[i].v.i == v) {
            return (uint16_t)i;
        }
    }
    if (e->module->const_count > (size_t)UINT16_MAX) {
        e->error = EMIT_CONSTANT_POOL_FULL;
        return 0u;
    }
    if (!emit_grow(e->module, (void **)&e->module->constants, &e->module->const_cap,
                   e->module->const_count + 1u, sizeof(UValue))) {
        e->error = EMIT_OOM;
        return 0u;
    }
    {
        const size_t idx = e->module->const_count;
        int p;
        e->module->constants[idx].kind = (uint8_t)UVAL_INT;
        /* Clear pad bytes for deterministic serialization. */
        for (p = 0; p < 7; p++) e->module->constants[idx]._pad[p] = 0u;
        e->module->constants[idx].v.i = v;
        e->module->const_count++;
        return (uint16_t)idx;
    }
}

/* Append one absolute-line checkpoint to abs_lines.  Uses emit_grow. */
static void emit_push_abs_line(UEmitter *e, const uint32_t pc, const uint32_t line) {
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
   incremented so the new slot is at [instr_count - 1]. */
static void emit_push_line_delta(UEmitter *e, const int8_t delta) {
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
   No-op when e->error is already set. */
static void emit_instr(UEmitter *e, const uint32_t ins, const uint32_t line) {
    uint32_t pc;
    int8_t delta;
    bool needs_abs;

    if (e->error != EMIT_OK) return;
    if (line > (uint32_t)INT32_MAX) { e->error = EMIT_LINE_OVERFLOW; return; }
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
        for (int i = fs->nactvar - 1; i >= 0; i--) {
            if (fs->actvars[i].name == canonical) {
                slot = (int)fs->actvars[i].slot;
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
        ULocalVar *lv = &fs->actvars[fs->nactvar++];
        lv->name       = canonical;
        lv->name_len   = n->u.var_decl.name_len;
        lv->slot       = reg_before;
        lv->is_captured = false;
        lv->is_lazy    = false;

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
        int jmp_to_else = (int)e->module->instr_count;
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
        int jmp_to_end = (int)e->module->instr_count;
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0u, 32768u), (uint32_t)n->line);

        /* 7. Patch jmp_to_else → current pc (start of else/nil arm). */
        {
            int alt_target = (int)e->module->instr_count;
            int alt_offset = alt_target - (jmp_to_else + 1);
            e->module->instructions[jmp_to_else] =
                uinstr_enc_abx(OP_JMP, 0u, (uint16_t)(32768 + alt_offset));
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
            int end_target = (int)e->module->instr_count;
            int end_offset = end_target - (jmp_to_end + 1);
            e->module->instructions[jmp_to_end] =
                uinstr_enc_abx(OP_JMP, 0u, (uint16_t)(32768 + end_offset));
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

        int loop_start = (int)e->module->instr_count;

        /* 1. Compile cond into rx. */
        uint8_t rx = e->next_reg;
        emit_expr(e, n->u.while_stmt.cond);
        if (e->error != EMIT_OK) return 0u;

        /* 2. TEST rx, 0, 1 — skip JMP-to-exit when cond is truthy. */
        emit_instr(e, uinstr_enc_abc(OP_TEST, rx, 0u, 1u), (uint32_t)n->line);

        /* 3. JMP placeholder to exit (patched later). */
        int jmp_to_exit = (int)e->module->instr_count;
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
                int back_offset = loop_start - ((int)e->module->instr_count + 1);
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
            int exit_target = (int)e->module->instr_count;
            int exit_offset = exit_target - (jmp_to_exit + 1);
            e->module->instructions[jmp_to_exit] =
                uinstr_enc_abx(OP_JMP, 0u, (uint16_t)(32768 + exit_offset));
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
    case OP_MAX:          break;
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
        case OP_CLOSURE:
            ok = dis_printf(buf, cap, &off, "%04zu  CLOSURE R%u, P%u\n",
                            i, (unsigned)a, (unsigned)uinstr_bx(ins));
            break;
        case OP_JMP:
            ok = dis_printf(buf, cap, &off, "%04zu  JMP %d\n",
                            i, (int)uinstr_bx(ins) - 32768);
            break;
        case OP_LOADNIL:
            ok = dis_printf(buf, cap, &off, "%04zu  LOADNIL R%u\n",
                            i, (unsigned)a);
            break;
        case OP_LOADBOOL:
            ok = dis_printf(buf, cap, &off, "%04zu  LOADBOOL R%u, %u, %u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
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
            ok = dis_printf(buf, cap, &off, "%04zu  SETUPVAL R%u, U%u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins));
            break;
        case OP_CLOSE:
            ok = dis_printf(buf, cap, &off, "%04zu  CLOSE R%u\n",
                            i, (unsigned)a);
            break;
        case OP_CALL:
            ok = dis_printf(buf, cap, &off, "%04zu  CALL R%u, %u, %u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
            break;
        case OP_TEST:
            ok = dis_printf(buf, cap, &off, "%04zu  TEST R%u, %u\n",
                            i, (unsigned)a, (unsigned)uinstr_c(ins));
            break;
        case OP_TESTSET:
            ok = dis_printf(buf, cap, &off, "%04zu  TESTSET R%u, R%u, %u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
            break;
        case OP_EQ:
            ok = dis_printf(buf, cap, &off, "%04zu  EQ %u, R%u, R%u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
            break;
        case OP_NEQ:
            ok = dis_printf(buf, cap, &off, "%04zu  NEQ %u, R%u, R%u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
            break;
        case OP_LT:
            ok = dis_printf(buf, cap, &off, "%04zu  LT %u, R%u, R%u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
            break;
        case OP_LE:
            ok = dis_printf(buf, cap, &off, "%04zu  LE %u, R%u, R%u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
            break;
        case OP_YIELD:
            ok = dis_printf(buf, cap, &off, "%04zu  YIELD\n", i);
            break;
        case OP_FORK_DETACH:
            ok = dis_printf(buf, cap, &off, "%04zu  FORK_DETACH\n", i);
            break;
        case OP_FORK_JOIN:
            ok = dis_printf(buf, cap, &off, "%04zu  FORK_JOIN\n", i);
            break;
        case OP_JOIN_WAIT:
            ok = dis_printf(buf, cap, &off, "%04zu  JOIN_WAIT\n", i);
            break;
        case OP_GETSLOT:
            ok = dis_printf(buf, cap, &off, "%04zu  GETSLOT R%u, R%u, %u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
            break;
        case OP_SETSLOT:
            ok = dis_printf(buf, cap, &off, "%04zu  SETSLOT R%u, R%u, %u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
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
    buf[4] = 0x11u;              /* version v1.1 */
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
    /* T14: roll fs->max_reg_seen into target_proto->max_reg.
     * At T6, current_fs unwinds to parent; no proto wiring yet. */
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
