/* SPDX-License-Identifier: BSD-3-Clause */
/* URBI_GC_STRESS disarm (v0.13.2): C-API scaffolding suite — GC cells
 * (tags/events/objects/closures) held in bare C locals and/or synthetic
 * strands outside the realm graph, by design, to drive one primitive in
 * isolation.  Collect-on-every-alloc sweeps them between paired
 * allocations; fine on normal builds where host-C call sequences cannot
 * be interrupted by a collection.  Each test sets vm.gc_stress_armed = 0
 * after init.  Structural-by-design, not a runtime rooting bug
 * (refactor-3 TEST-GAP-01 stress-exempt list). */
/* test_tag_create.c — TDD tests for urbi_tag_create (Gap M, v0.7.1).
 *
 * Four sub-tests:
 *   1. Returns non-NULL with valid args; state is URBI_TAG_RUNNING.
 *   2. Tag is parented under realm (urbi_tag_info reports has_parent=true).
 *   3. NULL on OOM (heap-lock pattern).
 *   4. Name is interned (two creates with same name produce different UTag*
 *      but same name string pointer when looked up from tag->name). */

#include "utest.h"

#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#include "tag/utag.h"           /* UTag, UTAG_FLAG_* */
#include "realm/urealm.h"       /* URealm */
#include "vm/uvm.h"
#include "urbi/urbi.h"
#include "urbi/types.h"

#define UTEST(name) static void name(void)

/* === Sub-test 1: returns non-NULL; state is URBI_TAG_RUNNING === */

UTEST(tag_create_returns_nonnull_in_running_state)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */
    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UTag *tag = urbi_tag_create(&vm, realm, "test", 4);
    UASSERT(tag != NULL);

    if (tag != NULL) {
        urbi_tag_info_t info;
        int rc = urbi_tag_info(&vm, tag, &info);
        UASSERT_EQ(rc, URBI_OK);
        UASSERT_EQ((int)info.state, (int)URBI_TAG_RUNNING);
    }

    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* === Sub-test 2: tag is parented under realm (has_parent = true) === */

UTEST(tag_create_has_parent_under_realm)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */
    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UTag *tag = urbi_tag_create(&vm, realm, "child", 5);
    UASSERT(tag != NULL);

    if (tag != NULL) {
        urbi_tag_info_t info;
        int rc = urbi_tag_info(&vm, tag, &info);
        UASSERT_EQ(rc, URBI_OK);
        UASSERT(info.has_parent);
        /* The parent must be the realm's root tag. */
        UASSERT(tag->parent == realm->tag);
    }

    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* === Sub-test 3: NULL on OOM (heap-lock) === */

UTEST(tag_create_null_on_oom)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */
    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Lock heap so GC allocations fail.  utag_create needs urbi_gc_alloc. */
    urbi_lock_heap(&vm);

    UTag *tag = urbi_tag_create(&vm, realm, "oom", 3);
    UASSERT(tag == NULL);

    /* Unlock (restore working allocator) before destroy to avoid leak. */
    vm.heap_locked = 0;

    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* === Sub-test 4: name is interned (same name pointer from two tags) === */

UTEST(tag_create_name_is_interned)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */
    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UTag *tag1 = urbi_tag_create(&vm, realm, "shared_name", 11);
    UTag *tag2 = urbi_tag_create(&vm, realm, "shared_name", 11);
    UASSERT(tag1 != NULL);
    UASSERT(tag2 != NULL);

    if (tag1 != NULL && tag2 != NULL) {
        /* Two different UTag* (different allocations). */
        UASSERT(tag1 != tag2);
        /* But same interned name pointer (pointer equality for byte-equal strings). */
        UASSERT_EQ((int)tag1->name.kind, (int)UVAL_STR);
        UASSERT_EQ((int)tag2->name.kind, (int)UVAL_STR);
        UASSERT(tag1->name.v.p == tag2->name.v.p);
    }

    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* === Suite entry === */

void
test_tag_create_suite(void)
{
    utest_run("tag_create_returns_nonnull_in_running_state", tag_create_returns_nonnull_in_running_state);
    utest_run("tag_create_has_parent_under_realm",           tag_create_has_parent_under_realm);
    utest_run("tag_create_null_on_oom",                      tag_create_null_on_oom);
    utest_run("tag_create_name_is_interned",                 tag_create_name_is_interned);
}
