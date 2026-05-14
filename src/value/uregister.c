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
#include "vm/uvm_error.h"       /* urbi_set_error_internal (Gap P) */

int
urbi_register(struct UVM *vm, struct URealm *realm,
              const char *name, urbi_native_method_fn fn)
{
    struct UClosure *c;
    UValue v;
    size_t name_len;
    int rc;

    if (!vm || !name || !fn) {
        urbi_set_error_internal(vm, URBI_ERR_INVALID_ARG,
            "urbi_register: vm, name, or fn is NULL",
            NULL, 0, "urbi_register");
        return URBI_ERR_INVALID_ARG;
    }

    /* NULL realm → use the VM's global realm (auto-created on first call). */
    if (realm == NULL) {
        realm = urbi_realm_global(vm);
        if (realm == NULL) {
            urbi_set_error_internal(vm, URBI_ERR_OOM,
                "urbi_register: OOM creating global realm",
                NULL, 0, "urbi_register");
            return URBI_ERR_OOM;
        }
    }

    c = urbi_make_native_closure(vm, fn);
    if (!c) {
        urbi_set_error_internal(vm, URBI_ERR_OOM,
            "urbi_register: OOM allocating native closure",
            NULL, 0, "urbi_register");
        return URBI_ERR_OOM;
    }

    v = urbi_make_closure(c);
    name_len = urbi_strlen(name);
    rc = urbi_realm_set_global_const(vm, realm, name, name_len, v);
    if (rc != URBI_OK) {
        urbi_set_error_internal(vm, rc,
            "urbi_register: failed to install global (name already taken or OOM)",
            NULL, 0, "urbi_register");
    }
    return rc;
}
