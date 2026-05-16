/* SPDX-License-Identifier: BSD-3-Clause */
/* End-to-end: scripted at sync (event?) body fires inline through real
 * bytecode dispatch when c_event_emit_sync deposits a payload.
 *
 * Companion to test_at_sync_scripted.c (cond-watcher AT_SYNC) and
 * test_tag_stop_onleave_scripted.c (drain onleave) — exercises the
 * AT_EVENT_SYNC subscriber path: each subscriber's body runs synchronously
 * via run_event_body_on_scratch → urbi_run_closure_on_scratch_with_payload
 * before c_event_emit_sync returns.
 *
 * Construction approach (per plan simplified path):
 *   - Note: the AST_AT_EVENT scripted-install register-allocation desync was
 *     a real codegen bug — fixed in v0.5.7-fixes Phase 2 (commits
 *     3af426c..dc98956), tracked in REVIVAL §14 row S-emit-freereg-discipline.
 *   - Event.new() and scripted `at sync (e?) body` install are still not
 *     wirable end-to-end: Event.new() awaits M6 stdlib (the desync fix
 *     unblocks the codegen path but the constructor itself is still missing).
 *     See tests/unit/test_parse_at_event.c
 *     emit_at_event_global_member_event_expr_disjoint_regs for the
 *     desync regression test.
 *   - Instead: compile a one-arg body closure from urbiscript
 *     (`function(p) { Realm.received = p }`), retrieve the resulting
 *     UClosure value from urbi_run_chunk's return slot, construct a UEvent
 *     in C, and install the AT_EVENT_SYNC watcher directly via
 *     install_at_event_runtime.
 *   - c_event_emit_sync(vm, e, payload) then exercises run_event_body_on_scratch
 *     end-to-end via the real bytecode dispatcher.
 *
 * The compiled body closure is held alive by vm->last_return_closure until
 * the next urbi_run_chunk or urbi_vm_destroy — so the test must NOT call
 * urbi_run_chunk a second time after the closure is captured.
 *
 * No test hooks installed — the body runs through the real scratch helper. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include <stddef.h>

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "value/uarena.h"
#include "module/umodule.h"
#include "vm/uvm.h"
#include "watcher/uwatcher.h"
#include "watcher/uwatcher_install.h"
#include "event/uevent.h"
#include "event/uevent_emit.h"

#define UTEST(name) static void name(void)

/* compile_and_run / make_int now live in utest_e2e_helpers.{h,c}.  This
 * file uses the _with_module variant because the captured body closure
 * must remain alive after the call (it gets installed in an
 * AT_EVENT_SYNC watcher and fired via c_event_emit_sync below). */

/* ===================================================================
 * Test: scripted_event_sync_emit_delivers_payload
 *
 * 1. Pre-install Realm.received = 0.
 * 2. Compile + run `function(p) { Realm.received = p }`; capture the
 *    returned UClosure value.  The chunk leaves it pinned in
 *    vm->last_return_closure, so it stays alive for the rest of the test.
 * 3. Construct a UEvent in C; install an AT_EVENT_SYNC watcher with the
 *    captured body closure via install_at_event_runtime.
 * 4. Call c_event_emit_sync(vm, e, payload=42).  This walks the
 *    at_watchers_head chain and calls run_event_body_on_scratch for the
 *    sync subscriber, which dispatches the body via the scratch helper
 *    with payload in R[0].
 * 5. Verify Realm.received == 42 — confirms the body fired with payload.
 * 6. Re-fire with a different payload to confirm the wire isn't a one-shot.
 * =================================================================== */
UTEST(scripted_event_sync_emit_delivers_payload)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *gr = urbi_realm_global(&vm);
    UASSERT(gr != NULL);
    if (gr == NULL) { urbi_vm_destroy(&vm); return; }

    int rc = urbi_realm_set_global(&vm, gr, "received", 8, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    /* === Phase 1: compile a one-arg body closure that writes to
     *              Realm.received.  The chunk's RET value is the
     *              function-literal closure. === */
    UArena   arena;
    UModule  module = {0};
    uarena_init(&arena, 4096);

    UValue closure_val = {0};
    rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "function(p) { Realm.received = p }",
        &closure_val);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_CLOSURE, (int)closure_val.kind);
    if (rc != URBI_OK || closure_val.kind != UVAL_CLOSURE) {
        umodule_destroy(&module, NULL);
        uarena_destroy(&arena);
        urbi_vm_destroy(&vm);
        return;
    }

    UClosure *body = (UClosure *)closure_val.v.p;
    UASSERT(body != NULL);

    /* === Phase 2: construct UEvent + install AT_EVENT_SYNC watcher === */
    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);
    if (e == NULL) {
        umodule_destroy(&module, NULL);
        uarena_destroy(&arena);
        urbi_vm_destroy(&vm);
        return;
    }

    /* install_at_event_runtime needs a strand for resolve_owning_tag /
     * realm wiring.  Use a transient stack strand pointed at the global
     * realm — same pattern as test_event_emit_sync.c. */
    UStrand inst_strand;
    ustrand_init(&inst_strand, &vm);
    inst_strand.realm = gr;

    UWatcherInstallResult ir =
        install_at_event_runtime(&vm, &inst_strand,
                                  UWATCHER_AT_EVENT_SYNC, e, body, NULL);
    UASSERT_EQ((int)URBI_INSTALL_OK, (int)ir);

    UASSERT(e->at_watchers_head != NULL);
    UASSERT_EQ((int)UWATCHER_AT_EVENT_SYNC,
               (int)e->at_watchers_head->mode);

    /* === Phase 3: fire the event === */
    c_event_emit_sync(&vm, e, utest_e2e_make_int(42));

    UValue received = {0};
    rc = urbi_realm_get_global(&vm, gr, "received", 8, &received);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)received.kind);
    UASSERT_EQ(42, (int)received.v.i);

    /* === Phase 4: re-fire with a different payload === */
    c_event_emit_sync(&vm, e, utest_e2e_make_int(7));

    rc = urbi_realm_get_global(&vm, gr, "received", 8, &received);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)received.kind);
    UASSERT_EQ(7, (int)received.v.i);

    /* === Cleanup ===
     *
     * Unregister the watcher BEFORE destroying the strand / module / arena
     * so the watcher doesn't outlive its body closure's backing memory. */
    while (e->at_watchers_head != NULL)
        urbi_watcher_unregister_internal(&vm, e->at_watchers_head);
    ustrand_destroy(&inst_strand, &vm);
    umodule_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_event_sync_emit_scripted_suite(void)
{
    printf("test_event_sync_emit_scripted\n");
    utest_run("scripted_event_sync_emit_delivers_payload",
              scripted_event_sync_emit_delivers_payload);
}
