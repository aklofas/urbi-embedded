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
    for (i = 0u; i < n; i++) pd[i] = ps[i];
}

/* Local byte-move (overlapping-safe right shift).  Used by the prologue
   prepend helper to shift instruction / line-delta arrays rightward. */
static inline void emit_memmove_right(void *dst, const void *src, size_t n) {
    unsigned char *pd = (unsigned char *)dst;
    const unsigned char *ps = (const unsigned char *)src;
    size_t i = n;
    while (i > 0u) { i--; pd[i] = ps[i]; }
}

/* --- Module allocator helper --- */

#if __STDC_HOSTED__
#  include <stdlib.h>

static inline void *emit_stdlib_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;
    if (nbytes == 0u) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

#endif  /* __STDC_HOSTED__ */

/* Return the allocator to use for module c.  Available in both hosted and
   freestanding builds so that emit_grow can call it unconditionally.
   In freestanding builds the stdlib fallback is absent; the caller must have
   supplied alloc_fn, and emit_grow will return false if it is NULL. */
static inline UModuleAllocFn emit_alloc_for(const UModule *c) {
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
void   emit_patch_instr(UEmitter *e, int pc, uint32_t new_instr);
size_t emit_instr_count(const UEmitter *e);
uint8_t fs_temp_floor(const UFuncState *fs);

/* Buffer-growth helpers (defined in uemit.c).
 * Promoted from static so uemit_funcstate.c can call them cross-TU. */
bool emit_grow(UModule *c, void **data, size_t *cap,
               size_t new_cap, size_t elem_size);
bool proto_grow(UModule *module, UProto *proto,
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
bool uemit_open_block(UEmitter *e, bool is_loop);
bool uemit_close_block(UEmitter *e);
void uemit_emit_loop_back_close(UEmitter *e);
int find_or_install_upvalue(UEmitter *e, UFuncState *fs,
                            const char *name, int name_len);

/* --- Sentinels and biases shared across emit TUs --- */

#define UEMIT_NO_OPERAND      ((uint8_t)0xFFu)   /* "no body / no onleave" — replaces inline 0xFFu (EMIT-023) */
#define UEMIT_JMP_BIAS        32768              /* used by emit_stmt + emit_expr (EMIT-024) */
#define UEMIT_REG_LIMIT       UFS_MAX_REGS       /* alias for clarity at exhaustion-guard sites (EMIT-025) */

/* --- Register-allocator micro-helpers (inline for zero overhead) ---
 * Promoted from static in uemit.c so that extracted TUs (uemit_react.c, etc.)
 * can use them without implicit-declaration warnings. */

/* Bump the register-allocator cursor and track high-water mark.
 * Returns the allocated register index.  Sets EMIT_REG_EXHAUSTED if
 * all 256 slots are consumed (cursor at 255 before call). */
static inline uint8_t alloc_reg(UEmitter *e) {
    if (e->next_reg == 255u) { e->error = EMIT_REG_EXHAUSTED; return 0u; }
    uint8_t r = e->next_reg++;
    if (r > e->max_reg_seen) e->max_reg_seen = r;
    return r;
}

/* Release the most-recently-allocated register (stack discipline). */
static inline void free_reg(UEmitter *e) {
    if (e->next_reg > 0u) e->next_reg--;
}

/* Statement / control-flow AST arm helpers (defined in uemit_stmt.c).
 * Called from emit_expr via forwarding stubs; bodies live in uemit_stmt.c. */
uint8_t emit_if_arm(UEmitter *e, UAstNode *n);
uint8_t emit_while_arm(UEmitter *e, UAstNode *n);
uint8_t emit_call_arm(UEmitter *e, UAstNode *n);
uint8_t emit_return_arm(UEmitter *e, UAstNode *n);
uint8_t emit_function_arm(UEmitter *e, UAstNode *n);

/* Leaf-expression AST arm helpers (defined in uemit_expr.c).
 * Called from emit_expr via forwarding stubs; bodies live in uemit_expr.c. */
uint8_t emit_int_arm(UEmitter *e, UAstNode *n);
uint8_t emit_bool_arm(UEmitter *e, UAstNode *n);
uint8_t emit_nil_arm(UEmitter *e, UAstNode *n);
uint8_t emit_noop_arm(UEmitter *e, UAstNode *n);
uint8_t emit_unary_arm(UEmitter *e, UAstNode *n);
uint8_t emit_binary_arm(UEmitter *e, UAstNode *n);
uint8_t emit_compare_arm(UEmitter *e, UAstNode *n);
uint8_t emit_ident_arm(UEmitter *e, UAstNode *n);
uint8_t emit_var_decl_arm(UEmitter *e, UAstNode *n);
uint8_t emit_assign_arm(UEmitter *e, UAstNode *n);
uint8_t emit_nary_arm(UEmitter *e, UAstNode *n);
uint8_t emit_bin_sep_arm(UEmitter *e, UAstNode *n);
uint8_t emit_block_arm(UEmitter *e, UAstNode *n);

/* Constant-pool helpers (defined in uemit.c).
 * Promoted from static so uemit_expr.c can call them cross-TU. */
uint16_t add_const_int(UEmitter *e, int64_t v);
UOpcode  binop_to_opcode(UAstBinaryOp op);

#endif /* UEMIT_INTERNAL_H */
