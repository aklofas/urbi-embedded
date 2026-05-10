/* SPDX-License-Identifier: BSD-3-Clause */
/* namespaces.c — M6 Phase 8: C-native namespace globals.
 *
 * Math / System / System.Platform / Global / CallMessage — see banner in
 * namespaces.h.  Phase 8 grows incrementally task-by-task: T85 shell,
 * T86 Math constants, T87 System primitives, T88 System.Platform.kind,
 * T90 Global.length, T91 CallMessage stub.
 *
 * Allocation pattern mirrors runtime_types.c (Exception primitive proto):
 * a vanilla URBI_ATOM_OBJECT-family UObject per namespace, methods
 * installed via a per-namespace method table walked by install_methods.
 * GC reachability comes from object_roots_walker (uobject.c) which
 * shades each vm->*_proto field during MARK_ROOTS. */

#include "stdlib/namespaces.h"

#include "module/umodule.h"            /* UValue / UVAL_* */
#include "object/uobject.h"            /* urbi_object_alloc + set_local_slot */
#include "realm/urealm.h"              /* URealm */
#include "runtime/umacros.h"           /* urbi_strlen */
#include "urbi/object.h"               /* URBI_ATOM_OBJECT */
#include "urbi/types.h"                /* urbi_value_nil */
#include "urbi/urbi.h"                 /* URBI_OK / URBI_ERR_* / urbi_realm_set_global */
#include "value/uintern.h"             /* ustr_intern + USymbol */
#include "vm/uvm.h"                    /* UVM */

#include <stdint.h>
#include <stddef.h>

/* === UValue construction helpers ========================================= */

static UValue
val_float(double d)
{
    UValue v = urbi_value_nil();
    v.kind = (uint8_t)UVAL_FLOAT;
    v.v.f  = d;
    return v;
}

static UValue
val_obj(UObject *o)
{
    UValue v = urbi_value_nil();
    v.kind = (uint8_t)UVAL_OBJECT;
    v.v.p  = o;
    return v;
}

/* Install a constant slot (UValue) on proto, looking up the symbol via
 * ustr_intern.  Returns URBI_OK / URBI_ERR_OOM. */
static int
install_const_slot(UVM *vm, UObject *proto, const char *name, UValue value)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, name, urbi_strlen(name));
    if (sym == NULL) return URBI_ERR_OOM;
    if (urbi_object_set_local_slot(vm, proto, sym, value) != 0)
        return URBI_ERR_OOM;
    return URBI_OK;
}

/* === urbi_stdlib_register_namespaces ====================================
 *
 * Allocates Math / System / Global / CallMessage proto UObjects per task.
 * T86: Math with pi / e / nan / infinity constants.  GC reachability via
 * object_roots_walker shading vm->math_proto.
 *
 * Idempotent: re-allocates each proto only when its vm field is NULL. */

int
urbi_stdlib_register_namespaces(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    int rc;

    /* --- T86 Math: pi / e / nan / infinity --- */
    if (vm->math_proto == NULL) {
        UObject *m = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
        if (m == NULL) return URBI_ERR_OOM;
        vm->math_proto = m;
    }
    rc = install_const_slot(vm, vm->math_proto, "pi",       val_float(3.141592653589793));
    if (rc != URBI_OK) return rc;
    rc = install_const_slot(vm, vm->math_proto, "e",        val_float(2.718281828459045));
    if (rc != URBI_OK) return rc;
    /* IEEE-754 NaN / +Inf via 0.0/0.0 / 1.0/0.0.  Compilers fold these at
     * compile time per IEEE arithmetic; if a target's compiler refuses,
     * switch to (double)NAN / (double)INFINITY from <math.h>. */
    rc = install_const_slot(vm, vm->math_proto, "nan",      val_float(0.0 / 0.0));
    if (rc != URBI_OK) return rc;
    rc = install_const_slot(vm, vm->math_proto, "infinity", val_float(1.0 / 0.0));
    if (rc != URBI_OK) return rc;

    return URBI_OK;
}

/* === urbi_stdlib_register_namespace_globals =============================
 *
 * Post-registry hook: bind namespaces as realm globals on `realm`.  Lands
 * at slots 15+, past the v1.0 packed-flag CONSTANT enforcement range
 * (slots 0..7).  Mirrors urbi_stdlib_register_runtime_globals. */

int
urbi_stdlib_register_namespace_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;

    int rc;
    if (vm->math_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "Math", 4, val_obj(vm->math_proto));
        if (rc != URBI_OK) return rc;
    }
    return URBI_OK;
}
