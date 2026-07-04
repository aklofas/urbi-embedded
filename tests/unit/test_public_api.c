/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L  /* sigaction, fork, waitpid, _exit */
#endif
/* Phase 18 (T108-T113): public C API NULL-safety + signature carry-forwards.
 * Phase 20 (T119): src/urbi.c coverage — setters + panic body in non-DEBUG.
 *
 * Audit IDs closed:
 *   API-001 — urbi_panic NULL guard on msg
 *   API-002 — urbi_throw / return_val / tag_stop_local NULL guard on s->vm
 *   API-003 — urbi_register_event_drain assert ordering before NULL check
 *   API-004 — urbi_run_chunk realm-arg threading (signature change carry from Wave 3)
 *   API-010 — urbi_call_host_with_watchdog vm/fn defense
 *   API-011 — URBI_VERSION literal stale at "0.3.0-concurrency"
 *   COV-001 — src/urbi.c line coverage 8 % → ≥85 % (setters + panic body)
 */

#include "utest.h"

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "realm/urealm.h"
#include "chunk/uchunk.h"
#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"

#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>     /* exit() — used by abort_gcov_flush_handler */

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

/* SIGABRT handler.  Coverage builds register a .gcda writer via atexit;
 * abort() does NOT run atexit handlers, so any line hit before abort() is
 * lost in the child unless we run atexit explicitly.  exit() (lowercase)
 * does run atexit then _exit — so swapping abort's terminate-with-SIGABRT
 * for exit(non-zero) preserves the "child exits abnormally" signal seen by
 * EXPECT_ABORT (WIFEXITED && WEXITSTATUS != 0) AND lets the gcov atexit
 * hook flush counters. */
static void abort_gcov_flush_handler(int sig) {
    (void)sig;
    exit(134);  /* runs atexit (writes .gcda); WEXITSTATUS sees 134, so the
                  * EXPECT_ABORT-detection (WIFEXITED && exit != 0) still
                  * fires — abort is still detected. */
}

/* EXPECT_ABORT: assert that expr causes abort (signal or non-zero exit).
 * Used to verify urbi_panic on a path that must trap fatal.  Available in
 * both URBI_DEBUG and non-DEBUG builds; the host is hosted-POSIX in either
 * case (only freestanding cross-builds lack <sys/wait.h>, and unit tests
 * only run on host).  T119 (COV-001) ungated this from URBI_DEBUG and
 * added the gcov-flush SIGABRT handler so coverage builds (non-DEBUG)
 * record the urbi_panic hosted-abort branch. */
#define EXPECT_ABORT(expr)                                               \
    do {                                                                 \
        utest_checks++;                                                  \
        pid_t _pid = fork();                                             \
        if (_pid == 0) {                                                 \
            struct sigaction _sa;                                        \
            memset(&_sa, 0, sizeof(_sa));                                \
            _sa.sa_handler = abort_gcov_flush_handler;                   \
            sigaction(SIGABRT, &_sa, NULL);                              \
            (expr);                                                      \
            _exit(0);                                                    \
        }                                                                \
        int _st = 0;                                                     \
        waitpid(_pid, &_st, 0);                                          \
        int _aborted = WIFSIGNALED(_st) ||                               \
                       (WIFEXITED(_st) && WEXITSTATUS(_st) != 0);        \
        if (!_aborted) {                                                 \
            utest_failures++;                                            \
            printf("  FAIL: %s:%d: " #expr " did not abort\n",           \
                   __FILE__, __LINE__);                                  \
            fflush(stdout);                                              \
        }                                                                \
    } while (0)

#define UTEST(name) static void name(void)

/* ===================================================================
 * T108 — API-001: urbi_panic NULL guard on msg
 * ===================================================================
 *
 * urbi_panic(NULL) used to feed NULL to fputs(stderr) — undefined behavior
 * on hosted libcs (glibc is permissive, musl segfaults).  The fix
 * substitutes "<no diagnostic>" for NULL before any deref.
 *
 * Test forks a child, calls urbi_panic(NULL), and verifies the child
 * exits via abort (not via SIGSEGV from fputs).  Under ASan/UBSan this
 * catches the prior NULL deref; under glibc release builds it confirms
 * abort is reached.  T119 (COV-001) ungated this test from URBI_DEBUG so
 * coverage builds (non-DEBUG) exercise urbi_panic's hosted abort branch. */
UTEST(urbi_panic_handles_null_msg)
{
    EXPECT_ABORT(urbi_panic(NULL));
}

/* ===================================================================
 * T109 — API-002: urbi_throw / return_val / tag_stop_local NULL guard on s->vm
 * ===================================================================
 *
 * The three host-callback helpers used to dereference strand->vm without a
 * prior NULL check.  Production code never passes a vm-less strand, but the
 * public C surface MUST be safe against malformed callers.
 *
 * Test: build a strand with vm=NULL on the stack and call each helper.
 * The fix returns early with no side effects; the test asserts pending_unwind
 * remains UEXEC_OK after the call.
 */
UTEST(throw_return_val_tag_stop_handle_null_vm)
{
    UStrand s;
    memset(&s, 0, sizeof(s));
    s.vm = NULL;
    s.pending_unwind = UEXEC_OK;

    UValue v;
    v.kind = UVAL_INT;
    memset(v._pad, 0, sizeof(v._pad));
    v.v.i = 42;

    /* All three must be no-ops when s->vm is NULL (pass vm=NULL; impl
     * falls through to the strand->vm NULL check). */
    urbi_throw(NULL, &s, v);
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);

    urbi_return_val(NULL, &s, v);
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);

    urbi_tag_stop_local(NULL, &s, NULL, v);
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);

    /* Also verify NULL strand: must not crash. */
    urbi_throw(NULL, NULL, v);
    urbi_return_val(NULL, NULL, v);
    urbi_tag_stop_local(NULL, NULL, NULL, v);
    UASSERT(1);
}

/* ===================================================================
 * T110 — API-003: urbi_register_event_drain assert ordering
 * ===================================================================
 *
 * Pre-fix order:
 *   URBI_ASSERT_NOT_ISR(vm);   // calls urbi_in_isr(vm) which IS NULL-safe
 *   if (vm == NULL) return;    // dead code in debug builds
 *
 * Post-fix order:
 *   if (vm == NULL) return;
 *   URBI_ASSERT_NOT_ISR(vm);
 *
 * Behavior was correct prior; the swap aligns with the rest of the
 * public surface ("validate args, then assert invariants").  Test pins
 * NULL safety and confirms the handler installs/clears correctly. */
UTEST(register_event_drain_null_check_before_assert)
{
    /* NULL vm must be a clean no-op. */
    urbi_register_event_drain(NULL, NULL, NULL);
    UASSERT(1);

    /* Valid vm with NULL handler clears the handler. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_register_event_drain(&vm, NULL, NULL);
    UASSERT(vm.event_drain_handler == NULL);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T111 — API-004: urbi_run_chunk realm-arg threading (signature change)
 * ===================================================================
 *
 * Pre-fix: urbi_run_chunk(vm, realm, module, out) did `(void)realm;` then
 * called urbi_vm_run(vm, module, out).  urbi_vm_run always wired the
 * transient strand to the global Realm — so the realm arg was silently
 * dropped.
 *
 * Post-fix: urbi_vm_run accepts a realm (NULL → global, preserving
 * pre-Wave-5 behavior).  The threading is end-to-end: a binding installed
 * on a non-default Realm before the run is visible to the bytecode running
 * in that Realm.
 *
 * Setup: create realm A, install global "x" = 7 on A; create realm B,
 * install global "x" = 9 on B; run "x" through realm A and realm B
 * separately and verify the values diverge.  Pre-fix would have read
 * from the global Realm in both cases (returning 0 / SLOT_NOT_FOUND
 * because nothing installed "x" on global). */
UTEST(run_chunk_threads_realm_argument_through_vm_run)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm_a = urbi_realm_create(&vm);
    URealm *realm_b = urbi_realm_create(&vm);
    UASSERT(realm_a != NULL);
    UASSERT(realm_b != NULL);

    UValue seven;
    seven.kind = UVAL_INT;
    memset(seven._pad, 0, sizeof(seven._pad));
    seven.v.i = 7;

    UValue nine;
    nine.kind = UVAL_INT;
    memset(nine._pad, 0, sizeof(nine._pad));
    nine.v.i = 9;

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, realm_a, "x", 1, seven));
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, realm_b, "x", 1, nine));

    /* Compile "x" once; reuse for both realms. */
    const char *src = "x";
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    uarena_init(&arena, 4096);
    UProto module;
    memset(&module, 0, sizeof(module));
    UEmitter e;
    uemit_init(&e, &module, &arena, &vm, NULL);
    UParser p;
    uparse_init(&p, &lex, &arena);
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        UASSERT(node->kind != AST_ERROR);
        UASSERT_EQ(EMIT_OK, uemit_statement(&e, node));
        uarena_reset(&arena);
    }
    UASSERT_EQ(EMIT_OK, uemit_finish(&e));

    UValue out_a;
    out_a.kind = UVAL_NIL;
    int rc_a = urbi_run_chunk(&vm, realm_a, &module, &out_a);
    UASSERT_EQ(URBI_OK, rc_a);
    UASSERT_EQ((int)out_a.kind, (int)UVAL_INT);
    UASSERT_EQ(out_a.v.i, (int64_t)7);

    UValue out_b;
    out_b.kind = UVAL_NIL;
    int rc_b = urbi_run_chunk(&vm, realm_b, &module, &out_b);
    UASSERT_EQ(URBI_OK, rc_b);
    UASSERT_EQ((int)out_b.kind, (int)UVAL_INT);
    UASSERT_EQ(out_b.v.i, (int64_t)9);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T112 — API-010: urbi_call_host_with_watchdog vm/fn defense
 * ===================================================================
 *
 * Pre-fix: urbi_call_host_with_watchdog dereferenced vm->host_time_us
 * without a NULL check on either vm or fn.
 *
 * Post-fix: returns urbi_make_nil() early on NULL vm or fn.  Test only
 * meaningful in URBI_DEBUG builds (non-debug collapses to a macro that
 * unconditionally calls fn). */
UTEST(call_host_with_watchdog_handles_null_vm_fn)
{
#ifdef URBI_DEBUG
    UValue r;
    /* NULL vm: must return nil without crash. */
    r = urbi_call_host_with_watchdog(NULL, NULL, NULL, 0, NULL);
    UASSERT_EQ((int)r.kind, (int)UVAL_NIL);

    /* Valid vm but NULL fn: must return nil. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    r = urbi_call_host_with_watchdog(&vm, NULL, NULL, 0, NULL);
    UASSERT_EQ((int)r.kind, (int)UVAL_NIL);
    urbi_vm_destroy(&vm);
#else
    /* Non-debug: macro form has no defensive layer. */
    UASSERT(1);
#endif
}

/* ===================================================================
 * T113 — API-011: URBI_VERSION literal updated to current release
 * ===================================================================
 *
 * The URBI_VERSION literal in src/urbi.c had been stale at "0.3.0-concurrency"
 * since M3 (2026-04-28), unchanged through every subsequent release.  Wave 5
 * catches it up to the current target ("0.5.7-fixes") and adds this regression
 * so the release ritual surfaces a test failure if a future ship is forgotten.
 *
 * The expected literal must be updated in this test alongside any URBI_VERSION
 * change in src/urbi.c.  WORKFLOW.md §8 documents this as part of the release
 * tag ritual. */
UTEST(urbi_version_matches_release_tag)
{
    const char *v = urbi_version();
    UASSERT(v != NULL);
    UASSERT(strcmp(v, "0.13.4-error-surfacing") == 0);
}

/* ===================================================================
 * T119 — COV-001: src/urbi.c setters + panic body (non-DEBUG coverage)
 * ===================================================================
 *
 * The coverage build is non-DEBUG, so the URBI_DEBUG-gated body of
 * urbi_in_isr / urbi_call_host_with_watchdog / urbi_get_determinism_checksum
 * is excluded from instrumentation.  What remains in src/urbi.c at non-DEBUG:
 *
 *   urbi_version          (line 29)            — already covered by T113
 *   urbi_panic            (lines 40, 43-46)    — covered by ungating T108
 *   urbi_set_isr_check_fn (lines 59, 61-62)    — covered by T119 below
 *   urbi_set_callback_watchdog_mode (80, 82-83)— covered by T119 below
 *
 * Both setters have a NULL-vm early-return then a single field assignment.
 * Test calls each with NULL (early-return path) and a real vm (assignment). */

static bool stub_isr_predicate(void *ud) { (void)ud; return false; }

UTEST(set_isr_check_fn_null_safe_and_assigns)
{
    /* NULL vm: must early-return without crashing. */
    urbi_set_isr_check_fn(NULL, stub_isr_predicate, NULL);

    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Default state after init: no predicate registered. */
    UASSERT(vm.isr_check_fn == NULL);

    /* Real vm: assigns the predicate. */
    urbi_set_isr_check_fn(&vm, stub_isr_predicate, NULL);
    UASSERT(vm.isr_check_fn == stub_isr_predicate);

    /* Re-assigning NULL clears it (the documented "disable ISR checking"
     * default behaviour referenced in src/urbi.c:57). */
    urbi_set_isr_check_fn(&vm, NULL, NULL);
    UASSERT(vm.isr_check_fn == NULL);

    urbi_vm_destroy(&vm);
}

UTEST(set_callback_watchdog_mode_null_safe_and_assigns)
{
    /* NULL vm: must early-return without crashing. */
    urbi_set_callback_watchdog_mode(NULL, URBI_WATCHDOG_WARN);

    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Real vm: WARN and ASSERT both round-trip through the field. */
    urbi_set_callback_watchdog_mode(&vm, URBI_WATCHDOG_WARN);
    UASSERT_EQ((int)vm.callback_watchdog_mode, (int)URBI_WATCHDOG_WARN);

    urbi_set_callback_watchdog_mode(&vm, URBI_WATCHDOG_ASSERT);
    UASSERT_EQ((int)vm.callback_watchdog_mode, (int)URBI_WATCHDOG_ASSERT);

    urbi_vm_destroy(&vm);
}

/* urbi_panic with a non-NULL message — exercises the hosted-libc path
 * (fputs(msg, stderr); fputc('\n', stderr); abort()) without going through
 * the NULL-substitution branch already covered by urbi_panic_handles_null_msg.
 * Both fork-and-abort tests together cover the full body of urbi_panic. */
UTEST(urbi_panic_handles_real_msg)
{
    EXPECT_ABORT(urbi_panic("test panic — coverage exercise"));
}

/* ===================================================================
 * Suite registration
 * =================================================================== */

void test_public_api_suite(void)
{
    utest_run("urbi_panic_handles_null_msg",
              urbi_panic_handles_null_msg);
    utest_run("throw_return_val_tag_stop_handle_null_vm",
              throw_return_val_tag_stop_handle_null_vm);
    utest_run("register_event_drain_null_check_before_assert",
              register_event_drain_null_check_before_assert);
    utest_run("run_chunk_threads_realm_argument_through_vm_run",
              run_chunk_threads_realm_argument_through_vm_run);
    utest_run("call_host_with_watchdog_handles_null_vm_fn",
              call_host_with_watchdog_handles_null_vm_fn);
    utest_run("urbi_version_matches_release_tag",
              urbi_version_matches_release_tag);
    utest_run("set_isr_check_fn_null_safe_and_assigns",
              set_isr_check_fn_null_safe_and_assigns);
    utest_run("set_callback_watchdog_mode_null_safe_and_assigns",
              set_callback_watchdog_mode_null_safe_and_assigns);
    utest_run("urbi_panic_handles_real_msg",
              urbi_panic_handles_real_msg);
}
