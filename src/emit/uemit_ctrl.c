/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_ctrl.c — sequences, blocks and plain control flow: if, while,
 * switch, break, continue, return, throw.
 *
 * Loops and switches push a loop record whose patch lists collect the
 * break and continue sites; the loop patches them once it knows its
 * targets.  A site that crosses try or tag scopes opened inside the loop
 * is an UNWIND_TO (absolute target) instead of a JMP (relative), so the
 * walker pops those scopes on the way. */

#include "emit/uemit_internal.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"
#include "util/uarena.h"

#include <stddef.h>
#include <stdint.h>

static uint32_t line_of(const UAstNode *n) {
    return n->line > 0 ? (uint32_t)n->line : 0U;
}

/* The value of `n` in `want`, or anywhere when want < 0. */
static uint8_t value_of(UEmitter *e, UAstNode *n, int want) {
    if (want < 0) return uexpr_any(e, n);
    uexpr_to(e, n, (uint8_t)want);
    return (uint8_t)want;
}

static uint8_t load_nil(UEmitter *e, int want, uint32_t line) {
    uint8_t d = uemit_target(e, want);
    (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADNIL, d, 0U, 0U), line);
    return d;
}

/* --- sequences and blocks ------------------------------------------------- */

uint8_t uctrl_seq(UEmitter *e, UAstNode *n, int want) {
    if (n->u.seq.separator == SEP_COMMA) return useq_comma(e, n, want);
    if (n->u.seq.separator == SEP_AMP) return useq_amp(e, n, want);
    int count = n->u.seq.count;
    for (int i = 0; i < count - 1; i++) {
        ustmt(e, n->u.seq.children[i]);
        /* `;` is a sequence point: the strand may yield between children. */
        if (n->u.seq.separator == SEP_SEMI)
            (void)uinstr_emit(e, uinstr_enc_abc(OP_YIELD, 0U, 0U, 0U), e->fs->prev_line);
    }
    if (count == 0) return load_nil(e, want, line_of(n));
    return value_of(e, n->u.seq.children[count - 1], want);
}

uint8_t uctrl_block(UEmitter *e, UAstNode *n, int want) {
    int count = n->u.block.count;
    uint8_t entry = e->fs->freereg;
    if (!ublock_open(e, false)) return 0U;
    for (int i = 0; i < count - 1; i++) ustmt(e, n->u.block.stmts[i]);
    if (count == 0) {
        (void)ublock_close(e);
        return load_nil(e, want, line_of(n));
    }
    if (want >= 0) {
        /* `want` was allocated before the block opened, so it is below
         * every local the block declares. */
        uexpr_to(e, n->u.block.stmts[count - 1], (uint8_t)want);
        (void)ublock_close(e);
        return (uint8_t)want;
    }
    uint8_t r = uexpr_any(e, n->u.block.stmts[count - 1]);
    (void)ublock_close(e);
    if (r < entry) return r;             /* a local declared outside the block */
    /* The value sits above the register the block started at: in one of
     * its locals, or a temporary above them.  The close emitted no write,
     * so the register still holds it; move it down to the first free one. */
    uint8_t d = ureg_alloc(e);
    if (r != d) (void)uinstr_emit(e, uinstr_enc_abc(OP_MOVE, d, r, 0U), line_of(n));
    return d;
}

/* Opens a block and compiles a case or loop body's statements in it,
 * values discarded.  The caller closes the block: a loop has to place
 * its continue target and back-edge CLOSE before the close. */
static void body_statements(UEmitter *e, UAstNode *body, bool is_loop) {
    if (!ublock_open(e, is_loop)) return;
    if (body->kind == AST_BLOCK) {
        for (int i = 0; i < body->u.block.count; i++) ustmt(e, body->u.block.stmts[i]);
    } else {
        ustmt(e, body);
    }
}

/* --- if ------------------------------------------------------------------- */

uint8_t uctrl_if(UEmitter *e, UAstNode *n, int want) {
    uint32_t line = line_of(n);
    uint8_t d = uemit_target(e, want);
    uint8_t base = e->fs->freereg;
    uint8_t c = uexpr_any(e, n->u.if_stmt.cond);
    /* TEST skips the JMP to the else arm when the condition is truthy. */
    (void)uinstr_emit(e, uinstr_enc_abc(OP_TEST, c, 0U, 1U), line);
    int to_else = ujmp_emit(e, line);
    ureg_free_to(e, base);
    uexpr_to(e, n->u.if_stmt.then_block, d);
    int to_end = ujmp_emit(e, line);
    ujmp_patch_here(e, to_else);
    if (n->u.if_stmt.else_block != NULL) uexpr_to(e, n->u.if_stmt.else_block, d);
    else (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADNIL, d, 0U, 0U), line);
    ujmp_patch_here(e, to_end);
    return d;
}

/* --- loop records ---------------------------------------------------------- */

static ULoop *loop_push(UEmitter *e, bool is_switch) {
    UFuncState *fs = e->fs;
    if (fs->nloops >= UEMIT_LOOP_MAX) {
        if (e->error == EMIT_OK)
            urbi_emit_diag_error(e, NULL, "loop nesting too deep (max %d)", UEMIT_LOOP_MAX);
        (void)uemit_fail(e, EMIT_NESTING_TOO_DEEP);
        return NULL;
    }
    ULoop *lp = &fs->loops[fs->nloops++];
    urbi_zero(lp, sizeof *lp);
    lp->is_switch = is_switch;
    lp->scope_depth_on_enter = uscope_depth(e);
    return lp;
}

static void loop_pop(UEmitter *e) {
    e->fs->nloops--;
}

static void patch_list(UEmitter *e, const UPatchList *l, int target) {
    for (int i = 0; i < l->count && e->error == EMIT_OK; i++) {
        int pc = l->pcs[i];
        if (l->is_unwind[i]) {
            if (target > (int)UINT16_MAX) { (void)uemit_fail(e, EMIT_JUMP_TOO_FAR); return; }
            uint8_t depth = uinstr_a(e->fs->proto->instructions[pc]);
            uinstr_patch(e, pc, uinstr_enc_abx(OP_UNWIND_TO, depth, (uint16_t)target));
        } else {
            ujmp_patch_to(e, pc, target);
        }
    }
}

/* A break or continue site into `l`: a JMP, or an UNWIND_TO when try or
 * tag scopes opened inside the loop lie between the site and the loop. */
static void jump_out(UEmitter *e, const ULoop *lp, UPatchList *l, const UAstNode *n) {
    if (e->error != EMIT_OK) return;
    if (l->count >= UEMIT_PATCH_MAX) {
        urbi_emit_diag_error(e, n, "too many break or continue sites in one loop (max %d)", UEMIT_PATCH_MAX);
        (void)uemit_fail(e, EMIT_PATCH_LIST_FULL);
        return;
    }
    int depth = uscope_depth(e) - lp->scope_depth_on_enter;
    int pc;
    if (depth > 0) pc = uinstr_emit(e, uinstr_enc_abx(OP_UNWIND_TO, (uint8_t)depth, 0U), line_of(n));
    else pc = ujmp_emit(e, line_of(n));
    l->pcs[l->count] = pc;
    l->is_unwind[l->count] = depth > 0 ? 1U : 0U;
    l->count++;
}

/* --- while ---------------------------------------------------------------- */

uint8_t uctrl_while(UEmitter *e, UAstNode *n, int want) {
    uint32_t line = line_of(n);
    uint8_t d = uemit_target(e, want);
    uint8_t base = e->fs->freereg;
    ULoop *lp = loop_push(e, false);
    if (lp == NULL) return d;
    int top = uinstr_pc(e);
    uint8_t c = uexpr_any(e, n->u.while_stmt.cond);
    (void)uinstr_emit(e, uinstr_enc_abc(OP_TEST, c, 0U, 1U), line);
    int to_exit = ujmp_emit(e, line);
    ureg_free_to(e, base);

    body_statements(e, n->u.while_stmt.body, true);
    if (e->error != EMIT_OK) return d;
    /* `continue` lands here, so the iteration's captured cells close
     * before the next one reuses their registers. */
    patch_list(e, &lp->continues, uinstr_pc(e));
    const UBlock *b = &e->fs->blocks[e->fs->nblocks - 1];
    if (b->has_captured)
        (void)uinstr_emit(e, uinstr_enc_abc(OP_CLOSE, b->freereg_on_enter, 0U, 0U), line);
    ujmp_back(e, top, line);
    /* The exit lands on the block's closing CLOSE, which a break needs:
     * it jumped past the body's own. */
    int exit = uinstr_pc(e);
    (void)ublock_close(e);
    ujmp_patch_to(e, to_exit, exit);
    patch_list(e, &lp->breaks, exit);
    loop_pop(e);
    (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADNIL, d, 0U, 0U), line);
    return d;
}

/* --- switch ------------------------------------------------------------------ */

uint8_t uctrl_switch(UEmitter *e, UAstNode *n, int want) {
    uint32_t line = line_of(n);
    uint8_t d = uemit_target(e, want);
    int ncases = n->u.switch_stmt.case_count;
    int *exits = (int *)uarena_alloc(e->arena, (size_t)(ncases + 1) * sizeof(int));
    if (exits == NULL) { (void)uemit_fail(e, EMIT_OOM); return d; }

    /* The subject is evaluated once and pinned across every arm. */
    uint8_t s = upin(e);
    uexpr_to(e, n->u.switch_stmt.expr, s);
    ULoop *lp = loop_push(e, true);
    if (lp == NULL) return d;
    /* One block around the arms, so a break out of any arm lands on a
     * CLOSE covering the cells that arm's own close was skipped for. */
    if (!ublock_open(e, false)) return d;

    int nexits = 0;
    for (int i = 0; i < ncases && e->error == EMIT_OK; i++) {
        uint8_t base = e->fs->freereg;
        uint8_t v = uexpr_any(e, n->u.switch_stmt.case_vals[i]);
        /* EQ skips the JMP to the next arm when subject == value. */
        (void)uinstr_emit(e, uinstr_enc_abc(OP_EQ, 0U, s, v), line);
        int to_next = ujmp_emit(e, line);
        ureg_free_to(e, base);
        body_statements(e, n->u.switch_stmt.case_bodies[i], false);
        (void)ublock_close(e);
        exits[nexits++] = ujmp_emit(e, line);
        ujmp_patch_here(e, to_next);
    }
    if (n->u.switch_stmt.default_body != NULL && e->error == EMIT_OK) {
        body_statements(e, n->u.switch_stmt.default_body, false);
        (void)ublock_close(e);
    }
    if (e->error != EMIT_OK) return d;
    int exit = uinstr_pc(e);
    (void)ublock_close(e);
    for (int i = 0; i < nexits; i++) ujmp_patch_to(e, exits[i], exit);
    patch_list(e, &lp->breaks, exit);
    loop_pop(e);
    uunpin(e, s);
    (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADNIL, d, 0U, 0U), line);
    return d;
}

/* --- break and continue -------------------------------------------------- */

uint8_t uctrl_break(UEmitter *e, const UAstNode *n, int want) {
    UFuncState *fs = e->fs;
    if (fs->nloops == 0) { (void)uemit_fail(e, EMIT_UNSUPPORTED_AST); return 0U; }
    ULoop *lp = &fs->loops[fs->nloops - 1];
    jump_out(e, lp, &lp->breaks, n);
    /* Nothing after the jump runs; the register only keeps the caller's
     * bookkeeping whole. */
    return uemit_target(e, want);
}

uint8_t uctrl_continue(UEmitter *e, const UAstNode *n, int want) {
    UFuncState *fs = e->fs;
    /* A switch passes `continue` through to the loop around it. */
    for (int i = fs->nloops - 1; i >= 0; i--) {
        if (!fs->loops[i].is_switch) {
            jump_out(e, &fs->loops[i], &fs->loops[i].continues, n);
            return uemit_target(e, want);
        }
    }
    (void)uemit_fail(e, EMIT_UNSUPPORTED_AST);
    return 0U;
}

/* --- return and throw --------------------------------------------------------- */

uint8_t uctrl_return(UEmitter *e, UAstNode *n, int want) {
    uint8_t r = (n->u.ret.value != NULL) ? uexpr_any(e, n->u.ret.value)
                                          : load_nil(e, -1, line_of(n));
    /* A frame that still owns try or tag scopes hands the return to the
     * walker, which runs the finally bodies on the way out. */
    (void)uinstr_emit(e, uinstr_enc_abc(OP_RET, r, 0U, 0U), line_of(n));
    return want >= 0 ? (uint8_t)want : r;
}

uint8_t uctrl_throw(UEmitter *e, UAstNode *n, int want) {
    uint8_t r = uexpr_any(e, n->u.throw_expr.value);
    (void)uinstr_emit(e, uinstr_enc_abc(OP_THROW, r, 0U, 0U), line_of(n));
    return want >= 0 ? (uint8_t)want : r;
}
