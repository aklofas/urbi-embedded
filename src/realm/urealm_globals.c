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

#include "realm/urealm_globals.h"
#include "runtime/umacros.h"  /* urbi_strlen */
#include "vm/uvm.h"              /* UVM, atom_* fields, event_proto, tag_proto, urbi_native_protos_init */
#include "realm/urealm.h"     /* URealm, global_object */
#include "value/uintern.h"          /* ustr_intern */
#include "object/uobject.h"   /* urbi_object_root, urbi_object_atom, urbi_object_set_local_slot,
                               *   urbi_object_install_property */
#include "object/ushape.h"    /* urbi_shape_find_slot */
#include "stdlib/stdlib_boot.h" /* urbi_stdlib_boot — M6 Phase 3 */
#include "stdlib/containers.h"  /* urbi_stdlib_register_container_globals — M6 Phase 6 */
#include "stdlib/runtime_types.h"  /* urbi_stdlib_register_runtime_globals — M6 Phase 7 */
#include "stdlib/namespaces.h"     /* urbi_stdlib_register_namespace_globals — M6 Phase 8 */
#include "stdlib/primitives.h"     /* urbi_stdlib_register_primitives_globals — M6 Phase 9 */
#include "stdlib/job_proto.h"      /* urbi_job_proto_register_globals — v0.10.10 D7-A */
#include "stdlib/lobby_native.h"   /* urbi_lobby_native_register_globals — v0.9.1 Phase 5 */
#include "stdlib/temporal.h"       /* urbi_temporal_native_register_globals — v0.9.4 Phase 5 */
#include "stdlib/control_native.h" /* urbi_control_native_register_globals — v0.10.10 D7-C */
#ifdef URBI_ENABLE_REPL
#  include "stdlib/debug_namespace.h" /* urbi_debug_namespace_register_globals — v0.9.1 */
#endif
#include "urbi/urbi.h"        /* UErrCode, URBI_OK, URBI_ERR_OOM */
#include "urbi/object.h"      /* URBI_ATOM_* family tags */
#include "chunk/uchunk.h"
#include <stdint.h>

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
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_INTEGER));
}

static UValue
resolve_atom_float(UVM *vm)
{
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_FLOAT));
}

static UValue
resolve_atom_string(UVM *vm)
{
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_STRING));
}

/* M6 Phase 4 (T48): Boolean / Nil / Void atom protos now exist.
 * Each resolver lazy-allocates the corresponding atom singleton
 * (urbi_object_atom is idempotent — replaces the M5 baseline
 * resolve_nil_placeholder used at REALM-018). */
static UValue
resolve_atom_boolean(UVM *vm)
{
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_BOOLEAN));
}

static UValue
resolve_atom_nil_proto(UVM *vm)
{
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_NIL));
}

static UValue
resolve_atom_void_proto(UVM *vm)
{
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_VOID));
}

static UValue
resolve_atom_list(UVM *vm)
{
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_LIST));
}

static UValue
resolve_atom_dict(UVM *vm)
{
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_DICT));
}

static UValue
resolve_atom_symbol(UVM *vm)
{
    return rg_make_object(urbi_object_atom(vm, URBI_ATOM_SYMBOL));
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

    /* Boolean / Nil / Void: M6 Phase 4 promoted the M5 placeholders to
     * real atom protos.  The "Boolean" name replaces the M5 placeholder
     * "Bool" (legacy precedent + spec §5.2 boot order spell out the
     * full name).  Note the case distinction: lowercase `nil` / `void`
     * are the value singletons (UVAL_NIL / UVAL_VOID), while
     * `Nil` / `Void` are the protos — separate rows below. */
    { "Boolean", resolve_atom_boolean,    true,  false },
    { "Nil",     resolve_atom_nil_proto,  true,  false },
    { "Void",    resolve_atom_void_proto, true,  false },

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

/* === REALM-023 / T70: realm_install_const shared helper ===
 *
 * Common CONSTANT-install logic shared by urbi_populate_realm_globals (the
 * 15-row registry-driven realm boot path) and urbi_realm_set_global_const
 * (the public C API).  Both call sites previously open-coded the same
 * three-step pattern (already-CONSTANT check → set_local_slot →
 * install_property with URBI_SLOT_FLAG_CONSTANT) — keeping them in lockstep
 * is important because the T67 CONST-overwrite guard MUST apply to BOTH
 * paths consistently (the public API can be called BEFORE populate
 * finishes via host-supplied UVMAllocFn callbacks; future v1.x multi-realm
 * extension may also re-enter populate after public installs).
 *
 * Behaviour:
 *   1. If `reject_if_already_const` is true and the slot already exists
 *      and its CONSTANT bit is set (packed-nibble for slots 0..7,
 *      UProps.constant for slots >= 8), return URBI_ERR_CONST_SLOT_WRITE.
 *      The populate path passes false here — populate iterates a fresh
 *      global_object whose slots do not pre-exist; the public API passes
 *      true so host overwrites of registry-installed constants like
 *      "Object" are rejected.
 *   2. Call urbi_object_set_local_slot to materialise / update the slot.
 *      OOM here returns URBI_ERR_OOM.
 *   3. Call urbi_object_install_property with URBI_SLOT_FLAG_CONSTANT to
 *      flag the slot as constant.  At slots 0..7 this writes the CONSTANT
 *      bit into the packed shape nibble (IC-enforced for script writes);
 *      at slots >= 8 the bit lives only in UProps (not IC-enforced at
 *      v1.0 baseline; M6 spill side-table will lift the cap).  OOM here
 *      returns URBI_ERR_OOM. */
static int
realm_install_const(UVM *vm, URealm *realm, USymbol *sym, UValue value,
                    bool reject_if_already_const)
{
    if (reject_if_already_const) {
        int32_t existing =
            urbi_shape_find_slot(realm->global_object->shape, sym);
        if (existing >= 0) {
            bool already_const = false;
            if (existing < 8) {
                const uint32_t shift      = (uint32_t)existing * 4U;
                const uint32_t old_nibble =
                    (realm->global_object->shape->flags >> shift) & 0xFU;
                already_const = (old_nibble & URBI_SLOT_FLAG_CONSTANT) != 0U;
            } else if (realm->global_object->shape->props_table != NULL) {
                const UProps *p =
                    realm->global_object->shape->props_table[existing];
                already_const = (p != NULL) && (p->constant != 0U);
            }
            if (already_const) {
                return URBI_ERR_CONST_SLOT_WRITE;
            }
        }
    }

    int rc = urbi_object_set_local_slot(vm, realm->global_object, sym, value);
    if (rc != 0) {
        return URBI_ERR_OOM;
    }
    /* Gate install_property on slot < 8: the v1.0 packed-nibble form of
     * UShape.flags is only 4 bits/slot across a single uint32_t (8 slots
     * worth).  urbi_shape_transition_property's bit-shift arithmetic
     * (shift = slot_index * 4) is undefined behaviour at slot_index >= 8
     * — UB that pre-T70 populate's `idx < 8` gate suppressed.  The M6
     * spill side-table will lift the cap; until then this gate is the
     * single source of truth.
     *
     * Caller observability: the slot is still locally installed via
     * set_local_slot above (its value is reachable by name); only the
     * CONSTANT bit is dropped on the floor.  Public-API callers that
     * need true CONSTANT enforcement past slot 7 must wait for M6 — the
     * v1.0 limitation is tracked in the design-risks register (S-globals-cap-8). */
    int32_t idx = urbi_shape_find_slot(realm->global_object->shape, sym);
    if (idx >= 0 && idx < 8) {
        rc = urbi_object_install_property(vm, realm->global_object, sym,
                                          URBI_SLOT_FLAG_CONSTANT, value);
        if (rc != 0) {
            return URBI_ERR_OOM;
        }
    }
    return URBI_OK;
}

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

    /* M6 Phase 3: register Object root C-native methods on vm->atom_object
     * BEFORE the resolver loop installs Object as a realm global.  This way
     * the realm-global "Object" already carries setSlot/getSlot/clone/etc.
     * for the very first urbiscript chunk that references it.  Idempotent:
     * vm->stdlib_booted gates re-entry. */
    {
        UErrCode rc = (UErrCode)urbi_stdlib_boot(vm);
        if (rc != URBI_OK) {
            return rc;
        }
    }

    for (i = 0; i < urbi_builtin_registry_count; i++) {
        const URegistryEntry *e = &urbi_builtin_registry[i];

        /* Resolve value; override for the self-loop case (REALM-028).
         * When is_self_ref is set (only the "Realm" entry), the resolver
         * return value is intentionally discarded — the slot must point at
         * realm->global_object for the spec §3.3 self-loop.  The resolver
         * is still called to keep the loop uniform; its nil return is a
         * harmless default that is never used by the slot install below. */
        UValue v = e->resolver(vm);
        if (e->is_self_ref) {
            /* "Realm" entry: point at this realm's global_object. */
            v = rg_make_object(realm->global_object);
        } else if (v.kind == UVAL_OBJECT && v.v.p == NULL) {
            /* Resolver returned NULL — OOM from a lazy-init function. */
            return URBI_ERR_OOM;
        }

        /* Intern the name to get a canonical USymbol pointer. */
        USymbol *sym = (USymbol *)ustr_intern(vm, e->name, urbi_strlen(e->name));
        if (sym == NULL) {
            return URBI_ERR_OOM;
        }

        /* T70: route CONSTANT installs through the shared helper so the
         * populate path and the public set_global_const API share the
         * same set_local_slot → install_property sequence.  populate
         * iterates a fresh global_object whose slots do not pre-exist
         * yet, so the already-const check is unnecessary here and we
         * pass reject_if_already_const = false.  Non-const entries
         * (none in the v1.0 registry — every row has is_const = true,
         * but the field is honoured for forward-compat) take the simple
         * set_local_slot path. */
        if (e->is_const) {
            int rc = realm_install_const(vm, realm, sym, v,
                                         /*reject_if_already_const=*/false);
            if (rc != URBI_OK) {
                return rc;
            }
        } else {
            int rc = urbi_object_set_local_slot(vm, realm->global_object,
                                                sym, v);
            if (rc != 0) {
                return URBI_ERR_OOM;
            }
        }
    }

    /* M6 Phase 6: post-registry container globals (Pair / Triplet / Tuple).
     * Lands at slots 15+, past the v1.0 packed-flag CONSTANT enforcement
     * range (slots 0..7), so it cannot displace the registry's stable
     * Object..List layout. */
    {
        int rc = urbi_stdlib_register_container_globals(vm, realm);
        if (rc != URBI_OK) {
            return (UErrCode)rc;
        }
    }

    /* M6 Phase 7: post-registry runtime-type globals (Exception).  Same
     * post-loop pattern as containers — lands at slots 15+, past the
     * v1.0 packed-flag CONSTANT enforcement range. */
    {
        int rc = urbi_stdlib_register_runtime_globals(vm, realm);
        if (rc != URBI_OK) {
            return (UErrCode)rc;
        }
    }

    /* M6 Phase 8: post-registry namespace globals (Math / System /
     * Global / CallMessage).  Same post-loop pattern.  Note: Platform
     * is nested as a slot on System, NOT a top-level realm global —
     * scripts access it as System.Platform.kind. */
    {
        int rc = urbi_stdlib_register_namespace_globals(vm, realm);
        if (rc != URBI_OK) {
            return (UErrCode)rc;
        }
    }

    /* M6 Phase 9: post-registry primitive globals (Mutex / Date /
     * Duration).  Same post-loop pattern. */
    {
        int rc = urbi_stdlib_register_primitives_globals(vm, realm);
        if (rc != URBI_OK) {
            return (UErrCode)rc;
        }
    }

    /* v0.9.1 Phase 5: bind Lobby as a realm global.  Same post-loop
     * pattern — slot 15+, past the v1.0 packed-flag CONSTANT enforcement
     * range.  The proto carries __builtin_lobby_send (installed at
     * stdlib_boot); the script-side echo/wall/handleDisconnect/lobbies/
     * onDisconnect slots are added by the lobby.u overlay run in the
     * deferred urbi_run_chunk step below. */
    {
        int rc = urbi_lobby_native_register_globals(vm, realm);
        if (rc != URBI_OK) {
            return (UErrCode)rc;
        }
    }

    /* v0.9.4 Phase 5: bind "every" as a realm global pointing at
     * vm->every_native_closure.  Same post-loop pattern. */
    {
        int rc = urbi_temporal_native_register_globals(vm, realm);
        if (rc != URBI_OK) {
            return (UErrCode)rc;
        }
    }

    /* v0.10.10 / D7-A: bind "Job" as a realm global pointing at
     * vm->job_proto.  Same post-loop pattern — slot 15+, past the
     * v1.0 packed-flag CONSTANT enforcement range. */
    {
        int rc = urbi_job_proto_register_globals(vm, realm);
        if (rc != URBI_OK) {
            return (UErrCode)rc;
        }
    }

    /* v0.10.10 / D7-C: bind __detach_strand + __disown_strand as realm
     * globals backing the detach/disown overlay wrappers. */
    {
        int rc = urbi_control_native_register_globals(vm, realm);
        if (rc != URBI_OK) {
            return (UErrCode)rc;
        }
    }

#ifdef URBI_ENABLE_REPL
    /* v0.9.1: bind Debug namespace as a realm global.  No-op if
     * urbi_debug_namespace_register has not been called yet (the proto
     * is allocated lazily from stdlib_boot). */
    {
        int rc = urbi_debug_namespace_register_globals(vm, realm);
        if (rc != URBI_OK) {
            return (UErrCode)rc;
        }
    }
#endif

    /* M6 Phase 10: run the baked-in stdlib bytecode chunk.  Top-level
     * statements (currently only `class X : public Y {}` declarations)
     * install themselves as realm globals at this point — the C-native
     * registry is fully populated, so resolved-name references inside the
     * .u source (e.g. `public Exception`) walk the same realm-global
     * lookup that any user chunk does.
     *
     * This step is gated on vm->stdlib_module being non-NULL — empty
     * STDLIB_ORDER.txt → no module → skip cleanly.  We pass the realm
     * directly (not NULL) because urbi_run_chunk's NULL-realm path calls
     * urbi_realm_global(vm) which would recurse back into
     * urbi_realm_create / urbi_populate_realm_globals while the global
     * Realm is mid-population.
     *
     * The class-decl emit path writes Foo into the realm-global slot
     * directly via OP_SETSLOT on global_object — no further wiring
     * needed here. */
    if (vm->stdlib_module != NULL) {
        UValue out;
        int rc = urbi_run_chunk(vm, realm, vm->stdlib_module, &out);
        if (rc != URBI_OK) {
            return (UErrCode)rc;
        }
    }

    return URBI_OK;
}

/* === M5 public C API: realm global slot install / read (spec #5 §7) ===
 *
 * Three thin wrappers over ustr_intern + urbi_object_set_local_slot /
 * urbi_object_install_property / urbi_object_resolve_slot.  The
 * implementations use urbi_strlen (from runtime/umacros.h) for the
 * freestanding (no <string.h>) discipline. */

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
    /* REALM-004 / T68: a non-const set_global on an existing CONSTANT slot
     * must also reject — otherwise the host could bypass CONSTANT via the
     * non-const variant.  Same packed-nibble + UProps inspection as
     * set_global_const (T67). */
    int32_t existing = urbi_shape_find_slot(realm->global_object->shape, sym);
    if (existing >= 0) {
        bool already_const = false;
        if (existing < 8) {
            const uint32_t shift      = (uint32_t)existing * 4U;
            const uint32_t old_nibble =
                (realm->global_object->shape->flags >> shift) & 0xFU;
            already_const = (old_nibble & URBI_SLOT_FLAG_CONSTANT) != 0U;
        } else if (realm->global_object->shape->props_table != NULL) {
            const UProps *p =
                realm->global_object->shape->props_table[existing];
            already_const = (p != NULL) && (p->constant != 0U);
        }
        if (already_const) {
            return URBI_ERR_CONST_SLOT_WRITE;
        }
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
    /* T70: shared helper does the already-CONSTANT check (T67),
     * set_local_slot, and install_property in lockstep.  Public-API
     * callers MUST reject overwrites of existing CONSTANT slots —
     * otherwise host code could silently bypass the CONSTANT flag. */
    return realm_install_const(vm, realm, sym, value,
                               /*reject_if_already_const=*/true);
}

int
urbi_realm_get_global(UVM *vm, URealm *realm,
                      const char *name, size_t name_len, UValue *out_value)
{
    if (vm == NULL || realm == NULL || realm->global_object == NULL ||
            name == NULL || out_value == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    const USymbol *sym = (const USymbol *)ustr_intern(vm, name, name_len);
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
    /* found == -1: prototype-graph DFS exhausted the fixed 64-deep resolve
     * stack at uobject_slot.c:561.  REALM-010 / T68: this is distinct from
     * OOM (no allocation has been attempted on this path) — surface it as
     * URBI_ERR_PROTO_DEPTH so callers can disambiguate "your prototype
     * graph is too wide/deep" from "the host is out of memory". */
    return URBI_ERR_PROTO_DEPTH;
}
