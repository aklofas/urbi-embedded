/* SPDX-License-Identifier: BSD-3-Clause */
/* src/value/umake_str.c — urbi_make_str_interned (Gap N str variant)
 *
 * Thin wrapper around ustr_intern.  Interns bytes[0..len) into vm's string
 * table and returns a UVAL_STR UValue holding the canonical pointer.  Returns
 * urbi_make_nil() on OOM or invalid arguments.
 *
 * Declaration lives in <urbi/urbi.h>. */

#include "urbi/urbi.h"     /* urbi_make_nil, UValue */
#include "urbi/types.h"    /* UVAL_STR */
#include "vm/uvm.h"        /* UVM typedef */
#include "value/uintern.h" /* ustr_intern */

/* urbi_make_str_interned: intern s[0..len) and return a UVAL_STR UValue.
 *
 * Two calls with byte-equal inputs return the same pointer (pointer-identity
 * implies content-equality).  On OOM or invalid args, returns urbi_make_nil().
 *
 * Thread safety: MAIN. */
UValue
urbi_make_str_interned(struct UVM *vm, const char *s, size_t len)
{
    if (!vm || (!s && len > 0)) return urbi_make_nil();

    const char *interned = ustr_intern(vm, s ? s : "", len);
    if (!interned) return urbi_make_nil();  /* OOM */

    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_STR;
    v.v.p  = (void *)interned;
    return v;
}
