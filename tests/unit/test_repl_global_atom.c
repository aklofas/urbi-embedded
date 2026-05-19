/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_global_atom.c — v0.9.1 Global mutable atom proto.
 *
 * Spec §4.1: Global is the designated mutable cross-session shared atom.
 * It already existed as vm->global_namespace_proto since M6 Phase 8 (with
 * a `length` method).  v0.9.1 confirms two properties:
 *
 *  1. Global is exposed as a realm global.
 *  2. Global accepts script-side slot installs (NOT marked readonly by
 *     urbi_atom_protos_mark_readonly).
 *  3. Two REPL realms backed by the same VM see each other's writes to
 *     Global slots — the proto is a single VM-singleton, not a per-realm
 *     copy.
 *
 * The plan's separate Global UProto allocation is unnecessary: M6 Phase 8
 * already created vm->global_namespace_proto.  We adopt it as the spec's
 * Global mutable atom.
 */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "urbi/object.h"
#include "object/uobject.h"
#include "realm/urealm.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ---- T1: Global is bound as a realm global and is a UObject ----------- */
UTEST(global_proto_bound_in_realm_globals)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_global(&vm);
    UValue v;
    int rc = urbi_realm_get_global(&vm, r, "Global", 6, &v);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ(v.kind, (uint8_t)UVAL_OBJECT);

    UObject *p = (UObject *)v.v.p;
    UASSERT(p != NULL);

    urbi_vm_destroy(&vm);
}

/* ---- T2: Global slot install + read-back inside a single realm -------- */
UTEST(global_proto_mutable_slot_roundtrip)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_create_repl(&vm);
    UASSERT(r != NULL);

    char buf[256];
    int rc;

    /* Set Global.x = 42 — must succeed (no readonly bit). */
    buf[0] = '\0';
    rc = urbi_repl_eval(&vm, r, "Global.x = 42", 13, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);

    /* Read back. */
    buf[0] = '\0';
    rc = urbi_repl_eval(&vm, r, "Global.x", 8, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(strcmp(buf, "42") == 0);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- T3: Global is the same UObject across realms --------------------- */
UTEST(global_proto_shared_across_realms)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *a = urbi_realm_create_repl(&vm);
    URealm *b = urbi_realm_create_repl(&vm);
    UASSERT(a != NULL && b != NULL);

    UValue va, vb;
    UASSERT_EQ(urbi_realm_get_global(&vm, a, "Global", 6, &va), URBI_OK);
    UASSERT_EQ(urbi_realm_get_global(&vm, b, "Global", 6, &vb), URBI_OK);

    /* Both realms see the same proto pointer. */
    UASSERT(va.v.p == vb.v.p);

    urbi_realm_destroy(&vm, a);
    urbi_realm_destroy(&vm, b);
    urbi_vm_destroy(&vm);
}

/* ---- T4: write in realm A is visible from realm B --------------------- */
UTEST(global_atom_cross_realm_visible)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *a = urbi_realm_create_repl(&vm);
    URealm *b = urbi_realm_create_repl(&vm);
    UASSERT(a != NULL && b != NULL);

    char buf[256];
    buf[0] = '\0';
    int rc = urbi_repl_eval(&vm, a,
                            "Global.shared = 99", 18, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);

    buf[0] = '\0';
    rc = urbi_repl_eval(&vm, b, "Global.shared", 13, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(strcmp(buf, "99") == 0);

    urbi_realm_destroy(&vm, a);
    urbi_realm_destroy(&vm, b);
    urbi_vm_destroy(&vm);
}

/* ---- suite entry ------------------------------------------------------ */
void
test_repl_global_atom_suite(void)
{
    printf("test_repl_global_atom\n");
    utest_run("global_proto_bound_in_realm_globals",   global_proto_bound_in_realm_globals);
    utest_run("global_proto_mutable_slot_roundtrip",   global_proto_mutable_slot_roundtrip);
    utest_run("global_proto_shared_across_realms",     global_proto_shared_across_realms);
    utest_run("global_atom_cross_realm_visible",       global_atom_cross_realm_visible);
}
