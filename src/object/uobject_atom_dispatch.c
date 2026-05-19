/* SPDX-License-Identifier: BSD-3-Clause */
/* uobject_atom_dispatch.c — Phase 2: atom-receiver to atom-proto routing.
 *
 * Per the v0.6.0 stdlib-scaffold plan §3.1 Phase 2: slot lookup starting
 * from a non-UVAL_OBJECT value must route through the realm-global atom
 * proto.  This file holds the value-kind-to-atom-proto routing table.
 * The actual slot lookup against that proto reuses the existing UObject
 * lookup path in src/object/uic.c (urbi_slot_get_slow) and the prototype
 * walk in src/object/uobject_lookup.c.
 *
 * PERF CONTRACT (S-atom-clone-perf): once Phase 3 lands the Object root
 * C-native `clone` method, `clone` on a UVAL_INT receiver MUST return the
 * receiver value directly — no urbi_object_alloc call.  Same for UVAL_FLOAT
 * (atoms are immutable).  Until Phase 3, the dispatch arrives here; Phase
 * 3's `clone` arm implements the short-circuit.  Test
 * int_method_dispatch_returns_slot_value in test_atom_dispatch.c proves
 * the dispatch arrives; a perf-counter test (test_atom_clone_no_alloc.c)
 * lands at Phase 3 to verify zero allocation. */

#include "object/uobject.h"
#include "vm/uvm.h"
#include "chunk/umodule.h"   /* UValue + UVAL_* enumerators */
#include "urbi/object.h"      /* URBIAtomFamily */

#ifdef URBI_DEBUG
#include <stdio.h>
#endif

UObject *
urbi_atom_proto_for_value(struct UVM *vm, UValue v)
{
    /* Fast arms: each known UValKind routes to its dedicated atom proto.
     * Phase 4 widens the per-kind map by promoting UVAL_BOOL / UVAL_NIL /
     * UVAL_VOID — previously bucketed into root Object — into their own
     * URBI_ATOM_BOOLEAN / NIL / VOID protos.  The remaining script-side
     * kinds (UVAL_CLOSURE / UVAL_STRAND / UVAL_HOST_FN) still fall through
     * to root Object since they have no scripted-method surface at
     * v1.0. */
    switch ((UValKind)v.kind) {
        case UVAL_OBJECT:
            return (UObject *)v.v.p;

        case UVAL_INT:
            return urbi_object_atom(vm, URBI_ATOM_INTEGER);

        case UVAL_FLOAT:
            return urbi_object_atom(vm, URBI_ATOM_FLOAT);

        case UVAL_STR:
            return urbi_object_atom(vm, URBI_ATOM_STRING);

        case UVAL_BOOL:
            return urbi_object_atom(vm, URBI_ATOM_BOOLEAN);

        case UVAL_NIL:
            return urbi_object_atom(vm, URBI_ATOM_NIL);

        case UVAL_VOID:
            return urbi_object_atom(vm, URBI_ATOM_VOID);

        case UVAL_EVENT:
            /* Tag values flow through the Object kind today (no UVAL_TAG
             * in the public union); UEvent has its own UValKind, and
             * Phase 4 maps it to URBI_ATOM_EVENT here directly. */
            return urbi_object_atom(vm, URBI_ATOM_EVENT);

        case UVAL_CLOSURE:
        case UVAL_STRAND:
        case UVAL_HOST_FN:
        default:
            break;   /* fall through to root-Object + diagnostic */
    }

    /* Unrecognised kinds (closures / strands / host_fn — no scripted-method
     * surface at v1.0): URBI_DEBUG diagnostic for the routed kind.
     * Production builds compile out the stderr write. */
#ifdef URBI_DEBUG
    if (v.kind != (uint8_t)UVAL_CLOSURE
        && v.kind != (uint8_t)UVAL_STRAND
        && v.kind != (uint8_t)UVAL_HOST_FN) {
        fprintf(stderr,
                "urbi_atom_proto_for_value: unhandled kind %s\n",
                urbi_atom_family_name((URBIAtomFamily)v.kind));
    }
#endif
    return urbi_object_atom(vm, URBI_ATOM_OBJECT);
}
