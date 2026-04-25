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
    UASSERT_EQ((uint8_t)0, fs->freereg);
    UASSERT_EQ((uint8_t)0, fs->max_reg_seen);
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
    UFuncState *inner = uemit_open_function(&e, outer);
    UASSERT(inner->parent == outer);

    uemit_close_function(&e);                /* close inner */
    uemit_close_function(&e);                /* close outer */
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
}
