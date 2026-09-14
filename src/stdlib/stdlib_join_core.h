/* SPDX-License-Identifier: BSD-3-Clause */
/* stdlib_join_core.h — the one implementation of join, shared by
 * String.join (separator is the receiver) and List.join (separator is
 * the argument).  It is static in each translation unit that includes
 * it; both call sites validate their own receiver first.
 *
 * Two linear passes over the list — measure, then fill — inside a single
 * synchronous C call.  No closure runs between them, so the list cannot
 * change underfoot and the result always reflects the elements as they
 * were at entry. */

#ifndef URBI_STDLIB_JOIN_CORE_H
#define URBI_STDLIB_JOIN_CORE_H

#include "rt/ustdlib_glue.h"

/* Concatenates `list`'s String elements with `sep` between them.
 * UEXEC_OK with the result in *out, or UEXEC_THROW: TypeError when an
 * element is not a String, OutOfMemoryError when the working buffer or
 * the result cannot be allocated. */
static int join_core(UVM *vm, UValue sep, UValue list, UValue *out)
{
    uint32_t count = urbi_list_len(list);
    size_t seplen = urbi_str_size(sep);
    size_t total = 0;

    for (uint32_t i = 0; i < count; i++) {
        UValue e = urbi_list_get(list, i);
        if (!urbi_is_str(e))
            return urbi_raise_type(vm, "join: all elements must be String", out);
        total += urbi_str_size(e);
        if (i + 1u < count) total += seplen;
    }

    char *buf = (char *)vm->gc.alloc(NULL, total + 1u, vm->gc.alloc_ud);
    if (buf == NULL) return urbi_raise_oom(vm, out);

    size_t off = 0;
    for (uint32_t i = 0; i < count; i++) {
        UValue e = urbi_list_get(list, i);
        size_t el = urbi_str_size(e);
        urbi_memcpy(buf + off, urbi_str_cstr(e), el);
        off += el;
        if (i + 1u < count) { urbi_memcpy(buf + off, urbi_str_cstr(sep), seplen); off += seplen; }
    }
    buf[off] = '\0';

    /* The buffer is plain host memory, not a GC cell, so the allocation
     * inside urbi_make_str cannot invalidate it. */
    UValue v = urbi_make_str(vm, buf, off);
    vm->gc.alloc(buf, 0u, vm->gc.alloc_ud);
    if (v.kind == UV_NIL) return urbi_raise_oom(vm, out);
    *out = v;
    return UEXEC_OK;
}

#endif /* URBI_STDLIB_JOIN_CORE_H */
