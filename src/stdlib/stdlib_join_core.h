/* SPDX-License-Identifier: BSD-3-Clause */
/* stdlib_join_core.h — shared static join implementation.
 *
 * Included by atoms.c (String.join, self=sep) and containers.c
 * (List.join, arg=sep) so that both call sites share one implementation.
 * The function is static in each translation unit.
 *
 * Callers are responsible for validating their own self/arg types before
 * calling join_core.  Specifically:
 *   - sep must be a non-NULL, NUL-terminated C string of `seplen` bytes.
 *   - list_obj must be a non-NULL UObject pointing to a List instance.
 *
 * If list_obj carries no _storage backing (e.g. a bare clone of the List
 * proto), join_core raises the house-style catchable TypeError
 * "join: missing _storage" — the same contract every other List/Dict
 * method in containers.c follows.
 *
 * Mutation contract: join_core performs two linear scans over the
 * backing store (measure pass, then fill pass) within a single
 * synchronous C call.  No closures are invoked; the list cannot be
 * mutated between the two passes.  The result always reflects the
 * element set at call entry. */

#ifndef URBI_STDLIB_JOIN_CORE_H
#define URBI_STDLIB_JOIN_CORE_H

#include <stddef.h>
#include <stdint.h>

/* All included headers already appear in atoms.c and containers.c;
 * the guards below prevent double-inclusion. */
#include "stdlib/containers.h"   /* urbi_stdlib_list_len / _get / _storage_present */
#include "stdlib/object_root.h"  /* urbi_raise_type / urbi_raise_oom */
#include "runtime/umacros.h"     /* urbi_strlen */
#include "sched/ustrand.h"       /* UEXEC_OK / UEXEC_THROW */
#include "value/uintern.h"       /* ustr_intern */
#include "urbi/types.h"          /* UValue, urbi_make_nil */
#include "vm/uvm.h"              /* UVM */

/* join_core: concatenate String elements of list_obj with sep as separator.
 *
 * Returns UEXEC_OK with *out = interned result String, or UEXEC_THROW
 * when the receiver has no _storage backing or any element is not a
 * String (TypeError), or allocation fails (OOM).
 */
static int
join_core(UVM *vm, const char *sep, size_t seplen,
          struct UObject *list_obj, UValue *out)
{
    /* House-style storage guard: len/get alone cannot distinguish a
     * storage-less object from an empty list, so probe explicitly. */
    if (!urbi_stdlib_list_storage_present(vm, list_obj))
        return urbi_raise_type(vm, "join: missing _storage", out);

    size_t count = urbi_stdlib_list_len(vm, list_obj);
    size_t i, total = 0U;

    /* Measure pass: accumulate total byte count. */
    for (i = 0U; i < count; i++) {
        UValue e = urbi_stdlib_list_get(vm, list_obj, i);
        if (e.kind != (uint8_t)UVAL_STR)
            return urbi_raise_type(vm, "join: all elements must be String", out);
        total += urbi_strlen((const char *)e.v.p);
        if (i + 1U < count) total += seplen;
    }

    /* Allocate working buffer (+1 for NUL terminator). */
    if (vm->alloc_fn == NULL) return urbi_raise_oom(vm, out);
    char *buf = (char *)vm->alloc_fn(NULL, total + 1U, vm->alloc_ud);
    if (buf == NULL) return urbi_raise_oom(vm, out);

    /* Fill pass. */
    size_t off = 0U;
    for (i = 0U; i < count; i++) {
        UValue e = urbi_stdlib_list_get(vm, list_obj, i);
        const char *es = (const char *)e.v.p;
        size_t k, el = urbi_strlen(es);
        for (k = 0U; k < el; k++) buf[off++] = es[k];
        if (i + 1U < count) {
            for (k = 0U; k < seplen; k++) buf[off++] = sep[k];
        }
    }
    buf[off] = '\0';

    /* Intern and free working buffer. */
    const char *interned = ustr_intern(vm, buf, total);
    vm->alloc_fn(buf, 0U, vm->alloc_ud);
    if (interned == NULL) return urbi_raise_oom(vm, out);

    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_STR;
    v.v.p = (void *)interned;
    *out = v;
    return UEXEC_OK;
}

#endif /* URBI_STDLIB_JOIN_CORE_H */
