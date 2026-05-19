/* SPDX-License-Identifier: BSD-3-Clause */
/* test_dangling_cl_function_body_install — regression net for the function-
 * body install path (at-handler installed from inside a callee, calling
 * strand dies before the watcher fires).
 *
 * Passes today via the M6 Phase 3 closure migration at urbi_vm_run exit
 * (src/vm/uvm_run.c:174-196) and the OWNS_BODY ownership transfer in
 * install_at_event_runtime.  v0.7.3 closure-lifetime reform replaces both
 * mechanisms (migration deleted at T25; OWNS_BODY deleted at T20) with a
 * single GC-root mechanism — the watcher walker (uwatcher_gc.c) already
 * yields w->body as a UVAL_CLOSURE root, so once UClosure is GC-managed
 * (T14) the lifetime is governed by reachability alone.
 *
 * Guards against any future change that removes either mechanism without
 * a working replacement.  Should remain green throughout the closure-
 * lifetime reform and beyond. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "chunk/uchunk.h"
#include "value/uarena.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

static UStepResult
drain_to_quiescent(UVM *vm)
{
    UStepResult r = URBI_STEP_QUIESCENT;
    int i;
    for (i = 0; i < 2000; i++) {
        r = urbi_step(vm, 500, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT ||
            r == URBI_STEP_FATAL) {
            return r;
        }
    }
    return r;
}

/* Install at-event watcher from inside a function body; let the strand
 * die; then inject the event and verify body fires (dangling-cl repro). */
UTEST(dangling_cl_at_event_from_function_body)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t fire = urbi_event_register(&vm, r, "fire", NULL, NULL);
    UASSERT(fire != URBI_EVENT_ID_INVALID);

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    /* Install the at-handler from inside a function body.  After install()
     * returns, the chunk-strand continues; chunk_strand eventually dies
     * via urbi_vm_run cleanup.  At that point the at-handler's body
     * UClosure (allocated by OP_CLOSURE inside install()'s frame) is
     * GC-managed and kept alive by the watcher's reference. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "Realm.fired = 0;"
        "var install = function () {"
        "  at (fire?) Realm.fired = Realm.fired + 1"
        "};"
        "install()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Strand that ran the chunk + install() is dead by now.  Inject fire. */
    urbi_inject_event(&vm, (uint32_t)fire, NULL, 0U);
    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue fired = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "fired", 5, &fired));
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(1LL, fired.v.i);

    uarena_destroy(&arena);
    uchunk_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

void
test_dangling_cl_function_body_install_suite(void)
{
    utest_run("dangling_cl: at-event installed from function body fires after strand death",
              dangling_cl_at_event_from_function_body);
}
