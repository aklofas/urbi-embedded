/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/utest_e2e_helpers.c — see utest_e2e_helpers.h.
 *
 * Bodies match the canonical copies that previously lived in
 * test_at_scripted_e2e.c (compile_and_run, run_to_no_runnable, make_int)
 * and test_tag_stop_onleave_scripted.c (make_nil).  The
 * _with_module variant comes from test_event_sync_emit_scripted.c, where
 * the caller needs to keep arena + module live to retain a returned
 * UVAL_CLOSURE.
 */

#include "utest_e2e_helpers.h"

#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "emit/uemit.h"
#include "realm/urealm.h"
#include "runtime/umacros.h"

#define UTEST_E2E_MAX_ITERS 1000

UValue
utest_e2e_make_int(int64_t n)
{
    UValue v;
    v.kind = UVAL_INT;
    v.v.i  = n;
    return v;
}

UValue
utest_e2e_make_nil(void)
{
    return urbi_make_nil();
}

int
utest_e2e_compile_and_run_with_module(UVM *vm,
                                      UArena *arena,
                                      UModule *module,
                                      const char *src,
                                      UValue *out_result)
{
    URealm *realm = urbi_realm_global(vm);
    if (realm == NULL) return URBI_ERR_OOM;

    ULexer   lex;
    UEmitter e;
    UParser  p;
    UAstNode *node;

    ulex_init(&lex, src, urbi_strlen(src));
    uemit_init(&e, module, arena, vm, NULL);
    uparse_init(&p, &lex, arena);

    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) return URBI_ERR_COMPILE;
        if (uemit_statement(&e, node) != EMIT_OK) return URBI_ERR_COMPILE;
        uarena_reset(arena);
    }
    if (uemit_finish(&e) != EMIT_OK) return URBI_ERR_COMPILE;

    UValue result = {0};
    int rc = urbi_run_chunk(vm, realm, module, &result);
    if (out_result != NULL) {
        *out_result = result;
    }
    return rc;
}

int
utest_e2e_compile_and_run(UVM *vm, const char *src, UValue *out_result)
{
    UArena  arena;
    UModule module = {0};

    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(vm, &arena, &module,
                                                   src, out_result);

    uarena_destroy(&arena);
    umodule_destroy(&module, vm);
    return rc;
}

int
utest_e2e_run_to_no_runnable(UVM *vm)
{
    int i;
    for (i = 0; i < UTEST_E2E_MAX_ITERS; i++) {
        UStepResult sr = urbi_step(vm, 1000, NULL);
        if (sr == URBI_STEP_FATAL)     return -1;
        if (sr == URBI_STEP_WAKE_AT)   return 1;
        if (sr == URBI_STEP_QUIESCENT) return 1;
        if (vm->strand_runnable_count == 0) return 1;
    }
    return 0;
}
