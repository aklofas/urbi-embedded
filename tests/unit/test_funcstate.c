/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include <stdio.h>

#include "value/uarena.h"
#include "emit/uemit.h"
#include "value/uintern.h"
#include "module/umodule.h"
#include "vm/uvm.h"

/* Expose find_or_install_upvalue for cascade tests. */
int find_or_install_upvalue(struct UEmitter *e, struct UFuncState *fs,
                            const char *name, int name_len);

#define UTEST(name) static void name(void)

/* --- helpers --- */
static void setup(UEmitter *e, UModule *m, UArena *a, UVM *v) {
    *m = (UModule){0};
    uarena_init(a, 0);
    urbi_vm_init(v, NULL, NULL);
    uemit_init(e, m, a, v, "test");
}
static void teardown(UModule *m, UArena *a, UVM *v) {
    uarena_destroy(a);
    umodule_destroy(m, NULL);
    urbi_vm_destroy(v);
}

UTEST(funcstate_open_zeroes_freereg_and_nactvar) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);

    UFuncState *fs = uemit_open_function(&e, NULL);
    UASSERT(fs != NULL);
    /* T73: chunk-top pre-reserves r_global_slot=R0, so max_reg_seen starts at 1. */
    UASSERT_EQ((uint8_t)1, fs->max_reg_seen);  // cppcheck-suppress nullPointerRedundantCheck
    UASSERT_EQ(0, fs->nactvar);
    UASSERT_EQ(0, fs->nupvalues);
    UASSERT_EQ(0, fs->nblocks);
    UASSERT(fs->parent == NULL);

    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(funcstate_declare_local_pushes_actvar_and_advances_freereg) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    UFuncState *fs = uemit_open_function(&e, NULL);

    const char *name = ustr_intern(&v, "x", 1);
    int slot = uemit_declare_local(&e, name, 1);
    /* T73: chunk-top pre-reserves R0, so first local is at slot 1. */
    UASSERT_EQ(1, slot);
    UASSERT_EQ(1, fs->nactvar);
    UASSERT_EQ((uint8_t)2, fs->freereg);
    UASSERT(fs->actvars[0].name == name);    /* canonical pointer eq */
    UASSERT_EQ((uint8_t)1, fs->actvars[0].slot);
    UASSERT_EQ((uint8_t)2, fs->max_reg_seen);

    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(funcstate_declare_three_locals) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    UFuncState *fs = uemit_open_function(&e, NULL);

    int s1 = uemit_declare_local(&e, ustr_intern(&v, "a", 1), 1);
    int s2 = uemit_declare_local(&e, ustr_intern(&v, "b", 1), 1);
    int s3 = uemit_declare_local(&e, ustr_intern(&v, "c", 1), 1);
    /* T73: chunk-top pre-reserves R0, so locals start at slot 1. */
    UASSERT_EQ(1, s1);
    UASSERT_EQ(2, s2);
    UASSERT_EQ(3, s3);
    UASSERT_EQ(3, fs->nactvar);
    UASSERT_EQ((uint8_t)4, fs->freereg);

    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(funcstate_redeclare_in_same_scope_errors) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    uemit_open_function(&e, NULL);

    const char *name = ustr_intern(&v, "x", 1);
    UASSERT_EQ(1, uemit_declare_local(&e, name, 1));  /* T73: first local at slot 1 */
    UASSERT_EQ(-1, uemit_declare_local(&e, name, 1));      /* duplicate */
    UASSERT_EQ((int)EMIT_LOCAL_REDECLARE, (int)e.error);

    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(funcstate_max_locals_exhausts_with_proper_error) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    uemit_open_function(&e, NULL);

    /* Push UFS_MAX_LOCALS distinct names. */
    char buf[8];
    for (int i = 0; i < UFS_MAX_LOCALS; i++) {
        int len = snprintf(buf, sizeof buf, "v%04d", i);
        const char *n = ustr_intern(&v, buf, (size_t)len);
        UASSERT(uemit_declare_local(&e, n, len) >= 0);
    }
    /* Next one must fail. */
    const char *over = ustr_intern(&v, "boom", 4);
    UASSERT_EQ(-1, uemit_declare_local(&e, over, 4));
    UASSERT_EQ((int)EMIT_REG_EXHAUSTED, (int)e.error);

    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(funcstate_close_function_pops_to_parent) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);

    UFuncState *outer = uemit_open_function(&e, NULL);
    const UFuncState *inner = uemit_open_function(&e, outer);
    UASSERT(inner->parent == outer);

    uemit_close_function(&e);                /* close inner */
    uemit_close_function(&e);                /* close outer */
    teardown(&m, &a, &v);
}

UTEST(block_open_pushes_ctx_with_snapshot) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    UFuncState *fs = uemit_open_function(&e, NULL);

    uemit_declare_local(&e, ustr_intern(&v, "x", 1), 1);   /* slot 0 */
    UASSERT(uemit_open_block(&e, false));

    UASSERT_EQ(1, fs->nblocks);
    UASSERT_EQ(1, fs->blocks[0].nactvar_on_enter);
    UASSERT_EQ(1, fs->blocks[0].first_local_idx);
    UASSERT(fs->blocks[0].is_loop == false);
    UASSERT(fs->blocks[0].has_captured == false);

    uemit_close_block(&e);
    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(block_close_restores_nactvar_and_freereg) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    UFuncState *fs = uemit_open_function(&e, NULL);

    uemit_declare_local(&e, ustr_intern(&v, "outer", 5), 5);  /* slot 1 (T73: R0 pre-reserved) */
    uemit_open_block(&e, false);
    uemit_declare_local(&e, ustr_intern(&v, "inner_a", 7), 7); /* slot 2 */
    uemit_declare_local(&e, ustr_intern(&v, "inner_b", 7), 7); /* slot 3 */
    UASSERT_EQ(3, fs->nactvar);
    UASSERT_EQ((uint8_t)4, fs->freereg);

    uemit_close_block(&e);
    UASSERT_EQ(1, fs->nactvar);
    UASSERT_EQ((uint8_t)2, fs->freereg);
    UASSERT_EQ(0, fs->nblocks);

    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(block_nested_three_levels) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    UFuncState *fs = uemit_open_function(&e, NULL);

    UASSERT(uemit_open_block(&e, false));
    UASSERT(uemit_open_block(&e, false));
    UASSERT(uemit_open_block(&e, false));
    UASSERT_EQ(3, fs->nblocks);
    uemit_close_block(&e);
    uemit_close_block(&e);
    uemit_close_block(&e);
    UASSERT_EQ(0, fs->nblocks);
    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(block_exhaust_with_proper_error) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    uemit_open_function(&e, NULL);

    for (int i = 0; i < UFS_MAX_BLOCKS; i++) {
        UASSERT(uemit_open_block(&e, false));
    }
    UASSERT(!uemit_open_block(&e, false));
    UASSERT_EQ((int)EMIT_NESTING_TOO_DEEP, (int)e.error);

    /* Unwind: clear error so close path runs. */
    e.error = EMIT_OK;
    for (int i = 0; i < UFS_MAX_BLOCKS; i++) uemit_close_block(&e);
    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(block_close_with_captured_emits_op_close) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    UFuncState *fs = uemit_open_function(&e, NULL);

    uemit_open_block(&e, false);
    uemit_declare_local(&e, ustr_intern(&v, "captured", 8), 8);  /* slot 0 in this block */
    fs->actvars[0].is_captured = true;
    fs->blocks[0].has_captured = true;

    /* Capture instr_count before block close. */
    size_t pre_count = m.root_proto->instr_count;
    uemit_close_block(&e);
    UASSERT_EQ(pre_count + 1, m.root_proto->instr_count);

    uint32_t last = m.root_proto->instructions[m.root_proto->instr_count - 1];
    UASSERT_EQ((uint32_t)OP_CLOSE, (uint32_t)(last & 0xFFU));

    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(block_close_on_empty_stack_sets_error) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    uemit_open_function(&e, NULL);

    UASSERT(uemit_close_block(&e) == false);
    UASSERT_EQ((int)EMIT_UNSUPPORTED_AST, (int)e.error);

    /* clear error so close_function can run */
    e.error = EMIT_OK;
    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(upvalue_capture_immediate_parent_marks_in_stack) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);

    UFuncState *outer = uemit_open_function(&e, NULL);
    const char *x = ustr_intern(&v, "x", 1);
    uemit_declare_local(&e, x, 1);                    /* slot 1 in outer (T73: R0 pre-reserved) */

    UFuncState *inner = uemit_open_function(&e, outer);
    int idx = find_or_install_upvalue(&e, inner, x, 1);
    UASSERT_EQ(0, idx);
    UASSERT_EQ(1, inner->nupvalues);
    UASSERT(inner->upvalues[0].in_stack == true);
    UASSERT_EQ((uint8_t)1, inner->upvalues[0].idx);  /* T73: outer local is at slot 1 */
    UASSERT(outer->actvars[0].is_captured == true);   /* cppcheck-suppress nullPointerRedundantCheck */

    uemit_close_function(&e);
    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(upvalue_two_level_cascade_intermediate_in_stack_false) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);

    UFuncState *outer = uemit_open_function(&e, NULL);
    const char *x = ustr_intern(&v, "x", 1);
    uemit_declare_local(&e, x, 1);                   /* slot 1 outer (T73: R0 pre-reserved) */

    UFuncState *mid   = uemit_open_function(&e, outer);
    UFuncState *inner = uemit_open_function(&e, mid);

    int idx = find_or_install_upvalue(&e, inner, x, 1);
    UASSERT_EQ(0, idx);
    UASSERT_EQ(1, mid->nupvalues);                   /* cppcheck-suppress nullPointerRedundantCheck */
    UASSERT(mid->upvalues[0].in_stack == true);
    UASSERT_EQ((uint8_t)1, mid->upvalues[0].idx);    /* T73: outer local is at slot 1 */
    UASSERT_EQ(1, inner->nupvalues);                 /* cppcheck-suppress nullPointerRedundantCheck */
    UASSERT(inner->upvalues[0].in_stack == false);
    UASSERT_EQ((uint8_t)0, inner->upvalues[0].idx);
    UASSERT(outer->actvars[0].is_captured == true);   /* cppcheck-suppress nullPointerRedundantCheck */

    uemit_close_function(&e);
    uemit_close_function(&e);
    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(upvalue_repeated_lookup_returns_same_idx) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);

    UFuncState *outer = uemit_open_function(&e, NULL);
    const char *x = ustr_intern(&v, "x", 1);
    uemit_declare_local(&e, x, 1);

    UFuncState *inner = uemit_open_function(&e, outer);
    int a1 = find_or_install_upvalue(&e, inner, x, 1);
    int a2 = find_or_install_upvalue(&e, inner, x, 1);
    UASSERT_EQ(a1, a2);
    UASSERT_EQ(1, inner->nupvalues);                 /* cppcheck-suppress nullPointerRedundantCheck */

    uemit_close_function(&e);
    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(upvalue_unresolved_returns_negative) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    UFuncState *outer = uemit_open_function(&e, NULL);
    UFuncState *inner = uemit_open_function(&e, outer);

    const char *missing = ustr_intern(&v, "ghost", 5);
    int idx = find_or_install_upvalue(&e, inner, missing, 5);
    UASSERT_EQ(-1, idx);
    UASSERT_EQ(0, inner->nupvalues);                 /* cppcheck-suppress nullPointerRedundantCheck */

    uemit_close_function(&e);
    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(upvalue_exhaustion_errors) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);

    UFuncState *outer = uemit_open_function(&e, NULL);
    char buf[8];
    /* Declare UFS_MAX_UPVALUES + 1 locals in outer. */
    for (int i = 0; i <= UFS_MAX_UPVALUES; i++) {
        int len = snprintf(buf, sizeof buf, "v%04d", i);
        uemit_declare_local(&e, ustr_intern(&v, buf, (size_t)len), len);
    }

    UFuncState *inner = uemit_open_function(&e, outer);
    /* Capture UFS_MAX_UPVALUES of them — fills the table. */
    for (int i = 0; i < UFS_MAX_UPVALUES; i++) {
        int len = snprintf(buf, sizeof buf, "v%04d", i);
        int slot = find_or_install_upvalue(&e, inner,
                    ustr_intern(&v, buf, (size_t)len), len);
        UASSERT(slot >= 0);
    }
    /* One more must fail. */
    int len = snprintf(buf, sizeof buf, "v%04d", UFS_MAX_UPVALUES);
    int over = find_or_install_upvalue(&e, inner,
                ustr_intern(&v, buf, (size_t)len), len);
    UASSERT_EQ(-1, over);
    UASSERT_EQ((int)EMIT_UPVAL_EXHAUSTED, (int)e.error);

    uemit_close_function(&e);
    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(loop_back_emit_close_when_captured) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    UFuncState *fs = uemit_open_function(&e, NULL);
    uemit_open_block(&e, /*is_loop=*/true);
    uemit_declare_local(&e, ustr_intern(&v, "i", 1), 1);
    fs->actvars[0].is_captured = true;               /* cppcheck-suppress nullPointerRedundantCheck */
    fs->blocks[0].has_captured = true;

    size_t pre = m.root_proto->instr_count;
    uemit_emit_loop_back_close(&e);
    UASSERT_EQ(pre + 1, m.root_proto->instr_count);

    uint32_t last = m.root_proto->instructions[m.root_proto->instr_count - 1];
    UASSERT_EQ((uint32_t)OP_CLOSE, (uint32_t)(last & 0xFFU));

    uemit_close_block(&e);
    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(loop_back_emit_close_no_op_when_not_captured) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    uemit_open_function(&e, NULL);
    uemit_open_block(&e, /*is_loop=*/true);
    /* has_captured stays false */

    size_t pre = m.root_proto->instr_count;
    uemit_emit_loop_back_close(&e);
    UASSERT_EQ(pre, m.root_proto->instr_count);   /* no instruction emitted */

    uemit_close_block(&e);
    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

/* --- M4 T15: per-function IC counter + ic_names side table --- */

UTEST(funcstate_ic_counter_increments_per_emitted_getslot) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    UFuncState *fs = uemit_open_function(&e, NULL);

    /* USymbol is opaque (forward-declared in umodule.h); ustr_intern returns
     * the canonical const char* — pointer-equality is identity.  Cast to
     * USymbol* here so the rest of the codebase can treat the ic_names array
     * as an opaque-symbol slot. */
    USymbol *foo = (USymbol *)ustr_intern(&v, "foo", 3);
    USymbol *bar = (USymbol *)ustr_intern(&v, "bar", 3);
    UASSERT(foo != NULL);
    UASSERT(bar != NULL);

    UASSERT_EQ(0, uemit_assign_ic_index(&e, foo));
    UASSERT_EQ(1, uemit_assign_ic_index(&e, bar));
    /* Same name, fresh index — every site is independently monomorphizable. */
    UASSERT_EQ(2, uemit_assign_ic_index(&e, foo));
    UASSERT_EQ((uint16_t)3, fs->ic_next);     /* cppcheck-suppress nullPointerRedundantCheck */
    UASSERT(fs->ic_names != NULL);
    UASSERT(fs->ic_names[0] == foo);
    UASSERT(fs->ic_names[1] == bar);
    UASSERT(fs->ic_names[2] == foo);
    UASSERT_EQ((int)EMIT_OK, (int)e.error);

    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(funcstate_ic_counter_caps_at_256_with_emit_too_many_ic_sites) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    uemit_open_function(&e, NULL);

    USymbol *x = (USymbol *)ustr_intern(&v, "x", 1);
    UASSERT(x != NULL);
    for (int i = 0; i < 256; i++) {
        int idx = uemit_assign_ic_index(&e, x);
        UASSERT(idx >= 0);
        UASSERT_EQ(i, idx);
    }
    /* The 257th call must fail with EMIT_TOO_MANY_IC_SITES. */
    UASSERT_EQ(-1, uemit_assign_ic_index(&e, x));
    UASSERT_EQ((int)EMIT_TOO_MANY_IC_SITES, (int)e.error);

    /* Clear error so close path runs cleanly. */
    e.error = EMIT_OK;
    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(funcstate_ic_close_copies_into_target_proto) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);

    /* Top-level funcstate plus a nested proto + child funcstate — that's
     * the only path where ic_count/ic_names actually land somewhere
     * (UProto has these fields, UModule does not). */
    UFuncState *parent = uemit_open_function(&e, NULL);
    UProto *child_proto = umodule_alloc_nested_proto(&m);
    UASSERT(child_proto != NULL);
    UFuncState *child = uemit_open_function(&e, parent);
    UASSERT(child != NULL);
    child->target_proto = child_proto;          /* cppcheck-suppress nullPointerRedundantCheck */

    USymbol *a1 = (USymbol *)ustr_intern(&v, "alpha", 5);
    USymbol *b1 = (USymbol *)ustr_intern(&v, "beta",  4);
    UASSERT_EQ(0, uemit_assign_ic_index(&e, a1));
    UASSERT_EQ(1, uemit_assign_ic_index(&e, b1));

    uemit_close_function(&e);                   /* close child */

    UASSERT_EQ((unsigned)2, (unsigned)child_proto->ic_count);
    UASSERT(child_proto->ic_names != NULL);
    UASSERT(child_proto->ic_names[0] == a1);
    UASSERT(child_proto->ic_names[1] == b1);

    /* Funcstate-side array was freed by close_function. */
    UASSERT(child->ic_names == NULL);
    UASSERT_EQ((uint16_t)0, child->ic_names_cap);

    uemit_close_function(&e);                   /* close parent */
    teardown(&m, &a, &v);                       /* umodule_destroy frees child_proto->ic_names */
}

UTEST(funcstate_ic_close_with_zero_sites_leaves_proto_null) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);

    UFuncState *parent = uemit_open_function(&e, NULL);
    UProto *child_proto = umodule_alloc_nested_proto(&m);
    UASSERT(child_proto != NULL);
    UFuncState *child = uemit_open_function(&e, parent);
    UASSERT(child != NULL);
    child->target_proto = child_proto;          /* cppcheck-suppress nullPointerRedundantCheck */

    /* Don't call uemit_assign_ic_index — ic_next stays 0. */
    uemit_close_function(&e);                   /* close child */

    UASSERT_EQ((unsigned)0, (unsigned)child_proto->ic_count);
    UASSERT(child_proto->ic_names == NULL);

    uemit_close_function(&e);                   /* close parent */
    teardown(&m, &a, &v);
}

void test_funcstate_suite(void) {
    utest_run("funcstate open zeroes freereg and nactvar",
        funcstate_open_zeroes_freereg_and_nactvar);
    utest_run("funcstate declare_local pushes actvar and advances freereg",
        funcstate_declare_local_pushes_actvar_and_advances_freereg);
    utest_run("funcstate declare three locals",
        funcstate_declare_three_locals);
    utest_run("funcstate redeclare in same scope errors",
        funcstate_redeclare_in_same_scope_errors);
    utest_run("funcstate max locals exhausts with proper error",
        funcstate_max_locals_exhausts_with_proper_error);
    utest_run("funcstate close_function pops to parent",
        funcstate_close_function_pops_to_parent);
    utest_run("block open pushes ctx with snapshot",
        block_open_pushes_ctx_with_snapshot);
    utest_run("block close restores nactvar and freereg",
        block_close_restores_nactvar_and_freereg);
    utest_run("block nested three levels",
        block_nested_three_levels);
    utest_run("block exhaust with proper error",
        block_exhaust_with_proper_error);
    utest_run("block close with captured emits OP_CLOSE",
        block_close_with_captured_emits_op_close);
    utest_run("block close on empty stack sets error",
        block_close_on_empty_stack_sets_error);
    utest_run("upvalue capture immediate parent marks in_stack",
        upvalue_capture_immediate_parent_marks_in_stack);
    utest_run("upvalue two-level cascade intermediate in_stack false",
        upvalue_two_level_cascade_intermediate_in_stack_false);
    utest_run("upvalue repeated lookup returns same idx",
        upvalue_repeated_lookup_returns_same_idx);
    utest_run("upvalue unresolved returns negative",
        upvalue_unresolved_returns_negative);
    utest_run("upvalue exhaustion errors",
        upvalue_exhaustion_errors);
    utest_run("loop back emit close when captured",
        loop_back_emit_close_when_captured);
    utest_run("loop back emit close no-op when not captured",
        loop_back_emit_close_no_op_when_not_captured);
    utest_run("funcstate ic counter increments per emitted GETSLOT",
        funcstate_ic_counter_increments_per_emitted_getslot);
    utest_run("funcstate ic counter caps at 256 with EMIT_TOO_MANY_IC_SITES",
        funcstate_ic_counter_caps_at_256_with_emit_too_many_ic_sites);
    utest_run("funcstate ic close copies into target proto",
        funcstate_ic_close_copies_into_target_proto);
    utest_run("funcstate ic close with zero sites leaves proto null",
        funcstate_ic_close_with_zero_sites_leaves_proto_null);
}
