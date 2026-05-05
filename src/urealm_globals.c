/* SPDX-License-Identifier: BSD-3-Clause */
/* urealm_globals.c — static built-in name registry + realm populate routine.
 *
 * Pre-M5 spec #5 §3–§4.  Row 13 / T69-T70.
 *
 * 15 built-ins registered at every realm creation:
 *   Object, Integer, Float, String, Bool, Nil, Void, List, Dict, Symbol,
 *   Tag, Event, Realm (self-ref), nil, void.
 *
 * Freestanding discipline: no <stdlib.h>.  Uses vm->alloc_fn indirectly via
 * ustr_intern and urbi_object_set_local_slot. */

#include <stddef.h>
#include <stdbool.h>

#include "urealm_globals.h"
#include "uvm.h"              /* UVM, atom_* fields, event_proto, tag_proto */
#include "realm/urealm.h"     /* URealm, global_object */
#include "uintern.h"          /* ustr_intern */
#include "object/uobject.h"   /* urbi_object_set_local_slot, urbi_object_install_property */
#include "urbi/urbi.h"        /* UErrCode, URBI_OK, URBI_ERR_OOM */

/* === Static string-length helper (avoids <string.h>) === */

static size_t
rg_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* === Zero-fill helper for UValue padding bytes === */

static UValue
rg_make_object(void *p)
{
    UValue v;
    int i;
    v.kind = UVAL_OBJECT;
    for (i = 0; i < 7; i++) v._pad[i] = 0;
    v.v.p = p;
    return v;
}

static UValue
rg_make_nil(void)
{
    UValue v;
    int i;
    v.kind = UVAL_NIL;
    for (i = 0; i < 7; i++) v._pad[i] = 0;
    v.v.i = 0;
    return v;
}

static UValue
rg_make_void(void)
{
    UValue v;
    int i;
    v.kind = UVAL_VOID;
    for (i = 0; i < 7; i++) v._pad[i] = 0;
    v.v.i = 0;
    return v;
}

/* === Resolver functions (15 total) ===
 *
 * Each returns a UValue wrapping the corresponding VM singleton.
 * Resolvers for atom protos that do not exist at M5 baseline
 * (Bool, Nil, Void) return uvalue_nil() as a placeholder per
 * spec #5 §3.2 note — these singletons land in M6 stdlib. */

static UValue
resolve_object_proto(UVM *vm)
{
    return rg_make_object(vm->atom_object);
}

static UValue
resolve_atom_int(UVM *vm)
{
    return rg_make_object(vm->atom_integer);
}

static UValue
resolve_atom_float(UVM *vm)
{
    return rg_make_object(vm->atom_float);
}

static UValue
resolve_atom_string(UVM *vm)
{
    return rg_make_object(vm->atom_string);
}

/* Bool: no atom_bool at M5 baseline → return nil placeholder. */
static UValue
resolve_atom_bool(UVM *vm)
{
    (void)vm;
    return rg_make_nil();
}

/* Nil proto: no atom_nil at M5 baseline → return nil placeholder. */
static UValue
resolve_atom_nil(UVM *vm)
{
    (void)vm;
    return rg_make_nil();
}

/* Void proto: no atom_void at M5 baseline → return nil placeholder. */
static UValue
resolve_atom_void(UVM *vm)
{
    (void)vm;
    return rg_make_nil();
}

static UValue
resolve_atom_list(UVM *vm)
{
    return rg_make_object(vm->atom_list);
}

static UValue
resolve_atom_dict(UVM *vm)
{
    return rg_make_object(vm->atom_dict);
}

static UValue
resolve_atom_symbol(UVM *vm)
{
    return rg_make_object(vm->atom_symbol);
}

static UValue
resolve_tag_proto(UVM *vm)
{
    return rg_make_object(vm->tag_proto);
}

static UValue
resolve_event_proto(UVM *vm)
{
    return rg_make_object(vm->event_proto);
}

/* Realm self-reference: sentinel; urbi_populate_realm_globals overrides. */
static UValue
resolve_realm_self(UVM *vm)
{
    /* Filled in by urbi_populate_realm_globals with the actual global_object.
     * Return a UVAL_OBJECT with p=NULL as a sentinel; the populate loop
     * detects NULL and substitutes realm->global_object. */
    (void)vm;
    return rg_make_object(NULL);
}

/* Value singleton nil (the scripting nil value, not the nil proto). */
static UValue
resolve_value_nil(UVM *vm)
{
    (void)vm;
    return rg_make_nil();
}

/* Value singleton void. */
static UValue
resolve_value_void(UVM *vm)
{
    (void)vm;
    return rg_make_void();
}

/* === The registry table (spec #5 §3.1) === */

const URegistryEntry urbi_builtin_registry[] = {
    /* Root object model */
    { "Object",  resolve_object_proto, true },

    /* Atom prototypes */
    { "Integer", resolve_atom_int,     true },
    { "Float",   resolve_atom_float,   true },
    { "String",  resolve_atom_string,  true },

    /* Bool/Nil/Void: no singleton at M5 baseline; resolver returns nil.
     * These entries still occupy registry slots so names are reserved in
     * the global namespace and M6 stdlib can overwrite them. */
    { "Bool",    resolve_atom_bool,    true },
    { "Nil",     resolve_atom_nil,     true },
    { "Void",    resolve_atom_void,    true },

    { "List",    resolve_atom_list,    true },
    { "Dict",    resolve_atom_dict,    true },
    { "Symbol",  resolve_atom_symbol,  true },

    /* Reactive built-ins (spec #3) */
    { "Tag",     resolve_tag_proto,    true },
    { "Event",   resolve_event_proto,  true },

    /* Reflective self-reference */
    { "Realm",   resolve_realm_self,   true },

    /* Value singletons */
    { "nil",     resolve_value_nil,    true },
    { "void",    resolve_value_void,   true },
};

const size_t urbi_builtin_registry_count =
    sizeof(urbi_builtin_registry) / sizeof(urbi_builtin_registry[0]);

/* === urbi_populate_realm_globals (spec #5 §4) ===
 *
 * Iterates the registry, resolves each value, interns the name,
 * and installs a constant slot on realm->global_object.
 *
 * The "Realm" entry receives a special override: its value is
 * realm->global_object itself (the self-loop; spec #5 §3.3). */

UErrCode
urbi_populate_realm_globals(UVM *vm, URealm *realm)
{
    size_t i;

    if (vm == NULL || realm == NULL || realm->global_object == NULL) {
        return URBI_ERR_INVALID_ARG;
    }

    for (i = 0; i < urbi_builtin_registry_count; i++) {
        const URegistryEntry *e = &urbi_builtin_registry[i];

        /* Resolve value; override for the self-loop case. */
        UValue v = e->resolver(vm);
        if (v.kind == UVAL_OBJECT && v.v.p == NULL) {
            /* resolve_realm_self sentinel: point at global_object. */
            v.v.p = realm->global_object;
        }

        /* Intern the name to get a canonical USymbol pointer. */
        USymbol *sym = (USymbol *)ustr_intern(vm, e->name, rg_strlen(e->name));
        if (sym == NULL) {
            return URBI_ERR_OOM;
        }

        /* Install the slot. */
        int rc = urbi_object_set_local_slot(vm, realm->global_object, sym, v);
        if (rc != 0) {
            return URBI_ERR_OOM;
        }

        /* Mark constant if required. */
        if (e->is_const) {
            rc = urbi_object_install_property(vm, realm->global_object, sym,
                                              URBI_SLOT_FLAG_CONSTANT,
                                              v /* payload unused for CONSTANT */);
            if (rc != 0) {
                return URBI_ERR_OOM;
            }
        }
    }

    return URBI_OK;
}
