/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_batch_error_surfacing.c — TDD tests for B1/LANG4-01 (v0.13.4):
 *
 *   vm_run_uncaught_throw_returns_error:
 *     urbi_vm_run on "throw 99" must return URBI_ERR_UNCAUGHT_THROW (-18).
 *     Pre-fix: urbi_vm_run returned 0 (URBI_OK) — B1 live.
 *
 *   vm_run_uncaught_exception_object_returns_error:
 *     urbi_vm_run on "throw Exception.new(\"boom\")" must return
 *     URBI_ERR_UNCAUGHT_THROW and populate vm.last_errmsg.
 *     Pre-fix: returned 0 (URBI_OK), last_errmsg empty — B1 live.
 *
 *   vm_run_clean_completion_stays_ok:
 *     urbi_vm_run on "1 + 1" must still return URBI_OK.
 *
 *   run_chunk_uncaught_throw_returns_error:
 *     urbi_run_chunk (via utest_e2e_compile_and_run) on "throw 99" must return
 *     URBI_ERR_UNCAUGHT_THROW.  Pre-fix: returned URBI_ERR_STRAND_FATAL (-2).
 *
 *   repl_eval_keeps_nil_recovery:
 *     urbi_repl_eval on "throw 99" must still return URBI_OK (interactive
 *     nil-recovery contract, pinned by throw_uncaught.chk).
 */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "value/uarena.h"
#include "chunk/uchunk.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "parse/uast.h"
#include "runtime/umacros.h"   /* urbi_zero */

#include <stddef.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ---------------------------------------------------------------------------
 * Local helper: lex + parse + emit `src` into a heap-allocated UProto, then
 * call urbi_vm_run (the transient-strand path, NOT urbi_run_chunk).
 * Mirrors tools/urbi.c:run_expression (CHSTR-027 heap-allocation pattern).
 * out_result may be NULL.  Returns the urbi_vm_run return code.
 * --------------------------------------------------------------------------- */
static int
utest_compile_and_vm_run(UVM *vm, const char *src, UValue *out_result)
{
    size_t src_len = strlen(src);
    ULexer lex;
    ulex_init(&lex, src, src_len);

    UArena arena;
    uarena_init(&arena, 4096);

    /* Heap-allocate root proto (CHSTR-027 pattern). */
    UProto *module = (UProto *)vm->alloc_fn(NULL, sizeof(UProto), vm->alloc_ud);
    if (module == NULL) {
        uarena_destroy(&arena);
        return URBI_ERR_OOM;
    }
    urbi_zero(module, sizeof *module);
    module->alloc_fn       = vm->alloc_fn;
    module->alloc_ud       = vm->alloc_ud;
    module->heap_allocated = true;

    UEmitter e;
    uemit_init(&e, module, &arena, vm, NULL);

    UParser p;
    uparse_init(&p, &lex, &arena);

    bool has_error = false;
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) { has_error = true; break; }
        if (uemit_statement(&e, node) != EMIT_OK) { has_error = true; break; }
        uarena_reset(&arena);
    }

    if (!has_error && uemit_finish(&e) != EMIT_OK)
        has_error = true;

    if (has_error) {
        urbi_emit_abandon(&e);
        uchunk_destroy(module, vm);
        uarena_destroy(&arena);
        return URBI_ERR_COMPILE;
    }

    UValue local_out;
    urbi_zero(&local_out, sizeof(local_out));
    local_out.kind = UVAL_NIL;
    UValue *out = out_result ? out_result : &local_out;

    int rc = urbi_vm_run(vm, NULL, module, out);

    uchunk_destroy(module, vm);
    uarena_destroy(&arena);
    return rc;
}

/* =========================================================================
 * Test 1: urbi_vm_run on "throw 99" returns URBI_ERR_UNCAUGHT_THROW
 * =========================================================================
 *
 * Oracle: URBI_ERR_UNCAUGHT_THROW == -18.
 *   Pre-fix: rc = 0 (URBI_OK) — batch entry point silently swallowed the throw.
 *   Post-fix: rc = -18. */
UTEST(vm_run_uncaught_throw_returns_error)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    int rc = utest_compile_and_vm_run(&vm, "throw 99", NULL);
    UASSERT_EQ(URBI_ERR_UNCAUGHT_THROW, rc);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Test 2: urbi_vm_run on "throw Exception.new("boom")" returns
 *         URBI_ERR_UNCAUGHT_THROW and populates vm.last_errmsg
 * =========================================================================
 *
 * Exception-typed throws surface an errmsg from the instance's message slot
 * (captured by capture_uncaught_throw_diag / uvalue_format fallback). */
UTEST(vm_run_uncaught_exception_object_returns_error)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    int rc = utest_compile_and_vm_run(&vm, "throw Exception.new(\"boom\")", NULL);
    UASSERT_EQ(URBI_ERR_UNCAUGHT_THROW, rc);
    UASSERT(vm.last_errmsg[0] != '\0');
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Test 3: urbi_vm_run on clean "1 + 1" still returns URBI_OK
 * =========================================================================
 *
 * Regression: the fatal-check path must be bypassed on clean completion. */
UTEST(vm_run_clean_completion_stays_ok)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    UValue out;
    int rc = utest_compile_and_vm_run(&vm, "1 + 1", &out);
    UASSERT_EQ(URBI_OK, rc);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Test 4: urbi_run_chunk on "throw 99" returns URBI_ERR_UNCAUGHT_THROW
 * =========================================================================
 *
 * utest_e2e_compile_and_run drives urbi_run_chunk (the persistent-loader
 * path, distinct from urbi_vm_run's transient-strand path).
 *   Pre-fix: rc = URBI_ERR_STRAND_FATAL (-2).
 *   Post-fix: rc = URBI_ERR_UNCAUGHT_THROW (-18). */
UTEST(run_chunk_uncaught_throw_returns_error)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    int rc = utest_e2e_compile_and_run(&vm, "throw 99", NULL);
    UASSERT_EQ(URBI_ERR_UNCAUGHT_THROW, rc);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Test 5: urbi_repl_eval on "throw 99" keeps nil-recovery (returns URBI_OK)
 * =========================================================================
 *
 * The interactive REPL recovers from uncaught scalar throws by returning nil
 * and URBI_OK — pinned by tests/chk/control_transfer/throw_uncaught.chk.
 * This contract is unchanged by the batch-path fix. */
UTEST(repl_eval_keeps_nil_recovery)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    char out_buf[64];
    out_buf[0] = '\0';
    int rc = urbi_repl_eval(&vm, NULL, "throw 99", 8, out_buf, sizeof(out_buf));
    UASSERT_EQ(URBI_OK, rc);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Test 6: urbi_vm_run delivers the thrown object via *out on UNCAUGHT_THROW
 * =========================================================================
 *
 * urbi_vm_run (transient-strand path) delivers the thrown value via *out
 * when non-NULL.  Exception.new("boom") is UVAL_OBJECT; pin the kind so
 * the delivery contract cannot silently regress.  out.kind is read from
 * the stack-allocated UValue — no heap dereference, ASan-clean. */
UTEST(vm_run_exception_object_delivered_via_out)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    UValue out;
    urbi_zero(&out, sizeof(out));
    out.kind = UVAL_NIL;
    int rc = utest_compile_and_vm_run(&vm, "throw Exception.new(\"boom\")", &out);
    UASSERT_EQ(URBI_ERR_UNCAUGHT_THROW, rc);
    UASSERT_EQ(UVAL_OBJECT, out.kind);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point
 * ========================================================================= */

void test_batch_error_surfacing_suite(void);

void
test_batch_error_surfacing_suite(void)
{
    utest_run("batch_error_surfacing: vm_run uncaught throw returns -18 (B1)",
              vm_run_uncaught_throw_returns_error);
    utest_run("batch_error_surfacing: vm_run uncaught exception returns -18 + errmsg",
              vm_run_uncaught_exception_object_returns_error);
    utest_run("batch_error_surfacing: vm_run clean completion stays URBI_OK",
              vm_run_clean_completion_stays_ok);
    utest_run("batch_error_surfacing: run_chunk uncaught throw returns -18 (B1)",
              run_chunk_uncaught_throw_returns_error);
    utest_run("batch_error_surfacing: repl_eval nil-recovery unchanged (LANG4-01)",
              repl_eval_keeps_nil_recovery);
    utest_run("batch_error_surfacing: vm_run delivers exception object via *out",
              vm_run_exception_object_delivered_via_out);
}
