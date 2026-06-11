/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_internal.h — private inter-TU API for the emit subsystem.
 *
 * Consumed only by src/emit/ TUs.  Public emit API is in src/emit/uemit.h.
 * Created v0.5.4-decompose; do NOT include from outside src/emit/. */

#ifndef UEMIT_INTERNAL_H
#define UEMIT_INTERNAL_H

#include "uemit.h"
#include "runtime/umacros.h"   /* urbi_zero */

#include <stddef.h>
#include <stdint.h>

/* --- Byte-copy / string helpers (freestanding-safe) --- */

/* Local byte-copy.  Replaces memcpy so the serializer compiles without
   a hosted <string.h>.  Same pattern as module_memcpy in umodule.c. */
static inline void emit_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *pd = (unsigned char *)dst;
    const unsigned char *ps = (const unsigned char *)src;
    size_t i;
    for (i = 0U; i < n; i++) pd[i] = ps[i];
}

/* Local byte-move (overlapping-safe right shift).  Used by the prologue
   prepend helper to shift instruction / line-delta arrays rightward. */
static inline void emit_memmove_right(void *dst, const void *src, size_t n) {
    unsigned char *pd = (unsigned char *)dst;
    const unsigned char *ps = (const unsigned char *)src;
    size_t i = n;
    while (i > 0U) { i--; pd[i] = ps[i]; }
}

/* --- Module allocator helper --- */

#if __STDC_HOSTED__
#  include <stdlib.h>

static inline void *emit_stdlib_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;
    if (nbytes == 0U) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

#endif  /* __STDC_HOSTED__ */

/* Return the allocator to use for root UProto c.  Available in both hosted
   and freestanding builds so that emit_grow can call it unconditionally.
   In freestanding builds the stdlib fallback is absent; the caller must have
   supplied alloc_fn, and emit_grow will return false if it is NULL. */
static inline UChunkAllocFn emit_alloc_for(const UProto *c) {
#if __STDC_HOSTED__
    return c->alloc_fn != NULL ? c->alloc_fn : emit_stdlib_alloc;
#else
    return c->alloc_fn;   /* freestanding: caller must supply */
#endif
}

/* --- Forward decls for cross-TU functions (extract-driven; added T6-T13) --- */

/* Core instruction emitters / helpers (defined in uemit.c).
 * Promoted to non-static so that extracted TUs (uemit_unwind.c, etc.)
 * can emit instructions without pulling uemit.c's internal statics. */
void   emit_instr(UEmitter *e, uint32_t ins, uint32_t line);
void   emit_patch_instr(const UEmitter *e, int pc, uint32_t new_instr);
size_t emit_instr_count(const UEmitter *e);
uint8_t fs_temp_floor(const UFuncState *fs);

/* Buffer-growth helpers (defined in uemit.c).
 * Promoted from static so uemit_funcstate.c can call them cross-TU. */
bool emit_grow(UProto *root, void **data, size_t *cap,
               size_t new_cap, size_t elem_size);
bool proto_grow(UProto *root, UProto *proto,
                void **data, size_t *cap,
                size_t new_cap, size_t elem_size);

/* Main expression walker (defined in uemit.c); called recursively by
 * arm helpers in extracted TUs. */
uint8_t emit_expr(UEmitter *e, UAstNode *n);

/* Diag-emit funnel (defined in uemit_diag.c). */
void emit_diag_warn(UEmitter *e, UAstNode *n, const char *fmt, ...);
void emit_diag_free_all(UEmitter *e);

/* Unwind AST arm helpers (defined in uemit_unwind.c).
 * Called from emit_expr via forwarding stubs; bodies live in uemit_unwind.c. */
uint8_t emit_throw_arm(UEmitter *e, UAstNode *n);
uint8_t emit_try_arm(UEmitter *e, UAstNode *n);
uint8_t emit_tag_prefix_arm(UEmitter *e, UAstNode *n);

/* Reactive AST arm helpers (defined in uemit_react.c).
 * Called from emit_expr via forwarding stubs; bodies live in uemit_react.c. */
uint8_t emit_watcher_arm(UEmitter *e, UAstNode *n);
uint8_t emit_waituntil_arm(UEmitter *e, UAstNode *n);
uint8_t emit_at_event_arm(UEmitter *e, UAstNode *n);
uint8_t emit_at_slot_change_arm(UEmitter *e, UAstNode *n);
uint8_t emit_member_get_arm(UEmitter *e, UAstNode *n);
uint8_t emit_member_set_arm(UEmitter *e, UAstNode *n);

/* Class-decl AST arm helper (defined in uemit_class.c).
 * Called from emit_expr via forwarding stub; body lives in uemit_class.c. */
uint8_t emit_class_decl_arm(UEmitter *e, UAstNode *n);

/* T41 — getter/setter parse sugar.  AST_PROPERTY_DECL emits a
 * `recv.setProperty(name, "oget"|"oset", function() body)` call sequence;
 * runtime `oget`/`oset` slot-property dispatch (M4 baseline) handles the
 * trigger on subsequent slot reads/writes.  When `recv` is NULL the emit
 * arm uses the implicit class receiver (class body) or the realm-global
 * `this` lookup (top-level form).  Lives in uemit_class.c. */
uint8_t emit_property_decl_arm(UEmitter *e, UAstNode *n);

/* Closure + thunk builders (defined in uemit.c).
 * Promoted from static so that uemit_react.c can build closures for
 * reactive arms without duplicating the logic. */
uint8_t emit_function_literal(UEmitter *e,
                              UAstNode **params, int nparams,
                              UAstNode  *body,
                              bool       as_expression);
uint8_t emit_lazy_thunk(UEmitter *e, UAstNode *expr);

/* Funcstate ops (defined in uemit_funcstate.c — T8+). */
UFuncState *uemit_open_function(UEmitter *e, UFuncState *parent);
UFuncState *uemit_close_function(UEmitter *e);
int uemit_assign_ic_index(UEmitter *e, USymbol *name);
int uemit_declare_local(UEmitter *e, const char *name, int name_len);
bool uemit_reserve_global_slot(UEmitter *e);
bool uemit_open_block(UEmitter *e, bool is_loop);
bool uemit_close_block(UEmitter *e);
void uemit_emit_loop_back_close(UEmitter *e);
int find_or_install_upvalue(UEmitter *e, UFuncState *fs,
                            const char *name, int name_len);

/* --- Sentinels and biases shared across emit TUs --- */

#define UEMIT_NO_OPERAND      ((uint8_t)0xFFU)   /* "no body / no onleave" — replaces inline 0xFFU (EMIT-023) */
/* JMP offset encoding: signed 16-bit offsets are stored as Bx with a
 * +0x8000 bias (UEMIT_JMP_BIAS) so 0x0000 means "jump back 0x8000",
 * 0x8000 means "no offset", 0xFFFF means "jump forward 0x7FFF".
 * UEMIT_JMP_FALLTHROUGH_BIAS encodes "+1 instr" — used by comparison
 * sites that always fall through to a single skip-over JMP. */
#define UEMIT_JMP_BIAS                32768U
#define UEMIT_JMP_FALLTHROUGH_BIAS    (UEMIT_JMP_BIAS + 1U)
#define UEMIT_REG_LIMIT       UFS_MAX_REGS       /* alias for clarity at exhaustion-guard sites (EMIT-025) */

/* EMIT-019 fix (Wave 5, v0.5.7): centralize OP_JMP Bx encoding in a
 * pc-based helper.  For FORWARD jumps the VM dispatches OP_JMP as
 * `pc += signed(Bx) - UEMIT_JMP_BIAS` AFTER the dispatch's pc++, so an
 * OP_JMP at from_pc landing at target_pc requires Bx = (target_pc -
 * from_pc - 1) + UEMIT_JMP_BIAS.  Back-edges do NOT get that pc++ —
 * they dispatch via the safepoint path; use uemit_jmp_offset_backward
 * for those (refactor-3 FE-01/B3).  Wave 3 named UEMIT_JMP_BIAS /
 * FALLTHROUGH_BIAS but left the arithmetic inline at every site; this
 * helper centralizes the encoding contract so future peephole /
 * extra-instr insertions cannot silently miscompute fall-through.
 * Returns the biased Bx value ready for uinstr_enc_abx.  Bytecode-
 * byte-identical with the pre-extract inline form.
 *
 * Direction assert: strict > — target == from_pc + 1 encodes offset 0,
 * which the forward/NEXT path handles correctly; target <= from_pc
 * must go through the backward encoder. */
static inline uint16_t uemit_jmp_offset(int from_pc, int target_pc) {
    URBI_INTERNAL_ASSERT(target_pc > from_pc);
    int offset = target_pc - from_pc - 1;
    return (uint16_t)((int)UEMIT_JMP_BIAS + offset);
}

/* Backward-jump encoder (refactor-3 FE-01/B3).  Backward OP_JMPs dispatch
 * via the VM's safepoint path, which executes the instruction at s->pc
 * directly — WITHOUT the implicit pc++ that NEXT() applies after forward
 * jumps.  The encoded offset must therefore be exactly (target - from),
 * not (target - from - 1).  Use this for every back-edge; uemit_jmp_offset
 * stays correct for forward jumps and forward patch sites.
 *
 * Direction assert: strict < — target == from_pc would encode offset 0,
 * which the VM's sign check sends down the forward/NEXT path, landing
 * at from_pc + 1 (silently wrong). */
static inline uint16_t uemit_jmp_offset_backward(int from_pc, int target_pc) {
    URBI_INTERNAL_ASSERT(target_pc < from_pc);
    int offset = target_pc - from_pc;
    return (uint16_t)((int)UEMIT_JMP_BIAS + offset);
}

/* --- Register-allocator micro-helpers (inline for zero overhead) ---
 * Promoted from static in uemit.c so that extracted TUs (uemit_react.c, etc.)
 * can use them without implicit-declaration warnings. */

/* Bump the register-allocator cursor and track high-water mark.
 * Returns the allocated register index.  Sets EMIT_REG_EXHAUSTED if
 * all 256 slots are consumed (cursor at 255 before call).
 *
 * EMIT-011 fix (Wave 5, v0.5.7): also bump the per-FuncState
 * fs->max_reg_seen.  uemit_close_function rolls fs->max_reg_seen into
 * the nested proto's max_reg; the VM allocates (proto->max_reg + 1)
 * register slots at runtime.  Pre-fix, alloc_reg only updated the
 * EMITTER's global high-water (e->max_reg_seen), so leaf-expression
 * paths (AST_INT / AST_BOOL / AST_NIL / AST_NOOP) that allocate
 * temps without going through emit_compare or emit_ident (which sync
 * fs->max_reg_seen explicitly) caused proto->max_reg to under-report
 * the actual peak — out-of-bounds register access at runtime.
 *
 * NULL-guard on current_fs: alloc_reg may be called during the brief
 * window before uemit_statement opens the lazy top-level FuncState. */
static inline uint8_t alloc_reg(UEmitter *e) {
    if (e->next_reg == 255U) { e->error = EMIT_REG_EXHAUSTED; return 0U; }
    uint8_t r = e->next_reg++;
    if (r > e->max_reg_seen) e->max_reg_seen = r;
    if (e->current_fs != NULL && r > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = r;
    return r;
}

/* Release the most-recently-allocated register (stack discipline).
 *
 * EMIT-012 fix (Wave 5, v0.5.7): respect fs_temp_floor — temp registers
 * live at indices [floor, ...) where floor = nactvar + (1 if
 * global_slot_reserved else 0).  A bare next_reg-- with no floor guard
 * decrements *into* the local zone when the caller miscounted free_reg
 * against alloc_reg, then a subsequent alloc_reg returns a slot that
 * aliases a still-live local.  Guard against the underflow by treating
 * the call as a no-op when next_reg is already at or below the floor.
 *
 * NULL-guard on current_fs: free_reg may be called during the brief
 * window before uemit_statement opens the lazy top-level FuncState. */
static inline void free_reg(UEmitter *e) {
    if (e->next_reg == 0U) return;
    if (e->current_fs != NULL) {
        uint8_t floor_val = fs_temp_floor(e->current_fs);
        if (e->next_reg <= floor_val) return;
    }
    e->next_reg--;
}

/* Release a register that was bumped through *both* next_reg AND the
 * FuncState freereg cursor (the pattern that emit_function_literal
 * leaves behind on the parent FuncState — closure dst pulled from
 * freereg, then `freereg++` and `next_reg = freereg`).
 *
 * EMIT-010 fix (Wave 5, v0.5.7): watcher / waituntil / at-event install
 * arms compile their cond/body/onleave/event closures via
 * emit_function_literal, which raises freereg in lockstep with next_reg.
 * Plain free_reg() decrements only next_reg, leaving freereg promoted
 * 1-N slots above the now-decremented next_reg.  Subsequent
 * declarations / temp allocations then land at the leaked freereg
 * floor instead of the actual top of the live stack, wasting register
 * slots and inflating proto.max_reg.  Use free_reg_freereg_synced at
 * each install-site teardown to symmetrically unwind both cursors. */
static inline void free_reg_freereg_synced(UEmitter *e) {
    if (e->next_reg > 0U) e->next_reg--;
    if (e->current_fs != NULL && e->current_fs->freereg > e->next_reg)
        e->current_fs->freereg = e->next_reg;
}

/* === W1/v0.10.5: loop-context helpers (inline — shared by uemit_stmt.c) ===
 * These must be placed AFTER uemit_jmp_offset and emit_patch_instr are
 * declared/defined so the inline patch helpers can reference them.
 *
 * uemit_loop_push — open a new loop context.  Returns false (sets
 *   EMIT_NESTING_TOO_DEEP) on overflow.
 * uemit_loop_pop — close the current loop context.
 * uemit_loop_record_break / _continue — record placeholder JMP PCs.
 * uemit_loop_patch_breaks / _continues — batch-patch to a known target. */

static inline bool uemit_loop_push(UEmitter *e, ULoopFrameKind kind) {
    if (e->loop_depth >= UEMIT_LOOP_CTX_MAX) {
        e->error = EMIT_NESTING_TOO_DEEP;
        return false;
    }
    ULoopCtx *ctx = &e->loop_stack[e->loop_depth];
    ctx->break_count    = 0;
    ctx->continue_count = 0;
    ctx->kind           = kind;
    e->loop_depth++;
    return true;
}

static inline void uemit_loop_pop(UEmitter *e) {
    if (e->loop_depth > 0) e->loop_depth--;
}

static inline void uemit_loop_record_break(UEmitter *e, int pc) {
    if (e->loop_depth == 0) return;
    ULoopCtx *ctx = &e->loop_stack[e->loop_depth - 1];
    if (ctx->break_count < UEMIT_LOOP_PATCH_MAX) {
        ctx->break_pcs[ctx->break_count++] = pc;
    } else {
        e->error = EMIT_PATCH_LIST_FULL;  /* refactor-3 FE-06: the excess
                                             site's placeholder JMP would
                                             never be patched — silent no-op */
    }
}

static inline void uemit_loop_record_continue(UEmitter *e, int pc) {
    /* Walk back from topmost frame; record on nearest LOOP frame.
     * Switch frames are transparent to `continue`.  If no LOOP frame
     * exists in the stack the parser already rejected this; defensive
     * no-op. */
    int d;
    for (d = e->loop_depth; d > 0; d--) {
        ULoopCtx *ctx = &e->loop_stack[d - 1];
        if (ctx->kind == ULOOP_FRAME_LOOP) {
            if (ctx->continue_count < UEMIT_LOOP_PATCH_MAX) {
                ctx->continue_pcs[ctx->continue_count++] = pc;
            } else {
                e->error = EMIT_PATCH_LIST_FULL;  /* refactor-3 FE-06 */
            }
            return;
        }
    }
}

static inline void uemit_loop_patch_breaks(UEmitter *e, int exit_target) {
    if (e->loop_depth == 0) return;
    ULoopCtx *ctx = &e->loop_stack[e->loop_depth - 1];
    int i;
    for (i = 0; i < ctx->break_count; i++) {
        int from_pc = ctx->break_pcs[i];
        emit_patch_instr(e, from_pc,
            uinstr_enc_abx(OP_JMP, 0U,
                           uemit_jmp_offset(from_pc, exit_target)));
    }
}

static inline void uemit_loop_patch_continues(UEmitter *e, int cont_target) {
    if (e->loop_depth == 0) return;
    ULoopCtx *ctx = &e->loop_stack[e->loop_depth - 1];
    int i;
    for (i = 0; i < ctx->continue_count; i++) {
        int from_pc = ctx->continue_pcs[i];
        emit_patch_instr(e, from_pc,
            uinstr_enc_abx(OP_JMP, 0U,
                           uemit_jmp_offset(from_pc, cont_target)));
    }
}
/* === end W1/v0.10.5: loop-context helpers === */

/* Statement / control-flow AST arm helpers (defined in uemit_stmt.c).
 * Called from emit_expr via forwarding stubs; bodies live in uemit_stmt.c. */
uint8_t emit_if_arm(UEmitter *e, UAstNode *n);
uint8_t emit_while_arm(UEmitter *e, UAstNode *n);
uint8_t emit_call_arm(UEmitter *e, UAstNode *n);
uint8_t emit_return_arm(UEmitter *e, UAstNode *n);
uint8_t emit_function_arm(UEmitter *e, UAstNode *n);
/* W3/v0.10.5: assert keyword */
uint8_t emit_assert_arm(UEmitter *e, UAstNode *n);
/* === W10/v0.10.5: list/dict literals + subscript === */
uint8_t emit_list_lit_arm(UEmitter *e, UAstNode *n);
uint8_t emit_dict_lit_arm(UEmitter *e, UAstNode *n);
uint8_t emit_subscript_get_arm(UEmitter *e, UAstNode *n);
uint8_t emit_subscript_set_arm(UEmitter *e, UAstNode *n);
/* === end W10/v0.10.5 === */
/* === W1/v0.10.5: control flow === */
uint8_t emit_for_each_arm(UEmitter *e, UAstNode *n);
uint8_t emit_break_arm(UEmitter *e, const UAstNode *n);
uint8_t emit_continue_arm(UEmitter *e, const UAstNode *n);
uint8_t emit_switch_arm(UEmitter *e, UAstNode *n);
/* === end W1/v0.10.5: control flow === */

/* Leaf-expression AST arm helpers (defined in uemit_expr.c).
 * Called from emit_expr via forwarding stubs; bodies live in uemit_expr.c. */
uint8_t emit_int_arm(UEmitter *e, const UAstNode *n);
uint8_t emit_float_arm(UEmitter *e, const UAstNode *n);
uint8_t emit_bool_arm(UEmitter *e, const UAstNode *n);
uint8_t emit_nil_arm(UEmitter *e, const UAstNode *n);
uint8_t emit_string_arm(UEmitter *e, const UAstNode *n);
uint8_t emit_this_arm(UEmitter *e, const UAstNode *n);
uint8_t emit_noop_arm(UEmitter *e, const UAstNode *n);
uint8_t emit_unary_arm(UEmitter *e, UAstNode *n);
uint8_t emit_binary_arm(UEmitter *e, UAstNode *n);
uint8_t emit_compare_arm(UEmitter *e, UAstNode *n);
uint8_t emit_logical_arm(UEmitter *e, UAstNode *n);
uint8_t emit_ident_arm(UEmitter *e, const UAstNode *n);
uint8_t emit_var_decl_arm(UEmitter *e, UAstNode *n);
uint8_t emit_assign_arm(UEmitter *e, UAstNode *n);
uint8_t emit_nary_arm(UEmitter *e, UAstNode *n);
uint8_t emit_bin_sep_arm(UEmitter *e, UAstNode *n);
uint8_t emit_block_arm(UEmitter *e, UAstNode *n);

/* Constant-pool helpers (defined in uemit.c).
 * Promoted from static so uemit_expr.c can call them cross-TU. */
uint16_t add_const_int(UEmitter *e, int64_t v);
uint16_t add_const_float(UEmitter *e, double v);
uint16_t add_const_str(UEmitter *e, const char *interned);
UOpcode  binop_to_opcode(UAstBinaryOp op);

#endif /* UEMIT_INTERNAL_H */
