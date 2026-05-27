/* SPDX-License-Identifier: BSD-3-Clause */
/* channel_native.c — v0.10.11 / D6 Channel realm-globals registration.
 *
 * See channel_native.h banner for design rationale.
 *
 * Implementation approach (vs. eval-string):
 *   urbi_eval_string is not available as an internal API; the public
 *   urbi_repl_eval is gated on URBI_ENABLE_REPL and requires a live
 *   REPL session.  Instead, Channel instances are created directly via
 *   urbi_object_clone(vm, vm->channel_proto) + slot patches.  This
 *   mirrors how urbi_job_make creates Job instances.
 *
 *   The Channel.init() method sets `var this.name = n` on the instance.
 *   We replicate that by calling urbi_object_set_local_slot with a
 *   UVAL_STR for "name" and urbi_make_bool(false) for "quote" (cerr).
 *   The "enabled" slot is inherited from the proto (value true) and
 *   not overridden on any instance. */

#include "stdlib/channel_native.h"

#include <stddef.h>
#include <stdbool.h>

#include "object/uobject.h"     /* urbi_object_clone, urbi_object_set_local_slot */
#include "realm/urealm.h"       /* URealm, urbi_realm_set_global */
#include "runtime/umacros.h"    /* urbi_zero */
#include "urbi/urbi.h"          /* URBI_OK, URBI_ERR_*, urbi_realm_get_global,
                                  *   urbi_realm_set_global, urbi_make_bool,
                                  *   urbi_make_str_interned */
#include "urbi/types.h"         /* UValue, UVAL_OBJECT, UVAL_STR */
#include "value/uintern.h"      /* ustr_intern, USymbol */
#include "vm/uvm.h"             /* UVM, channel_proto */

/* === urbi_channel_proto_resolve =========================================
 *
 * The channel_overlay.u bake run installs "Channel" as a realm global.
 * Resolve it once per VM from the first realm that completes its
 * bake-blob run. */

int
urbi_channel_proto_resolve(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->channel_proto != NULL) return URBI_OK;   /* idempotent */

    UValue v;
    urbi_zero(&v, sizeof(v));
    int rc = urbi_realm_get_global(vm, realm, "Channel", 7, &v);
    if (rc != URBI_OK) return rc;   /* not found → URBI_ERR_SLOT_NOT_FOUND */
    if (v.kind != (uint8_t)UVAL_OBJECT) return URBI_ERR_INVALID_STATE;
    vm->channel_proto = (UObject *)v.v.p;
    return URBI_OK;
}

/* === install_channel_instance ===========================================
 *
 * Clone vm->channel_proto, set the `name` slot to the given C string,
 * optionally set `quote = false` (cerr), and bind the instance as a
 * realm global under `gname`. */
static int
install_channel_instance(UVM *vm, URealm *realm,
                         const char *gname, size_t glen,
                         const char *name,  size_t nlen,
                         bool set_quote_false)
{
    UObject *inst = urbi_object_clone(vm, vm->channel_proto);
    if (inst == NULL) return URBI_ERR_OOM;

    /* Set instance.name = name (UVAL_STR interned). */
    UValue name_val = urbi_make_str_interned(vm, name, nlen);
    if (name_val.kind != (uint8_t)UVAL_STR) return URBI_ERR_OOM;
    USymbol *name_sym = (USymbol *)ustr_intern(vm, "name", 4);
    if (name_sym == NULL) return URBI_ERR_OOM;
    int rc = urbi_object_set_local_slot(vm, inst, name_sym, name_val);
    if (rc != 0) return URBI_ERR_OOM;

    /* cerr: set instance.quote = false. */
    if (set_quote_false) {
        UValue false_val = urbi_make_bool(false);
        USymbol *quote_sym = (USymbol *)ustr_intern(vm, "quote", 5);
        if (quote_sym == NULL) return URBI_ERR_OOM;
        rc = urbi_object_set_local_slot(vm, inst, quote_sym, false_val);
        if (rc != 0) return URBI_ERR_OOM;
    }

    UValue inst_val;
    urbi_zero(&inst_val, sizeof(inst_val));
    inst_val.kind = (uint8_t)UVAL_OBJECT;
    inst_val.v.p  = (void *)inst;
    return urbi_realm_set_global(vm, realm, gname, glen, inst_val);
}

/* === urbi_channel_register_globals =====================================
 *
 * Bind Channel + cout/cerr/clog as realm globals.  Called per-realm
 * from urbi_populate_realm_globals AFTER urbi_channel_proto_resolve. */

int
urbi_channel_register_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->channel_proto == NULL) return URBI_OK;   /* not yet resolved */

    /* Bind "Channel" realm-global pointing at the cached proto. */
    UValue cv;
    urbi_zero(&cv, sizeof(cv));
    cv.kind = (uint8_t)UVAL_OBJECT;
    cv.v.p  = (void *)vm->channel_proto;
    int rc = urbi_realm_set_global(vm, realm, "Channel", 7, cv);
    if (rc != URBI_OK) return rc;

    /* Per-realm cout/cerr/clog instances. */
    rc = install_channel_instance(vm, realm, "cout", 4, "output", 6, false);
    if (rc != URBI_OK) return rc;
    rc = install_channel_instance(vm, realm, "cerr", 4, "error",  5, true);
    if (rc != URBI_OK) return rc;
    rc = install_channel_instance(vm, realm, "clog", 4, "clog",   4, false);
    if (rc != URBI_OK) return rc;

    return URBI_OK;
}
