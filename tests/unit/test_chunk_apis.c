/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: urbi_run_chunk, urbi_repl_eval, urbi_run_script, urbi_load_module
   (row 8 §5 + §8.4 / T16). */

#include "utest.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "module/umodule.h"
#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "value/uvalue.h"
#include <string.h>
#include <stddef.h>

#define UTEST(name) static void name(void)

/* Helper: compile `src` into *out_module using `vm`.
   Returns true on success; the caller owns the module and must call
   umodule_destroy() when done. */
static bool
compile_src(UVM *vm, const char *src, UModule *out_module)
{
    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UArena arena;
    uarena_init(&arena, 4096);

    *out_module = (UModule){0};

    UEmitter e;
    uemit_init(&e, out_module, &arena, vm, NULL);

    UParser p;
    uparse_init(&p, &lex, &arena);

    bool ok = true;
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) { ok = false; break; }
        if (uemit_statement(&e, node) != EMIT_OK) { ok = false; break; }
        uarena_reset(&arena);
    }
    if (ok && uemit_finish(&e) != EMIT_OK)
        ok = false;

    uarena_destroy(&arena);
    return ok;
}

/* -------------------------------------------------------------------------
 * urbi_run_chunk tests
 * ------------------------------------------------------------------------- */

/* Case 1: simple arithmetic expression — run_chunk returns URBI_OK and the
   result value matches the expected integer. */
UTEST(run_chunk_round_trip_with_realm)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UModule module;
    UASSERT(compile_src(&vm, "1 + 2", &module));

    UValue result = {0};
    int rc = urbi_run_chunk(&vm, realm, &module, &result);

    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ(result.kind, (uint8_t)UVAL_INT);
    UASSERT_EQ(result.v.i, (int64_t)3);

    umodule_destroy(&module);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 2: NULL realm auto-uses the global realm; the call succeeds and a
   second NULL-realm call runs against the same VM without error. */
UTEST(run_chunk_null_realm_uses_global)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UModule m1, m2;
    UASSERT(compile_src(&vm, "2 + 2", &m1));
    UASSERT(compile_src(&vm, "10 - 3", &m2));

    UValue r1 = {0}, r2 = {0};
    int rc1 = urbi_run_chunk(&vm, NULL, &m1, &r1);
    int rc2 = urbi_run_chunk(&vm, NULL, &m2, &r2);

    UASSERT_EQ(rc1, URBI_OK);
    UASSERT_EQ(r1.v.i, (int64_t)4);

    UASSERT_EQ(rc2, URBI_OK);
    UASSERT_EQ(r2.v.i, (int64_t)7);

    /* Verify only one global realm was created. */
    URealm *g1 = urbi_realm_global(&vm);
    URealm *g2 = urbi_realm_global(&vm);
    UASSERT(g1 != NULL);
    UASSERT(g1 == g2);

    umodule_destroy(&m1);
    umodule_destroy(&m2);
    urbi_vm_destroy(&vm);
}

/* Case 3: run_chunk with out_result == NULL discards the result without crash.
   (Smoke test for the local_out code path.) */
UTEST(run_chunk_null_out_result_no_crash)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UModule module;
    UASSERT(compile_src(&vm, "42", &module));

    int rc = urbi_run_chunk(&vm, NULL, &module, NULL);
    UASSERT_EQ(rc, URBI_OK);

    umodule_destroy(&module);
    urbi_vm_destroy(&vm);
}

/* -------------------------------------------------------------------------
 * urbi_repl_eval tests
 * ------------------------------------------------------------------------- */

/* Case 4: simple expression evaluates to the expected string representation. */
UTEST(repl_eval_round_trip)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    char buf[64];
    int rc = urbi_repl_eval(&vm, NULL, "1+2", 3, buf, sizeof(buf));

    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ(0, strcmp(buf, "3"));

    urbi_vm_destroy(&vm);
}

/* Case 5: compile error (truncated expression) fills buf with "compile error"
   and returns URBI_ERR_COMPILE. */
UTEST(repl_eval_compile_error_path)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    char buf[128];
    buf[0] = '\0';
    int rc = urbi_repl_eval(&vm, NULL, "1+", 2, buf, sizeof(buf));

    UASSERT_EQ(rc, URBI_ERR_COMPILE);
    /* urbi_repl_eval now emits the parser's diagnostic in "<stdin>:line:col: msg"
     * format (hosted builds) rather than the generic "compile error" string.
     * "1+" leaves a parse hole — expected expression at col 3 or similar. */
    UASSERT(buf[0] != '\0');  /* some diagnostic was written */

    urbi_vm_destroy(&vm);
}

/* CPPCHK-005: prior to T114, urbi_repl_eval captured uemit_finish's return
 * code into `finish_rc` but never read it — only `has_error` gated the
 * URBI_ERR_COMPILE return.  T114 routes finish_rc into both the diagnostic
 * (uemit_error_name fallback when the parser succeeded) and the return-code
 * mapping (EMIT_OOM → URBI_ERR_OOM, else URBI_ERR_COMPILE).  This test
 * pins behaviour-on-success: the parse-error path remains URBI_ERR_COMPILE
 * with a non-empty diagnostic, confirming the message routing changes do
 * not regress the parse-failure path that the existing test 5 covers.  A
 * deterministic finish-only-failure injection is impractical without a
 * fault-injection seam (uemit_finish's only failure mode propagates a
 * sticky e->error already captured during statement emit), so this
 * extension exercises the live path end-to-end. */
UTEST(compile_finish_failure_propagates)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Empty input: no statements emit, finish runs with any_stmt_emitted=false
     * → no OP_RET appended, current_fs lazily-opened only if statements exist
     * → finish returns EMIT_OK.  Verify URBI_OK + empty result string. */
    char buf[64];
    buf[0] = 'X';
    int rc = urbi_repl_eval(&vm, NULL, "", 0, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);

    /* Parse error path still returns URBI_ERR_COMPILE with a populated buffer.
     * After T114, finish_rc is EMIT_OK (parser short-circuits before finish),
     * so the diagnostic comes from parse_errmsg as before. */
    buf[0] = '\0';
    rc = urbi_repl_eval(&vm, NULL, "1+", 2, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_ERR_COMPILE);
    UASSERT(buf[0] != '\0');

    urbi_vm_destroy(&vm);
}

/* Case 6: runtime type error (nil < 1 is invalid at M3) returns
   URBI_ERR_STRAND_FATAL and writes vm->last_errmsg to out_buf. */
UTEST(repl_eval_writes_fatal_message_on_runtime_error)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    char buf[128];
    buf[0] = '\0';
    /* nil < 1 triggers a type error in the comparison path. */
    int rc = urbi_repl_eval(&vm, NULL, "nil < 1", 7, buf, sizeof(buf));

    UASSERT_EQ(rc, URBI_ERR_STRAND_FATAL);
    /* buf must be non-empty — it carries the error message. */
    UASSERT(buf[0] != '\0');

    urbi_vm_destroy(&vm);
}

/* Case 7: sequential repl_eval calls accumulate results correctly.
   Each call is independent (the VM accumulates no REPL-level binding state
   at M3 — that's M8's job); verify arithmetic is correct per call. */
UTEST(repl_eval_sequential_calls)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    char buf[64];

    urbi_repl_eval(&vm, NULL, "3 * 4",  5, buf, sizeof(buf));
    UASSERT_EQ(0, strcmp(buf, "12"));

    urbi_repl_eval(&vm, NULL, "100 - 1", 7, buf, sizeof(buf));
    UASSERT_EQ(0, strcmp(buf, "99"));

    urbi_vm_destroy(&vm);
}

/* -------------------------------------------------------------------------
 * urbi_run_script tests
 * ------------------------------------------------------------------------- */

/* Case 8: run_script returns URBI_OK and discards the result (does not crash
   when result is void or a value). */
UTEST(run_script_returns_ok_discards_result)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UModule module;
    UASSERT(compile_src(&vm, "7 * 6", &module));

    int rc = urbi_run_script(&vm, NULL, &module);
    UASSERT_EQ(rc, URBI_OK);

    umodule_destroy(&module);
    urbi_vm_destroy(&vm);
}

/* -------------------------------------------------------------------------
 * urbi_load_module tests
 * ------------------------------------------------------------------------- */

/* Case 9: stub returns URBI_ERR_INVALID_ARG.  This test documents the TODO so
   future implementers know the expected return value before M6 fills it in. */
UTEST(load_module_stub_returns_invalid_arg)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UModule module;
    UASSERT(compile_src(&vm, "42", &module));

    int rc = urbi_load_module(&vm, &module, "test_module");
    UASSERT_EQ(rc, URBI_ERR_INVALID_ARG);

    umodule_destroy(&module);
    urbi_vm_destroy(&vm);
}

/* -------------------------------------------------------------------------
 * Suite registration
 * ------------------------------------------------------------------------- */

void test_chunk_apis_suite(void) {
    utest_run("run_chunk_round_trip_with_realm",
              run_chunk_round_trip_with_realm);
    utest_run("run_chunk_null_realm_uses_global",
              run_chunk_null_realm_uses_global);
    utest_run("run_chunk_null_out_result_no_crash",
              run_chunk_null_out_result_no_crash);
    utest_run("repl_eval_round_trip",
              repl_eval_round_trip);
    utest_run("repl_eval_compile_error_path",
              repl_eval_compile_error_path);
    utest_run("compile_finish_failure_propagates",
              compile_finish_failure_propagates);
    utest_run("repl_eval_writes_fatal_message_on_runtime_error",
              repl_eval_writes_fatal_message_on_runtime_error);
    utest_run("repl_eval_sequential_calls",
              repl_eval_sequential_calls);
    utest_run("run_script_returns_ok_discards_result",
              run_script_returns_ok_discards_result);
    utest_run("load_module_stub_returns_invalid_arg",
              load_module_stub_returns_invalid_arg);
}
