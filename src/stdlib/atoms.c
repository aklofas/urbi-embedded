/* SPDX-License-Identifier: BSD-3-Clause */
/* atoms.c — M6 Phase 5: C-native methods on atom protos.
 *
 * Phase 5 task taxonomy (one commit per group):
 *   T35 — file shell + per-proto registration helper
 *   T36 — Boolean.negate
 *   T39 — Integer.asString / asFloat / asBoolean
 *   T40 — Integer.bitand / bitor / bitxor / bitnot / shl / shr
 *   T42 — Float.sqrt / sin / cos / tan / asin / acos / atan / atan2 /
 *         log / log10 / exp / pow / floor / ceil / abs / round
 *   T43 — Float.isNaN / isInfinite
 *   T44 — Float.asString / asInteger / asBoolean
 *   T45 — String.size / isEmpty / charAt / asciiAt
 *   T47 — String.toUpper / toLower
 *   T48 — String.indexOf / contains / startsWith / endsWith
 *   T49 — String.asInteger / asFloat
 *   T54 — close-out (no code change; CHANGELOG)
 *
 * Tasks T37 (Integer arithmetic), T38 (Integer comparison), T41 (Float
 * arith), T46 (String concat) are dropped: those operations are inline
 * VM opcodes (OP_ADD / OP_LT / OP_EQ / etc. in src/vm/uvm.c), not slot
 * lookups, so registering them as slots would have no effect on the
 * `1 + 2` source form.  The plan templated against an atom-method-only
 * dispatch model the Wave 1 VM does not use.
 */

#include "stdlib/atoms.h"
#include "stdlib/object_root.h"        /* urbi_native_closure_create + raise helpers */

#include "module/umodule.h"            /* UValue / UVAL_* */
#include "object/uobject.h"            /* urbi_object_atom, set_local_slot */
#include "runtime/uclosure.h"          /* urbi_native_method_fn */
#include "runtime/umacros.h"           /* urbi_strlen, urbi_zero */
#include "sched/ustrand.h"             /* UEXEC_OK / UEXEC_THROW */
#include "urbi/object.h"               /* URBI_ATOM_* */
#include "urbi/types.h"                /* urbi_value_nil, UExecStatus */
#include "urbi/urbi.h"                 /* URBI_OK / URBI_ERR_OOM */
#include "value/uintern.h"             /* ustr_intern + USymbol */
#include "vm/uvm.h"                    /* UVM */

#include <stddef.h>
#include <stdint.h>

/* === Method-table entry + per-proto installer ============================= */

typedef struct {
    const char           *name;
    urbi_native_method_fn fn;
} AtomMethodEntry;

static int
install_methods(UVM *vm, UObject *proto,
                const AtomMethodEntry *table, size_t count)
{
    size_t i;
    if (proto == NULL) return URBI_ERR_OOM;
    for (i = 0; i < count; i++) {
        UClosure *cl = urbi_native_closure_create(vm, table[i].fn);
        if (cl == NULL) return URBI_ERR_OOM;

        USymbol *sym = (USymbol *)ustr_intern(
            vm, table[i].name, urbi_strlen(table[i].name));
        if (sym == NULL) return URBI_ERR_OOM;

        UValue v = urbi_value_nil();
        v.kind = (uint8_t)UVAL_CLOSURE;
        v.v.p = cl;
        int rc = urbi_object_set_local_slot(vm, proto, sym, v);
        if (rc != 0) return URBI_ERR_OOM;
    }
    return URBI_OK;
}

/* === Per-family method tables (filled across T36-T54) ===================== */

static const AtomMethodEntry BOOL_METHODS[]    = { {NULL, NULL} };
static const AtomMethodEntry INT_METHODS[]     = { {NULL, NULL} };
static const AtomMethodEntry FLOAT_METHODS[]   = { {NULL, NULL} };
static const AtomMethodEntry STR_METHODS[]     = { {NULL, NULL} };

/* Each table has a NULL sentinel so an empty table is encoded as
 * `count == 0` (computed via `n - 1` of the sentinel-bearing array).  As
 * actual methods land, tasks remove the sentinel and grow the table; the
 * count macro stays correct. */
#define BOOL_METHODS_COUNT    ((sizeof(BOOL_METHODS)  / sizeof(BOOL_METHODS[0]))  - 1U)
#define INT_METHODS_COUNT     ((sizeof(INT_METHODS)   / sizeof(INT_METHODS[0]))   - 1U)
#define FLOAT_METHODS_COUNT   ((sizeof(FLOAT_METHODS) / sizeof(FLOAT_METHODS[0])) - 1U)
#define STR_METHODS_COUNT     ((sizeof(STR_METHODS)   / sizeof(STR_METHODS[0]))   - 1U)

/* === urbi_stdlib_register_atom_methods (T35 entry) ========================
 *
 * T35 lands the helper + boot wiring; the per-family method tables fill
 * in across T36-T54.  At T35 baseline all four tables are sentinel-only
 * (count == 0); install_methods is a no-op for those but the call sites
 * are wired so subsequent tasks only edit the table arrays. */

int
urbi_stdlib_register_atom_methods(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;

    int rc;
    rc = install_methods(vm, urbi_object_atom(vm, URBI_ATOM_BOOLEAN),
                         BOOL_METHODS, BOOL_METHODS_COUNT);
    if (rc != URBI_OK) return rc;

    rc = install_methods(vm, urbi_object_atom(vm, URBI_ATOM_INTEGER),
                         INT_METHODS, INT_METHODS_COUNT);
    if (rc != URBI_OK) return rc;

    rc = install_methods(vm, urbi_object_atom(vm, URBI_ATOM_FLOAT),
                         FLOAT_METHODS, FLOAT_METHODS_COUNT);
    if (rc != URBI_OK) return rc;

    rc = install_methods(vm, urbi_object_atom(vm, URBI_ATOM_STRING),
                         STR_METHODS, STR_METHODS_COUNT);
    if (rc != URBI_OK) return rc;

    return URBI_OK;
}
