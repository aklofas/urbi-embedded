/* SPDX-License-Identifier: BSD-3-Clause */
/* test_module_refcount — direct unit tests for the v0.8.0 UModule refcount
 * mechanism.  Mirrors the v0.7.3 UProto refcount pattern (Piece A of the
 * closure-lifetime spec).
 *
 * v0.8.1 Phase 2 (Variant B fusion): strand-bind refcount has moved from
 * module->refcount to module->root_proto->refcount.  module->refcount is
 * retained in the struct (always 0 after the redirect) until Task 11 removes
 * it.  Deferred-destroy now reads root_proto->refcount.
 *
 * Tests that probed the old module->refcount field directly are updated here. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "module/umodule.h"
#include "vm/uvm.h"
#include "value/uarena.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Task 1: struct fields exist and zero-init correctly. */
UTEST(refcount_fields_zero_initialized)
{
    UModule m = {0};
    UASSERT_EQ((unsigned)0, (unsigned)m.refcount);
    UASSERT_EQ(false, m.destroy_requested);
}

/* Task 2: inc/dec helpers mutate refcount correctly.
 * v0.8.1 Phase 2: module->refcount is retained but no longer bumped by
 * strand binds.  umodule_refcount_inc/dec still work on the field directly;
 * new callers use umodule_proto_refcount_inc/dec on root_proto instead. */
UTEST(refcount_inc_dec_basic)
{
    UModule m = {0};
    umodule_refcount_inc(&m, NULL);
    UASSERT_EQ((unsigned)1, (unsigned)m.refcount);
    umodule_refcount_inc(&m, NULL);
    UASSERT_EQ((unsigned)2, (unsigned)m.refcount);
    umodule_refcount_dec(&m, NULL);
    UASSERT_EQ((unsigned)1, (unsigned)m.refcount);
    umodule_refcount_dec(&m, NULL);
    UASSERT_EQ((unsigned)0, (unsigned)m.refcount);
    /* Verify field is still writable (not removed yet). */
    UASSERT_EQ((unsigned)0, (unsigned)m.refcount);
}

UTEST(refcount_inc_saturates_at_uint16_max)
{
    UModule m = {0};
    m.refcount = UINT16_MAX;
    umodule_refcount_inc(&m, NULL);
    UASSERT_EQ((unsigned)UINT16_MAX, (unsigned)m.refcount);
    /* No crash, no wrap.  Saturation policy matches v0.7.3 UProto. */
}

UTEST(refcount_dec_at_saturation_no_change)
{
    UModule m = {0};
    m.refcount = UINT16_MAX;
    umodule_refcount_dec(&m, NULL);
    UASSERT_EQ((unsigned)UINT16_MAX, (unsigned)m.refcount);
    /* No decrement.  Saturation policy: once frozen, stay frozen. */
}

/* Probe the deferred-destroy path: if root_proto->refcount > 0 when
 * umodule_destroy is called, the struct must survive (the host destroys
 * their ref but strands still reference the module via root_proto).
 * Free fires when the last strand-bind ref drops via umodule_strand_refcount_dec.
 *
 * v0.8.1 Phase 2: deferred-destroy check reads root_proto->refcount (not
 * module->refcount).  We simulate a strand binding by bumping root_proto
 * directly via umodule_proto_refcount_inc. */
UTEST(umodule_destroy_defers_when_refcount_nonzero)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Allocate a minimal UModule + a synthetic root_proto on the heap so
     * we can probe them post-destroy.  Stack-allocated would not let us
     * observe the deferred state; we need the deferred path to keep the
     * heap allocation alive until the final dec. */
    UModule *m = (UModule *)vm.alloc_fn(NULL, sizeof(UModule), vm.alloc_ud);
    UASSERT(m != NULL);
    memset(m, 0, sizeof(UModule));

    /* Allocate a synthetic root_proto and attach it to the module.
     * umodule_destroy_internal frees m->root_proto via module_allocator
     * (stdlib_alloc on hosted builds) — so rp is NOT freed separately. */
    UProto *rp = (UProto *)vm.alloc_fn(NULL, sizeof(UProto), vm.alloc_ud);
    UASSERT(rp != NULL);
    memset(rp, 0, sizeof(UProto));
    m->root_proto = rp;

    /* Simulate a strand binding: bump root_proto->refcount. */
    umodule_proto_refcount_inc(rp);
    UASSERT_EQ((unsigned)1, (unsigned)rp->refcount);

    umodule_destroy(m, &vm);          /* host releases ref */
    /* root_proto->refcount > 0 — m must survive.  destroy_requested set. */
    UASSERT_EQ(true, m->destroy_requested);
    UASSERT_EQ((unsigned)1, (unsigned)rp->refcount);

    /* Simulate strand death: umodule_strand_refcount_dec fires deferred destroy.
     * umodule_destroy_internal will free rp (module->root_proto) via
     * stdlib_alloc; do NOT touch rp after this call. */
    umodule_strand_refcount_dec(m, rp, &vm);
    /* refcount dropped to 0; destroy_requested was true → deferred free
     * fired via umodule_destroy_internal.  UModule struct allocation itself
     * is still valid heap memory (umodule_destroy_internal zeroes the struct
     * fields but does not free the struct — lifetime is caller-owned). */
    vm.alloc_fn(m, 0, vm.alloc_ud);   /* release the heap UModule struct */

    urbi_vm_destroy(&vm);
}

UTEST(umodule_destroy_immediate_when_refcount_zero)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UModule *m = (UModule *)vm.alloc_fn(NULL, sizeof(UModule), vm.alloc_ud);
    UASSERT(m != NULL);
    memset(m, 0, sizeof(UModule));

    /* refcount == 0 (no strand binding); umodule_destroy frees sub-buffers
     * and zeroes the struct immediately (legacy behavior unchanged).
     * The UModule struct allocation itself is caller-owned; free it here. */
    umodule_destroy(m, &vm);
    vm.alloc_fn(m, 0, vm.alloc_ud);  /* release the heap UModule struct */

    urbi_vm_destroy(&vm);
}

/* End-to-end: compile a minimal chunk, drive it via urbi_run_chunk,
 * verify the binding bump+decrement cycle leaves root_proto->refcount at zero.
 *
 * v0.8.1 Phase 2: strand-bind refcount moved to root_proto.  After the
 * strand dies, root_proto->refcount must be 0 (bump+unbind balanced).
 * module.refcount is always 0 (nothing bumps it); checking it is vacuous. */
UTEST(refcount_bump_decrement_via_strand_binding)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    /* Compile + run a trivial chunk via urbi_run_chunk — the public path. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "42", NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* After the strand dies, root_proto->refcount must be 0
     * (strand-bind bump + ustrand_destroy dec are balanced). */
    UASSERT(module.root_proto != NULL);
    UASSERT_EQ((unsigned)0, (unsigned)module.root_proto->refcount);
    /* module.refcount is always 0 after Phase 2 redirect. */
    UASSERT_EQ((unsigned)0, (unsigned)module.refcount);

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

void test_module_refcount_suite(void) {
    utest_run("module_refcount: fields zero-initialized",
              refcount_fields_zero_initialized);
    utest_run("module_refcount: inc/dec basic",
              refcount_inc_dec_basic);
    utest_run("module_refcount: inc saturates at UINT16_MAX",
              refcount_inc_saturates_at_uint16_max);
    utest_run("module_refcount: dec at saturation no change",
              refcount_dec_at_saturation_no_change);
    utest_run("module_refcount: umodule_destroy defers when refcount nonzero",
              umodule_destroy_defers_when_refcount_nonzero);
    utest_run("module_refcount: umodule_destroy immediate when refcount zero",
              umodule_destroy_immediate_when_refcount_zero);
    utest_run("module_refcount: bump/decrement via strand binding",
              refcount_bump_decrement_via_strand_binding);
}
