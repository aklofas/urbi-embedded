/* SPDX-License-Identifier: BSD-3-Clause */
/* stdlib_boot.c — stdlib bootstrap.
 *
 * Registers the nine Object root C-native methods on vm->atom_object,
 * then loads atom proto methods + container internals + .u overlay blob.
 *
 * Boot order:
 *   1. C-native Object root methods (urbi_object_root_register)
 *   2. C-native atom proto stubs (urbi_atom_protos_register)
 *   3. Deserialize the baked .u stdlib bytecode blob into
 *      vm->stdlib_module + bind a per-VM UChunkInstance
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
#include "stdlib/regexp.h"
#include "stdlib/isa_method.h"
#include "stdlib/job_proto.h"
#include "stdlib/lobby_native.h"
#include "stdlib/temporal.h"
#ifdef URBI_ENABLE_REPL
#  include "stdlib/debug_namespace.h"
#endif
#include "urbi/ros.h"   /* urbi_ros_register — self-gated by URBI_ENABLE_ROS2 */
#include "urbi/urobotics.h"   /* urbi_urobotics_register — self-gated by URBI_ENABLE_UROBOTICS */

#include "urbi/urbi.h"               /* URBI_OK, URBI_ERR_* */
#include "chunk/uchunk.h"          /* UModule, uchunk_deserialize, uchunk_destroy */
#include "object/uchunk_instance.h" /* urbi_get_or_create_chunk_instance */
#include "runtime/umacros.h"         /* urbi_zero */
#include "vm/uvm.h"

static int
stdlib_boot_impl(UVM *vm)
{
    int rc = urbi_object_root_register(vm);
    if (rc != URBI_OK) return rc;

    rc = urbi_atom_protos_register(vm);
    if (rc != URBI_OK) return rc;

    rc = urbi_stdlib_register_atom_methods(vm);
    if (rc != URBI_OK) return rc;

    rc = urbi_stdlib_register_containers(vm);
    if (rc != URBI_OK) return rc;

    rc = urbi_stdlib_register_runtime_types(vm);
    if (rc != URBI_OK) return rc;

    rc = urbi_stdlib_register_namespaces(vm);
    if (rc != URBI_OK) return rc;

    rc = urbi_stdlib_register_primitives(vm);
    if (rc != URBI_OK) return rc;

    /* v1.0 stdlib-completeness: RegExp proto.  Allocates vm->regexp_proto
     * with its native methods (new / test / match) and the compact
     * backtracking matcher.  Realm-global binding for "RegExp" is deferred
     * to urbi_stdlib_register_regexp_globals (post-registry loop), same
     * pattern as the primitive protos.  See src/stdlib/regexp.c. */
    rc = urbi_stdlib_register_regexp(vm);
    if (rc != URBI_OK) return rc;

    rc = urbi_lobby_native_register(vm);
    if (rc != URBI_OK) return rc;

    rc = urbi_job_proto_register(vm);
    if (rc != URBI_OK) return rc;

    rc = urbi_temporal_native_register(vm);
    if (rc != URBI_OK) return rc;

#ifdef URBI_ENABLE_REPL
    rc = urbi_debug_namespace_register(vm);
    if (rc != URBI_OK) return rc;
#endif

#ifdef URBI_ENABLE_ROS2
    {
        int rc_ros = urbi_ros_register(vm);
        if (rc_ros != URBI_OK) return rc_ros;
    }
#endif

    if (urbi_stdlib_bytecode_len > 0) {
        if (vm->alloc_fn == NULL) {
            return URBI_ERR_STDLIB_BOOT_FAILED;
        }
        UProto *m = NULL;
        UChunkLoadError lerr = uchunk_deserialize(
            &m, urbi_stdlib_bytecode, urbi_stdlib_bytecode_len,
            vm->alloc_fn, vm->alloc_ud, NULL, 0);
        if (lerr != UCHUNK_LOAD_OK) {
            /* uchunk_deserialize cleaned up partial allocations on failure. */
            return URBI_ERR_STDLIB_BOOT_FAILED;
        }
        if (urbi_get_or_create_chunk_instance(vm, m) == NULL) {
            uchunk_destroy(m, vm);
            return URBI_ERR_OOM;
        }
        m->vm_owned = true;   /* GC-18: freed by urbi_vm_destroy, never by
                                 realm teardown (urbi_realm_destroy Step 2b
                                 keys on this flag). */
        vm->stdlib_module = m;
        /* Note: running the root chunk of the stdlib module is deferred
         * to a later phase — urbi_stdlib_boot is invoked from inside
         * urbi_populate_realm_globals during realm creation, and
         * urbi_run_chunk would re-enter the realm-create path.  Phase
         * 10 will arrange the run via a deferred-execution hook once
         * the global Realm is fully populated. */
    }

#ifdef URBI_ENABLE_UROBOTICS
    {
        int rc_uro = urbi_urobotics_register(vm);
        if (rc_uro != URBI_OK) return rc_uro;
    }
#endif

    {
        int rc_ro = urbi_atom_protos_mark_readonly(vm);
        if (rc_ro != URBI_OK) return rc_ro;
    }

    {
        int rc_isa = urbi_isa_method_register(vm);
        if (rc_isa != URBI_OK) return rc_isa;
    }

    vm->stdlib_booted = 1U;
    return URBI_OK;
}

int
urbi_stdlib_boot(UVM *vm)
{
    int     rc;
    uint8_t saved_pause;

    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->stdlib_booted) return URBI_OK;

    saved_pause   = vm->gc_paused;
    vm->gc_paused = 1U;
    rc = stdlib_boot_impl(vm);
    vm->gc_paused = saved_pause;
    return rc;
}
