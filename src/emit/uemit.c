/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode emitter. */

#include "emit/uemit_internal.h"
#include "runtime/umacros.h"
#include "value/uintern.h"
#include "watcher/uwatcher.h"  /* UWATCHER_AT / _AT_SYNC / _WHENEVER — AST_WATCHER emit */

#include <limits.h>
#include <stddef.h>
#include "emit/uemit.h"
#include "module/umodule.h"
#include "parse/uast.h"
#include "value/uarena.h"
#include <stdint.h>

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
    size_t len = urbi_strlen(src);
    UModuleAllocFn alloc = emit_alloc_for(e->module);
    char *copy = (char *)alloc(NULL, len + 1U, e->module->alloc_ud);
    if (copy == NULL) { e->error = EMIT_OOM; return; }
    emit_memcpy(copy, src, len + 1U);
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
   array in the emitter.
   Promoted from static so uemit_funcstate.c can call it cross-TU. */
bool emit_grow(UModule *c, void **data, size_t *cap,
               size_t new_cap, size_t elem_size) {
    if (*cap >= new_cap) return true;
    UModuleAllocFn alloc = emit_alloc_for(c);
    if (alloc == NULL) return false;
    size_t target = *cap == 0U ? 8U : *cap;
    while (target < new_cap) target *= 2U;
    void *fresh = alloc(*data, target * elem_size, c->alloc_ud);
    if (fresh == NULL) return false;
    *data  = fresh;
    *cap   = target;
    return true;
}

/* Grow a buffer owned by either the module root or a nested UProto.
 * When `proto` is NULL, delegates to emit_grow (module root path).
 * Promoted from static so uemit_funcstate.c can call it cross-TU. */
bool proto_grow(UModule *module, UProto *proto,
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
        floor_val = (uint8_t)(fs->nactvar + 1U);
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
        return 0U;
    }
    if (!proto_grow(e->module, p, (void **)pool, cap, *count + 1U, sizeof(UValue))) {
        e->error = EMIT_OOM;
        return 0U;
    }
    {
        const size_t idx = *count;
        int pad;
        (*pool)[idx].kind = (uint8_t)UVAL_INT;
        for (pad = 0; pad < 7; pad++) (*pool)[idx]._pad[pad] = 0U;
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
                        p->abs_line_count + 1U, sizeof(UAbsLine))) {
            e->error = EMIT_OOM;
            return;
        }
        p->abs_lines[p->abs_line_count].pc   = pc;
        p->abs_lines[p->abs_line_count].line = line;
        p->abs_line_count++;
        return;
    }
    if (!emit_grow(e->module, (void **)&e->module->abs_lines, &e->module->abs_line_cap,
                   e->module->abs_line_count + 1U, sizeof(UAbsLine))) {
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
        p->line_deltas[p->instr_count - 1U] = delta;
        return;
    }
    UModuleAllocFn alloc = emit_alloc_for(e->module);
    if (alloc == NULL) { e->error = EMIT_OOM; return; }
    void *fresh = alloc(e->module->line_deltas,
                        e->module->instr_count * sizeof(int8_t),
                        e->module->alloc_ud);
    if (fresh == NULL) { e->error = EMIT_OOM; return; }
    e->module->line_deltas = (int8_t *)fresh;
    e->module->line_deltas[e->module->instr_count - 1U] = delta;
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
                        &p->instr_cap, p->instr_count + 1U, sizeof(uint32_t))) {
            e->error = EMIT_OOM;
            return;
        }
        p->instructions[p->instr_count++] = ins;

        pc = (uint32_t)(p->instr_count - 1U);
        delta = 0;
        needs_abs = false;
        if (e->prev_line == 0U) {
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
                   e->module->instr_count + 1U, sizeof(uint32_t))) {
        e->error = EMIT_OOM;
        return;
    }
    e->module->instructions[e->module->instr_count++] = ins;

    /* Delta encoding.  INT8_MIN (-128) is the sentinel; valid range [-127,+127]. */
    pc = (uint32_t)(e->module->instr_count - 1U);
    delta = 0;
    needs_abs = false;
    if (e->prev_line == 0U) {
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
    if (e->error != EMIT_OK) return 0U;
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
        return 0U;
    case AST_ERROR:
        e->error = EMIT_AST_ERROR;
        return 0U;
    }
    e->error = EMIT_UNSUPPORTED_AST;
    return 0U;
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
        emit_instr(e, uinstr_enc_abc(OP_RET, e->last_result_reg, 0U, 0U),
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

/* UFuncState lifecycle + upvalue cascade + block stack + IC index assign
 * + prologue_prepend_instr moved to uemit_funcstate.c (T13).
 *
 * Unwind opcode encoders (uemit_throw, uemit_tag_stop, uemit_try_begin,
 * uemit_try_end, uemit_push_tag, uemit_pop_tag, uemit_push_frame_guard,
 * uemit_resume, uemit_load_catch_value) moved to uemit_unwind.c. */
