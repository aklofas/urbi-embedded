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
#include "uvm.h"              /* UVM, atom_* fields, event_proto, tag_proto, urbi_native_protos_init */
#include "realm/urealm.h"     /* URealm, global_object */
#include "uintern.h"          /* ustr_intern */
#include "object/uobject.h"   /* urbi_object_root, urbi_object_atom, urbi_object_set_local_slot,
                               *   urbi_object_install_property */
#include "object/ushape.h"    /* urbi_shape_find_slot */
#include "urbi/urbi.h"        /* UErrCode, URBI_OK, URBI_ERR_OOM */
#include "urbi/object.h"      /* URBI_ATOM_*_F family tags */

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
 * Each resolver triggers lazy initialization of the VM singleton it wraps.
 * urbi_object_root / urbi_object_atom allocate on first call and are
 * idempotent thereafter.  Returning rg_make_object(NULL) signals OOM to the
 * populate loop, which returns URBI_ERR_OOM.
 *
 * Resolvers for atom protos that do not exist at M5 baseline (Bool, Nil, Void)
 * return rg_make_nil() as a placeholder per spec #5 §3.2 note — these
 * singletons land in M6 stdlib. */

static UValue
resolve_object_proto(UVM *vm)
{
    /* urbi_object_root lazy-allocates vm->atom_object on first call. */
    return rg_make_object(urbi_object_root(vm));
}

static UValue
resolve_atom_int(UVM *vm)
{
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_INTEGER_F));
}

static UValue
resolve_atom_float(UVM *vm)
{
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_FLOAT_F));
}

static UValue
resolve_atom_string(UVM *vm)
{
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_STRING_F));
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
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_LIST_F));
}

static UValue
resolve_atom_dict(UVM *vm)
{
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_DICT_F));
}

static UValue
resolve_atom_symbol(UVM *vm)
{
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_SYMBOL_F));
}

static UValue
resolve_tag_proto(UVM *vm)
{
    /* vm->tag_proto is populated by urbi_native_protos_init, which
     * urbi_populate_realm_globals calls before entering the resolver loop. */
    return rg_make_object(vm->tag_proto);
}

static UValue
resolve_event_proto(UVM *vm)
{
    /* vm->event_proto is populated by urbi_native_protos_init, same as above. */
    return rg_make_object(vm->event_proto);
}

/* Realm self-reference: value overridden by the populate loop using is_self_ref. */
static UValue
resolve_realm_self(UVM *vm)
{
    /* The is_self_ref flag in the registry causes urbi_populate_realm_globals
     * to replace this value with realm->global_object.  Returning nil here
     * is just a safe default; the value is never used by the loop. */
    (void)vm;
    return rg_make_nil();
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

/* is_self_ref column: true only for "Realm" — the populate loop replaces
 * its value with realm->global_object instead of using the resolver result. */
const URegistryEntry urbi_builtin_registry[] = {
    /* is_const, is_self_ref */
    /* Root object model */
    { "Object",  resolve_object_proto, true,  false },

    /* Atom prototypes */
    { "Integer", resolve_atom_int,     true,  false },
    { "Float",   resolve_atom_float,   true,  false },
    { "String",  resolve_atom_string,  true,  false },

    /* Bool/Nil/Void: no singleton at M5 baseline; resolver returns nil.
     * These entries still occupy registry slots so names are reserved in
     * the global namespace and M6 stdlib can overwrite them. */
    { "Bool",    resolve_atom_bool,    true,  false },
    { "Nil",     resolve_atom_nil,     true,  false },
    { "Void",    resolve_atom_void,    true,  false },

    { "List",    resolve_atom_list,    true,  false },
    { "Dict",    resolve_atom_dict,    true,  false },
    { "Symbol",  resolve_atom_symbol,  true,  false },

    /* Reactive built-ins (spec #3) */
    { "Tag",     resolve_tag_proto,    true,  false },
    { "Event",   resolve_event_proto,  true,  false },

    /* Reflective self-reference — is_self_ref=true → loop uses global_object */
    { "Realm",   resolve_realm_self,   true,  true  },

    /* Value singletons */
    { "nil",     resolve_value_nil,    true,  false },
    { "void",    resolve_value_void,   true,  false },
};

const size_t urbi_builtin_registry_count =
    sizeof(urbi_builtin_registry) / sizeof(urbi_builtin_registry[0]);

/* === urbi_populate_realm_globals (spec #5 §4) ===
 *
 * Iterates the registry, resolves each value, interns the name,
 * and installs a constant slot on realm->global_object.
 *
 * The "Realm" entry's is_self_ref flag causes its value to be replaced with
 * realm->global_object (the self-loop; spec #5 §3.3).
 *
 * Calls urbi_native_protos_init(vm) on first call (guarded by vm->event_proto
 * being NULL) so that resolve_tag_proto / resolve_event_proto see live
 * pointers.  This is the wiring described in uvm.c §T59 comment. */

UErrCode
urbi_populate_realm_globals(UVM *vm, URealm *realm)
{
    size_t i;

    if (vm == NULL || realm == NULL || realm->global_object == NULL) {
        return URBI_ERR_INVALID_ARG;
    }

    /* Ensure event_proto + tag_proto are allocated.  Idempotent: guarded by
     * the NULL check inside event_native_register / tag_native_register.
     * After this call vm->atom_object is also live (urbi_object_alloc in
     * event/tag_native_register drives urbi_shape_root which allocates the
     * root shape, and urbi_object_root is called by resolve_object_proto
     * inside the resolver loop below). */
    if (vm->event_proto == NULL) {
        urbi_native_protos_init(vm);
    }

    for (i = 0; i < urbi_builtin_registry_count; i++) {
        const URegistryEntry *e = &urbi_builtin_registry[i];

        /* Resolve value; override for the self-loop case. */
        UValue v = e->resolver(vm);
        if (e->is_self_ref) {
            /* "Realm" entry: point at this realm's global_object. */
            v = rg_make_object(realm->global_object);
        } else if (v.kind == UVAL_OBJECT && v.v.p == NULL) {
            /* Resolver returned NULL — OOM from a lazy-init function. */
            return URBI_ERR_OOM;
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

        /* Mark constant via the packed flags (slots 0..7 only).
         * UShape.flags packs 4 bits/slot in a 32-bit word — the v1.0 cap of
         * 8 slots in packed form (T15 spill side-table deferred to later).
         * For slot indices 0..7 we set CONSTANT via install_property which
         * uses UProps; for indices >= 8 we defer the const marking to the
         * M6 side-table tier.  The is_const flag in the registry is the
         * authoritative source; the runtime IC path only checks packed flags
         * so indices 8..14 are not write-protected at M5 baseline. */
        if (e->is_const) {
            int32_t idx = urbi_shape_find_slot(realm->global_object->shape, sym);
            if (idx >= 0 && idx < 8) {
                rc = urbi_object_install_property(vm, realm->global_object, sym,
                                                  URBI_SLOT_FLAG_CONSTANT,
                                                  v /* payload unused for CONSTANT */);
                if (rc != 0) {
                    return URBI_ERR_OOM;
                }
            }
        }
    }

    return URBI_OK;
}

/* === M5 public C API: realm global slot install / read (spec #5 §7) ===
 *
 * Three thin wrappers over ustr_intern + urbi_object_set_local_slot /
 * urbi_object_install_property / urbi_object_resolve_slot.  The
 * implementations reuse rg_strlen for the freestanding (no <string.h>) discipline. */

int
urbi_realm_set_global(UVM *vm, URealm *realm,
                      const char *name, size_t name_len, UValue value)
{
    if (vm == NULL || realm == NULL || realm->global_object == NULL ||
            name == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    USymbol *sym = (USymbol *)ustr_intern(vm, name, name_len);
    if (sym == NULL) {
        return URBI_ERR_OOM;
    }
    int rc = urbi_object_set_local_slot(vm, realm->global_object, sym, value);
    return (rc == 0) ? URBI_OK : URBI_ERR_OOM;
}

int
urbi_realm_set_global_const(UVM *vm, URealm *realm,
                             const char *name, size_t name_len, UValue value)
{
    if (vm == NULL || realm == NULL || realm->global_object == NULL ||
            name == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    USymbol *sym = (USymbol *)ustr_intern(vm, name, name_len);
    if (sym == NULL) {
        return URBI_ERR_OOM;
    }
    int rc = urbi_object_set_local_slot(vm, realm->global_object, sym, value);
    if (rc != 0) {
        return URBI_ERR_OOM;
    }
    rc = urbi_object_install_property(vm, realm->global_object, sym,
                                      URBI_SLOT_FLAG_CONSTANT, value);
    return (rc == 0) ? URBI_OK : URBI_ERR_OOM;
}

int
urbi_realm_get_global(UVM *vm, URealm *realm,
                      const char *name, size_t name_len, UValue *out_value)
{
    if (vm == NULL || realm == NULL || realm->global_object == NULL ||
            name == NULL || out_value == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    USymbol *sym = (USymbol *)ustr_intern(vm, name, name_len);
    if (sym == NULL) {
        return URBI_ERR_OOM;
    }
    UObject  *holder = NULL;
    uint32_t  idx    = 0;
    int found = urbi_object_resolve_slot(vm, realm->global_object, sym,
                                         &holder, &idx);
    if (found == 1) {
        *out_value = holder->slots[idx];
        return URBI_OK;
    }
    if (found == 0) {
        return URBI_ERR_SLOT_NOT_FOUND;
    }
    /* found == -1: resolve-stack depth overflow or other error */
    return URBI_ERR_OOM;
}
