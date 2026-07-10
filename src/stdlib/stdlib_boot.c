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
 * Step 3 only runs when urbi_stdlib_bytecode_len > 0.  At Phase 4
 * baseline the blob is empty (STDLIB_ORDER.txt empty), so this branch
 * is dead code that becomes live in Phase 10 when the order file is
 * populated.  Parser-independent: the blob is bytecode, not source —
 * verified by the URBI_BYTECODE_ONLY=1 build (v0.7.0-c-api)
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

    /* Phase 4 (atom proto stubs).  Allocates Boolean / Nil / Void
     * singletons + installs the baseline family-specific methods (Boolean
     * .toString, String.length).  Integer / Float / Nil / Void protos
     * exist but inherit clone + getSlot/etc. from Object root via the
     * prototype chain. */
    rc = urbi_atom_protos_register(vm);
    if (rc != URBI_OK) return rc;

    /* Phase 5 (atom-proto Tier 1 methods).  Installs C-native methods on
     * Boolean / Integer / Float / String atom protos.  Symbolic operators
     * (`+`, `==`, …) remain inline VM opcodes; only named methods (asString,
     * and, sqrt, length, …) land here.  See src/stdlib/atoms.c banner. */
    rc = urbi_stdlib_register_atom_methods(vm);
    if (rc != URBI_OK) return rc;

    /* Phase 6: C-native containers.  Installs methods on the
     * existing URBI_ATOM_LIST / URBI_ATOM_DICT atom protos (the
     * realm-populate registry already publishes these as "List" / "Dict"
     * globals).  Pair / Triplet / Tuple realm-global registration is
     * deferred to a post-loop hook in urbi_populate_realm_globals so the
     * registry's slot 0..7 layout (Object .. List) stays stable for the
     * v1.0 packed-flag CONSTANT enforcement range.  See
     * src/stdlib/containers.c. */
    rc = urbi_stdlib_register_containers(vm);
    if (rc != URBI_OK) return rc;

    /* Phase 7: runtime-type protos.  Allocates
     * vm->exception_proto and installs Exception.new / Exception.raise.
     * Realm-global binding for "Exception" is deferred to the post-loop
     * hook urbi_stdlib_register_runtime_globals, mirroring container
     * globals so the registry's slot 0..7 layout stays stable.  See
     * src/stdlib/runtime_types.c. */
    rc = urbi_stdlib_register_runtime_types(vm);
    if (rc != URBI_OK) return rc;

    /* Phase 8: namespace protos.  Allocates Math / System /
     * System.Platform / Global / CallMessage proto UObjects with their
     * constants + native methods.  Realm-global binding for the
     * namespace names is deferred to urbi_stdlib_register_namespace_-
     * globals, again preserving the registry's slot 0..7 layout.  See
     * src/stdlib/namespaces.c. */
    rc = urbi_stdlib_register_namespaces(vm);
    if (rc != URBI_OK) return rc;

    /* Phase 9: primitive protos.  Allocates Mutex / Date /
     * Duration proto UObjects with their native methods.  Realm-global
     * binding for the primitive names is deferred to urbi_stdlib_-
     * register_primitives_globals, again preserving the registry's
     * slot 0..7 layout.  See src/stdlib/primitives.c. */
    rc = urbi_stdlib_register_primitives(vm);
    if (rc != URBI_OK) return rc;

    /* v1.0 stdlib-completeness: RegExp proto.  Allocates vm->regexp_proto
     * with its native methods (new / test / match) and the compact
     * backtracking matcher.  Realm-global binding for "RegExp" is deferred
     * to urbi_stdlib_register_regexp_globals (post-registry loop), same
     * pattern as the primitive protos.  See src/stdlib/regexp.c. */
    rc = urbi_stdlib_register_regexp(vm);
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

    /* v0.10.10 / D7-A: Job proto singleton.  Allocates vm->job_proto +
     * installs four C-native methods (current, tags, uid, status).
     * Realm-global binding for "Job" is deferred to
     * urbi_job_proto_register_globals (post-loop hook in
     * urbi_populate_realm_globals). */
    rc = urbi_job_proto_register(vm);
    if (rc != URBI_OK) return rc;

    /* v0.10.10 / D7-D: scopeTag is bound per-realm in
     * urbi_tag_globals_register_globals (called from urbi_populate_-
     * realm_globals).  No VM-level setup needed here — the call-style
     * native is just a realm-global closure, mirroring the `every` /
     * `sleep` patterns. */

    /* v0.9.4 Phase 5: every() periodic-spawn primitive.  Allocates the
     * C-native UClosure stored on vm->every_native_closure.  Realm-global
     * binding for "every" is deferred to urbi_temporal_native_register_-
     * globals (post-loop hook in urbi_populate_realm_globals).  No proto
     * to mark readonly — the closure itself is the binding. */
    rc = urbi_temporal_native_register(vm);
    if (rc != URBI_OK) return rc;

#ifdef URBI_ENABLE_REPL
    /* v0.9.1: Debug namespace.  Allocates the singleton proto +
     * binds 9 native-method slots.  The realm-global "Debug" binding is
     * deferred to urbi_debug_namespace_register_globals (called from
     * urbi_populate_realm_globals AFTER the mark_readonly pass).  The
     * proto itself is NOT marked readonly — symmetry with Global, the
     * other reflective namespace. */
    rc = urbi_debug_namespace_register(vm);
    if (rc != URBI_OK) return rc;
#endif

#ifdef URBI_ENABLE_ROS2
    /* v0.12.0: allocate + cache vm->ros_proto (the `ros` namespace proto).
     * Runs after all other native registrations so the root Object chain is
     * fully in place.  Gated by URBI_ENABLE_ROS2; compiles away otherwise. */
    {
        int rc_ros = urbi_ros_register(vm);
        if (rc_ros != URBI_OK) return rc_ros;
    }
#endif

    /* Deserialize the baked stdlib bytecode blob
     * and bind a per-VM UChunkInstance.  Empty blob (Phase 4 baseline)
     * skips this entirely. */
    if (urbi_stdlib_bytecode_len > 0) {
        if (vm->alloc_fn == NULL) {
            return URBI_ERR_STDLIB_BOOT_FAILED;
        }
        /* v0.9.2: uchunk_deserialize allocates root UProto via alloc_fn. */
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
    /* v0.12.2: deserialize + cache the Robotics facet overlay module.
     * Runs after the main stdlib blob loads; its root chunk is run later
     * during realm-globals population (urbi_urobotics_run). */
    {
        int rc_uro = urbi_urobotics_register(vm);
        if (rc_uro != URBI_OK) return rc_uro;
    }
#endif

    /* v0.9.1: mark every builtin atom + runtime-type proto readonly
     * AFTER all population phases (1-9) so the method-install passes are not
     * blocked by their own readonly bits.  Spec §4.2.  The Global namespace
     * proto (vm->global_namespace_proto) is left mutable by design. */
    {
        int rc_ro = urbi_atom_protos_mark_readonly(vm);
        if (rc_ro != URBI_OK) return rc_ro;
    }

    /* v0.10.11 / Cat. E Cluster #17: isA on Object root.  Runs after
     * mark_readonly because C-side urbi_object_set_local_slot ignores
     * URBI_OBJ_FLAG_READONLY; safe either way.  isA is reachable from
     * every proto chain (atom_object is the root). */
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

    /* Bootstrap GC pause (v0.13.2): every registration phase below holds
     * fresh cells (protos, native closures) in C locals across further
     * allocations.  Host code, no strand — see the rationale banner above
     * populate_realm_globals_impl in realm/urealm_globals.c.  Guarded at
     * this entry point as well as in the populate wrapper because unit
     * tests call urbi_stdlib_boot directly on armed stress builds.
     * Save/restore so a host-held urbi_gc_pause latch survives. */
    saved_pause   = vm->gc_paused;
    vm->gc_paused = 1U;
    rc = stdlib_boot_impl(vm);
    vm->gc_paused = saved_pause;
    return rc;
}
