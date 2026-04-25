/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include <stdio.h>

#include "uarena.h"
#include "uemit.h"
#include "uemit_internal.h"
#include "uintern.h"
#include "umodule.h"
#include "uvm.h"

#define UTEST(name) static void name(void)

/* --- helpers --- */
static void setup(UEmitter *e, UModule *m, UArena *a, UVM *v) {
    *m = (UModule){0};
    uarena_init(a, 0);
    uvm_init(v, NULL, NULL);
    uemit_init(e, m, a, v, "test");
}
static void teardown(UModule *m, UArena *a, UVM *v) {
    uarena_destroy(a);
    umodule_destroy(m);
    uvm_destroy(v);
}

UTEST(funcstate_open_zeroes_freereg_and_nactvar) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);

    UFuncState *fs = uemit_open_function(&e, NULL);
    UASSERT(fs != NULL);
    UASSERT_EQ((uint8_t)0, fs->max_reg_seen);  // cppcheck-suppress nullPointerRedundantCheck
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
    UASSERT_EQ(0, slot);
    UASSERT_EQ(1, fs->nactvar);
    UASSERT_EQ((uint8_t)1, fs->freereg);
    UASSERT(fs->actvars[0].name == name);    /* canonical pointer eq */
    UASSERT_EQ((uint8_t)0, fs->actvars[0].slot);
    UASSERT_EQ((uint8_t)1, fs->max_reg_seen);

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
    UASSERT_EQ(0, s1);
    UASSERT_EQ(1, s2);
    UASSERT_EQ(2, s3);
    UASSERT_EQ(3, fs->nactvar);
    UASSERT_EQ((uint8_t)3, fs->freereg);

    uemit_close_function(&e);
    teardown(&m, &a, &v);
}

UTEST(funcstate_redeclare_in_same_scope_errors) {
    UEmitter e; UModule m; UArena a; UVM v;
    setup(&e, &m, &a, &v);
    uemit_open_function(&e, NULL);

    const char *name = ustr_intern(&v, "x", 1);
    UASSERT_EQ(0, uemit_declare_local(&e, name, 1));
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

    uemit_declare_local(&e, ustr_intern(&v, "outer", 5), 5);  /* slot 0 */
    uemit_open_block(&e, false);
    uemit_declare_local(&e, ustr_intern(&v, "inner_a", 7), 7); /* slot 1 */
    uemit_declare_local(&e, ustr_intern(&v, "inner_b", 7), 7); /* slot 2 */
    UASSERT_EQ(3, fs->nactvar);
    UASSERT_EQ((uint8_t)3, fs->freereg);

    uemit_close_block(&e);
    UASSERT_EQ(1, fs->nactvar);
    UASSERT_EQ((uint8_t)1, fs->freereg);
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
    size_t pre_count = m.instr_count;
    uemit_close_block(&e);
    UASSERT_EQ(pre_count + 1, m.instr_count);

    uint32_t last = m.instructions[m.instr_count - 1];
    UASSERT_EQ((uint32_t)OP_CLOSE, (uint32_t)(last & 0xFFu));

    uemit_close_function(&e);
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
}
