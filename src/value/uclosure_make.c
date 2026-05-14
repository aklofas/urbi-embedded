/* SPDX-License-Identifier: BSD-3-Clause */
/* src/value/uclosure_make.c — public API: urbi_make_native_closure (Gap L).
 *
 * Exposes UClosure construction for host C code without requiring callers to
 * include internal headers.  Delegates to the existing internal helper
 * urbi_native_closure_create (object_root.c). */

#include "urbi/urbi.h"          /* urbi_make_native_closure + urbi_native_method_fn */
#include "vm/uvm.h"             /* UVM typedef */
#include "runtime/uclosure.h"   /* struct UClosure full definition */
#include "stdlib/object_root.h" /* urbi_native_closure_create */

struct UClosure *
urbi_make_native_closure(struct UVM *vm, urbi_native_method_fn fn)
{
    if (vm == NULL || fn == NULL) return NULL;
    return urbi_native_closure_create(vm, fn);
}
