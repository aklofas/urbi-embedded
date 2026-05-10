/* SPDX-License-Identifier: BSD-3-Clause */
/* stdlib_boot.c — M6 Phase 3 stdlib bootstrap.
 *
 * Wave 1 minimum: register the nine Object root C-native methods on
 * vm->atom_object.  Wave 2 grows this to load atom proto methods +
 * container internals + .u overlay blob.
 *
 * Idempotent: vm->stdlib_booted gates re-entry. */

#include "stdlib/stdlib_boot.h"
#include "stdlib/object_root.h"
#include "stdlib/atom_protos.h"

#include "urbi/urbi.h"   /* URBI_OK, URBI_ERR_* */
#include "vm/uvm.h"

int
urbi_stdlib_boot(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->stdlib_booted) return URBI_OK;

    int rc = urbi_object_root_register(vm);
    if (rc != URBI_OK) return rc;

    /* Phase 4 (atom proto stubs).  Allocates Boolean / Nil / Void
     * singletons + installs Wave-1 family-specific methods (Boolean
     * .toString, String.length).  Integer / Float / Nil / Void protos
     * exist but inherit clone + getSlot/etc. from Object root via the
     * prototype chain. */
    rc = urbi_atom_protos_register(vm);
    if (rc != URBI_OK) return rc;

    /* Phase 7 (Event.new / Tag.new scripted constructors) hooks in here. */

    vm->stdlib_booted = 1U;
    return URBI_OK;
}
