/* SPDX-License-Identifier: BSD-3-Clause */
/* stdlib_boot.c — M6 Phase 3/4 stdlib bootstrap.
 *
 * Wave 1 minimum: register the nine Object root C-native methods on
 * vm->atom_object.  Wave 2 grows this to load atom proto methods +
 * container internals + .u overlay blob.
 *
 * Boot order (M6 Phase 4 / Wave 2):
 *   1. C-native Object root methods (urbi_object_root_register)
 *   2. C-native atom proto stubs (urbi_atom_protos_register)
 *   3. Deserialize the baked .u stdlib bytecode blob into
 *      vm->stdlib_module + bind a per-VM UModuleInstance
 *
 * Step 3 only runs when urbi_stdlib_bytecode_len > 0.  At Phase 4
 * baseline the blob is empty (STDLIB_ORDER.txt empty), so this branch
 * is dead code that becomes live in Phase 10 when the order file is
 * populated.  Parser-independent: the blob is bytecode, not source —
 * verified by Phase 13's URBI_BYTECODE_ONLY smoke build.
 *
 * The deserialized UModule lives on vm->stdlib_module, freed at
 * urbi_vm_destroy (see src/vm/uvm_init.c).  Idempotent:
 * vm->stdlib_booted gates re-entry. */

#include "stdlib/stdlib_boot.h"
#include "stdlib/object_root.h"
#include "stdlib/atom_protos.h"

#include "urbi/urbi.h"               /* URBI_OK, URBI_ERR_* */
#include "module/umodule.h"          /* UModule, umodule_deserialize, umodule_destroy */
#include "object/umodule_instance.h" /* urbi_get_or_create_module_instance */
#include "runtime/umacros.h"         /* urbi_zero */
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

    /* M6 Phase 4 (Wave 2): deserialize the baked stdlib bytecode blob
     * and bind a per-VM UModuleInstance.  Empty blob (Phase 4 baseline)
     * skips this entirely. */
    if (urbi_stdlib_bytecode_len > 0) {
        if (vm->alloc_fn == NULL) {
            return URBI_ERR_STDLIB_BOOT_FAILED;
        }
        UModule *m = vm->alloc_fn(NULL, sizeof(UModule), vm->alloc_ud);
        if (m == NULL) return URBI_ERR_OOM;
        /* Zero-init: umodule_destroy on a zero UModule is safe (header
         * §470).  urbi_zero used (not memset) per freestanding
         * discipline. */
        urbi_zero(m, sizeof(UModule));
        UModuleLoadError lerr = umodule_deserialize(
            m, urbi_stdlib_bytecode, urbi_stdlib_bytecode_len, NULL, 0);
        if (lerr != ULOAD_OK) {
            umodule_destroy(m);
            vm->alloc_fn(m, 0, vm->alloc_ud);
            return URBI_ERR_STDLIB_BOOT_FAILED;
        }
        if (urbi_get_or_create_module_instance(vm, m) == NULL) {
            umodule_destroy(m);
            vm->alloc_fn(m, 0, vm->alloc_ud);
            return URBI_ERR_OOM;
        }
        vm->stdlib_module = m;
        /* Note: running the root chunk of the stdlib module is deferred
         * to a later phase — urbi_stdlib_boot is invoked from inside
         * urbi_populate_realm_globals during realm creation, and
         * urbi_run_chunk would re-enter the realm-create path.  Phase
         * 10 will arrange the run via a deferred-execution hook once
         * the global Realm is fully populated. */
    }

    vm->stdlib_booted = 1U;
    return URBI_OK;
}
