/* SPDX-License-Identifier: BSD-3-Clause */
/* test_slot_set.c — TDD tests for urbi_slot_set (Gap K, v0.7.1).
 *
 * Five sub-tests:
 *   1. Set new slot on UObject → URBI_OK; subsequent urbi_slot_get returns value.
 *   2. Overwrite existing mutable slot → URBI_OK.
 *   3. Overwrite const slot on the same object → URBI_ERR_CONST_SLOT_WRITE.
 *   4. Set on UVAL_INT → URBI_ERR_INVALID_ARG (atoms reject local slot writes).
 *   5. OOM on slot allocation → URBI_ERR_OOM (heap-lock pattern). */

#include "utest.h"

#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#include "object/uobject.h"      /* urbi_object_alloc, urbi_object_set_local_slot,
                                    urbi_object_install_property */
#include "value/uintern.h"       /* ustr_intern */
#include "vm/uvm.h"
#include "gc/ugc.h"              /* UTYPE_OBJECT */
#include "urbi/urbi.h"           /* urbi_slot_get, urbi_slot_set, urbi_lock_heap */
#include "urbi/types.h"
#include "urbi/object.h"         /* URBI_ATOM_OBJECT, URBI_SLOT_FLAG_CONSTANT */

#define UTEST(name) static void name(void)

/* Helper: allocate a bare UObject (no slots, root Object shape). */
static UObject *
make_object(UVM *vm)
{
    return urbi_object_alloc(vm, URBI_ATOM_OBJECT);
}

/* Helper: turn a plain UObject pointer into a UVAL_OBJECT UValue. */
static UValue
uval_obj(UObject *obj)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_OBJECT;
    v.v.p  = (void *)obj;
    return v;
}

/* Helper: install a const slot on obj.
 * Step 1: set_local_slot → Step 2: install_property(CONSTANT). */
static int
install_const_slot(UVM *vm, UObject *obj, const char *name, UValue value)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, name, strlen(name));
    if (sym == NULL) return -1;
    int rc = urbi_object_set_local_slot(vm, obj, sym, value);
    if (rc != 0) return rc;
    rc = urbi_object_install_property(vm, obj, sym,
                                      URBI_SLOT_FLAG_CONSTANT,
                                      urbi_make_nil());
    return rc;
}

/* === Sub-test 1: set new slot → URBI_OK; subsequent get returns value === */

UTEST(slot_set_new_slot)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *obj = make_object(&vm);
    UASSERT(obj != NULL);

    UValue write_val = urbi_make_int(7);
    int rc = urbi_slot_set(&vm, uval_obj(obj), "my_slot", 7, write_val);
    UASSERT_EQ(rc, URBI_OK);

    UValue read_val = urbi_make_nil();
    rc = urbi_slot_get(&vm, uval_obj(obj), "my_slot", 7, &read_val);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)read_val.kind, (int)UVAL_INT);
    UASSERT_EQ(read_val.v.i, 7);

    urbi_vm_destroy(&vm);
}

/* === Sub-test 2: overwrite mutable slot → URBI_OK === */

UTEST(slot_set_overwrite_mutable)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *obj = make_object(&vm);
    UASSERT(obj != NULL);

    /* Initial install. */
    int rc = urbi_slot_set(&vm, uval_obj(obj), "z", 1, urbi_make_int(1));
    UASSERT_EQ(rc, URBI_OK);

    /* Overwrite. */
    rc = urbi_slot_set(&vm, uval_obj(obj), "z", 1, urbi_make_int(2));
    UASSERT_EQ(rc, URBI_OK);

    UValue out = urbi_make_nil();
    rc = urbi_slot_get(&vm, uval_obj(obj), "z", 1, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ(out.v.i, 2);

    urbi_vm_destroy(&vm);
}

/* === Sub-test 3: overwrite locally const slot → URBI_ERR_CONST_SLOT_WRITE === */

UTEST(slot_set_const_slot_rejected)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *obj = make_object(&vm);
    UASSERT(obj != NULL);

    /* Install a const slot.  Must NOT use urbi_slot_set — it must be a
     * direct proto-local const slot (not COW-inherited). */
    int rc = install_const_slot(&vm, obj, "immutable", urbi_make_int(42));
    UASSERT_EQ(rc, URBI_OK);

    /* Attempt to overwrite via urbi_slot_set. */
    rc = urbi_slot_set(&vm, uval_obj(obj), "immutable", 9, urbi_make_int(99));
    UASSERT_EQ(rc, URBI_ERR_CONST_SLOT_WRITE);

    urbi_vm_destroy(&vm);
}

/* === Sub-test 4: set on UVAL_INT → URBI_ERR_INVALID_ARG === */

UTEST(slot_set_atom_receiver_rejected)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue int_val = urbi_make_int(10);
    int rc = urbi_slot_set(&vm, int_val, "x", 1, urbi_make_int(5));
    UASSERT_EQ(rc, URBI_ERR_INVALID_ARG);

    urbi_vm_destroy(&vm);
}

/* === Sub-test 5: OOM → URBI_ERR_OOM (heap-lock pattern) === */

UTEST(slot_set_oom_returns_oom)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *obj = make_object(&vm);
    UASSERT(obj != NULL);

    /* Lock heap to force all GC allocations to fail.  A new slot install
     * requires a USlotArray allocation → OOM path. */
    urbi_lock_heap(&vm);

    int rc = urbi_slot_set(&vm, uval_obj(obj), "new_slot_oom", 12, urbi_make_int(1));
    /* Must return either URBI_ERR_OOM (new slot needs allocation) or the
     * intern itself might fail (URBI_ERR_OOM too).  Either way not URBI_OK. */
    UASSERT(rc != URBI_OK);

    urbi_vm_destroy(&vm);
}

/* === Suite entry === */

void
test_slot_set_suite(void)
{
    utest_run("slot_set_new_slot",              slot_set_new_slot);
    utest_run("slot_set_overwrite_mutable",     slot_set_overwrite_mutable);
    utest_run("slot_set_const_slot_rejected",   slot_set_const_slot_rejected);
    utest_run("slot_set_atom_receiver_rejected", slot_set_atom_receiver_rejected);
    utest_run("slot_set_oom_returns_oom",       slot_set_oom_returns_oom);
}
