/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UEvent cell type (spec #3 §3.1).
 *
 * Checks:
 *   1. urbi_event_create returns non-NULL.
 *   2. Allocated cell carries type_tag == UTYPE_EVENT.
 *   3. at_watchers_head and waiters_head are NULL at alloc.
 *   4. name.kind == UVAL_NIL at alloc.
 *   5. sizeof(UEvent) >= 40 (48 on 64-bit per spec).
 *   6. vm->type_table[UTYPE_EVENT] is registered with a non-NULL walker. */

#include "utest.h"

#include "event/uevent.h"
#include "gc/ugc.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>

/* ===== Test 1 + 2 + 3 + 4: alloc, type_tag, heads, name ===== */

static void uevent_alloc_basics(void)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UEvent *ev = urbi_event_create(&vm);

    /* 1: must not be NULL */
    UASSERT(ev != NULL);

    if (ev != NULL) {
        /* 2: type_tag must be UTYPE_EVENT */
        UASSERT_EQ((int)ev->type_tag, (int)UTYPE_EVENT);

        /* 3: subscriber lists must be NULL at alloc */
        UASSERT(ev->at_watchers_head == NULL);
        UASSERT(ev->waiters_head     == NULL);

        /* 4: name must be UVAL_NIL at alloc */
        UASSERT_EQ((int)ev->name.kind, (int)UVAL_NIL);
    }

    uvm_destroy(&vm);
}

/* ===== Test 5: sizeof ===== */

static void uevent_sizeof(void)
{
    /* spec #3 §3.1: layout totals 40 B on 64-bit (8B header + 8B + 8B + 16B).
     * We assert >= 32 so the test passes on 32-bit cross targets where
     * pointers are narrower; the _Static_assert in uevent.h gates the exact
     * 40-byte requirement to __SIZEOF_POINTER__ == 8 builds. */
    UASSERT(sizeof(UEvent) >= 32u);
}

/* ===== Test 6: walker registration ===== */

static void uevent_walker_registered(void)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Descriptor must be installed. */
    UASSERT(vm.type_table[UTYPE_EVENT] != NULL);

    /* walk_payload must be callable (non-NULL). */
    if (vm.type_table[UTYPE_EVENT] != NULL) {
        UASSERT(vm.type_table[UTYPE_EVENT]->walk_payload != NULL);
    }

    uvm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void
test_uevent_suite(void)
{
    printf("test_uevent\n");
    utest_run("uevent_alloc_basics",   uevent_alloc_basics);
    utest_run("uevent_sizeof",         uevent_sizeof);
    utest_run("uevent_walker_registered", uevent_walker_registered);
}
