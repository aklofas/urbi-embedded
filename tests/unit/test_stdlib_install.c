/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_stdlib_install.c — urbi_install_native_methods / URBI_REGISTER_METHODS
 *
 * TDD: RED before the installer is defined in object_root.c; GREEN after.
 *
 * Case 1 — urbi_install_native_methods_installs_slots:
 *   Install a 2-entry UNativeMethodDef table on a fresh UObject; both slots
 *   must resolve to UVAL_CLOSURE via urbi_slot_get.
 *
 * Case 2 — urbi_register_methods_macro_callable_via_repl:
 *   Install a 3-entry table via URBI_REGISTER_METHODS (sizeof-based count
 *   macro); verify all 3 slots present; bind proto as realm global; assert
 *   urbi_repl_eval invocation returns URBI_OK. */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "urbi/object.h"
#include "object/uobject.h"
#include "stdlib/object_root.h"   /* UNativeMethodDef, urbi_install_native_methods,
                                     URBI_REGISTER_METHODS */
#include "vm/uvm.h"

#include <stddef.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Minimal probe functions — each returns UEXEC_OK with a nil output. */
static int probe_a(struct UVM *vm, UValue self, UValue *args,
                   uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_make_nil();
    return 0; /* UEXEC_OK */
}

static int probe_b(struct UVM *vm, UValue self, UValue *args,
                   uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_make_nil();
    return 0;
}

static int probe_c(struct UVM *vm, UValue self, UValue *args,
                   uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_make_nil();
    return 0;
}

/* --- Case 1: 2-entry table, both slots become UVAL_CLOSURE -------------- */

UTEST(urbi_install_native_methods_installs_slots)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    vm.gc_stress_armed = 0;   /* primitive-semantics suite; not GC-stress exercised */

    UObject *proto = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(proto != NULL);

    static const UNativeMethodDef TABLE[2] = {
        { "probeA", probe_a },
        { "probeB", probe_b }
    };

    int rc = urbi_install_native_methods(&vm, proto, TABLE, 2);
    UASSERT_EQ(rc, URBI_OK);

    UValue obj_v = urbi_make_object(proto);

    UValue slot_a = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(&vm, obj_v, "probeA", 6, &slot_a), URBI_OK);
    UASSERT_EQ((int)slot_a.kind, (int)UVAL_CLOSURE);

    UValue slot_b = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(&vm, obj_v, "probeB", 6, &slot_b), URBI_OK);
    UASSERT_EQ((int)slot_b.kind, (int)UVAL_CLOSURE);

    urbi_vm_destroy(&vm);
}

/* --- Case 2: URBI_REGISTER_METHODS macro, 3-entry table, repl callable -- */

UTEST(urbi_register_methods_macro_callable_via_repl)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    vm.gc_stress_armed = 0;

    UObject *proto = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(proto != NULL);

    static const UNativeMethodDef TABLE[3] = {
        { "probeA", probe_a },
        { "probeB", probe_b },
        { "probeC", probe_c }
    };

    int rc = URBI_REGISTER_METHODS(&vm, proto, TABLE);
    UASSERT_EQ(rc, URBI_OK);

    /* All three slots present as UVAL_CLOSURE. */
    UValue obj_v = urbi_make_object(proto);

    UValue slot_a = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(&vm, obj_v, "probeA", 6, &slot_a), URBI_OK);
    UASSERT_EQ((int)slot_a.kind, (int)UVAL_CLOSURE);

    UValue slot_b = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(&vm, obj_v, "probeB", 6, &slot_b), URBI_OK);
    UASSERT_EQ((int)slot_b.kind, (int)UVAL_CLOSURE);

    UValue slot_c = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(&vm, obj_v, "probeC", 6, &slot_c), URBI_OK);
    UASSERT_EQ((int)slot_c.kind, (int)UVAL_CLOSURE);

    /* Bind the proto as a realm global so script can reach it. */
    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);
    rc = urbi_realm_set_global(&vm, realm, "SITestProto", 11, obj_v);
    UASSERT_EQ(rc, URBI_OK);

    /* Invoke one method via urbi_repl_eval: must not fatal. */
    char buf[256];
    buf[0] = '\0';
    rc = urbi_repl_eval(&vm, NULL, "SITestProto.probeA()", 20, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);

    urbi_vm_destroy(&vm);
}

/* --- Suite registration -------------------------------------------------- */

void test_stdlib_install_suite(void)
{
    utest_run("urbi_install_native_methods_installs_slots",
              urbi_install_native_methods_installs_slots);
    utest_run("urbi_register_methods_macro_callable_via_repl",
              urbi_register_methods_macro_callable_via_repl);
}
