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
 *   - Event.new() and scripted `at sync (e?) body` install are not yet
 *     wirable end-to-end (T59 Event stdlib defer at v0.5.1; the original
 *     v0.5.1 retrospective also flagged an AST_AT_EVENT register-allocation
 *     desync, fixed in v0.5.2 — see tests/unit/test_parse_at_event.c
 *     emit_at_event_global_member_event_expr_disjoint_regs for the
 *     regression test, so only the Event.new() defer still applies).
 *   - Instead: compile a one-arg body closure from urbiscript
 *     (`function(p) { Realm.received = p }`), retrieve the resulting
 *     UClosure value from urbi_run_chunk's return slot, construct a UEvent
 *     in C, and install the AT_EVENT_SYNC watcher directly via
 *     install_at_event_runtime.
 *   - c_event_emit_sync(vm, e, payload) then exercises run_event_body_on_scratch
 *     end-to-end via the real bytecode dispatcher.
 *
 * The compiled body closure is held alive by vm->last_return_closure until
 * the next urbi_run_chunk or uvm_destroy — so the test must NOT call
 * urbi_run_chunk a second time after the closure is captured.
 *
 * No test hooks installed — the body runs through the real scratch helper. */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "umodule.h"
#include "parse/uparse.h"
#include "uvm.h"
#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "ustrand.h"
#include "watcher/uwatcher.h"
#include "watcher/uwatcher_install.h"
#include "uevent.h"
#include "uevent_emit.h"

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

static UValue
make_int(int64_t n)
{
    UValue v;
    v.kind = UVAL_INT;
    v.v.i  = n;
    return v;
}

/* Compile + run `src`, return the chunk's result through *out_result.
 * Caller owns `arena` and `module` and must keep them alive as long as
 * any returned UVAL_CLOSURE is in use. */
static int
compile_and_run(UVM *vm, UArena *arena, UModule *module,
                const char *src, UValue *out_result)
{
    URealm *realm = urbi_realm_global(vm);
    if (realm == NULL) return URBI_ERR_OOM;

    ULexer   lex;
    UEmitter e;
    UParser  p;
    UAstNode *node;

    ulex_init(&lex, src, strlen(src));
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
    uvm_init(&vm, NULL, NULL);

    URealm *gr = urbi_realm_global(&vm);
    UASSERT(gr != NULL);
    if (gr == NULL) { uvm_destroy(&vm); return; }

    int rc = urbi_realm_set_global(&vm, gr, "received", 8, make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    /* === Phase 1: compile a one-arg body closure that writes to
     *              Realm.received.  The chunk's RET value is the
     *              function-literal closure. === */
    UArena   arena;
    UModule  module = {0};
    uarena_init(&arena, 4096);

    UValue closure_val = {0};
    rc = compile_and_run(&vm, &arena, &module,
        "function(p) { Realm.received = p }",
        &closure_val);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_CLOSURE, (int)closure_val.kind);
    if (rc != URBI_OK || closure_val.kind != UVAL_CLOSURE) {
        umodule_destroy(&module);
        uarena_destroy(&arena);
        uvm_destroy(&vm);
        return;
    }

    UClosure *body = (UClosure *)closure_val.v.p;
    UASSERT(body != NULL);

    /* === Phase 2: construct UEvent + install AT_EVENT_SYNC watcher === */
    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);
    if (e == NULL) {
        umodule_destroy(&module);
        uarena_destroy(&arena);
        uvm_destroy(&vm);
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
    c_event_emit_sync(&vm, e, make_int(42));

    UValue received = {0};
    rc = urbi_realm_get_global(&vm, gr, "received", 8, &received);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)received.kind);
    UASSERT_EQ(42, (int)received.v.i);

    /* === Phase 4: re-fire with a different payload === */
    c_event_emit_sync(&vm, e, make_int(7));

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
    umodule_destroy(&module);
    uarena_destroy(&arena);
    uvm_destroy(&vm);
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
