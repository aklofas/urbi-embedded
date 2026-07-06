/* SPDX-License-Identifier: BSD-3-Clause */
/* control_native.c — v0.10.10 / D7-C: detach / disown implementations. */

#include "stdlib/control_native.h"
#include "stdlib/object_root.h"       /* urbi_native_closure_create + raise helpers */

#include "chunk/uproto.h"             /* UProto, urbi_proto_strand_ref_acquire */
#include "realm/urealm.h"             /* URealm */
#include "runtime/uclosure.h"         /* UClosure full definition */
#include "runtime/ucleanup.h"         /* UCleanupEntry, UCLEANUP_TAG_SCOPE */
#include "runtime/umacros.h"          /* urbi_zero */
#include "sched/ustrand.h"            /* urbi_strand_create, urbi_strand_arm_from_closure,
                                         urbi_strand_start, UStrand,
                                         urbi_sched_strand_unlink_member_entry */
#include "urbi/types.h"               /* urbi_make_nil, UValue, UVAL_CLOSURE */
#include "urbi/urbi.h"                /* URBI_OK / URBI_ERR_* / urbi_realm_set_global */
#include "vm/uvm.h"                   /* UVM */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* spawn_child_from_closure
 *
 * Create + arm + start a child strand running `cl`, inheriting the parent
 * strand's ambient tag chain.  Mirrors the sequence in fork_spawn_child
 * (uop_fork.c) without the OP_FORK_DETACH / OP_FORK_JOIN specifics.
 *
 * Callers (detach_native / disown_native) are responsible for any extra
 * cleanup-chain surgery BEFORE calling urbi_strand_start.
 *
 * Returns the newly created child (READY state), or NULL on OOM. */
static UStrand *
spawn_child_from_closure(UVM *vm, UClosure *cl)
{
    UStrand *parent = vm->cur_strand;
    URealm  *r      = parent ? parent->realm : vm->realms_head;

    UStrand *child = urbi_strand_create(vm, r, cl);
    if (child == NULL) return NULL;

    /* Arm execution state (pc, pc_base, cur_consts, register stack). */
    if (urbi_strand_arm_from_closure(child, cl, /*nargs=*/0) != 0) {
        urbi_strand_destroy(vm, child);
        return NULL;
    }

    /* Inherit root_proto and module_instance from parent so OP_GETSLOT /
     * OP_SETSLOT can resolve the IC table.  Matches fork_spawn_child. */
    if (parent != NULL) {
        child->root_proto      = parent->root_proto;
        child->module_instance = parent->module_instance;
        if (child->root_proto != NULL) {
            urbi_proto_strand_ref_acquire(child->root_proto,
                                          URBI_PROTO_REF_OWNER_FORK);
        }
    }

    /* Also inherit the parent's ambient tag chain (all TAG_SCOPE entries
     * above realm->tag — the realm->tag is already attached by
     * urbi_strand_create).  This mirrors fork_spawn_child §4.1. */
    if (parent != NULL && parent->cleanup_depth > 1) {
        /* Collect the extra TAG_SCOPEs above the bottom realm->tag entry. */
        struct UTag *chain[URBI_CLEANUP_MAX];
        size_t n = 0;
        size_t i;
        for (i = 1; i < parent->cleanup_depth && n < URBI_CLEANUP_MAX; i++) {
            UCleanupEntry *e = &parent->cleanup_base[i];
            if (e->kind == UCLEANUP_TAG_SCOPE && e->owning_tag != NULL) {
                chain[n++] = e->owning_tag;
            }
        }
        if (n > 0) {
            urbi_strand_attach_ambient_tags(child, chain, n);
        }
    }

    urbi_strand_start(vm, child);
    return child;
}

/* === __detach_strand: spawn child inheriting ambient tag chain ===========
 *
 * Receives the implicit closure produced by the lazy-arg wrapper.
 * The child inherits the parent's full cleanup chain. */

static int
detach_native(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    UClosure *cl;

    (void)self;
    if (nargs != 1)
        return urbi_raise_arity(vm, "__detach_strand", 1, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_CLOSURE)
        return urbi_raise_type(vm, "__detach_strand: arg must be a closure", out);

    cl = (UClosure *)args[0].v.p;
    if (spawn_child_from_closure(vm, cl) == NULL) return urbi_raise_oom(vm, out);

    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* === __disown_strand: spawn child + strip inherited TAG_SCOPE entries ====
 *
 * Per D7 spec: keep only the bottom-most TAG_SCOPE (the connection tag,
 * which is realm->tag — required for session-disconnect reaping).
 * Everything above is a user-applied tag scope that disown removes.
 * Compact the cleanup stack in-place after unlinking the dropped entries. */

static int
disown_native(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    UClosure       *cl;
    const URealm   *r;
    UStrand        *child;
    size_t    write_idx;
    size_t    i;
    bool      kept_root_scope;

    (void)self;
    if (nargs != 1)
        return urbi_raise_arity(vm, "__disown_strand", 1, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_CLOSURE)
        return urbi_raise_type(vm, "__disown_strand: arg must be a closure", out);

    cl    = (UClosure *)args[0].v.p;
    r     = vm->cur_strand ? vm->cur_strand->realm : vm->realms_head;
    child = spawn_child_from_closure(vm, cl);
    if (child == NULL) return urbi_raise_oom(vm, out);

    /* Walk the inherited cleanup chain; keep only the bottom-most
     * TAG_SCOPE (realm->tag, the connection tag).  Compact in-place.
     * spawn_child_from_closure already started the child (READY state);
     * it hasn't run yet so cleanup_depth is accurate. */
    write_idx       = 0;
    kept_root_scope = false;
    for (i = 0; i < child->cleanup_depth; i++) {
        UCleanupEntry *e = &child->cleanup_base[i];
        if (e->kind == UCLEANUP_TAG_SCOPE) {
            if (r->tag != NULL && e->owning_tag == r->tag && !kept_root_scope) {
                /* Keep the realm's connection tag entry. */
                if (write_idx != i) child->cleanup_base[write_idx] = *e;
                write_idx++;
                kept_root_scope = true;
            } else {
                /* Strip this user-applied tag scope. */
                urbi_sched_strand_unlink_member_entry(e);
                /* Don't copy: drops this entry. */
            }
        } else {
            /* Keep non-TAG_SCOPE entries (e.g. catch frames). */
            if (write_idx != i) child->cleanup_base[write_idx] = *e;
            write_idx++;
        }
    }
    child->cleanup_depth = write_idx;

    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* === Realm-global binding =============================================== */

int
urbi_control_native_register_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;

    {
        UClosure *cl = urbi_native_closure_create(vm, detach_native);
        UValue v;
        int rc;
        if (cl == NULL) return URBI_ERR_OOM;
        v.kind = (uint8_t)UVAL_CLOSURE;
        v.v.p  = (void *)cl;
        rc = urbi_realm_set_global(vm, realm, "__detach_strand", 15, v);
        if (rc != URBI_OK) return rc;
    }
    {
        UClosure *cl = urbi_native_closure_create(vm, disown_native);
        UValue v;
        int rc;
        if (cl == NULL) return URBI_ERR_OOM;
        v.kind = (uint8_t)UVAL_CLOSURE;
        v.v.p  = (void *)cl;
        rc = urbi_realm_set_global(vm, realm, "__disown_strand", 15, v);
        if (rc != URBI_OK) return rc;
    }
    return URBI_OK;
}
