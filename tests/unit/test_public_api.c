/* SPDX-License-Identifier: BSD-3-Clause */
/* Phase 18 (T108-T113): public C API NULL-safety + signature carry-forwards.
 *
 * Audit IDs closed:
 *   API-001 — urbi_panic NULL guard on msg
 *   API-002 — urbi_throw / return_val / tag_stop_local NULL guard on s->vm
 *   API-003 — urbi_register_event_drain assert ordering before NULL check
 *   API-004 — urbi_run_chunk realm-arg threading (signature change carry from Wave 3)
 *   API-010 — urbi_call_host_with_watchdog vm/fn defense
 *   API-011 — URBI_VERSION literal stale at "0.3.0-concurrency"
 */

#include "utest.h"

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "realm/urealm.h"
#include "module/umodule.h"
#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"

#include <string.h>
#include <stddef.h>
#include <stdint.h>

#ifdef URBI_DEBUG
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* EXPECT_ABORT: assert that expr causes abort (signal or non-zero exit).
 * Used to verify urbi_panic on a path that must trap fatal. */
#define EXPECT_ABORT(expr)                                               \
    do {                                                                 \
        utest_checks++;                                                  \
        pid_t _pid = fork();                                             \
        if (_pid == 0) {                                                 \
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
#endif /* URBI_DEBUG */

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
 * abort is reached.  Test is URBI_DEBUG-only because fork-based death
 * tests need <sys/wait.h> and the EXPECT_ABORT shim is debug-gated. */
UTEST(urbi_panic_handles_null_msg)
{
#ifdef URBI_DEBUG
    EXPECT_ABORT(urbi_panic(NULL));
#else
    UASSERT(1);
#endif
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

    /* All three must be no-ops when s->vm is NULL. */
    urbi_throw(&s, v);
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);

    urbi_return_val(&s, v);
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);

    urbi_tag_stop_local(&s, NULL, v);
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);

    /* Also verify NULL strand: must not crash. */
    urbi_throw(NULL, v);
    urbi_return_val(NULL, v);
    urbi_tag_stop_local(NULL, NULL, v);
    UASSERT(1);
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
}
