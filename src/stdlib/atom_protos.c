/* SPDX-License-Identifier: BSD-3-Clause */
/* atom_protos.c — atom proto C-native method stubs.
 *
 * Each atom family (Boolean, Integer, Float, String, nil, void) gets a
 * minimum method set here.  The full Tier 1 method sets (logical ops on
 * Boolean, arithmetic on Integer/Float, encode/decode on String, etc.)
 * are installed by the atom-method modules (src/stdlib/atoms.c).
 *
 * Equality stays in OP_EQ at the VM level — atom == atom is dispatched
 * without slot lookup, so no per-family `==` install is needed.
 *
 * The Integer / Float / Nil / Void protos exist but inherit clone +
 * getSlot / setSlot / etc. from the Object root via the prototype chain
 * (each non-root atom singleton's protos field points at root Object
 * per src/object/uobject.c urbi_object_atom). */

#include "stdlib/atom_protos.h"
#include "stdlib/object_root.h"

#include "chunk/uchunk.h"        /* UValue, UVAL_*, UClosure typedef */
#include "object/uobject.h"        /* urbi_object_*, urbi_object_atom */
#include "runtime/uclosure.h"      /* struct UClosure full def + native_method_fn typedef */
#include "runtime/umacros.h"       /* urbi_strlen */
#include "sched/ustrand.h"         /* UEXEC_OK, UEXEC_THROW */
#include "urbi/object.h"           /* URBI_ATOM_* family tags */
#include "urbi/types.h"            /* UErrCode, urbi_make_nil */
#include "urbi/urbi.h"             /* URBI_OK, URBI_ERR_OOM */
#include "value/uintern.h"         /* ustr_intern + USymbol */
#include "vm/uvm.h"                /* UVM */

#include <stdint.h>
#include <stddef.h>

/* === Boolean.toString — true/false → "true"/"false" ====================== */

static int
bool_toString(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Boolean.toString", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_BOOL)
        return urbi_raise_type(vm, "Boolean.toString: self must be Boolean", out);

    const char *s = (self.v.i != 0) ? "true" : "false";
    USymbol *sym = (USymbol *)ustr_intern(vm, s, urbi_strlen(s));
    if (sym == NULL) return urbi_raise_oom(vm, out);

    *out = urbi_make_nil();
    out->kind = (uint8_t)UVAL_STR;
    out->v.p = sym;
    return UEXEC_OK;
}

/* === String.length — byte count of the interned symbol ==================
 *
 * UVAL_STR.v.p is a NUL-terminated `const char *` returned by ustr_intern;
 * its byte count is recoverable by urbi_strlen.  The interned-table entry
 * (UInternStr) carries a precomputed length, but its struct definition
 * is private to src/value/uintern.c; routing through urbi_strlen keeps
 * stdlib/ free of intern-table internals.  Strings are short at v1.0 (no
 * known fixture exceeds a few dozen bytes), so the linear walk is
 * negligible. */

static int
string_length(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "String.length", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.length: self must be String", out);

    const char *s = (const char *)self.v.p;
    if (s == NULL) return urbi_raise_type(vm, "String.length: NULL string", out);

    *out = urbi_make_nil();
    out->kind = (uint8_t)UVAL_INT;
    out->v.i = (int64_t)urbi_strlen(s);
    return UEXEC_OK;
}

/* Per-family method tables (UNativeMethodDef from stdlib/object_root.h). */

static const UNativeMethodDef BOOL_METHODS[] = {
    { "toString", bool_toString }
};

static const UNativeMethodDef STRING_METHODS[] = {
    { "length",   string_length }
};

/* === urbi_atom_protos_register ========================================== */

int
urbi_atom_protos_register(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;

    /* Allocate singletons (lazy-init via urbi_object_atom).  Boolean /
     * String are populated below; Integer / Float / Nil / Void are
     * touched here so the singletons exist at boot time even though no
     * family-specific methods install on them in this module — they inherit
     * clone / setSlot / etc. from root Object via the proto chain.
     *
     * The Integer/Float/Nil/Void protos are pointer-to-const here because
     * this module does not write to them (the family-specific Integer /
     * Float / String methods install from src/stdlib/atoms.c on separately
     * obtained mutable handles).  The compile
     * is otherwise unobserved between bool_proto/str_proto (mutated below)
     * and the four read-only ones, so the const distinction is honoured. */
    UObject       *bool_proto  = urbi_object_atom(vm, URBI_ATOM_BOOLEAN);
    UObject       *str_proto   = urbi_object_atom(vm, URBI_ATOM_STRING);
    const UObject *int_proto   = urbi_object_atom(vm, URBI_ATOM_INTEGER);
    const UObject *float_proto = urbi_object_atom(vm, URBI_ATOM_FLOAT);
    const UObject *nil_proto   = urbi_object_atom(vm, URBI_ATOM_NIL);
    const UObject *void_proto  = urbi_object_atom(vm, URBI_ATOM_VOID);

    if (bool_proto == NULL || str_proto == NULL || int_proto == NULL
            || float_proto == NULL || nil_proto == NULL || void_proto == NULL) {
        return URBI_ERR_OOM;
    }
    /* int_proto / float_proto / nil_proto / void_proto exist for boot-
     * time singleton allocation only in this module (they inherit Object
     * root's methods via the proto chain).  Mark them `(void)` so the
     * compiler sees they are intentionally unused here. */
    (void)int_proto;
    (void)float_proto;
    (void)nil_proto;
    (void)void_proto;

    int rc;
    rc = URBI_REGISTER_METHODS(vm, bool_proto, BOOL_METHODS);
    if (rc != URBI_OK) return rc;

    rc = URBI_REGISTER_METHODS(vm, str_proto, STRING_METHODS);
    if (rc != URBI_OK) return rc;

    return URBI_OK;
}

/*
 * Walks the builtin atom + runtime-type protos registered earlier in
 * urbi_stdlib_boot and sets URBI_OBJ_FLAG_READONLY on each so that
 * urbiscript-side mutation (OP_SETSLOT) raises TypeError.
 *
 * Idempotent: setting the same bit on the same UObject repeatedly is a
 * no-op.  No allocation, no failure mode. */
int
urbi_atom_protos_mark_readonly(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;

    /* Atom singletons populated by urbi_object_atom at boot.
     * NB: URBI_ATOM_OBJECT is intentionally absent — D5 unfroze it. */
    static const URBIAtomFamily ATOM_FAMILIES[] = {
        URBI_ATOM_INTEGER,   /* Number  (legacy: Integer)         */
        URBI_ATOM_FLOAT,     /* Float                              */
        URBI_ATOM_STRING,    /* String                             */
        URBI_ATOM_BOOLEAN,   /* Bool    (legacy: Boolean)         */
        URBI_ATOM_NIL,       /* Nil                                */
        URBI_ATOM_LIST,      /* List                               */
        URBI_ATOM_DICT,      /* Dict                               */
        URBI_ATOM_SYMBOL,    /* Symbol  (codebase-only; v1.x parity)*/
        URBI_ATOM_VOID       /* Void    (codebase-only; v1.x parity)*/
    };
    size_t i;
    for (i = 0; i < sizeof(ATOM_FAMILIES) / sizeof(ATOM_FAMILIES[0]); i++) {
        UObject *p = urbi_object_atom(vm, ATOM_FAMILIES[i]);
        if (p != NULL) {
            p->flags |= URBI_OBJ_FLAG_READONLY;
        }
    }

    if (vm->tag_proto      != NULL) vm->tag_proto->flags      |= URBI_OBJ_FLAG_READONLY;
    if (vm->event_proto    != NULL) vm->event_proto->flags    |= URBI_OBJ_FLAG_READONLY;
    if (vm->mutex_proto    != NULL) vm->mutex_proto->flags    |= URBI_OBJ_FLAG_READONLY;
    if (vm->date_proto     != NULL) vm->date_proto->flags     |= URBI_OBJ_FLAG_READONLY;
    if (vm->duration_proto != NULL) vm->duration_proto->flags |= URBI_OBJ_FLAG_READONLY;
    if (vm->lobby_proto    != NULL) vm->lobby_proto->flags    |= URBI_OBJ_FLAG_READONLY;

    /* Global (vm->global_namespace_proto): intentionally NOT marked
     * readonly per spec §4.1 — it's the designated mutable cross-session
     * namespace.  Documented here so a future refactor doesn't sweep it
     * into the loop above. */
    /* if (vm->global_namespace_proto != NULL) { ... }  -- DO NOT ENABLE */

    return URBI_OK;
}
