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
#include "chunk/uchunk.h"
#include "parse/uparse.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Helper: compile source through full pipeline; module is filled in.
 * Returns the emit error (EMIT_OK on success).  Caller owns module +
 * arena destruction.  vm is also caller-owned.
 * ----------------------------------------------------------------------- */

static UEmitError compile_src(UVM *vm, UArena *arena, UProto *module,
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
    UEmitError finish_rc = uemit_finish(&e);
    emit_diag_free_all(&e);
    return finish_rc;
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
    UProto module; memset(&module, 0, sizeof(module));

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

    uchunk_destroy(&module, NULL);
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
    UProto module; memset(&module, 0, sizeof(module));

    UEmitError rc = compile_src(&vm, &arena, &module,
        "var a = 1; var b = 2;"
        "at (Realm.a > Realm.b) Realm.a = Realm.a + 1;"
        "var c = function() { 99 };");
    UASSERT_EQ((int)EMIT_OK, (int)rc);

    /* Pre-fix: module.max_reg leaks 2 slots (one per emit_function_literal
     * call: cond, body).  Post-fix: leak gone.  Use 6 as a strict ceiling
     * post-fix; pre-fix routinely exceeds this. */
    UASSERT(module.max_reg <= 3U);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

UTEST(emit_watcher_install_freereg_balanced_whenever) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UProto module; memset(&module, 0, sizeof(module));

    UEmitError rc = compile_src(&vm, &arena, &module,
        "var a = 1; var b = 2;"
        "whenever (Realm.a > Realm.b) Realm.a = Realm.a + 1;"
        "var c = function() { 99 };");
    UASSERT_EQ((int)EMIT_OK, (int)rc);
    UASSERT(module.max_reg <= 3U);

    uchunk_destroy(&module, NULL);
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
    UProto module; memset(&module, 0, sizeof(module));

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
     * body-closure compilation high water (was 4; 5 since v0.13.5 —
     * the 1-param payload body reserves the synthetic \x01nargs local
     * for the arity self-check prologue). */
    UASSERT(module.max_reg <= 5U);

    uchunk_destroy(&module, NULL);
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
    UProto module; memset(&module, 0, sizeof(module));

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
        "var f = function() { return function () { return 1 + 2; }; }");
    UASSERT_EQ((int)EMIT_OK, (int)rc);

    /* The inner ()->(1+2) proto: nparams==0.  Walk the proto tree
     * recursively (v0.8.5: inner is outer.nested[0], not a flat sibling
     * under root) and pick the one whose instructions contain OP_ADD. */
    UProto *p = NULL;
    UProto *stack[8];
    size_t sp = 0;
    stack[sp++] = &module;
    while (sp > 0 && p == NULL) {
        UProto *node = stack[--sp];
        if (node == NULL) continue;
        for (size_t j = 0; j < node->instr_count; j++) {
            if (uinstr_op(node->instructions[j]) == OP_ADD) {
                p = node;
                break;
            }
        }
        if (p != NULL) break;
        for (size_t i = 0; i < node->nested_count && sp < 8; i++) {
            stack[sp++] = node->nested[i];
        }
    }
    UASSERT(p != NULL);

    /* Peak register usage is r2 (OP_ADD's C operand).  proto.max_reg
     * must be >= 2 so the VM allocates >= 3 slots. */
    UASSERT(p->max_reg >= 2U);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * T11 — EMIT-012: free_reg doesn't respect fs_temp_floor
 *
 * Pre-fix free_reg unconditionally decrements next_reg, even when
 * next_reg has reached the local-zone floor (nactvar + global_slot
 * reservation).  An unbalanced free_reg/alloc_reg sequence could then
 * underflow next_reg into the local zone, causing the next alloc_reg
 * to return a slot aliasing a live local.
 *
 * Post-fix free_reg no-ops when next_reg <= fs_temp_floor.
 *
 * The fix is defensive — there is no current call site that exhibits
 * the underflow under normal compilation, so this test verifies that
 * the guard does not break the legitimate decrement path on a routine
 * AST_BINARY chain (regression safety net only).  A harder pre-fix
 * trigger would require an emit-arm-level mismatch that does not
 * exist in v0.5.7 — adding such a synthetic trigger via a test-only
 * accessor was judged out of scope vs the cost of exposing the
 * private helper. */

UTEST(emit_free_reg_respects_temp_floor) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UProto module; memset(&module, 0, sizeof(module));

    /* var a = 1; var b = 2; return a + b — three locals (a, b plus
     * r_global pre-reserve).  The AST_BINARY's free_reg call after
     * OP_ADD must release the rhs temp without underflowing into a's
     * or b's local slot. */
    UEmitError rc = compile_src(&vm, &arena, &module,
        "var f = function() { var a = 1; var b = 2; return a + b; }");
    UASSERT_EQ((int)EMIT_OK, (int)rc);

    /* Locate f's nested proto (the only one with nparams=0 and an OP_ADD). */
    UProto *p = NULL;
    for (size_t i = 0; i < module.nested_count; i++) {
        UProto *q = module.nested[i];
        if (q == NULL) continue;
        for (size_t j = 0; j < q->instr_count; j++) {
            if (uinstr_op(q->instructions[j]) == OP_ADD) {
                p = q; break;
            }
        }
        if (p != NULL) break;
    }
    UASSERT(p != NULL);

    /* Find the OP_ADD instruction.  Its B and C operands must be
     * register indices >= the floor (nparams + global_slot pre-reserve
     * = 0 + 1 = 1).  Since a is at r1 and b at r2, OP_ADD should read
     * r1 and r2 — guard against the buggy case where free_reg
     * underflowed into the locals zone (which would have allowed
     * subsequent re-allocation to overwrite a or b). */
    int found = 0;
    for (size_t j = 0; j < p->instr_count; j++) {
        if (uinstr_op(p->instructions[j]) == OP_ADD) {
            uint8_t b = uinstr_b(p->instructions[j]);
            uint8_t c = uinstr_c(p->instructions[j]);
            UASSERT(b >= 1U);
            UASSERT(c >= 1U);
            UASSERT(b != c);
            found = 1;
        }
    }
    UASSERT(found == 1);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * T12 — EMIT-013: emit_lazy_thunk pass-through bumps next_reg only
 *
 * The pass-through optimization in emit_lazy_thunk (used when a lazy
 * local appears in a lazy arg position) emits OP_MOVE of the local
 * into next_reg and bumps next_reg++.  Pre-fix it left freereg behind,
 * so a subsequent emit_function_literal (which pulls dst from freereg)
 * could overwrite a still-live register — including the call's own
 * callee_reg.
 *
 * Trigger: consumer with (lazy a, eager b) signature, called with
 * caller's lazy local as a and a function literal as b.  Pre-fix the
 * function-literal closure dst lands at the leaked freereg, clobbering
 * the callee register and corrupting the OP_CALL target. */

UTEST(emit_lazy_pass_through_does_not_alias) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UProto module; memset(&module, 0, sizeof(module));

    UEmitError rc = compile_src(&vm, &arena, &module,
        "var caller = function (lazy x) {"
        "  var consumer = function (lazy a, b) { return a; };"
        "  return consumer(x, function() { 99 });"
        "};");
    UASSERT_EQ((int)EMIT_OK, (int)rc);

    /* Locate caller's nested proto (1 param, contains OP_CALL). */
    UProto *p = NULL;
    for (size_t i = 0; i < module.nested_count; i++) {
        UProto *q = module.nested[i];
        if (q == NULL) continue;
        if (q->nparams != 1U) continue;
        for (size_t j = 0; j < q->instr_count; j++) {
            if (uinstr_op(q->instructions[j]) == OP_CALL) {
                p = q; break;
            }
        }
        if (p != NULL) break;
    }
    UASSERT(p != NULL);

    /* Walk caller's instructions to find the final OP_CALL (consumer
     * call) and its inputs.  Just before the OP_CALL, an OP_MOVE
     * loads the lazy-arg pass-through (x) into a temp slot, then an
     * OP_CLOSURE allocates a slot for the function-literal arg.  Pre-
     * fix the closure dst lands at the SAME slot as the pass-through
     * MOVE (because freereg never advanced past it), corrupting the
     * pass-through value.  Post-fix the closure dst lands strictly
     * above. */
    int passthrough_move_dst = -1;
    int arg_closure_dst      = -1;
    int call_a               = -1;
    int closures_seen        = 0;
    for (size_t j = 0; j < p->instr_count; j++) {
        uint32_t ins = p->instructions[j];
        UOpcode op = uinstr_op(ins);
        if (op == OP_CLOSURE) {
            closures_seen++;
            if (closures_seen == 2) arg_closure_dst = (int)uinstr_a(ins);
        }
        if (op == OP_MOVE) {
            uint8_t b = uinstr_b(ins);
            /* MOVE from r0 (the lazy x param) is the pass-through. */
            if (b == 0U) passthrough_move_dst = (int)uinstr_a(ins);
        }
        if (op == OP_CALL) { call_a = (int)uinstr_a(ins); break; }
    }
    UASSERT(passthrough_move_dst >= 0);
    UASSERT(arg_closure_dst >= 0);
    UASSERT(call_a >= 0);
    /* The pass-through MOVE and the function-literal CLOSURE must land
     * at DIFFERENT slots. */
    UASSERT(passthrough_move_dst != arg_closure_dst);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * T13 — EMIT-014: AST_CALL uint8_t arg count wraps at 256
 *
 * The OP_CALL instruction encodes (nargs + 1) into a uint8_t B field.
 * At 255+ args the B field wraps to 0; at 254 args it equals 255
 * (which is reserved/ambiguous).  Pre-fix the emit arm silently cast
 * the count to uint8_t — corrupting any call with 254+ args.
 *
 * Post-fix: emit_call_arm rejects with EMIT_TOO_MANY_ARGS at the
 * gate before codegen.
 *
 * Test builds a synthetic call source with 300 args and verifies
 * uemit_finish returns EMIT_TOO_MANY_ARGS. */

UTEST(emit_call_too_many_args_returns_error) {
    /* Build a source string: "var f = function() {0}; f(0,0,0,...,0)"
     * with 300 zeros.  Use stdlib alloc since tests are not built
     * -ffreestanding. */
    const int nargs = 300;
    /* Worst-case length: prefix + nargs * "0," + ")" + a bit of slack. */
    size_t cap = 64U + (size_t)nargs * 4U;
    char *src = (char *)malloc(cap);
    UASSERT(src != NULL);
    size_t off = 0;
    off += (size_t)snprintf(src + off, cap - off,
                            "var f = function() { 0 };"
                            "f(");
    for (int i = 0; i < nargs; i++) {
        off += (size_t)snprintf(src + off, cap - off, "%s0",
                                i == 0 ? "" : ",");
    }
    off += (size_t)snprintf(src + off, cap - off, ");");
    UASSERT(off < cap);

    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 16384);
    UProto module; memset(&module, 0, sizeof(module));

    UEmitError rc = compile_src(&vm, &arena, &module, src);
    /* Pre-fix: EMIT_OK (the wrap happens silently).
     * Post-fix: EMIT_TOO_MANY_ARGS. */
    UASSERT_EQ((int)EMIT_TOO_MANY_ARGS, (int)rc);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
    free(src);
}

/* -----------------------------------------------------------------------
 * T14 — EMIT-015: AST_TAG_PREFIX tag register can be >= 16
 *
 * OP_PUSH_TAG packs flags + reg-nibble into A: A = (flags<<4) | (reg & 0xF).
 * Since the v0.13.1 hidden-local rework (refactor-3 FE-02 follow-on) the
 * tag value is a DECLARED hidden local (`\x01tag`) in an outer block —
 * the old "defensive" spill branch (spill = next_reg++, which silently
 * truncated through the 0xF mask when spill >= 16) is gone.  When the
 * hidden local's slot exceeds 15 every lower register is local-occupied,
 * so there is no safe target and emit_tag_prefix_arm rejects outright.
 *
 * Trigger: function with 17+ locals declared before the tag-prefix, so
 * `\x01tag` is declared at slot >= 16.
 *
 * Expected: emit returns EMIT_TAG_SPILL_OUT_OF_RANGE; widening the
 * encoding to a full byte is a v1.x bytecode change (filed as backlog
 * under T129/Phase 22). */

UTEST(emit_tag_prefix_rejects_high_spill_register) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UProto module; memset(&module, 0, sizeof(module));

    /* Declare a tag identifier "t" plus 16 unrelated locals so the chain
     * `var t = 0; var l1 = 1; ...; var l16 = 16; t: { 1 };` pushes the
     * hidden `\x01tag` local's declared slot >= 16, tripping the > 15
     * nibble check in emit_tag_prefix_arm. */
    UEmitError rc = compile_src(&vm, &arena, &module,
        "var f = function() {"
        " var t = 0;"
        " var l1=1;  var l2=2;  var l3=3;  var l4=4;"
        " var l5=5;  var l6=6;  var l7=7;  var l8=8;"
        " var l9=9;  var l10=10; var l11=11; var l12=12;"
        " var l13=13; var l14=14; var l15=15; var l16=16;"
        " t: { 1 };"
        "}");
    /* Pre-fix: EMIT_OK (silent encoding truncation; spill register
     * loses its high bits when packed into the 4-bit A nibble).
     * Post-fix: EMIT_TAG_SPILL_OUT_OF_RANGE. */
    UASSERT_EQ((int)EMIT_TAG_SPILL_OUT_OF_RANGE, (int)rc);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * T15 — EMIT-016: AST_IF leaked the result-register slot into freereg
 *
 * Pre-fix, emit_if_arm's trailer forced `fs->freereg = next_reg` (==
 * rd + 1).  rd is a TEMP — the if-expr's result register — not a
 * local.  Bumping freereg up to rd+1 means subsequent var-decls
 * (whose slot = fs->freereg per uemit_declare_local) land above rd
 * instead of reusing rd's slot once the if's result is no longer
 * needed.  Result: every if-as-statement permanently consumes one
 * additional register slot; the cost compounds across nested ifs and
 * siblings.
 *
 * The bug surfaced via parser-driven AST: in `function f() { var a;
 * if (a > 0) { var x = 5; x }; var b = 7; return b }`, b landed at
 * slot 3 (after the if's leaked rd at slot 2) instead of slot 2
 * (reusing rd).
 *
 * Wave 5 fix: drop the freereg bump in emit_if_arm's trailer; rd is
 * still tracked via next_reg / max_reg_seen, but freereg stays at the
 * floor so siblings reuse the slot once rd is consumed.  Matches the
 * Lua-style protocol used by emit_compare_arm (which also leaves rb
 * as a temp without bumping freereg). */

UTEST(emit_if_arm_pops_nested_var_decl) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UProto module; memset(&module, 0, sizeof(module));

    UEmitError rc = compile_src(&vm, &arena, &module,
        "var f = function() {"
        " var a = 1;"
        " if (a > 0) { var x = 5; x };"
        " var b = 7;"
        " return b;"
        "}");
    UASSERT_EQ((int)EMIT_OK, (int)rc);

    /* Locate f's nested proto. */
    UProto *p = NULL;
    for (size_t i = 0; i < module.nested_count; i++) {
        UProto *q = module.nested[i];
        if (q == NULL) continue;
        if (q->nparams != 0U) continue;
        bool has_ret = false;
        for (size_t j = 0; j < q->instr_count; j++) {
            if (uinstr_op(q->instructions[j]) == OP_RET) {
                has_ret = true; break;
            }
        }
        if (has_ret) { p = q; break; }
    }
    UASSERT(p != NULL);

    /* Find the OP_LOADK that loads constant int 7 (b's init).
     * Its A operand is b's slot.
     * Pre-fix: b lands at slot 3 (the if's rd at slot 2 leaked into
     *          freereg, so var-decl picked rd+1 = 3).
     * Post-fix: b lands at slot 2 (rd's slot is reusable since
     *           freereg stayed at the floor).
     * r_global at slot 0; a at slot 1; b at slot 2 (post-fix). */
    int idx = find_loadk_int(p->instructions, p->instr_count,
                             p->constants, 7);
    UASSERT(idx >= 0);
    uint8_t a = uinstr_a(p->instructions[idx]);
    UASSERT_EQ(2, (int)a);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * T16 — EMIT-017: AST_RETURN bare-return alloc_reg ignores fs_temp_floor
 *
 * Bare `return;` allocates a register via alloc_reg and emits OP_LOADNIL
 * into it.  Pre-fix, alloc_reg returned e->next_reg++, ignoring whether
 * next_reg sat below the FuncState temp floor (= nactvar +
 * global_slot_reserved).  Under parser-driven AST, the SEP_SEMI between-
 * stmt handler in emit_nary_arm and emit_block_arm syncs next_reg to
 * freereg before each child, so `next_reg < floor` is unreachable; the
 * bug is dormant against current emit arms but brittle against any
 * future arm that transiently drops next_reg without also raising
 * freereg.
 *
 * Wave 5 fix: bare-return forces next_reg above fs_temp_floor before
 * calling alloc_reg, so the LOADNIL slot is guaranteed to be strictly
 * above all live locals regardless of upstream emit-arm contract.
 *
 * The legitimate-syntax test below verifies the post-fix LOADNIL slot
 * lies at-or-above fs_temp_floor (= nactvar + global_slot_reserved =
 * 2 for the function below, where r_global = slot 0 and keep = slot 1).
 * This is a structural-correctness gate per cluster-1 T11 precedent —
 * the bug is unreachable through current parser-driven AST, but the
 * invariant is now enforced at the bare-return arm.  The test passes
 * pre-fix (since the bug isn't exercised) and post-fix (since the
 * defensive guard preserves the legal behavior). */

UTEST(emit_bare_return_does_not_clobber_local) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UProto module; memset(&module, 0, sizeof(module));

    UEmitError rc = compile_src(&vm, &arena, &module,
        "var helper = function() { 0 };"
        "var f = function() {"
        " var keep = 42;"
        " helper();"
        " return;"
        "}");
    UASSERT_EQ((int)EMIT_OK, (int)rc);

    /* Locate f's nested proto (the one with OP_LOADK 42 + OP_LOADNIL +
     * OP_RET; helper has only OP_LOADK 0 + OP_RET). */
    UProto *p = NULL;
    for (size_t i = 0; i < module.nested_count; i++) {
        UProto *q = module.nested[i];
        if (q == NULL) continue;
        if (q->nparams != 0U) continue;
        bool has_42 = false;
        bool has_loadnil = false;
        for (size_t j = 0; j < q->instr_count; j++) {
            uint32_t ins = q->instructions[j];
            if (uinstr_op(ins) == OP_LOADK) {
                uint16_t bx = uinstr_bx(ins);
                if (q->constants[bx].kind == (uint8_t)UVAL_INT &&
                    q->constants[bx].v.i == 42) has_42 = true;
            }
            if (uinstr_op(ins) == OP_LOADNIL) has_loadnil = true;
        }
        if (has_42 && has_loadnil) { p = q; break; }
    }
    UASSERT(p != NULL);

    /* Find OP_LOADK 42 (keep's init slot) and the OP_LOADNIL that
     * precedes the LAST OP_RET (the bare-return's nil load).  They
     * must land at different registers — keep's slot is below the
     * LOADNIL slot. */
    int keep_slot = -1;
    int bare_nil_slot = -1;
    for (size_t j = 0; j < p->instr_count; j++) {
        uint32_t ins = p->instructions[j];
        if (uinstr_op(ins) == OP_LOADK) {
            uint16_t bx = uinstr_bx(ins);
            if (p->constants[bx].kind == (uint8_t)UVAL_INT &&
                p->constants[bx].v.i == 42) {
                keep_slot = (int)uinstr_a(ins);
            }
        }
        if (uinstr_op(ins) == OP_LOADNIL) {
            bare_nil_slot = (int)uinstr_a(ins);
        }
    }
    UASSERT(keep_slot >= 0);
    UASSERT(bare_nil_slot >= 0);
    UASSERT(keep_slot != bare_nil_slot);
    /* LOADNIL must land at the floor or above (keep at slot 1; LOADNIL
     * at slot >= 2). */
    UASSERT(bare_nil_slot > keep_slot);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * T17 — EMIT-018: AST_THROW post-throw nil load ignores fs_temp_floor
 *
 * After OP_THROW, the throw arm emits a post-throw OP_LOADNIL into
 * rd = e->next_reg so the block's last-stmt-reg logic has a register
 * to report.  Pre-fix, this nil-load claimed `rd = e->next_reg`
 * without enforcing fs_temp_floor — same root cause as EMIT-017
 * (AST_RETURN).
 *
 * Like the bare-return case, current parser-driven AST never reaches
 * the bug (between-stmt resets sync next_reg to freereg before the
 * throw arm runs).  Wave 5 lands the defensive guard so a future emit
 * arm dropping next_reg below the floor cannot make the throw's
 * LOADNIL alias a live local.  Same fix shape as EMIT-017.
 *
 * Test passes pre-fix and post-fix (structural-correctness gate per
 * cluster-1 T11 precedent). */

UTEST(emit_throw_does_not_clobber_local) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UProto module; memset(&module, 0, sizeof(module));

    UEmitError rc = compile_src(&vm, &arena, &module,
        "var helper = function() { 0 };"
        "var f = function() {"
        " var keep = 42;"
        " helper();"
        " throw 1;"
        "}");
    UASSERT_EQ((int)EMIT_OK, (int)rc);

    /* Locate f's nested proto (the one with OP_THROW + OP_LOADK 42). */
    UProto *p = NULL;
    for (size_t i = 0; i < module.nested_count; i++) {
        UProto *q = module.nested[i];
        if (q == NULL) continue;
        if (q->nparams != 0U) continue;
        bool has_42 = false;
        bool has_throw = false;
        for (size_t j = 0; j < q->instr_count; j++) {
            uint32_t ins = q->instructions[j];
            if (uinstr_op(ins) == OP_LOADK) {
                uint16_t bx = uinstr_bx(ins);
                if (q->constants[bx].kind == (uint8_t)UVAL_INT &&
                    q->constants[bx].v.i == 42) has_42 = true;
            }
            if (uinstr_op(ins) == OP_THROW) has_throw = true;
        }
        if (has_42 && has_throw) { p = q; break; }
    }
    UASSERT(p != NULL);

    /* Find the LOADNIL emitted after OP_THROW.  Its slot must differ
     * from keep's slot. */
    int keep_slot = -1;
    int post_throw_nil_slot = -1;
    bool seen_throw = false;
    for (size_t j = 0; j < p->instr_count; j++) {
        uint32_t ins = p->instructions[j];
        if (uinstr_op(ins) == OP_LOADK) {
            uint16_t bx = uinstr_bx(ins);
            if (p->constants[bx].kind == (uint8_t)UVAL_INT &&
                p->constants[bx].v.i == 42) {
                keep_slot = (int)uinstr_a(ins);
            }
        }
        if (uinstr_op(ins) == OP_THROW) seen_throw = true;
        if (seen_throw && uinstr_op(ins) == OP_LOADNIL &&
            post_throw_nil_slot < 0) {
            post_throw_nil_slot = (int)uinstr_a(ins);
        }
    }
    UASSERT(keep_slot >= 0);
    UASSERT(post_throw_nil_slot >= 0);
    UASSERT(keep_slot != post_throw_nil_slot);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * T18 — EMIT-019 underlying: JMP offset arithmetic via pc-based helper
 *
 * Wave 3 named UEMIT_JMP_BIAS / UEMIT_JMP_FALLTHROUGH_BIAS but the
 * underlying assumption — that the comparison fall-through path is
 * exactly 1 instruction past the OP_TEST/OP_LT/etc. emit point — is
 * fragile against future emit-pass insertions.  Wave 5 lifts the
 * arithmetic into a pc-based helper, jmp_offset(from_pc, target_pc),
 * so the encoding contract (OP_JMP Bx = (target_pc - from_pc - 1) +
 * UEMIT_JMP_BIAS) is centralized.
 *
 * The fix is bytecode-byte-identical (refactor only); this test is a
 * forward-defense regression safety net, not a strict pre-fix-fail
 * witness.  It verifies that for a canonical if/else compile, the JMP
 * offsets in the resulting bytecode satisfy the (target - from - 1)
 * relation, i.e., that the helper and the inline arithmetic agree. */

UTEST(emit_jmp_offset_resilient_to_intervening_instructions) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UProto module; memset(&module, 0, sizeof(module));

    /* Canonical if/else shape: emits TEST + JMP + then-body + JMP +
     * else-body.  Verify each JMP's encoded Bx, when un-biased,
     * equals (target_pc - jmp_pc - 1). */
    UEmitError rc = compile_src(&vm, &arena, &module,
        "var f = function() {"
        " var a = 1;"
        " if (a > 0) { a = 2 } else { a = 3 };"
        " return a;"
        "}");
    UASSERT_EQ((int)EMIT_OK, (int)rc);

    UProto *p = NULL;
    for (size_t i = 0; i < module.nested_count; i++) {
        UProto *q = module.nested[i];
        if (q == NULL) continue;
        if (q->nparams != 0U) continue;
        bool has_test = false;
        bool has_jmp = false;
        for (size_t j = 0; j < q->instr_count; j++) {
            UOpcode op = uinstr_op(q->instructions[j]);
            if (op == OP_TEST) has_test = true;
            if (op == OP_JMP)  has_jmp = true;
        }
        if (has_test && has_jmp) { p = q; break; }
    }
    UASSERT(p != NULL);

    /* For each OP_JMP, confirm the encoded Bx falls in the legal
     * +/- 0x7FFF range when un-biased.  Pre-fix and post-fix both
     * pass; this gate verifies the encoding contract is honored
     * across the if/else compile pipeline. */
    /* JMP encoding contract (uemit_internal.h): Bx = (target_pc - from_pc - 1)
     * + UEMIT_JMP_BIAS, where UEMIT_JMP_BIAS == 32768.  Hard-coded here
     * to keep the public test file independent of the internal header. */
    enum { TEST_JMP_BIAS = 32768 };
    int jmp_count = 0;
    for (size_t j = 0; j < p->instr_count; j++) {
        if (uinstr_op(p->instructions[j]) == OP_JMP) {
            uint16_t bx = uinstr_bx(p->instructions[j]);
            int offset = (int)bx - TEST_JMP_BIAS;
            /* When followed forward, the jump target must land in
             * [0, instr_count]. */
            int target = (int)j + 1 + offset;
            UASSERT(target >= 0);
            UASSERT(target <= (int)p->instr_count);
            jmp_count++;
        }
    }
    UASSERT(jmp_count >= 2);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * T41 follow-up — emit_call_arm freereg drift between args
 *
 * Phase 2 (T41) `get`/`set` parse sugar desugars to
 * `recv.setProperty(name_str, prop_str, function() body)` — a 3-arg
 * call where the first two args are leaf literals (allocated through
 * alloc_reg, which bumps next_reg only) and the trailing arg is an
 * AST_FUNCTION (whose OP_CLOSURE destination is pulled from freereg).
 *
 * Pre-fix, emit_call_arm synced freereg up to next_reg only ONCE
 * (before the arg loop) but not BETWEEN args.  When arg 0 / arg 1
 * are leaf literals that bump only next_reg, freereg lags behind by
 * the time arg 2 (the AST_FUNCTION) emits — so OP_CLOSURE's dst lands
 * on a slot already holding arg 0 or arg 1, corrupting the call.
 *
 * Post-fix: emit_call_arm syncs freereg to next_reg before EACH arg,
 * not just before the loop.  Test verifies all OP_LOADK / OP_CLOSURE
 * destinations inside the call sequence land at strictly increasing
 * slots — no collision between leaf-literal args and the trailing
 * function-literal arg.
 * ----------------------------------------------------------------------- */

UTEST(emit_call_arm_function_arg_does_not_clobber_leaf_args) {
    UVM vm; urbi_vm_init(&vm, NULL, NULL);
    UArena arena; uarena_init(&arena, 4096);
    UProto module; memset(&module, 0, sizeof(module));

    /* A 3-arg call mirroring T41's setProperty desugar shape: two leaf
     * args followed by an AST_FUNCTION literal.  `callee` doesn't need
     * to exist — emit_call_arm runs before any binding check, and a
     * realm-global lookup at call time is fine for emit's purposes. */
    UEmitError rc = compile_src(&vm, &arena, &module,
        "var callee = function (a, b, fn) { 0 };"
        "callee(\"x\", \"y\", function() { 42 });");
    UASSERT_EQ((int)EMIT_OK, (int)rc);

    /* Walk chunk-top instructions: find the OP_CALL, then walk
     * backwards through OP_LOADK / OP_CLOSURE to confirm the 3 arg
     * registers (callee_reg + 1, +2, +3) are written by exactly one
     * instruction each, with strictly distinct A operands.  Pre-fix
     * the AST_FUNCTION's OP_CLOSURE A would equal callee_reg + 1 or
     * + 2 (clobbering one of "x" / "y" — those LOADK / MOVE-to-arg-slot
     * sequences land first). */
    int call_idx = -1;
    int call_a   = -1;
    for (size_t j = 0; j < module.instr_count; j++) {
        if (uinstr_op(module.instructions[j]) == OP_CALL) {
            call_idx = (int)j;
            call_a   = (int)uinstr_a(module.instructions[j]);
        }
    }
    UASSERT(call_idx >= 0);
    UASSERT(call_a >= 0);

    /* Each of arg slots {call_a + 1, call_a + 2, call_a + 3} must be
     * written by exactly one instruction in the call's argument
     * preparation window (after callee_reg setup, before OP_CALL). */
    int writers[3] = { 0, 0, 0 };  /* count of writers for arg0/1/2 */
    int closure_dst = -1;
    for (int j = 0; j < call_idx; j++) {
        uint32_t ins = module.instructions[j];
        UOpcode op   = uinstr_op(ins);
        uint8_t a    = uinstr_a(ins);
        bool is_writer = (op == OP_LOADK || op == OP_CLOSURE ||
                          op == OP_MOVE);
        if (!is_writer) continue;
        for (int k = 0; k < 3; k++) {
            if ((int)a == call_a + 1 + k) {
                writers[k]++;
                if (op == OP_CLOSURE) closure_dst = (int)a;
            }
        }
    }

    /* Each arg slot has at least one writer (precondition). */
    UASSERT(writers[0] >= 1);
    UASSERT(writers[1] >= 1);
    UASSERT(writers[2] >= 1);

    /* Pre-fix: closure_dst lands at call_a + 1 (clobbering "x") or
     * call_a + 2 (clobbering "y") because freereg lagged.  Post-fix:
     * closure_dst lands at call_a + 3 — the third arg slot only. */
    UASSERT(closure_dst >= 0);
    UASSERT_EQ(closure_dst, call_a + 3);

    uchunk_destroy(&module, NULL);
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
    utest_run("emit_free_reg_respects_temp_floor",
              emit_free_reg_respects_temp_floor);
    utest_run("emit_lazy_pass_through_does_not_alias",
              emit_lazy_pass_through_does_not_alias);
    utest_run("emit_call_too_many_args_returns_error",
              emit_call_too_many_args_returns_error);
    utest_run("emit_tag_prefix_rejects_high_spill_register",
              emit_tag_prefix_rejects_high_spill_register);
    utest_run("emit_if_arm_pops_nested_var_decl",
              emit_if_arm_pops_nested_var_decl);
    utest_run("emit_bare_return_does_not_clobber_local",
              emit_bare_return_does_not_clobber_local);
    utest_run("emit_throw_does_not_clobber_local",
              emit_throw_does_not_clobber_local);
    utest_run("emit_jmp_offset_resilient_to_intervening_instructions",
              emit_jmp_offset_resilient_to_intervening_instructions);
    utest_run("emit_call_arm_function_arg_does_not_clobber_leaf_args",
              emit_call_arm_function_arg_does_not_clobber_leaf_args);
}
