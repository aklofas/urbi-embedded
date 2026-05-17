/* SPDX-License-Identifier: BSD-3-Clause */
/* test_module_refcount — direct unit tests for the v0.8.0 UModule refcount
 * mechanism.  Mirrors the v0.7.3 UProto refcount pattern (Piece A of the
 * closure-lifetime spec).
 *
 * Invariant: refcount = (1 per UStrand that has s->module = this module).
 * Bumped at strand binding; decremented at strand destroy.  Module is freed
 * when refcount == 0 AND destroy_requested is true (host has released its
 * reference). */

#include "utest.h"

#include "urbi/urbi.h"
#include "module/umodule.h"
#include "vm/uvm.h"

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

/* Task 2: inc/dec helpers mutate refcount correctly. */
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

/* Probe the deferred-destroy path: if refcount > 0 when umodule_destroy
 * is called, the struct must survive (the host destroys their ref but
 * strands still reference the module).  Free fires on the last refcount
 * drop. */
UTEST(umodule_destroy_defers_when_refcount_nonzero)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Allocate a minimal UModule on the heap so we can probe it post-
     * destroy.  Stack-allocated would not let us observe the deferred
     * state; we need the deferred path to keep the heap allocation
     * alive until the final dec. */
    UModule *m = (UModule *)vm.alloc_fn(NULL, sizeof(UModule), vm.alloc_ud);
    UASSERT(m != NULL);
    memset(m, 0, sizeof(UModule));

    umodule_refcount_inc(m, &vm);    /* simulated strand binding */
    UASSERT_EQ((unsigned)1, (unsigned)m->refcount);

    umodule_destroy(m, &vm);          /* host releases ref */
    /* refcount > 0 — m must survive.  destroy_requested set. */
    UASSERT_EQ(true, m->destroy_requested);
    UASSERT_EQ((unsigned)1, (unsigned)m->refcount);

    umodule_refcount_dec(m, &vm);    /* simulated strand dies */
    /* refcount drops to 0; destroy_requested was true → deferred free
     * fires via umodule_destroy_internal (zeroes sub-buffers, zeroes
     * the struct).  The UModule struct allocation itself is still valid
     * heap memory (umodule_destroy_internal zeroes but does not free the
     * struct — lifetime of the containing allocation is caller-owned). */
    vm.alloc_fn(m, 0, vm.alloc_ud);  /* release the heap UModule struct */

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
}
