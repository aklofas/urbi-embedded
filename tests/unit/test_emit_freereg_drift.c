/* SPDX-License-Identifier: BSD-3-Clause */
/* test_emit_freereg_drift.c — Phase 2 (Wave 5) emit register-allocation
 * drift cluster.
 *
 * Each test case targets one EMIT-NNN audit ID for the M2-NaryEmit
 * known-bug-pattern family: next_reg / freereg / max_reg_seen drift
 * between the UEmitter global cursor and the per-FuncState fields.
 *
 * Tests assert post-fix invariants by inspecting emitted bytecode (register
 * operands of specific instructions) or by checking that error paths fire
 * before integer wraparound (T13). */

#include "utest.h"

#include <stdint.h>
#include <string.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "module/umodule.h"
#include "parse/uparse.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Helper: compile source through full pipeline; module is filled in.
 * Returns the emit error (EMIT_OK on success).  Caller owns module +
 * arena destruction.  vm is also caller-owned.
 * ----------------------------------------------------------------------- */

static UEmitError compile_src(UVM *vm, UArena *arena, UModule *module,
                              const char *src) {
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UEmitter e;
    uemit_init(&e, module, arena, vm, NULL);
    UParser p;
    uparse_init(&p, &lex, arena);
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        (void)uemit_statement(&e, node);
        uarena_reset(arena);
    }
    return uemit_finish(&e);
}

/* Find the first instruction in `instrs[0..count-1]` whose opcode is
 * `op` AND (if op is OP_LOADK) whose constant pool entry equals
 * `match_int_value`.  Returns the index, or -1 if none. */
static int find_loadk_int(const uint32_t *instrs, size_t count,
                          const UValue *constants, int64_t match) {
    for (size_t i = 0; i < count; i++) {
        if (uinstr_op(instrs[i]) != OP_LOADK) continue;
        uint16_t bx = uinstr_bx(instrs[i]);
        const UValue *k = &constants[bx];
        if (k->kind == (uint8_t)UVAL_INT && k->v.i == match) {
            return (int)i;
        }
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * T8 — EMIT-009: SEP_PIPE next_reg-- without freereg sync
 *
 * Pre-fix: AST_BIN_SEP SEP_PIPE arm did `if (next_reg > 0U) next_reg--;`,
 * which decrements past freereg when the LHS bumps freereg permanently
 * (e.g., LHS ends in a function literal, which lifts freereg via
 * emit_function_literal's `e->current_fs->freereg++`).
 *
 * Trigger: `5 | function() { 1 } | 2` (left-folded SEP_PIPE) at chunk top.
 *   - Inner `5 | function(){1}`: LHS=5 at r1; RHS=function literal at r1
 *     (closure dst pulled from freereg=1, freereg→2, next_reg=2).
 *   - Outer `... | 2`: pre-fix next_reg-- → next_reg=1, freereg=2 (DESYNC).
 *     RHS=2 calls alloc_reg → A=1; LOADK 2 lands at r1, clobbering the
 *     closure register from `function(){1}`.  Post-fix: next_reg=freereg=2,
 *     LOADK 2 lands at r2.
 * ----------------------------------------------------------------------- */

UTEST(emit_sep_pipe_does_not_alias_lhs_temp_with_rhs) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UModule module; memset(&module, 0, sizeof(module));

    UEmitError rc = compile_src(&vm, &arena, &module,
                                "5 | function() { 1 } | 2");
    UASSERT_EQ((int)EMIT_OK, (int)rc);

    /* Find the outer LOADK 2 instruction in the chunk-top instructions. */
    int idx = find_loadk_int(module.instructions, module.instr_count,
                             module.constants, 2);
    UASSERT(idx >= 0);

    /* Pre-fix: A=1 (clobbers the inner closure at r1).
     * Post-fix: A>=2 (above freereg). */
    uint8_t a = uinstr_a(module.instructions[idx]);
    UASSERT(a >= 2U);

    umodule_destroy(&module);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Suite entry point
 * ----------------------------------------------------------------------- */

void test_emit_freereg_drift_suite(void) {
    utest_run("emit_sep_pipe_does_not_alias_lhs_temp_with_rhs",
              emit_sep_pipe_does_not_alias_lhs_temp_with_rhs);
}
