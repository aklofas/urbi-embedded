/* SPDX-License-Identifier: BSD-3-Clause */
/* primitives.c — M6 Phase 9: C-native primitives (Mutex, Date, Duration).
 *
 * Mutex / Date / Duration — see banner in primitives.h.  Phase 9 grows
 * incrementally task-by-task: T93 shell, T94 Mutex, T95 Date, T96
 * Duration, T97 Date.plus(Duration) seam.
 *
 * Allocation pattern mirrors namespaces.c / runtime_types.c: a vanilla
 * URBI_ATOM_OBJECT-family UObject per primitive proto, methods installed
 * via a per-primitive method table walked by install_methods.  GC
 * reachability comes from object_roots_walker (uobject.c) which shades
 * each vm->*_proto field during MARK_ROOTS. */

#include "stdlib/primitives.h"

#include "module/umodule.h"            /* UValue / UVAL_* */
#include "object/uobject.h"            /* urbi_object_alloc */
#include "realm/urealm.h"              /* URealm */
#include "urbi/object.h"               /* URBI_ATOM_OBJECT */
#include "urbi/types.h"                /* urbi_value_nil */
#include "urbi/urbi.h"                 /* URBI_OK / URBI_ERR_* / urbi_realm_set_global */
#include "vm/uvm.h"                    /* UVM */

#include <stdint.h>
#include <stddef.h>

/* === UValue construction helpers ========================================= */

static UValue
val_obj(UObject *o)
{
    UValue v = urbi_value_nil();
    v.kind = (uint8_t)UVAL_OBJECT;
    v.v.p  = o;
    return v;
}

/* === urbi_stdlib_register_primitives ====================================
 *
 * Allocates Mutex / Date / Duration proto UObjects per task.  T93 ships
 * the shell; subsequent T94/T95/T96 tasks add their native methods. */

int
urbi_stdlib_register_primitives(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    /* T93: shell only — proto allocation lands per-task with the methods. */
    return URBI_OK;
}

/* === urbi_stdlib_register_primitives_globals ============================
 *
 * Post-registry hook: bind primitives as realm globals on `realm`.  Lands
 * at slots 15+, past the v1.0 packed-flag CONSTANT enforcement range
 * (slots 0..7).  Mirrors urbi_stdlib_register_namespace_globals. */

int
urbi_stdlib_register_primitives_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;

    int rc;
    if (vm->mutex_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "Mutex", 5,
                                   val_obj(vm->mutex_proto));
        if (rc != URBI_OK) return rc;
    }
    if (vm->date_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "Date", 4,
                                   val_obj(vm->date_proto));
        if (rc != URBI_OK) return rc;
    }
    if (vm->duration_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "Duration", 8,
                                   val_obj(vm->duration_proto));
        if (rc != URBI_OK) return rc;
    }
    return URBI_OK;
}
