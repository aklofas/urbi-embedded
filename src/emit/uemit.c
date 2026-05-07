/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode emitter. */

#include "emit/uemit_internal.h"
#include "runtime/umacros.h"
#include "value/uintern.h"
#include "watcher/uwatcher.h"  /* UWATCHER_AT / _AT_SYNC / _WHENEVER — AST_WATCHER emit */

#include <limits.h>
#include <stddef.h>

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


/* Minimum register that freereg/next_reg may be reset to when releasing temps.
 * Normally equals nactvar (frame locals occupy [0, nactvar)).
 * If a global slot register has been pre-reserved (global_slot_reserved), the
 * global_slot register sits BELOW the first local: all nactvar locals occupy
 * [r_global_slot+1 .. r_global_slot+nactvar].  The temp zone then starts at
 * r_global_slot + nactvar + 1 = nactvar + 1 (when r_global_slot == 0).
 * More precisely: floor = nactvar + (global_slot_reserved ? 1 : 0).
 *
 * This formula holds because:
 *   - Without pre-reservation: locals occupy [0, nactvar), floor = nactvar.
 *   - With pre-reservation: r_global_slot is at index `nparams` (just above
 *     all params), locals follow at nparams+1 .. nparams+nactvar_excluding_params.
 *     But since params are counted in nactvar, the formula simplifies to
 *     nactvar + 1 in all cases where r_global_slot is placed at freereg
 *     (i.e., exactly once, between params and first body local). */
uint8_t fs_temp_floor(const UFuncState *fs) {
    uint8_t floor_val = (uint8_t)fs->nactvar;
    if (fs->global_slot_reserved) {
        floor_val = (uint8_t)(fs->nactvar + 1u);
    }
    return floor_val;
}

/* Linear-scan dedup over the integer pool.  Returns existing index if
   a UVAL_INT entry with the same value already exists; otherwise appends
   a new entry and returns its index.  Sets e->error and returns 0 on
   pool-full (> UINT16_MAX entries) or OOM.
   Routes to the nested UProto constant pool when in a nested function.
   Promoted from static so uemit_expr.c (T12) can call it cross-TU. */
uint16_t add_const_int(UEmitter *e, const int64_t v) {
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
void emit_instr(UEmitter *e, const uint32_t ins, const uint32_t line) {
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
void emit_patch_instr(UEmitter *e, int pc, uint32_t new_instr) {
    UProto *p = current_proto(e);
    if (p != NULL) {
        p->instructions[pc] = new_instr;
    } else {
        e->module->instructions[pc] = new_instr;
    }
}

/* Return the current instruction count in the active proto. */
size_t emit_instr_count(const UEmitter *e) {
    UProto *p = current_proto(e);
    if (p != NULL) return p->instr_count;
    return e->module->instr_count;
}

/* Map UAstBinaryOp to the corresponding arithmetic opcode.
 * Promoted from static so uemit_expr.c (T12) can call it cross-TU. */
UOpcode binop_to_opcode(const UAstBinaryOp op) {
    switch (op) {
    case BOP_ADD: return OP_ADD;
    case BOP_SUB: return OP_SUB;
    case BOP_MUL: return OP_MUL;
    case BOP_DIV: return OP_DIV;
    }
    /* unreachable — parser produces only these four. */
    return OP_ADD;
}

/* Forward declaration (emit_lazy_thunk + uemit_unwind.c call emit_expr). */
uint8_t emit_expr(UEmitter *e, UAstNode *n);

/* T31: Best-effort compile-time check — returns true when `n` contains a
 * direct write operation (AST_ASSIGN, AST_VAR_DECL, AST_MEMBER_SET,
 * AST_PROP_SET).  Used to warn when a watcher condition silently mutates
 * state.  AST_CALL is treated as opaque (returns false) to avoid false
 * positives on read-only methods.  Recurses through compound nodes;
 * the parser already caps nesting so stack overflow is not a concern.
 * Exported for unit tests via uemit.h test-friend section. */
bool cond_has_direct_side_effect(UAstNode *n) {
    if (n == NULL) return false;
    switch (n->kind) {
        case AST_ASSIGN:
        case AST_VAR_DECL:
        case AST_MEMBER_SET:
        case AST_PROP_SET:
            return true;
        case AST_NARY: {
            int i;
            for (i = 0; i < n->u.nary.count; i++)
                if (cond_has_direct_side_effect(n->u.nary.children[i])) return true;
            return false;
        }
        case AST_BIN_SEP:
            return cond_has_direct_side_effect(n->u.bin_sep.lhs)
                || cond_has_direct_side_effect(n->u.bin_sep.rhs);
        case AST_BINARY:
            return cond_has_direct_side_effect(n->u.binary.lhs)
                || cond_has_direct_side_effect(n->u.binary.rhs);
        case AST_UNARY:
            return cond_has_direct_side_effect(n->u.unary.operand);
        case AST_COMPARE:
            return cond_has_direct_side_effect(n->u.cmp.lhs)
                || cond_has_direct_side_effect(n->u.cmp.rhs);
        case AST_BLOCK: {
            int i;
            for (i = 0; i < n->u.block.count; i++)
                if (cond_has_direct_side_effect(n->u.block.stmts[i])) return true;
            return false;
        }
        case AST_CALL:   return false;  /* opaque — best-effort only */
        default:         return false;
    }
}

/* emit_lazy_thunk, emit_function_literal, and the statement arm helpers
 * (emit_if_arm, emit_while_arm, emit_call_arm, emit_return_arm,
 * emit_function_arm) live in uemit_stmt.c.
 * emit_catch_handler_section, emit_try_frame, and the unwind arm helpers
 * (emit_throw_arm, emit_try_arm, emit_tag_prefix_arm) live in
 * uemit_unwind.c.  See uemit_internal.h for all their declarations. */

/* AST walker — returns the register holding the result of the expression.
   Returns 0 and sets e->error on any failure. */
uint8_t emit_expr(UEmitter *e, UAstNode *n) {
    if (e->error != EMIT_OK) return 0u;
    /* Default arm returns EMIT_UNSUPPORTED_AST for AST kinds not yet
       emitted by this milestone. Later tasks will add explicit case arms as
       each construct's emit lands; the NOLINT suppresses clang's switch-
       exhaustiveness warning until that work completes. */
    // NOLINTNEXTLINE(clang-diagnostic-switch)
    switch (n->kind) {
    case AST_INT:        return emit_int_arm(e, n);
    case AST_BOOL:       return emit_bool_arm(e, n);
    case AST_NIL:        return emit_nil_arm(e, n);
    case AST_NOOP:       return emit_noop_arm(e, n);
    case AST_UNARY:      return emit_unary_arm(e, n);
    case AST_BINARY:     return emit_binary_arm(e, n);
    case AST_COMPARE:    return emit_compare_arm(e, n);
    case AST_IDENT:      return emit_ident_arm(e, n);
    case AST_VAR_DECL:   return emit_var_decl_arm(e, n);
    case AST_ASSIGN:     return emit_assign_arm(e, n);
    case AST_NARY:       return emit_nary_arm(e, n);
    case AST_BIN_SEP:    return emit_bin_sep_arm(e, n);
    case AST_BLOCK:      return emit_block_arm(e, n);
    case AST_IF:         return emit_if_arm(e, n);
    case AST_WHILE:    return emit_while_arm(e, n);
    case AST_CALL:     return emit_call_arm(e, n);
    case AST_RETURN:   return emit_return_arm(e, n);
    case AST_FUNCTION: return emit_function_arm(e, n);
    case AST_THROW:      return emit_throw_arm(e, n);
    case AST_TRY:        return emit_try_arm(e, n);
    case AST_TAG_PREFIX: return emit_tag_prefix_arm(e, n);
    case AST_MEMBER_GET:     return emit_member_get_arm(e, n);
    case AST_MEMBER_SET:     return emit_member_set_arm(e, n);
    case AST_WATCHER:        return emit_watcher_arm(e, n);
    case AST_WAITUNTIL:      return emit_waituntil_arm(e, n);
    case AST_AT_EVENT:       return emit_at_event_arm(e, n);
    case AST_AT_SLOT_CHANGE: return emit_at_slot_change_arm(e, n);
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


void uemit_init(UEmitter *e, UModule *module, UArena *arena,
                struct UVM *vm, const char *source_name) {
    urbi_zero(e, sizeof(*e));
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
    case EMIT_TOO_MANY_IC_SITES:           return "EMIT_TOO_MANY_IC_SITES";
    case EMIT_RESERVED_KEYWORD_AS_IDENT:   return "EMIT_RESERVED_KEYWORD_AS_IDENT";
    }
    return "EMIT_UNKNOWN";
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
    urbi_zero(fs, sizeof(UFuncState));
    fs->parent = parent;
    fs->target_proto = NULL;            /* T14 wires nested-proto bufs */

    /* T73: For the chunk-top funcstate (parent == NULL), pre-reserve
     * r_global_slot = 0 unconditionally, mirroring what emit_function_literal
     * does for nested functions.  This ensures r_global_slot is never
     * overwritten by a condition register (rd) in an if/while expression —
     * both would land at index 0 without the pre-reservation.  The prologue
     * OP_LOAD_REALM_GLOBAL is still only emitted iff references_global is
     * true; the reserved register is simply unused for pure-local chunks. */
    if (parent == NULL && fs->freereg < (uint8_t)(UFS_MAX_REGS - 1)) {
        fs->r_global_slot      = fs->freereg;
        fs->global_slot_reserved = true;
        fs->freereg++;
        if (fs->freereg > fs->max_reg_seen)
            fs->max_reg_seen = fs->freereg;
    }

    e->current_fs = fs;
    return fs;
}

/* M4 T15: assign the next IC index for the current function.  Allocates
 * fs->ic_names lazily in 16/32/64/128/256-slot chunks via the module
 * allocator (matches the rest of the emitter — funcstate itself is
 * arena-allocated, but variable-sized side tables go through the module
 * allocator so they survive into the proto and are freed via
 * umodule_proto_destroy_buffers). */
int uemit_assign_ic_index(UEmitter *e, USymbol *name) {
    if (e == NULL || e->current_fs == NULL) return -1;
    UFuncState *fs = e->current_fs;
    if (fs->ic_next >= 256u) {
        e->error = EMIT_TOO_MANY_IC_SITES;
        return -1;
    }
    if (fs->ic_next >= fs->ic_names_cap) {
        uint16_t new_cap = (fs->ic_names_cap == 0u) ? 16u
            : (fs->ic_names_cap < 128u ? (uint16_t)(fs->ic_names_cap * 2u) : 256u);
        UModuleAllocFn alloc = emit_alloc_for(e->module);
        if (alloc == NULL) { e->error = EMIT_OOM; return -1; }
        USymbol **fresh = (USymbol **)alloc(fs->ic_names,
                                            (size_t)new_cap * sizeof(USymbol *),
                                            e->module->alloc_ud);
        if (fresh == NULL) { e->error = EMIT_OOM; return -1; }
        fs->ic_names = fresh;
        fs->ic_names_cap = new_cap;
    }
    int idx = (int)fs->ic_next++;
    fs->ic_names[idx] = name;
    return idx;
}

/* T73: Prepend one instruction to the current function's instruction buffer.
 *
 * Routes to UProto.instructions / line_deltas (nested function) or
 * UModule.instructions / line_deltas (chunk root), mirroring the routing
 * done by emit_instr.
 *
 * All abs_lines entries' pc values are bumped by 1 because every existing
 * instruction is now at pc+1.  The prepended instruction itself gets a
 * line_delta entry of INT8_MIN (abs-checkpoint sentinel) with an abs_line
 * entry at pc=0 pointing at the first pre-existing instruction's line so
 * that a debugger can attribute the prologue to the function's start.
 *
 * Note on line_deltas capacity: line_deltas is resized to exactly
 * instr_count bytes on each append (no separate cap field); the prepend
 * reallocates it to (old_count + 1) bytes via the same realloc semantics.
 *
 * Returns true on success, false on OOM (sets e->error). */
static bool prologue_prepend_instr(UEmitter *e, uint32_t instr) {
    UProto *p = (e->current_fs && e->current_fs->target_proto)
                ? (UProto *)e->current_fs->target_proto
                : NULL;

    if (p != NULL) {
        /* === Nested proto path === */

        /* Instructions: grow by 1, shift right, insert at [0]. */
        if (!proto_grow(e->module, p,
                        (void **)&p->instructions, &p->instr_cap,
                        p->instr_count + 1u, sizeof(uint32_t))) {
            e->error = EMIT_OOM; return false;
        }
        if (p->instr_count > 0u) {
            emit_memmove_right(p->instructions + 1, p->instructions,
                               p->instr_count * sizeof(uint32_t));
        }
        p->instructions[0] = instr;
        p->instr_count++;

        /* Patch instructions that store absolute PCs in their Bx field.
         * OP_TRY_BEGIN Bx = handler_pc; OP_PUSH_TAG Bx = onleave_pc.
         * All targets shifted right by 1 — increment by 1. */
        for (size_t pi = 1u; pi < p->instr_count; pi++) {
            UOpcode op = uinstr_op(p->instructions[pi]);
            if (op == OP_TRY_BEGIN || op == OP_PUSH_TAG) {
                uint8_t  a  = uinstr_a(p->instructions[pi]);
                uint16_t bx = uinstr_bx(p->instructions[pi]);
                p->instructions[pi] = uinstr_enc_abx(op, a, (uint16_t)(bx + 1u));
            }
        }

        /* line_deltas: resize to new instr_count, shift right, insert at [0].
         * line_deltas uses no separate cap — allocate exactly instr_count bytes. */
        {
            UModuleAllocFn alloc = p->alloc_fn;
#if __STDC_HOSTED__
            if (alloc == NULL) alloc = emit_stdlib_alloc;
#else
            if (alloc == NULL) { e->error = EMIT_OOM; return false; }
#endif
            int8_t *fresh = (int8_t *)alloc(p->line_deltas,
                                            p->instr_count * sizeof(int8_t),
                                            p->alloc_ud);
            if (fresh == NULL) { e->error = EMIT_OOM; return false; }
            p->line_deltas = fresh;
        }
        if (p->instr_count > 1u) {
            emit_memmove_right(p->line_deltas + 1, p->line_deltas,
                               (p->instr_count - 1u) * sizeof(int8_t));
        }
        p->line_deltas[0] = (int8_t)-128;   /* abs-checkpoint sentinel */

        /* Bump all existing abs_lines pc values. */
        for (size_t ai = 0; ai < p->abs_line_count; ai++) {
            p->abs_lines[ai].pc++;
        }
        /* Insert an abs_line entry at pc=0 using the first real instruction's
         * line (which is now at abs_lines[0].pc == 1 after the bump). */
        uint32_t line0 = 0u;
        if (p->abs_line_count > 0u && p->abs_lines[0].pc == 1u) {
            line0 = p->abs_lines[0].line;
        }
        if (!proto_grow(e->module, p,
                        (void **)&p->abs_lines, &p->abs_line_cap,
                        p->abs_line_count + 1u, sizeof(UAbsLine))) {
            e->error = EMIT_OOM; return false;
        }
        if (p->abs_line_count > 0u) {
            emit_memmove_right(p->abs_lines + 1, p->abs_lines,
                               p->abs_line_count * sizeof(UAbsLine));
        }
        p->abs_lines[0].pc   = 0u;
        p->abs_lines[0].line = line0;
        p->abs_line_count++;

    } else {
        /* === Root module path === */

        /* Instructions. */
        if (!emit_grow(e->module,
                       (void **)&e->module->instructions,
                       &e->module->instr_cap,
                       e->module->instr_count + 1u, sizeof(uint32_t))) {
            e->error = EMIT_OOM; return false;
        }
        if (e->module->instr_count > 0u) {
            emit_memmove_right(e->module->instructions + 1,
                               e->module->instructions,
                               e->module->instr_count * sizeof(uint32_t));
        }
        e->module->instructions[0] = instr;
        e->module->instr_count++;

        /* Patch absolute-PC instructions shifted right by 1. */
        for (size_t pi = 1u; pi < e->module->instr_count; pi++) {
            UOpcode op = uinstr_op(e->module->instructions[pi]);
            if (op == OP_TRY_BEGIN || op == OP_PUSH_TAG) {
                uint8_t  a  = uinstr_a(e->module->instructions[pi]);
                uint16_t bx = uinstr_bx(e->module->instructions[pi]);
                e->module->instructions[pi] = uinstr_enc_abx(op, a, (uint16_t)(bx + 1u));
            }
        }

        /* line_deltas: reallocate to new instr_count bytes, shift, insert. */
        {
            UModuleAllocFn alloc = emit_alloc_for(e->module);
            if (alloc == NULL) { e->error = EMIT_OOM; return false; }
            int8_t *fresh = (int8_t *)alloc(e->module->line_deltas,
                                            e->module->instr_count * sizeof(int8_t),
                                            e->module->alloc_ud);
            if (fresh == NULL) { e->error = EMIT_OOM; return false; }
            e->module->line_deltas = fresh;
        }
        if (e->module->instr_count > 1u) {
            emit_memmove_right(e->module->line_deltas + 1,
                               e->module->line_deltas,
                               (e->module->instr_count - 1u) * sizeof(int8_t));
        }
        e->module->line_deltas[0] = (int8_t)-128;

        /* Bump existing abs_lines pc values. */
        for (size_t ai = 0; ai < e->module->abs_line_count; ai++) {
            e->module->abs_lines[ai].pc++;
        }
        uint32_t line0 = 0u;
        if (e->module->abs_line_count > 0u && e->module->abs_lines[0].pc == 1u) {
            line0 = e->module->abs_lines[0].line;
        }
        if (!emit_grow(e->module,
                       (void **)&e->module->abs_lines,
                       &e->module->abs_line_cap,
                       e->module->abs_line_count + 1u, sizeof(UAbsLine))) {
            e->error = EMIT_OOM; return false;
        }
        if (e->module->abs_line_count > 0u) {
            emit_memmove_right(e->module->abs_lines + 1,
                               e->module->abs_lines,
                               e->module->abs_line_count * sizeof(UAbsLine));
        }
        e->module->abs_lines[0].pc   = 0u;
        e->module->abs_lines[0].line = line0;
        e->module->abs_line_count++;
    }
    return true;
}

UFuncState *uemit_close_function(UEmitter *e) {
    UFuncState *fs = e->current_fs;
    if (fs == NULL) return NULL;

    /* T73: Frame prologue — prepend OP_LOAD_REALM_GLOBAL as the first
     * instruction iff this function body referenced at least one realm
     * global.  This guarantees r_global_slot holds realm->global_object
     * before any OP_GETSLOT / OP_SETSLOT that targets the global object,
     * even when the first global reference is inside a branch arm that
     * may not be taken at runtime.  Pure-local functions are left
     * prologue-free (no wasted instruction). */
    if (fs->references_global && e->error == EMIT_OK) {
        uint32_t prologue = uinstr_enc_abc(OP_LOAD_REALM_GLOBAL,
                                           fs->r_global_slot, 0u, 0u);
        prologue_prepend_instr(e, prologue);
    }

    /* Roll max_reg_seen into target_proto when closing a nested function. */
    if (fs->target_proto != NULL) {
        UProto *p = (UProto *)fs->target_proto;
        p->max_reg  = fs->max_reg_seen;
        p->nupvals  = (uint8_t)fs->nupvalues;

        /* M4 T15: copy IC names side table into the UProto.  Use the
         * proto's own allocator (inherited from the module at
         * umodule_alloc_nested_proto time); the resulting array is freed
         * by umodule_proto_destroy_buffers. */
        p->ic_count = fs->ic_next;
        if (p->ic_count > 0u) {
            UModuleAllocFn palloc = p->alloc_fn;
#if __STDC_HOSTED__
            if (palloc == NULL) palloc = emit_alloc_for(e->module);
#endif
            if (palloc == NULL) {
                e->error = EMIT_OOM;
            } else {
                USymbol **dst = (USymbol **)palloc(NULL,
                    (size_t)p->ic_count * sizeof(USymbol *), p->alloc_ud);
                if (dst == NULL) {
                    e->error = EMIT_OOM;
                    p->ic_count = 0;
                    p->ic_names = NULL;
                } else {
                    for (uint16_t i = 0; i < p->ic_count; i++) {
                        dst[i] = fs->ic_names[i];
                    }
                    p->ic_names = dst;
                }
            }
        } else {
            p->ic_names = NULL;
        }
    }
    /* M4 follow-up: top-level funcstate (no target_proto) — copy IC names
     * into UModule.ic_count / ic_names so urbi_module_instance_create can
     * populate proto_instances->entries[0].  Mirrors the UProto path above. */
    if (fs->target_proto == NULL && fs->parent == NULL && fs->ic_next > 0u) {
        UModule *mod = e->module;
        UModuleAllocFn malloc_fn = emit_alloc_for(e->module);
        if (malloc_fn == NULL) {
            e->error = EMIT_OOM;
        } else {
            USymbol **dst = (USymbol **)malloc_fn(NULL,
                (size_t)fs->ic_next * sizeof(USymbol *), e->module->alloc_ud);
            if (dst == NULL) {
                e->error = EMIT_OOM;
            } else {
                for (uint16_t i = 0; i < fs->ic_next; i++) {
                    dst[i] = fs->ic_names[i];
                }
                mod->ic_count = fs->ic_next;
                mod->ic_names = dst;
            }
        }
    }
    /* Always free the funcstate-side IC array (allocated via the module
     * allocator).  For nested funcstates the names were copied into UProto
     * above; for top-level funcstates they were copied into UModule by the
     * block above.  Either way the funcstate-side buffer is now redundant. */
    if (fs->ic_names != NULL) {
        UModuleAllocFn alloc = emit_alloc_for(e->module);
        if (alloc != NULL) {
            alloc(fs->ic_names, 0, e->module->alloc_ud);
        }
        fs->ic_names = NULL;
        fs->ic_names_cap = 0;
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
    blk->nactvar_on_enter  = fs->nactvar;
    blk->freereg_on_enter  = fs->freereg;
    blk->first_local_idx   = fs->nactvar;
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
     * Restore from the snapshot saved at open time: this correctly handles
     * the case where r_global_slot was pre-reserved (freereg != nactvar). */
    fs->nactvar = blk->nactvar_on_enter;
    fs->freereg = blk->freereg_on_enter;
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

/* Unwind opcode encoders (uemit_throw, uemit_tag_stop, uemit_try_begin,
 * uemit_try_end, uemit_push_tag, uemit_pop_tag, uemit_push_frame_guard,
 * uemit_resume, uemit_load_catch_value) moved to uemit_unwind.c. */
