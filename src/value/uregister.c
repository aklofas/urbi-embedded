/* SPDX-License-Identifier: BSD-3-Clause */
/* src/value/uregister.c — Gap A: urbi_register host-function registration.
 *
 * Composite of urbi_make_native_closure (Gap L) + urbi_realm_set_global_const.
 * A new UVAL_CLOSURE backed by `fn` is allocated and installed as a
 * CONSTANT realm global under `name`.  Const-by-default means re-registering
 * the same name returns URBI_ERR_CONST_SLOT_WRITE. */

#include "urbi/urbi.h"          /* urbi_register + urbi_native_method_fn */
#include "urbi/types.h"         /* urbi_make_closure */
#include "runtime/uclosure.h"   /* struct UClosure */
#include "runtime/umacros.h"    /* urbi_strlen */
#include "realm/urealm.h"       /* urbi_realm_global */

int
urbi_register(struct UVM *vm, struct URealm *realm,
              const char *name, urbi_native_method_fn fn)
{
    if (!vm || !name || !fn) return URBI_ERR_INVALID_ARG;

    /* NULL realm → use the VM's global realm (auto-created on first call). */
    if (realm == NULL) {
        realm = urbi_realm_global(vm);
        if (realm == NULL) return URBI_ERR_OOM;
    }

    struct UClosure *c = urbi_make_native_closure(vm, fn);
    if (!c) return URBI_ERR_OOM;

    UValue v = urbi_make_closure(c);
    size_t name_len = urbi_strlen(name);
    return urbi_realm_set_global_const(vm, realm, name, name_len, v);
}
