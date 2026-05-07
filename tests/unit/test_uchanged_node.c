/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UChangedNode cell type (spec #4 §3.1).
 *
 * Checks:
 *   1. UObject.changed_events_head is NULL at create (urbi_object_alloc).
 *   2. sizeof(UObject) >= 56 (48 B M4 baseline + 8 B new field on 64-bit).
 *   3. sizeof(UChangedNode) is 32 on 64-bit / 16 on 32-bit targets. */

#include "utest.h"

#include "object/uobject.h"   /* UObject, urbi_object_alloc */
#include "changed/uchanged_node.h"    /* UChangedNode, UTYPE_CHANGED_NODE */
#include "gc/ugc.h"           /* UTYPE_CHANGED_NODE */
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>

/* ===== Test 1: changed_events_head is NULL at alloc ===== */

static void uchanged_node_head_null_at_create(void)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);

    if (o != NULL) {
        /* spec #4 §3.1: lazy-alloc; must be NULL until first `obj.x.changed?` */
        UASSERT(o->changed_events_head == NULL);
    }

    urbi_vm_destroy(&vm);
}

/* ===== Test 2: sizeof(UObject) >= 56 ===== */

static void uchanged_node_uobject_size(void)
{
    /* UObject grew 48 → 56 B at M5 spec #4 §3.1 on 64-bit.
     * Assert >= 56 so the test also passes on 32-bit cross targets where
     * the struct is smaller; the _Static_assert in uobject.h gates the exact
     * 56-byte requirement to __SIZEOF_POINTER__ == 8 builds. */
    UASSERT(sizeof(UObject) >= 56U);
}

/* ===== Test 3: sizeof(UChangedNode) is 32 host / 16 32-bit ===== */

static void uchanged_node_sizeof(void)
{
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
    /* 64-bit host: 8B header + 8B name + 8B event + 8B next = 32 B */
    UASSERT_EQ((int)sizeof(UChangedNode), 32);
#else
    /* 32-bit cross target: 4B header + 4B name + 4B event + 4B next = 16 B */
    UASSERT_EQ((int)sizeof(UChangedNode), 16);
#endif
}

/* ===== Suite entry point ===== */

void
test_uchanged_node_suite(void)
{
    printf("test_uchanged_node\n");
    utest_run("uchanged_node: head null at create",  uchanged_node_head_null_at_create);
    utest_run("uchanged_node: uobject size >= 56",   uchanged_node_uobject_size);
    utest_run("uchanged_node: sizeof correct",        uchanged_node_sizeof);
}
