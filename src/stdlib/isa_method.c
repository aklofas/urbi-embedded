/* SPDX-License-Identifier: BSD-3-Clause */
/* isa_method.c — `x.isA(Proto)`, the universal type test.
 *
 * True when Proto appears anywhere in the receiver's transitive
 * prototype chain, the receiver itself included.  It lives on the Object
 * root, so it answers for every value: an atom is resolved to the
 * prototype it dispatches on first, which is what makes `1.isA(Integer)`
 * and `1.isA(Object)` both true. */

#include "rt/ustdlib_glue.h"
#include "stdlib/isa_method.h"

static int isa_native(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (args[0].kind != UV_OBJ)
        return urbi_raise_type(vm, "isA: argument must be a proto (Object)", out);
    UObject *recv = urbi_atom_proto_for_value(vm, self);
    UObject *target = (UObject *)args[0].v.p;
    /* uobj_is_a is the same diamond-safe, stamp-guarded walk slot
     * resolution uses, so isA and lookup can never disagree about what
     * is in a chain. */
    *out = urbi_make_bool(recv != NULL && uobj_is_a(vm, recv, target));
    return UEXEC_OK;
}

const UMethodDef k_isa_methods[K_ISA_NMETHODS] = {
    { "isA", isa_native, 1, 1 }
};
