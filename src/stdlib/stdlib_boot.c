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
 *      vm->stdlib_module + bind a per-VM UChunkInstance
 *
 * Step 3 only runs when urbi_stdlib_bytecode_len > 0.  At Phase 4
 * baseline the blob is empty (STDLIB_ORDER.txt empty), so this branch
 * is dead code that becomes live in Phase 10 when the order file is
 * populated.  Parser-independent: the blob is bytecode, not source —
 * verified by the URBI_BYTECODE_ONLY=1 build (T15 in v0.7.0-c-api)
 * which strips src/lex/, src/parse/, src/emit/ entirely.
 *
 * The deserialized UModule lives on vm->stdlib_module, freed at
 * urbi_vm_destroy (see src/vm/uvm_init.c).  Idempotent:
 * vm->stdlib_booted gates re-entry. */

#include "stdlib/stdlib_boot.h"
#include "stdlib/object_root.h"
#include "stdlib/atom_protos.h"
#include "stdlib/atoms.h"
#include "stdlib/containers.h"
#include "stdlib/runtime_types.h"
#include "stdlib/namespaces.h"
#include "stdlib/primitives.h"
#include "stdlib/lobby_native.h"
#ifdef URBI_ENABLE_REPL
#  include "stdlib/debug_namespace.h"
#endif

#include "urbi/urbi.h"               /* URBI_OK, URBI_ERR_* */
#include "chunk/uchunk.h"          /* UModule, umodule_deserialize, umodule_destroy */
#include "object/uchunk_instance.h" /* urbi_get_or_create_module_instance */
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

    /* Phase 5 (atom-proto Tier 1 methods).  Installs C-native methods on
     * Boolean / Integer / Float / String atom protos.  Symbolic operators
     * (`+`, `==`, …) remain inline VM opcodes; only named methods (asString,
     * bitand, sqrt, length, …) land here.  See src/stdlib/atoms.c banner. */
    rc = urbi_stdlib_register_atom_methods(vm);
    if (rc != URBI_OK) return rc;

    /* Phase 6 (M6 Wave 2): C-native containers.  Installs methods on the
     * existing URBI_ATOM_LIST / URBI_ATOM_DICT atom protos (the
     * realm-populate registry already publishes these as "List" / "Dict"
     * globals).  Pair / Triplet / Tuple realm-global registration is
     * deferred to a post-loop hook in urbi_populate_realm_globals so the
     * registry's slot 0..7 layout (Object .. List) stays stable for the
     * v1.0 packed-flag CONSTANT enforcement range.  See
     * src/stdlib/containers.c. */
    rc = urbi_stdlib_register_containers(vm);
    if (rc != URBI_OK) return rc;

    /* Phase 7 (M6 Wave 2): runtime-type protos.  Allocates
     * vm->exception_proto and installs Exception.new / Exception.raise.
     * Realm-global binding for "Exception" is deferred to the post-loop
     * hook urbi_stdlib_register_runtime_globals, mirroring container
     * globals so the registry's slot 0..7 layout stays stable.  See
     * src/stdlib/runtime_types.c. */
    rc = urbi_stdlib_register_runtime_types(vm);
    if (rc != URBI_OK) return rc;

    /* Phase 8 (M6 Wave 2): namespace protos.  Allocates Math / System /
     * System.Platform / Global / CallMessage proto UObjects with their
     * constants + native methods.  Realm-global binding for the
     * namespace names is deferred to urbi_stdlib_register_namespace_-
     * globals, again preserving the registry's slot 0..7 layout.  See
     * src/stdlib/namespaces.c. */
    rc = urbi_stdlib_register_namespaces(vm);
    if (rc != URBI_OK) return rc;

    /* Phase 9 (M6 Wave 2): primitive protos.  Allocates Mutex / Date /
     * Duration proto UObjects with their native methods.  Realm-global
     * binding for the primitive names is deferred to urbi_stdlib_-
     * register_primitives_globals, again preserving the registry's
     * slot 0..7 layout.  See src/stdlib/primitives.c. */
    rc = urbi_stdlib_register_primitives(vm);
    if (rc != URBI_OK) return rc;

    /* v0.9.1 Phase 5: Lobby proto + __builtin_lobby_send native primitive.
     * Allocates vm->lobby_proto chained on root Object and installs the
     * single native method.  The `lobbies` slot, `echo` / `wall` /
     * `handleDisconnect` methods, and the `onDisconnect` Event are added
     * by the lobby.u overlay during the post-loop bake-blob run.  Realm-
     * global binding for "Lobby" is deferred to urbi_lobby_native_register_-
     * globals (called by urbi_populate_realm_globals after the registry
     * loop).  Default-build (not REPL-gated): Lobby is part of the spec
     * §3.6 readonly cohort, and urbi_vm_write_in_realm — the routing path
     * — is also default-build. */
    rc = urbi_lobby_native_register(vm);
    if (rc != URBI_OK) return rc;

#ifdef URBI_ENABLE_REPL
    /* v0.9.1 Task 22: Debug namespace.  Allocates the singleton proto +
     * binds 9 native-method slots.  The realm-global "Debug" binding is
     * deferred to urbi_debug_namespace_register_globals (called from
     * urbi_populate_realm_globals AFTER the mark_readonly pass).  The
     * proto itself is NOT marked readonly — symmetry with Global, the
     * other reflective namespace. */
    rc = urbi_debug_namespace_register(vm);
    if (rc != URBI_OK) return rc;
#endif

    /* M6 Phase 4 (Wave 2): deserialize the baked stdlib bytecode blob
     * and bind a per-VM UChunkInstance.  Empty blob (Phase 4 baseline)
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
        /* Freestanding: umodule_deserialize requires module->alloc_fn for
         * the internal proto + buffer allocations.  Hosted builds fall
         * back to stdlib_alloc inside module_allocator(); freestanding
         * does not and returns UCHUNK_LOAD_OOM if alloc_fn is NULL.  Inherit
         * the VM's allocator so the stdlib module shares the VM heap. */
        m->alloc_fn = vm->alloc_fn;
        m->alloc_ud = vm->alloc_ud;
        UChunkLoadError lerr = umodule_deserialize(
            m, urbi_stdlib_bytecode, urbi_stdlib_bytecode_len, NULL, 0);
        if (lerr != UCHUNK_LOAD_OK) {
            umodule_destroy(m, vm);
            vm->alloc_fn(m, 0, vm->alloc_ud);
            return URBI_ERR_STDLIB_BOOT_FAILED;
        }
        if (urbi_get_or_create_module_instance(vm, m) == NULL) {
            umodule_destroy(m, vm);
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

    /* v0.9.1 Task 4: mark every builtin atom + runtime-type proto readonly
     * AFTER all population phases (1-9) so the method-install passes are not
     * blocked by their own readonly bits.  Spec §4.2.  The Global namespace
     * proto (vm->global_namespace_proto) is left mutable by design. */
    {
        int rc_ro = urbi_atom_protos_mark_readonly(vm);
        if (rc_ro != URBI_OK) return rc_ro;
    }

    vm->stdlib_booted = 1U;
    return URBI_OK;
}
