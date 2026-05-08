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
 * T9 — EMIT-010: Watcher-install free_reg leaves freereg stale
 *
 * emit_function_literal raises BOTH next_reg and freereg in lockstep
 * when compiling the cond / body / onleave closures.  The plain free_reg
 * called by the install arms only decrements next_reg, leaving freereg
 * promoted 1-3 slots above next_reg.  Subsequent var-decls then land at
 * slot freereg+0 instead of next_reg+0, wasting register slots.
 *
 * Fix: install arms use free_reg_freereg_synced, which clamps freereg
 * down in lockstep with next_reg.
 *
 * Each test compiles a function whose body issues a watcher install
 * followed by `var c = 99;`, then asserts that c lands at the expected
 * slot (immediately above any pre-existing locals + the LOADNIL slot
 * that watcher install writes).
 * ----------------------------------------------------------------------- */

/* The leak is observable at chunk-top: emit_function_literal called
 * during install raises freereg in lockstep with next_reg, but plain
 * free_reg() decrements only next_reg.  At chunk-top, statements are
 * NOT wrapped in an AST_BLOCK that resets freereg between siblings, so
 * uemit_statement's sync (next_reg = freereg) carries the leaked value
 * forward.  module.max_reg ends up inflated by the closure depth.
 *
 * Inside a function-body block, the leak is masked by emit_block_arm's
 * `freereg = fs_temp_floor(...)` reset between statements — so these
 * tests use chunk-top form. */

UTEST(emit_watcher_install_freereg_balanced_at) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UModule module; memset(&module, 0, sizeof(module));

    UEmitError rc = compile_src(&vm, &arena, &module,
        "var a = 1; var b = 2;"
        "at (Realm.a > Realm.b) Realm.a = Realm.a + 1;"
        "var c = function() { 99 };");
    UASSERT_EQ((int)EMIT_OK, (int)rc);

    /* Pre-fix: module.max_reg leaks 2 slots (one per emit_function_literal
     * call: cond, body).  Post-fix: leak gone.  Use 6 as a strict ceiling
     * post-fix; pre-fix routinely exceeds this. */
    UASSERT(module.max_reg <= 3U);

    umodule_destroy(&module);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

UTEST(emit_watcher_install_freereg_balanced_whenever) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UModule module; memset(&module, 0, sizeof(module));

    UEmitError rc = compile_src(&vm, &arena, &module,
        "var a = 1; var b = 2;"
        "whenever (Realm.a > Realm.b) Realm.a = Realm.a + 1;"
        "var c = function() { 99 };");
    UASSERT_EQ((int)EMIT_OK, (int)rc);
    UASSERT(module.max_reg <= 3U);

    umodule_destroy(&module);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* waituntil intentionally has no max_reg-observable test: it compiles a
 * single cond closure, so the install's one free_reg + LOADNIL sequence
 * is self-clamping (LOADNIL bumps freereg back up to next_reg).  The
 * fix at the WAITUNTIL_INSTALL site is symmetry-only — same shape as
 * AT_INSTALL but no measurable defect.  Coverage at the
 * free_reg_freereg_synced helper level (via the AT/whenever/at-event
 * tests) is sufficient. */

UTEST(emit_watcher_install_freereg_balanced_at_event) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UModule module; memset(&module, 0, sizeof(module));

    /* at-event leaks event_reg+body_reg+alt_reg slots (alt 0xFF skipped).
     * Stack two at-event installs to amplify the leak above the inner
     * body-closure compilation's natural max_reg. */
    UEmitError rc = compile_src(&vm, &arena, &module,
        "var a = 1; var b = 2;"
        "at (Realm.evt?) Realm.a = Realm.a + 1;"
        "at (Realm.evt2?) Realm.b = Realm.b + 1;"
        "var c = function() { 99 };");
    UASSERT_EQ((int)EMIT_OK, (int)rc);
    /* Pre-fix: each at-event leaks 1-2 slots; two installs push max_reg
     * past the post-fix ceiling.  Post-fix: max_reg stays at the inner
     * body-closure compilation high water (= 4). */
    UASSERT(module.max_reg <= 4U);

    umodule_destroy(&module);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * T10 — EMIT-011: alloc_reg doesn't bump fs->max_reg_seen
 *
 * alloc_reg only updates the EMITTER's high-water (e->max_reg_seen)
 * and not the per-FuncState fs->max_reg_seen.  uemit_close_function
 * rolls fs->max_reg_seen into target_proto->max_reg when closing a
 * nested function — so the nested proto's max_reg can under-report
 * the actual peak register usage of its body.  At runtime the VM
 * allocates only (proto->max_reg + 1) slots; under-reporting causes
 * corruption.
 *
 * Trigger: nested function whose body uses multiple temps via
 * leaf-expr alloc_reg paths (AST_INT, AST_BOOL, AST_NIL, AST_NOOP)
 * but never goes through emit_compare or emit_ident which already
 * sync fs->max_reg_seen.
 * ----------------------------------------------------------------------- */

UTEST(emit_nested_proto_max_reg_includes_inner_temps) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UModule module; memset(&module, 0, sizeof(module));

    /* Inner function body: chained AST_BINARY over integer literals
     * only — AST_INT goes through alloc_reg, which pre-fix did not
     * sync fs->max_reg_seen.  No AST_IDENT or AST_COMPARE in the
     * body, both of which already sync the FuncState high water.
     *
     * Pre-fix: peak register usage reaches r2 (LHS=1 at r1, RHS=2 at r2,
     * OP_ADD r1,r1,r2), but fs->max_reg_seen stays at 1 (the
     * global_slot pre-reservation).  proto.max_reg = 1 — VM allocates
     * 2 slots, instruction references r2 → out-of-bounds at runtime.
     *
     * Post-fix: alloc_reg syncs fs->max_reg_seen, so proto.max_reg
     * tracks the actual peak. */
    UEmitError rc = compile_src(&vm, &arena, &module,
        "function f() { return function () { return 1 + 2; }; }");
    UASSERT_EQ((int)EMIT_OK, (int)rc);

    /* The inner ()->(1+2) proto: nparams==0.  Both nested protos have
     * nparams==0 (f and the inner closure); pick the one whose
     * instructions contain OP_ADD. */
    UProto *p = NULL;
    for (size_t i = 0; i < module.nested_count; i++) {
        UProto *q = module.nested[i];
        if (q == NULL) continue;
        for (size_t j = 0; j < q->instr_count; j++) {
            if (uinstr_op(q->instructions[j]) == OP_ADD) {
                p = q;
                break;
            }
        }
        if (p != NULL) break;
    }
    UASSERT(p != NULL);

    /* Peak register usage is r2 (OP_ADD's C operand).  proto.max_reg
     * must be >= 2 so the VM allocates >= 3 slots. */
    UASSERT(p->max_reg >= 2U);

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
    utest_run("emit_watcher_install_freereg_balanced_at",
              emit_watcher_install_freereg_balanced_at);
    utest_run("emit_watcher_install_freereg_balanced_whenever",
              emit_watcher_install_freereg_balanced_whenever);
    utest_run("emit_watcher_install_freereg_balanced_at_event",
              emit_watcher_install_freereg_balanced_at_event);
    utest_run("emit_nested_proto_max_reg_includes_inner_temps",
              emit_nested_proto_max_reg_includes_inner_temps);
}
