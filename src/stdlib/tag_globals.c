/* SPDX-License-Identifier: BSD-3-Clause */

#include "stdlib/tag_globals.h"

#include "realm/urealm.h"
#include "runtime/uclosure.h"
#include "sched/ustrand.h"
#include "stdlib/object_root.h"   /* urbi_native_closure_create */
#include "tag/utag.h"             /* urbi_strand_scope_tag */
#include "urbi/types.h"           /* urbi_make_nil + urbi_make_tag */
#include "urbi/urbi.h"            /* URBI_OK / URBI_ERR_* / urbi_realm_set_global */
#include "vm/uvm.h"               /* UVM.cur_strand */

#include <stddef.h>
#include <stdint.h>

/* === scopeTag native (call-style) =======================================
 *
 * No-arg native callable.  Returns the innermost UCLEANUP_TAG_SCOPE.owning
 * _tag on the current strand's cleanup stack, wrapped as UVAL_TAG.
 * Returns nil if the strand has no tag scopes (cannot happen in practice
 * — realm->tag sits at the bottom always — but the resolver tolerates it
 * for unit-test contexts that build bare strands without ambient tags).
 *
 * Bound as a realm-global closure (mirrors the `every` and `sleep`
 * patterns in src/stdlib/temporal.c).  Script-side invocation: `scopeTag()`
 * with parens.
 */

static int
scope_tag_native(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                 UValue *out)
{
    (void)self;
    (void)args;
    if (nargs != 0) {
        return urbi_raise_arity(vm, "scopeTag", 0, nargs, out);
    }

    UStrand *cur = vm->cur_strand;
    if (cur == NULL) {
        *out = urbi_make_nil();
        return UEXEC_OK;
    }
    UTag *t = urbi_strand_scope_tag(cur);
    if (t == NULL) {
        *out = urbi_make_nil();
        return UEXEC_OK;
    }
    *out = urbi_make_tag(t);
    return UEXEC_OK;
}

int
urbi_tag_globals_register(UVM *vm)
{
    (void)vm;
    return URBI_OK;
}

/* Bind "scopeTag" as a realm-global pointing at a native closure that
 * wraps urbi_strand_scope_tag.  Called from urbi_populate_realm_globals
 * after the registry loop (slot 15+, past the v1.0 packed-flag CONSTANT
 * enforcement range).  Mirrors urbi_temporal_native_register_globals's
 * pattern (one allocation per realm; the closure is rooted via the
 * realm-global slot for the lifetime of the realm).
 *
 * Returns URBI_OK on success, URBI_ERR_INVALID_ARG on NULL args,
 * URBI_ERR_OOM on closure-alloc / slot-set failure. */
int
urbi_tag_globals_register_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    UClosure *cl = urbi_native_closure_create(vm, scope_tag_native);
    if (cl == NULL) {
        return URBI_ERR_OOM;
    }
    UValue cv = urbi_make_nil();
    cv.kind = (uint8_t)UVAL_CLOSURE;
    cv.v.p  = (void *)cl;
    return urbi_realm_set_global(vm, realm, "scopeTag", 8, cv);
}
