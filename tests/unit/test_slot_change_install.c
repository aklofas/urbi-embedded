/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: urbi_object_get_or_create_change_event (T60, spec #4 §6.3).
 *
 * Cases:
 *   1. same-name returns same UEvent (idempotent, chain length stays 1)
 *   2. distinct names produce distinct UEvents (chain grows to 2)
 *   3. OOM on node alloc returns NULL, UGC_HAS_SLOT_CHANGE_EVENT not set
 */

#include "utest.h"

#include "object/uobject.h"       /* UObject, urbi_object_alloc */
#include "changed/uchanged_node.h"        /* UChangedNode, urbi_object_get_or_create_change_event */
#include "gc/ugc_incremental.h"   /* UGC_HAS_SLOT_CHANGE_EVENT */
#include "value/uintern.h"              /* ustr_intern → USymbol* */
#include "chunk/umodule.h"              /* USymbol typedef */
#include "vm/uvm.h"
#include "urbi/object.h"          /* URBI_ATOM_OBJECT */

#include <stddef.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

/* Counting allocator that fails once alloc_calls exceeds fail_at. */
typedef struct {
    int alloc_calls;
    int fail_at;   /* -1 = never fail */
} AllocSpy60;

static void *
spy_alloc60(void *ptr, size_t n, void *ud)
{
    AllocSpy60 *spy = (AllocSpy60 *)ud;
    if (n > 0 && ptr == NULL) {
        spy->alloc_calls++;
        if (spy->fail_at >= 0 && spy->alloc_calls > spy->fail_at)
            return NULL;
    }
    if (n == 0) { free(ptr); return NULL; }
    return realloc(ptr, n);
}

/* ===================================================================
 * Test 1: same-name returns same UEvent (idempotent)
 * =================================================================== */

UTEST(get_or_create_returns_same_event_for_same_name)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (o == NULL) { urbi_vm_destroy(&vm); return; }

    USymbol *x = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(x != NULL);

    UEvent *e1 = urbi_object_get_or_create_change_event(&vm, o, x);
    UASSERT(e1 != NULL);

    UEvent *e2 = urbi_object_get_or_create_change_event(&vm, o, x);
    UASSERT(e2 != NULL);

    /* Same name → same UEvent pointer. */
    UASSERT(e1 == e2);

    /* Bit 7 must be set after the first install. */
    UASSERT((((UCell *)o)->gc_byte & UGC_HAS_SLOT_CHANGE_EVENT) != 0);

    /* Chain must have exactly one entry. */
    int count = 0;
    UChangedNode *n;
    for (n = o->changed_events_head; n != NULL; n = n->next) count++;
    UASSERT_EQ(1, count);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 2: distinct names produce distinct UEvents
 * =================================================================== */

UTEST(get_or_create_distinct_events_per_name)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (o == NULL) { urbi_vm_destroy(&vm); return; }

    USymbol *sx = (USymbol *)ustr_intern(&vm, "x", 1);
    USymbol *sy = (USymbol *)ustr_intern(&vm, "y", 1);
    UASSERT(sx != NULL);
    UASSERT(sy != NULL);

    UEvent *ex = urbi_object_get_or_create_change_event(&vm, o, sx);
    UEvent *ey = urbi_object_get_or_create_change_event(&vm, o, sy);
    UASSERT(ex != NULL);
    UASSERT(ey != NULL);

    /* Different names → different UEvent pointers. */
    UASSERT(ex != ey);

    /* Chain must have exactly two entries. */
    int count = 0;
    UChangedNode *n;
    for (n = o->changed_events_head; n != NULL; n = n->next) count++;
    UASSERT_EQ(2, count);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 3: OOM on node alloc returns NULL, bit 7 NOT set
 * =================================================================== */

UTEST(get_or_create_oom_returns_null_failsoft)
{
    /* Let urbi_vm_init succeed, then fail on the very next allocation request
     * (which will be the UChangedNode GC cell + sidecar pair). */
    AllocSpy60 spy;
    spy.alloc_calls = 0;
    spy.fail_at     = 0;   /* fail starting from the 1st subsequent alloc */

    UVM vm;
    urbi_vm_init(&vm, spy_alloc60, &spy);

    /* Disable fail_at while we set up the test objects. */
    spy.fail_at = -1;

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (o == NULL) { urbi_vm_destroy(&vm); return; }

    USymbol *x = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(x != NULL);

    /* Now arm the allocator to fail on the next alloc. */
    spy.alloc_calls = 0;
    spy.fail_at     = 0;

    UEvent *e = urbi_object_get_or_create_change_event(&vm, o, x);

    /* OOM path: must return NULL. */
    UASSERT(e == NULL);

    /* Bit 7 must NOT be set on failure. */
    UASSERT_EQ(0, (int)(((UCell *)o)->gc_byte & UGC_HAS_SLOT_CHANGE_EVENT));

    /* Re-arm to -1 so urbi_vm_destroy doesn't fail. */
    spy.fail_at = -1;
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry point
 * =================================================================== */

void
test_slot_change_install_suite(void)
{
    printf("test_slot_change_install\n");
    utest_run("get_or_create_returns_same_event_for_same_name",
              get_or_create_returns_same_event_for_same_name);
    utest_run("get_or_create_distinct_events_per_name",
              get_or_create_distinct_events_per_name);
    utest_run("get_or_create_oom_returns_null_failsoft",
              get_or_create_oom_returns_null_failsoft);
}
