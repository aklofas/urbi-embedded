/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_scope.c — scopes the runtime keeps on the strand's cleanup
 * stack: try (catch, else, finally) and the tag prefix `t: { body }`.
 *
 * The emitter mirrors that stack in `fs->scopes`, one record per entry
 * this frame has open at the point being compiled, so a break or continue
 * knows how many entries its UNWIND_TO pops.  A record is pushed with the
 * SCOPE_TRY or SCOPE_TAG that opens the entry and popped with the
 * SCOPE_POP that closes it.  A catch handler starts with its entry
 * already gone (the walker consumed it), and a finally body runs under
 * the walker's RUNNING marker, which counts as one entry of its own.
 *
 * The statement's value lives in a pinned register for the whole
 * construct, so the bodies and handlers write it without disturbing
 * each other's temporaries, and a finally body never overwrites it.
 *
 * A body the walker abandons -- a throw into a catch, a throw, jump or
 * return into a finally, a stop out of a tag scope -- skips its blocks'
 * CLOSEs, and the code the walker resumes reuses the body's registers.
 * So that code starts with a CLOSE of everything above the value pin
 * (the tag scope's lands on it), emitted when a function nested in the
 * body captured a local (fs->ncaptures moved).
 * The emitter does this rather than the walker because only the emitter
 * knows whether anything was captured and where the body's registers
 * start. */

#include "emit/uemit_internal.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"

#include <stdbool.h>
#include <stdint.h>

static uint32_t line_of(const UAstNode *n) {
    return n->line > 0 ? (uint32_t)n->line : 0U;
}

int uscope_depth(const UEmitter *e) {
    return e->fs->nscopes;
}

static bool scope_push(UEmitter *e, uint8_t kind, uint8_t tag_reg) {
    UFuncState *fs = e->fs;
    if (fs->nscopes >= UEMIT_SCOPE_MAX) {
        if (e->error == EMIT_OK)
            urbi_emit_diag_error(e, NULL, "try and tag scopes nested too deeply (max %d)", UEMIT_SCOPE_MAX);
        return uemit_fail(e, EMIT_NESTING_TOO_DEEP);
    }
    fs->scopes[fs->nscopes].kind = kind;
    fs->scopes[fs->nscopes].tag_reg = tag_reg;
    fs->nscopes++;
    return true;
}

static void scope_pop(UEmitter *e) {
    if (e->fs->nscopes > 0) e->fs->nscopes--;
}

/* SCOPE_TRY and SCOPE_TAG name their handler by absolute pc. */
static void patch_handler(UEmitter *e, int pc, UOpcode op, uint8_t a, int target) {
    if (e->error != EMIT_OK) return;
    if (target > (int)UINT16_MAX) {
        urbi_emit_diag_error(e, NULL, "scope handler past instruction %u", (unsigned)UINT16_MAX);
        (void)uemit_fail(e, EMIT_JUMP_TOO_FAR);
        return;
    }
    uinstr_patch(e, pc, uinstr_enc_abx(op, a, (uint16_t)target));
}

/* The CLOSE an abandoned body's resume point starts with. */
static void close_abandoned(UEmitter *e, bool captured, uint8_t base, uint32_t line) {
    if (captured) (void)uinstr_emit(e, uinstr_enc_abc(OP_CLOSE, base, 0U, 0U), line);
}

/* The value `rd` takes on the way out, in `want` or a fresh register.
 * Every pin is gone by now, so with no target this is `rd` itself. */
static uint8_t deliver(UEmitter *e, uint8_t rd, int want, uint32_t line) {
    uint8_t d = uemit_target(e, want);
    if (d != rd) (void)uinstr_emit(e, uinstr_enc_abc(OP_MOVE, d, rd, 0U), line);
    return d;
}

/* --- try ------------------------------------------------------------------ */

/* The catch handler, entered by the walker with the exception pending.
 * The catch variable is a local of a block around the handler; a guard
 * that is false rethrows it from inside that block. */
static void catch_handler(UEmitter *e, UAstNode *n, uint8_t rd, bool captured) {
    uint32_t line = line_of(n);
    close_abandoned(e, captured, e->fs->freereg, line);
    if (!ublock_open(e, false)) return;
    uint8_t x;
    if (n->u.try_stmt.catch_var_start != NULL) {
        const char *name = uemit_intern(e, n->u.try_stmt.catch_var_start, n->u.try_stmt.catch_var_len);
        if (name == NULL) return;
        int li = ulocal_declare(e, name);
        if (li < 0) return;
        x = e->fs->locals[li].reg;
    } else {
        /* No name: the value is still taken, which drops the strand's
         * reference to it. */
        x = upin(e);
    }
    (void)uinstr_emit(e, uinstr_enc_abc(OP_LOAD_CATCH_VALUE, x, 0U, 0U), line);
    int to_rethrow = -1;
    if (n->u.try_stmt.catch_guard != NULL) {
        uint8_t base = e->fs->freereg;
        uint8_t g = uexpr_any(e, n->u.try_stmt.catch_guard);
        /* TEST skips the JMP to the rethrow when the guard is truthy. */
        (void)uinstr_emit(e, uinstr_enc_abc(OP_TEST, g, 0U, 1U), line);
        to_rethrow = ujmp_emit(e, line);
        ureg_free_to(e, base);
    }
    uexpr_to(e, n->u.try_stmt.catch_body, rd);
    if (to_rethrow >= 0) {
        int over = ujmp_emit(e, line);
        ujmp_patch_here(e, to_rethrow);
        (void)uinstr_emit(e, uinstr_enc_abc(OP_THROW, x, 0U, 0U), line);
        ujmp_patch_here(e, over);
    }
    (void)ublock_close(e);
}

/* try { b } [catch (var x [if g]) { c }] [else { el }] [finally { f }]
 *
 *     LOADNIL rd
 *     [SCOPE_TRY has_finally -> fin]        outer, when there is a finally
 *     [SCOPE_TRY has_catch -> handler]      inner, when there is a catch
 *     <b -> rd>
 *     [SCOPE_POP try]
 *     [<el>]
 *     [JMP past_handler
 *   handler:
 *     [CLOSE base]
 *     LOAD_CATCH_VALUE x; [<g>; TEST; JMP rethrow]; <c -> rd>; [JMP over; rethrow: THROW x; over:]
 *   past_handler:]
 *     [SCOPE_POP try+finally                the walker runs fin, RESUME returns here
 *     JMP end
 *   fin:
 *     [CLOSE base]                          base: the first register above rd
 *     <f>                                   the only copy
 *     RESUME
 *   end:]
 *
 * The handler falls through to the outer pop, so a caught exception
 * still runs the finally on the normal path. */
uint8_t uscope_try(UEmitter *e, UAstNode *n, int want) {
    uint32_t line = line_of(n);
    bool has_catch = n->u.try_stmt.catch_body != NULL;
    bool has_finally = n->u.try_stmt.finally_body != NULL;
    uint8_t rd = upin(e);
    uint8_t base = e->fs->freereg;
    (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), line);

    int fin_open = -1, catch_open = -1;
    if (has_finally) {
        fin_open = uinstr_emit(e, uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_FINALLY, 0U), line);
        if (!scope_push(e, UEMIT_SCOPE_TRY, 0U)) return rd;
    }
    if (has_catch) {
        catch_open = uinstr_emit(e, uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_CATCH, 0U), line);
        if (!scope_push(e, UEMIT_SCOPE_TRY, 0U)) return rd;
    }
    uint32_t captures_on_enter = e->fs->ncaptures;
    uexpr_to(e, n->u.try_stmt.body, rd);
    bool body_captured = e->fs->ncaptures != captures_on_enter;
    if (has_catch) {
        (void)uinstr_emit(e, uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TRY, 0U, 0U), line);
        scope_pop(e);
    }
    if (n->u.try_stmt.else_body != NULL) ustmt(e, n->u.try_stmt.else_body);
    if (has_catch) {
        int past_handler = ujmp_emit(e, line);
        patch_handler(e, catch_open, OP_SCOPE_TRY, USCOPE_F_HAS_CATCH, uinstr_pc(e));
        catch_handler(e, n, rd, body_captured);
        ujmp_patch_here(e, past_handler);
    }
    if (has_finally) {
        bool guarded_captured = e->fs->ncaptures != captures_on_enter;   /* body, else and handler */
        (void)uinstr_emit(e, uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TRY | USCOPE_POP_RUN_FINALLY, 0U, 0U), line);
        scope_pop(e);
        int to_end = ujmp_emit(e, line);
        patch_handler(e, fin_open, OP_SCOPE_TRY, USCOPE_F_HAS_FINALLY, uinstr_pc(e));
        close_abandoned(e, guarded_captured, base, line);
        /* The body runs under the RUNNING marker: a jump out of it pops
         * the marker as one entry. */
        if (!scope_push(e, UEMIT_SCOPE_FINALLY, 0U)) return rd;
        ustmt(e, n->u.try_stmt.finally_body);
        scope_pop(e);
        (void)uinstr_emit(e, uinstr_enc_abc(OP_RESUME, 0U, 0U, 0U), line);
        ujmp_patch_here(e, to_end);
    }
    uunpin(e, rd);
    return deliver(e, rd, want, line);
}

/* --- tag scope ------------------------------------------------------------ */

/* t: { body }
 *
 *     LOADNIL rd
 *     <t -> rt>
 *     SCOPE_TAG rt -> after
 *     <body -> rd>
 *     SCOPE_POP tag
 *   after:
 *     [CLOSE base]                          base: the body's first register
 *
 * A stop that names this scope resumes at `after`. */
uint8_t uscope_tag(UEmitter *e, UAstNode *n, int want) {
    uint32_t line = line_of(n);
    if (n->u.tag_prefix.onleave != NULL) { (void)uemit_fail(e, EMIT_UNSUPPORTED_AST); return 0U; }
    uint8_t rd = upin(e);
    (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), line);
    uint8_t rt = USCOPE_NO_REG;
    if (n->u.tag_prefix.tag_expr != NULL) {
        rt = upin(e);
        uexpr_to(e, n->u.tag_prefix.tag_expr, rt);
    }
    uint8_t base = e->fs->freereg;
    int open = uinstr_emit(e, uinstr_enc_abx(OP_SCOPE_TAG, rt, 0U), line);
    if (!scope_push(e, UEMIT_SCOPE_TAG, rt)) return rd;
    uint32_t captures_on_enter = e->fs->ncaptures;
    uexpr_to(e, n->u.tag_prefix.body, rd);
    bool captured = e->fs->ncaptures != captures_on_enter;
    (void)uinstr_emit(e, uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TAG, 0U, 0U), line);
    scope_pop(e);
    patch_handler(e, open, OP_SCOPE_TAG, rt, uinstr_pc(e));
    close_abandoned(e, captured, base, line);
    if (rt != USCOPE_NO_REG) uunpin(e, rt);
    uunpin(e, rd);
    return deliver(e, rd, want, line);
}
