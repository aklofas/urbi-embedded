/* SPDX-License-Identifier: BSD-3-Clause */
/* test_proto_refcount — direct unit tests for the v0.7.3 UProto refcount
 * mechanism (Piece A of the closure-lifetime spec).
 *
 * The refcount invariant (post-T7):
 *   refcount = (1 if proto still in module->nested[]) + (1 per live closure)
 *              + (1 if proto is on vm->stdlib_protos — list ref, bumped at
 *                 rescue, discharged by the stdlib_protos sweep)
 *
 * Discharge sites: strand_closure_unlink detach (-1 from nested),
 * umodule_destroy's walk (-1 from nested), pool_free OWNS_* arms (-1 per
 * cl), stdlib_protos sweep (-1 list ref).  Proto is freed when refcount
 * hits 0.
 *
 * These tests pin the basic mechanics and the umodule_destroy → rescue →
 * pool_free → stdlib_protos sweep flow that exposed the v0.7.3 cascade-
 * fix double-free regression. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "chunk/uchunk.h"
#include "vm/uvm.h"
#include "value/uarena.h"
#include "realm/urealm.h"
#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* The double-free regression repro: install an at-event watcher, destroy
 * the module while the watcher is still alive (forces the rescue path),
 * then let urbi_vm_destroy unregister the watcher (pool_free) followed by
 * the stdlib_protos sweep.  Pre-fix: pool_free decs refcount to 0 and
 * frees the proto; sweep walks the (stale) stdlib_protos entry and
 * double-frees.  Post-fix: rescue bumps refcount so pool_free can never
 * drop it to 0; sweep is the only path that frees a rescued proto. */
UTEST(proto_refcount_rescue_then_pool_free_no_double_free)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    urbi_event_id_t fire = urbi_event_register(&vm, r, "fire", NULL, NULL);
    UASSERT(fire != URBI_EVENT_ID_INVALID);

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "Realm.x = 0;"
        "at (fire?) Realm.x = Realm.x + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Module destroyed FIRST — at-event watcher's body proto has
     * refcount > 0 (the body closure holds a ref).  Rescue path
     * transfers it to vm->stdlib_protos. */
    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);

    /* Fire the watcher once (validates the rescued proto still works). */
    urbi_inject_event(&vm, (uint32_t)fire, NULL, 0U);
    for (int j = 0; j < 100; j++) {
        UStepResult s = urbi_step(&vm, 500, NULL);
        if (s == URBI_STEP_QUIESCENT) break;
    }

    UValue x = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "x", 1, &x));
    UASSERT_EQ((int)UVAL_INT, (int)x.kind);
    UASSERT_EQ(1LL, x.v.i);

    /* vm_destroy will: (a) tear down realm → unregister watcher → pool_free
     * → dec refcount, (b) stdlib_protos sweep → free rescued proto.
     * Pre-fix: double-free SEGV here.  Post-fix: clean. */
    urbi_vm_destroy(&vm);
}

void
test_proto_refcount_suite(void)
{
    utest_run("proto_refcount: rescue + pool_free + sweep — no double-free",
              proto_refcount_rescue_then_pool_free_no_double_free);
}
