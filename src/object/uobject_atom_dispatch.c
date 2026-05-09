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
#include "module/umodule.h"   /* UValue + UVAL_* enumerators */
#include "urbi/object.h"      /* URBIAtomFamily */

#ifdef URBI_DEBUG
#include <stdio.h>
#endif

UObject *
urbi_atom_proto_for_value(struct UVM *vm, UValue v)
{
    switch ((UValKind)v.kind) {
        case UVAL_OBJECT:
            return (UObject *)v.v.p;

        case UVAL_INT:
            return urbi_object_atom(vm, URBI_ATOM_INTEGER);

        case UVAL_FLOAT:
            return urbi_object_atom(vm, URBI_ATOM_FLOAT);

        case UVAL_STR:
            return urbi_object_atom(vm, URBI_ATOM_STRING);

        case UVAL_EVENT:
            /* Tag values flow through the Object kind today (no UVAL_TAG
             * in the public union); UEvent has its own UValKind, and
             * Phase 4 maps it to URBI_ATOM_EVENT here directly. */
            return urbi_object_atom(vm, URBI_ATOM_EVENT);

        case UVAL_BOOL:
            /* No URBI_ATOM_BOOLEAN at Phase 2 baseline — that lands as a
             * Phase 4 deliverable.  Route Boolean to root Object until
             * then; Phase 4 tightens the routing. */
            return urbi_object_atom(vm, URBI_ATOM_OBJECT);

        case UVAL_NIL:
        case UVAL_VOID:
            /* nil and void route to root Object until Phase 4 lands their
             * dedicated protos.  Returning the root Object means slot
             * lookup against `1.foo` succeeds via Integer proto, but
             * `nil.foo` only resolves to whatever lives on root Object —
             * matching legacy semantics where nil.clone() fails (nil is
             * a singleton). */
            return urbi_object_atom(vm, URBI_ATOM_OBJECT);

        case UVAL_CLOSURE:
        case UVAL_STRAND:
        case UVAL_HOST_FN:
        default:
            /* Each of these has its own atom singleton candidate in v1.x.
             * For Phase 2 they all route through root Object.  The default
             * arm logs to stderr under URBI_DEBUG so audit-trail tools
             * can surface unexpected kinds reaching this site. */
#ifdef URBI_DEBUG
            fprintf(stderr,
                    "urbi_atom_proto_for_value: unhandled kind %s\n",
                    urbi_atom_family_name((URBIAtomFamily)v.kind));
#endif
            return urbi_object_atom(vm, URBI_ATOM_OBJECT);
    }
}
