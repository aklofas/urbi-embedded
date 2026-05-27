/* SPDX-License-Identifier: BSD-3-Clause */
/* isa_method.c — v0.10.11 isA universal type-test on Object root.
 *
 * obj.isA(Proto) -> Bool.  True iff Proto appears in obj's transitive
 * proto chain.  Atom-typed receivers are routed through
 * urbi_atom_proto_for_value (same routing table used by dispatch).
 * UVAL_OBJECT receivers walk their own UObject.protos chain.
 *
 * Per Cat. E re-audit Cluster #17 ratify (isA -> ship-v1.0).
 * Closes design-risks v0.10.7-E. */

#include "stdlib/isa_method.h"

#include <stdint.h>

#include "object/uobject.h"          /* urbi_object_proto_count,
                                      *   urbi_object_proto_at,
                                      *   urbi_object_atom,
                                      *   urbi_atom_proto_for_value */
#include "stdlib/object_root.h"      /* urbi_raise_arity, urbi_raise_type */
#include "value/uintern.h"           /* ustr_intern */
#include "vm/uvm.h"                  /* UVM */
#include "urbi/urbi.h"               /* URBI_OK, URBI_ERR_*, urbi_make_bool */
#include "urbi/object.h"             /* URBI_ATOM_OBJECT, URBIAtomFamily */
#include "runtime/umacros.h"         /* urbi_zero */

/* Walk receiver's transitive proto chain (inclusive — `start` itself
 * counts as a match).  Returns non-zero iff `target` is found.
 *
 * DFS with a numeric depth guard (64-level limit; matches the maximum
 * sensible proto-chain depth in this codebase).  uobject_lookup.c uses
 * a stamp-based cycle guard for the same purpose — different mechanism,
 * same goal of surviving any malformed proto cycle. */
#define ISA_MAX_DEPTH 64

static int
proto_chain_contains(const UObject *start, const UObject *target, int depth)
{
    if (start == NULL || target == NULL) return 0;
    if (start == target) return 1;
    if (depth <= 0) return 0;
    uint32_t n = urbi_object_proto_count(start);
    uint32_t i;
    for (i = 0; i < n; i++) {
        const UObject *p = urbi_object_proto_at(start, i);
        if (proto_chain_contains(p, target, depth - 1)) return 1;
    }
    return 0;
}

static int
isa_native(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1)
        return urbi_raise_arity(vm, "isA", 1, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "isA: argument must be a proto (Object)", out);

    /* Resolve the receiver to its logical proto.  For UVAL_OBJECT this
     * returns the receiver object itself; for atoms (UVAL_INT / FLOAT /
     * STR / BOOL / NIL / EVENT / TAG / …) it returns the per-VM atom
     * singleton that backs that type. */
    const UObject *recv_proto = urbi_atom_proto_for_value(vm, self);
    const UObject *target     = (const UObject *)args[0].v.p;

    int result = proto_chain_contains(recv_proto, target, ISA_MAX_DEPTH);
    *out = urbi_make_bool(result != 0);
    return UEXEC_OK;
}

/* === urbi_isa_method_register ==========================================
 *
 * Installs `isA` as a C-native closure on vm->atom_object (the root
 * Object proto).  Since every proto chain terminates at atom_object,
 * isA is reachable from all scripted objects and atom values.
 *
 * Called from urbi_stdlib_boot AFTER urbi_atom_protos_mark_readonly.
 * C-side urbi_object_set_local_slot ignores URBI_OBJ_FLAG_READONLY, so
 * ordering relative to mark_readonly does not matter in practice, but
 * post-readonly placement is conventional (mirrors lobby_native +
 * job_proto patterns). */
int
urbi_isa_method_register(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    UObject *root = urbi_object_atom(vm, URBI_ATOM_OBJECT);
    if (root == NULL) return URBI_ERR_INVALID_STATE;

    UClosure *cl = urbi_native_closure_create(vm, isa_native);
    if (cl == NULL) return URBI_ERR_OOM;

    USymbol *sym = (USymbol *)ustr_intern(vm, "isA", 3);
    if (sym == NULL) return URBI_ERR_OOM;

    UValue v;
    urbi_zero(&v, sizeof(v));
    v.kind = (uint8_t)UVAL_CLOSURE;
    v.v.p  = (void *)cl;

    if (urbi_object_set_local_slot(vm, root, sym, v) != 0)
        return URBI_ERR_OOM;
    return URBI_OK;
}
