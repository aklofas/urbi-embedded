/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_stop_path.c
 *
 * v0.10.7 W3: focused unit tests for the stop-path session teardown.
 *
 * Tests:
 *   1. stop_with_no_sessions       — baseline: stop on empty server is safe.
 *   2. stop_with_direct_session    — a session created directly (no reader
 *                                    thread) is destroyed by stop; no leak.
 *   3. stop_with_flagged_session   — session already flagged for teardown
 *                                    before stop is called; stop cleans up.
 *   4. stop_clears_sessions_head   — after stop, sessions_head is NULL.
 *
 * These exercise the stop path's single-owner contract documented in
 * docs/internals/repl-teardown.md §5. */

#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "repl/urepl.h"
#include "repl/urepl_dispatch.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Count sessions in server->sessions_head (walks the linked list). */
static int
count_sessions(UReplServer *server)
{
    int n = 0;
    UReplSession *s = server->sessions_head;
    while (s != NULL) {
        n++;
        s = s->next;
    }
    return n;
}

static UReplServer *
mk_server(UVM *vm)
{
    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = -1;
    return urbi_repl_serve(vm, &cfg, NULL);
}

/* ---- Tests --------------------------------------------------------------- */

UTEST(stop_with_no_sessions)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    UReplServer *server = mk_server(&vm);
    UASSERT_NE(server, NULL);

    /* No sessions created — stop must not crash. */
    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

UTEST(stop_with_direct_session)
{
    /* A session created via urepl_session_create (no POSIX reader thread,
     * no listener path) must be cleaned up by urbi_repl_stop.  Before W3
     * the stop path called urepl_session_destroy directly; the test verifies
     * that path still works correctly under ASan (no double-free, no leak). */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    UReplServer *server = mk_server(&vm);
    UASSERT_NE(server, NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT_NE(s, NULL);

    /* One session attached. */
    UASSERT_EQ(count_sessions(server), 1);

    urbi_repl_stop(server);

    /* After stop, sessions_head must be NULL — the session was destroyed. */
    UASSERT_EQ(server->sessions_head, NULL);

    urbi_vm_destroy(&vm);
}

UTEST(stop_with_flagged_session)
{
    /* If a session has needs_teardown set before stop is called (e.g. the
     * reader already requested teardown but the VM step loop ended before
     * the reaper fired), stop must still clean it up. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    UReplServer *server = mk_server(&vm);
    UASSERT_NE(server, NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT_NE(s, NULL);

    /* Pre-flag the session as if a reader requested teardown. */
    urepl_request_teardown(s);

    /* Session still in list (reaper hasn't run). */
    UASSERT_EQ(count_sessions(server), 1);

    urbi_repl_stop(server);

    /* After stop, all sessions must be gone. */
    UASSERT_EQ(server->sessions_head, NULL);

    urbi_vm_destroy(&vm);
}

UTEST(stop_clears_sessions_head)
{
    /* Multiple sessions: stop must clear all of them. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    UReplServer *server = mk_server(&vm);
    UASSERT_NE(server, NULL);

    /* Create three direct sessions. */
    UReplSession *s1 = urepl_session_create(server);
    UReplSession *s2 = urepl_session_create(server);
    UReplSession *s3 = urepl_session_create(server);
    UASSERT_NE(s1, NULL);
    UASSERT_NE(s2, NULL);
    UASSERT_NE(s3, NULL);
    UASSERT_EQ(count_sessions(server), 3);

    urbi_repl_stop(server);

    UASSERT_EQ(server->sessions_head, NULL);

    urbi_vm_destroy(&vm);
}

#endif /* URBI_ENABLE_REPL */

void
test_repl_stop_path_suite(void)
{
#ifdef URBI_ENABLE_REPL
    printf("test_repl_stop_path\n");
    utest_run("stop_with_no_sessions",    stop_with_no_sessions);
    utest_run("stop_with_direct_session", stop_with_direct_session);
    utest_run("stop_with_flagged_session",stop_with_flagged_session);
    utest_run("stop_clears_sessions_head",stop_clears_sessions_head);
#else
    printf("test_repl_stop_path (URBI_ENABLE_REPL=0, skipped)\n");
#endif
}
