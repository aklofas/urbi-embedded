/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * urobotics.c — register/run shim for the Robotics facet overlay.
 *
 * Deserializes the baked urobotics bytecode blob and mounts it onto the
 * VM as a chunk instance.  Compiled only when URBI_ENABLE_UROBOTICS is set;
 * the core VM has no direct knowledge of this subsystem.
 */
#ifdef URBI_ENABLE_UROBOTICS

#include "urbi/urobotics.h"

#include "urbi/urbi.h"               /* URBI_OK, URBI_ERR_*, urbi_run_chunk */
#include "chunk/uchunk.h"            /* UProto, uchunk_deserialize, uchunk_destroy */
#include "object/uchunk_instance.h"  /* urbi_get_or_create_chunk_instance */
#include "vm/uvm.h"

int
urbi_urobotics_register(struct UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    if (urbi_urobotics_bytecode_len == 0) return URBI_OK; /* nothing baked */
    if (vm->urobotics_module != NULL) return URBI_OK;     /* idempotent */
    if (vm->alloc_fn == NULL) return URBI_ERR_STDLIB_BOOT_FAILED;

    UProto *m = NULL;
    UChunkLoadError lerr = uchunk_deserialize(
        &m, urbi_urobotics_bytecode, urbi_urobotics_bytecode_len,
        vm->alloc_fn, vm->alloc_ud, NULL, 0);
    if (lerr != UCHUNK_LOAD_OK) return URBI_ERR_STDLIB_BOOT_FAILED;
    if (urbi_get_or_create_chunk_instance(vm, m) == NULL) {
        uchunk_destroy(m, vm);
        return URBI_ERR_OOM;
    }
    m->vm_owned = true;   /* GC-18: freed by urbi_vm_destroy, never by realm
                             teardown (urbi_realm_destroy Step 2b keys on
                             this flag). */
    vm->urobotics_module = m;
    return URBI_OK;
}

int
urbi_urobotics_run(struct UVM *vm, struct URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->urobotics_module == NULL) return URBI_OK;     /* not registered */
    UValue out;
    return urbi_run_chunk(vm, realm, vm->urobotics_module, &out);
}

#else
typedef int urbi_urobotics_tu_not_empty;
#endif /* URBI_ENABLE_UROBOTICS */
