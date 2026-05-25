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

#include "chunk/uchunk.h"            /* UValue / UVAL_* */
#include "object/uobject.h"            /* urbi_object_atom, set_local_slot */
#include "runtime/uclosure.h"          /* urbi_native_method_fn */
#include "runtime/umacros.h"           /* urbi_strlen, urbi_zero */
#include "sched/ustrand.h"             /* UEXEC_OK / UEXEC_THROW */
#include "urbi/object.h"               /* URBI_ATOM_* */
#include "urbi/types.h"                /* urbi_make_nil, UExecStatus */
#include "urbi/urbi.h"                 /* URBI_OK / URBI_ERR_OOM */
#include "value/uintern.h"             /* ustr_intern + USymbol */
#include "vm/uvm.h"                    /* UVM */

#include <stddef.h>
#include <stdint.h>

#if __STDC_HOSTED__
#  include <math.h>                    /* sqrt, sin, cos, ... for Float math */
#  include <stdio.h>                   /* snprintf for asString */
#  include <stdlib.h>                  /* strtoll, strtod for parse methods */
#endif

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

        UValue v = urbi_make_nil();
        v.kind = (uint8_t)UVAL_CLOSURE;
        v.v.p = cl;
        int rc = urbi_object_set_local_slot(vm, proto, sym, v);
        if (rc != 0) return URBI_ERR_OOM;
    }
    return URBI_OK;
}

/* === UValue construction helpers (file-private; zero pad bytes) =========== */

static UValue
val_int(int64_t i)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_INT;
    v.v.i = i;
    return v;
}

static UValue
val_float(double f)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_FLOAT;
#if URBI_FLOAT_TYPE == 8
    v.v.f = f;
#else
    v.v.f = (float)f;
#endif
    return v;
}

static UValue
val_bool(int b)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_BOOL;
    v.v.i = b ? 1 : 0;
    return v;
}

static UValue
val_str_intern(UVM *vm, const char *s, size_t n, int *oom)
{
    UValue v = urbi_make_nil();
    USymbol *sym = (USymbol *)ustr_intern(vm, s, n);
    if (sym == NULL) {
        if (oom != NULL) *oom = 1;
        return v;
    }
    v.kind = (uint8_t)UVAL_STR;
    v.v.p = sym;
    return v;
}

/* === Boolean.negate — return the unary inverse ============================
 *
 * Legacy `var '!' = false` (in share/urbi/boolean.u) installs the negation
 * as a slot value, not a method.  Wave 1 v1.0 uses the named-method form
 * `negate()` because slot-name dispatch through OP_GETSLOT requires a
 * UClosure value, not a UVAL_BOOL leaf.  The plan's `!` slot would not
 * dispatch from the v1.0 source `true.'!'` form (no quoted-name lex). */

static int
bool_negate(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Boolean.negate", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_BOOL)
        return urbi_raise_type(vm, "Boolean.negate: self must be Boolean", out);

    *out = val_bool(self.v.i == 0);
    return UEXEC_OK;
}

/* === Integer.asString / asFloat / asBoolean / asInteger (T39) =============
 *
 * asString prints base-10 via snprintf into a stack buffer, then interns.
 * Buffer 24 B is large enough for any int64_t (worst case 20 chars +
 * sign + NUL).  Freestanding builds without snprintf raise TypeError;
 * Wave 2 ships its own decimal formatter when the freestanding path
 * becomes load-bearing (urbi-embedded targets Cortex-M7 with newlib-nano,
 * which does provide snprintf — the freestanding fallback is reserved
 * for STM32 stripped-libc configurations). */

static int
int_asString(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Integer.asString", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Integer.asString: self must be Integer", out);

#if __STDC_HOSTED__
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)self.v.i);
    if (n <= 0 || (size_t)n >= sizeof(buf))
        return urbi_raise_type(vm, "Integer.asString: format failure", out);
    int oom = 0;
    UValue v = val_str_intern(vm, buf, (size_t)n, &oom);
    if (oom) return urbi_raise_oom(vm, out);
    *out = v;
    return UEXEC_OK;
#else
    (void)vm;
    return urbi_raise_type(vm,
        "Integer.asString: freestanding decimal formatter not yet linked", out);
#endif
}

static int
int_asFloat(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Integer.asFloat", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Integer.asFloat: self must be Integer", out);

    *out = val_float((double)self.v.i);
    return UEXEC_OK;
}

static int
int_asBoolean(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Integer.asBoolean", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Integer.asBoolean: self must be Integer", out);

    *out = val_bool(self.v.i != 0);
    return UEXEC_OK;
}

static int
int_asInteger(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Integer.asInteger", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Integer.asInteger: self must be Integer", out);

    *out = self;
    return UEXEC_OK;
}

/* === Integer bitops (T40) =================================================
 *
 * Bitwise ops are NAMED methods (no symbolic-operator lex tokens reserve
 * `&` / `|` for bitwise — `&` is the parallel-join concurrency separator).  Shift amount out of [0, 64) returns 0 (well-
 * defined; avoids signed shift UB on x86 + cross-arm-cortex-m). */

#define DEF_INT_BINOP(name, op)                                              \
    static int                                                               \
    int_##name(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out) \
    {                                                                        \
        if (nargs != 1) return urbi_raise_arity(vm, "Integer." #name, 1, nargs, out); \
        if (self.kind != (uint8_t)UVAL_INT)                                  \
            return urbi_raise_type(vm, "Integer." #name ": self must be Integer", out); \
        if (args[0].kind != (uint8_t)UVAL_INT)                               \
            return urbi_raise_type(vm, "Integer." #name ": argument must be Integer", out); \
        *out = val_int(self.v.i op args[0].v.i);                             \
        return UEXEC_OK;                                                     \
    }

DEF_INT_BINOP(bitand, &)
DEF_INT_BINOP(bitor,  |)
DEF_INT_BINOP(bitxor, ^)

#undef DEF_INT_BINOP

static int
int_bitnot(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Integer.bitnot", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Integer.bitnot: self must be Integer", out);

    *out = val_int(~self.v.i);
    return UEXEC_OK;
}

static int
int_shl(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "Integer.shl", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Integer.shl: self must be Integer", out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Integer.shl: argument must be Integer", out);

    int64_t a = self.v.i;
    int64_t s = args[0].v.i;
    if (s < 0 || s >= 64) {
        *out = val_int(0);
    } else {
        /* Cast through uint64_t to avoid signed-shift UB. */
        *out = val_int((int64_t)((uint64_t)a << (uint64_t)s));
    }
    return UEXEC_OK;
}

static int
int_shr(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "Integer.shr", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Integer.shr: self must be Integer", out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Integer.shr: argument must be Integer", out);

    int64_t a = self.v.i;
    int64_t s = args[0].v.i;
    if (s < 0 || s >= 64) {
        *out = val_int(0);
    } else {
        /* Logical shift right (uint64_t cast). */
        *out = val_int((int64_t)((uint64_t)a >> (uint64_t)s));
    }
    return UEXEC_OK;
}

/* === Float math + conversion (T42, T44) ===================================
 *
 * Hosted libm passthroughs.  Freestanding builds raise TypeError; libm is
 * provided by newlib-nano on Cortex-M / picolibc on rv32imc and is included
 * implicitly when the gcc driver builds the shared/static lib (no -lm
 * needed on those targets).  On hosted glibc, -lm becomes the linker
 * dependency — the v1.0 host build adds it via the implicit
 * `cc -o … -lm` chain in the Makefile if libm refs trigger the linker
 * (gcc auto-links libm on glibc).  Test commit will surface any missing
 * `-lm` and Phase 5 close-out can add it explicitly to LDFLAGS.
 */

#define FLOAT_OF_VALUE(uv) \
    ((uv).kind == (uint8_t)UVAL_FLOAT ? (double)(uv).v.f : \
     (uv).kind == (uint8_t)UVAL_INT   ? (double)(uv).v.i : 0.0)

#define DEF_FLOAT_UNARY(name, libm_call)                                     \
    static int                                                               \
    flt_##name(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out) \
    {                                                                        \
        (void)args;                                                          \
        if (nargs != 0) return urbi_raise_arity(vm, "Float." #name, 0, nargs, out); \
        if (self.kind != (uint8_t)UVAL_FLOAT)                                \
            return urbi_raise_type(vm, "Float." #name ": self must be Float", out); \
        *out = val_float(libm_call((double)self.v.f));                       \
        return UEXEC_OK;                                                     \
    }

#if __STDC_HOSTED__
DEF_FLOAT_UNARY(sqrt,  sqrt)
DEF_FLOAT_UNARY(sin,   sin)
DEF_FLOAT_UNARY(cos,   cos)
DEF_FLOAT_UNARY(tan,   tan)
DEF_FLOAT_UNARY(asin,  asin)
DEF_FLOAT_UNARY(acos,  acos)
DEF_FLOAT_UNARY(atan,  atan)
DEF_FLOAT_UNARY(log,   log)
DEF_FLOAT_UNARY(log10, log10)
DEF_FLOAT_UNARY(exp,   exp)
DEF_FLOAT_UNARY(floor, floor)
DEF_FLOAT_UNARY(ceil,  ceil)
DEF_FLOAT_UNARY(abs,   fabs)
DEF_FLOAT_UNARY(round, round)
#else
/* Freestanding: most Float methods need libm and stay stubbed (raise
 * TypeError "libm not linked").  But a handful are trivial-to-implement
 * without libm — abs is sign-bit-clear, floor/ceil/round are int casts
 * with edge-case fixups.  v0.8.2 freestanding-fix: provide real impls
 * for these so embedded ports get usable Float math without pulling in
 * libm.  On Cortex-M4F, the compiler maps fabsf() to a single VABS.F32
 * instruction; the inline ternary below compiles to the same. */

static int
flt_abs(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Float.abs", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_FLOAT)
        return urbi_raise_type(vm, "Float.abs: self must be Float", out);
    double x = (double)self.v.f;
    *out = val_float(x < 0.0 ? -x : x);
    return UEXEC_OK;
}

static int
flt_floor(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Float.floor", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_FLOAT)
        return urbi_raise_type(vm, "Float.floor: self must be Float", out);
    double x = (double)self.v.f;
    int64_t t = (int64_t)x;
    double tf = (double)t;
    /* For negatives where x != tf, truncation rounded TOWARD zero; floor
     * needs to round DOWN, so subtract 1.  Edge case: huge values that
     * overflow int64_t fall through unchanged — acceptable for v0.8.2. */
    if (x < 0.0 && tf != x) tf -= 1.0;
    *out = val_float(tf);
    return UEXEC_OK;
}

static int
flt_ceil(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Float.ceil", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_FLOAT)
        return urbi_raise_type(vm, "Float.ceil: self must be Float", out);
    double x = (double)self.v.f;
    int64_t t = (int64_t)x;
    double tf = (double)t;
    if (x > 0.0 && tf != x) tf += 1.0;
    *out = val_float(tf);
    return UEXEC_OK;
}

static int
flt_round(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Float.round", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_FLOAT)
        return urbi_raise_type(vm, "Float.round: self must be Float", out);
    double x = (double)self.v.f;
    /* Round half-away-from-zero (matches glibc round()). */
    double biased = x < 0.0 ? x - 0.5 : x + 0.5;
    *out = val_float((double)(int64_t)biased);
    return UEXEC_OK;
}

/* Remaining methods still need libm — stay stubbed. */
#  define DEF_FLOAT_UNARY_FREESTANDING(name)                                 \
    static int                                                               \
    flt_##name(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out) \
    {                                                                        \
        (void)self; (void)args; (void)nargs;                                 \
        return urbi_raise_type(vm, "Float." #name ": libm not linked", out); \
    }
DEF_FLOAT_UNARY_FREESTANDING(sqrt)
DEF_FLOAT_UNARY_FREESTANDING(sin)
DEF_FLOAT_UNARY_FREESTANDING(cos)
DEF_FLOAT_UNARY_FREESTANDING(tan)
DEF_FLOAT_UNARY_FREESTANDING(asin)
DEF_FLOAT_UNARY_FREESTANDING(acos)
DEF_FLOAT_UNARY_FREESTANDING(atan)
DEF_FLOAT_UNARY_FREESTANDING(log)
DEF_FLOAT_UNARY_FREESTANDING(log10)
DEF_FLOAT_UNARY_FREESTANDING(exp)
#  undef DEF_FLOAT_UNARY_FREESTANDING
#endif

#undef DEF_FLOAT_UNARY

/* atan2(y, x) — two-arg method */
static int
flt_atan2(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "Float.atan2", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_FLOAT)
        return urbi_raise_type(vm, "Float.atan2: self must be Float", out);

    double x = FLOAT_OF_VALUE(args[0]);
#if __STDC_HOSTED__
    *out = val_float(atan2((double)self.v.f, x));
    return UEXEC_OK;
#else
    (void)x;
    return urbi_raise_type(vm, "Float.atan2: libm not linked", out);
#endif
}

/* === Float.asString / asInteger / asBoolean (T44) =========================
 *
 * asString uses the same UVALUE_FLOAT_FMT (%.14g + Lua trailing-.0) as the
 * REPL printer for round-trip parity.  Wave 2 doesn't expose alternate
 * format selectors; %g is canonical.
 *
 * asInteger truncates toward zero (C99 (int64_t) cast).  Inf/NaN
 * conversions are implementation-defined in C; we explicitly raise
 * TypeError on those so the v1.0 surface is well-defined. */

static int
flt_asString(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Float.asString", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_FLOAT)
        return urbi_raise_type(vm, "Float.asString: self must be Float", out);

#if __STDC_HOSTED__
    char buf[40];
    int n = snprintf(buf, sizeof(buf), "%.14g", (double)self.v.f);
    if (n <= 0 || (size_t)n >= sizeof(buf))
        return urbi_raise_type(vm, "Float.asString: format failure", out);

    /* Lua 5.4 trailing-.0: append .0 if the result looks integer-valued
     * (no '.', 'e', 'E', 'n' for nan, 'i' for inf).  Mirrors the REPL
     * printer in src/value/uvalue.c. */
    int needs_dot_zero = 1;
    for (int k = 0; k < n; k++) {
        char c = buf[k];
        if (c == '.' || c == 'e' || c == 'E' || c == 'n' || c == 'i') {
            needs_dot_zero = 0;
            break;
        }
    }
    if (needs_dot_zero && (size_t)n + 2U < sizeof(buf)) {
        buf[n++] = '.';
        buf[n++] = '0';
        buf[n] = '\0';
    }

    int oom = 0;
    UValue v = val_str_intern(vm, buf, (size_t)n, &oom);
    if (oom) return urbi_raise_oom(vm, out);
    *out = v;
    return UEXEC_OK;
#else
    return urbi_raise_type(vm,
        "Float.asString: freestanding decimal formatter not yet linked", out);
#endif
}

static int
flt_asInteger(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Float.asInteger", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_FLOAT)
        return urbi_raise_type(vm, "Float.asInteger: self must be Float", out);

    double f = (double)self.v.f;
    /* Reject NaN / Inf — C99 conversion is implementation-defined; we
     * raise TypeError so callers get a clear failure mode. */
    if (f != f) return urbi_raise_type(vm, "Float.asInteger: NaN", out);
    if (f != 0.0 && (f - f) != 0.0) return urbi_raise_type(vm, "Float.asInteger: infinite", out);
    /* Out-of-range conversion is also implementation-defined; clamp at
     * INT64_MIN / INT64_MAX for safety. */
    if (f >= (double)INT64_MAX) { *out = val_int(INT64_MAX); return UEXEC_OK; }
    if (f <= (double)INT64_MIN) { *out = val_int(INT64_MIN); return UEXEC_OK; }
    *out = val_int((int64_t)f);
    return UEXEC_OK;
}

static int
flt_asBoolean(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Float.asBoolean", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_FLOAT)
        return urbi_raise_type(vm, "Float.asBoolean: self must be Float", out);

    double f = (double)self.v.f;
    /* Legacy semantics: NaN is truthy (non-comparable but not zero).
     * Inf is also truthy.  Only +/- zero is falsy. */
    *out = val_bool(f != 0.0);
    return UEXEC_OK;
}

/* === Float.isNaN / isInfinite (T43) ====================================== */

static int
flt_isNaN(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Float.isNaN", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_FLOAT)
        return urbi_raise_type(vm, "Float.isNaN: self must be Float", out);

    /* IEEE-754 NaN-detection: x != x is true iff x is NaN.  Avoids the
     * isnan() macro dependency on freestanding builds. */
    double f = (double)self.v.f;
    *out = val_bool(f != f);
    return UEXEC_OK;
}

static int
flt_isInfinite(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Float.isInfinite", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_FLOAT)
        return urbi_raise_type(vm, "Float.isInfinite: self must be Float", out);

    double f = (double)self.v.f;
    /* +/- inf detection: NaN compares unordered, so subtraction yields
     * NaN — guards the f - f == 0 trick.  inf - inf = NaN, so the
     * predicate excludes NaN.  Finite values: f - f = 0.  Infinity:
     * f - f = NaN (NaN != 0), and f != 0.
     *
     * Equivalent to isinf() under POSIX; we open-code to keep the
     * freestanding path identical. */
    *out = val_bool(f != 0.0 && (f - f) != 0.0 && f == f);
    return UEXEC_OK;
}

/* pow(self, exponent) — two-arg method */
static int
flt_pow(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "Float.pow", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_FLOAT)
        return urbi_raise_type(vm, "Float.pow: self must be Float", out);

    double e = FLOAT_OF_VALUE(args[0]);
#if __STDC_HOSTED__
    *out = val_float(pow((double)self.v.f, e));
    return UEXEC_OK;
#else
    (void)e;
    return urbi_raise_type(vm, "Float.pow: libm not linked", out);
#endif
}

/* === String basic methods (T45) ===========================================
 *
 * UVAL_STR.v.p is a NUL-terminated `const char *` from ustr_intern.
 * Wave 1's Boolean.toString + String.length already use urbi_strlen;
 * the runtime guarantees no embedded NULs in v1.0 strings (escape
 * `\0` is rejected by the lex; FUTURE Wave 2 backlog item LEX-035
 * extension).
 *
 * Strings are BYTE-counted at v1.0 (delta §3.2): length / size return
 * byte count, charAt indexes by byte.  Unicode-aware code-point indexing
 * is a Wave 2 follow-up. */

static int
str_size(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "String.size", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.size: self must be String", out);

    const char *s = (const char *)self.v.p;
    if (s == NULL) return urbi_raise_type(vm, "String.size: NULL string", out);
    *out = val_int((int64_t)urbi_strlen(s));
    return UEXEC_OK;
}

static int
str_isEmpty(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "String.isEmpty", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.isEmpty: self must be String", out);

    const char *s = (const char *)self.v.p;
    *out = val_bool(s == NULL || s[0] == '\0');
    return UEXEC_OK;
}

static int
str_charAt(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "String.charAt", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.charAt: self must be String", out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "String.charAt: index must be Integer", out);

    const char *s = (const char *)self.v.p;
    if (s == NULL) return urbi_raise_type(vm, "String.charAt: NULL string", out);
    size_t n = urbi_strlen(s);
    int64_t i = args[0].v.i;
    if (i < 0 || (size_t)i >= n)
        return urbi_raise_type(vm, "String.charAt: index out of range", out);

    /* Single-byte slice — interns into a 1-byte string. */
    char tmp[2];
    tmp[0] = s[i];
    tmp[1] = '\0';
    int oom = 0;
    UValue v = val_str_intern(vm, tmp, 1U, &oom);
    if (oom) return urbi_raise_oom(vm, out);
    *out = v;
    return UEXEC_OK;
}

/* === String case methods (T47) ============================================
 *
 * ASCII-only conversion at v1.0.  Non-ASCII bytes (>= 0x80) pass through
 * unchanged.  Wave 2 delivers Unicode-aware case folding when libicu /
 * the embedded NFC tables land — tracked as a stdlib backlog item.
 *
 * Allocation strategy: build the result in a heap buffer sized to the
 * input (case-conversion is byte-length-preserving for ASCII), intern,
 * and free.  For very long strings this is O(n) which is acceptable —
 * the v1.0 stdlib has no Tier-1 long-string benchmarks. */

static int
str_caseop(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out,
           int to_upper, const char *fn_name)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, fn_name, 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String case op: self must be String", out);

    const char *s = (const char *)self.v.p;
    if (s == NULL) return urbi_raise_type(vm, "String case op: NULL string", out);
    size_t n = urbi_strlen(s);

    if (n == 0) {
        int oom = 0;
        UValue v = val_str_intern(vm, "", 0U, &oom);
        if (oom) return urbi_raise_oom(vm, out);
        *out = v;
        return UEXEC_OK;
    }

    if (vm->alloc_fn == NULL) return urbi_raise_oom(vm, out);
    char *buf = (char *)vm->alloc_fn(NULL, n + 1U, vm->alloc_ud);
    if (buf == NULL) return urbi_raise_oom(vm, out);

    /* ASCII case toggle: bit 0x20 distinguishes 'A'..'Z' (0x41..0x5A)
     * from 'a'..'z' (0x61..0x7A).  toUpper clears the bit; toLower
     * sets it.  Avoids signed-narrowing warnings from (char)(c - DELTA). */
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (to_upper) {
            if (c >= (unsigned char)'a' && c <= (unsigned char)'z') c &= (unsigned char)~0x20U;
        } else {
            if (c >= (unsigned char)'A' && c <= (unsigned char)'Z') c |= (unsigned char)0x20U;
        }
        buf[i] = (char)c;
    }
    buf[n] = '\0';

    int oom = 0;
    UValue v = val_str_intern(vm, buf, n, &oom);
    vm->alloc_fn(buf, 0U, vm->alloc_ud);
    if (oom) return urbi_raise_oom(vm, out);
    *out = v;
    return UEXEC_OK;
}

static int
str_toUpper(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    return str_caseop(vm, self, args, nargs, out, 1, "String.toUpper");
}

static int
str_toLower(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    return str_caseop(vm, self, args, nargs, out, 0, "String.toLower");
}

/* === String search methods (T48) ==========================================
 *
 * Hosted builds use libc strstr/memcmp.  Freestanding builds open-code an
 * O(n*m) brute-force search to avoid the dependency.  v1.0 strings are
 * short (no Tier-1 long-haystack benchmarks); the brute-force fallback
 * is acceptable for the embedded path where libc is stripped. */

static int
strs_find(const char *hay, size_t hlen, const char *ndl, size_t nlen,
          int64_t *out_idx)
{
    if (nlen == 0) { *out_idx = 0; return 1; }
    if (nlen > hlen) { *out_idx = -1; return 0; }
    size_t i;
    for (i = 0; i + nlen <= hlen; i++) {
        size_t k;
        for (k = 0; k < nlen; k++) {
            if (hay[i + k] != ndl[k]) break;
        }
        if (k == nlen) { *out_idx = (int64_t)i; return 1; }
    }
    *out_idx = -1;
    return 0;
}

static int
str_indexOf(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "String.indexOf", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.indexOf: self must be String", out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.indexOf: argument must be String", out);

    const char *h = (const char *)self.v.p;
    const char *n = (const char *)args[0].v.p;
    if (h == NULL || n == NULL)
        return urbi_raise_type(vm, "String.indexOf: NULL string", out);

    int64_t idx;
    (void)strs_find(h, urbi_strlen(h), n, urbi_strlen(n), &idx);
    *out = val_int(idx);
    return UEXEC_OK;
}

static int
str_contains(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "String.contains", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.contains: self must be String", out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.contains: argument must be String", out);

    const char *h = (const char *)self.v.p;
    const char *n = (const char *)args[0].v.p;
    if (h == NULL || n == NULL)
        return urbi_raise_type(vm, "String.contains: NULL string", out);

    int64_t idx;
    int found = strs_find(h, urbi_strlen(h), n, urbi_strlen(n), &idx);
    *out = val_bool(found);
    return UEXEC_OK;
}

static int
str_starts_or_ends(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                   UValue *out, int starts, const char *fn_name)
{
    if (nargs != 1) return urbi_raise_arity(vm, fn_name, 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String prefix/suffix op: self must be String", out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String prefix/suffix op: argument must be String", out);

    const char *h = (const char *)self.v.p;
    const char *n = (const char *)args[0].v.p;
    if (h == NULL || n == NULL)
        return urbi_raise_type(vm, "String prefix/suffix op: NULL string", out);

    size_t hlen = urbi_strlen(h);
    size_t nlen = urbi_strlen(n);
    if (nlen == 0) { *out = val_bool(1); return UEXEC_OK; }
    if (nlen > hlen) { *out = val_bool(0); return UEXEC_OK; }

    const char *base = starts ? h : (h + (hlen - nlen));
    size_t k;
    for (k = 0; k < nlen; k++) {
        if (base[k] != n[k]) { *out = val_bool(0); return UEXEC_OK; }
    }
    *out = val_bool(1);
    return UEXEC_OK;
}

static int
str_startsWith(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    return str_starts_or_ends(vm, self, args, nargs, out, 1, "String.startsWith");
}

static int
str_endsWith(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    return str_starts_or_ends(vm, self, args, nargs, out, 0, "String.endsWith");
}

/* === String parse methods (T49) ===========================================
 *
 * asInteger / asFloat use strtoll / strtod (hosted libc).  Freestanding
 * builds raise TypeError; the embedded path can override with newlib's
 * lighter parsers if needed.  Parse failure (no leading numeric) raises
 * TypeError — the legacy 2014 stdlib silently returned 0 on parse
 * failure but that's a footgun.  v1.0 chose strict semantics. */

static int
str_asInteger(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "String.asInteger", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.asInteger: self must be String", out);

    const char *s = (const char *)self.v.p;
    if (s == NULL || s[0] == '\0')
        return urbi_raise_type(vm, "String.asInteger: empty / NULL string", out);

#if __STDC_HOSTED__
    char *endptr = NULL;
    long long v = strtoll(s, &endptr, 10);
    if (endptr == s)
        return urbi_raise_type(vm, "String.asInteger: not a number", out);
    /* Trailing garbage is rejected (legacy semantics — full-string parse). */
    while (*endptr == ' ' || *endptr == '\t') endptr++;
    if (*endptr != '\0')
        return urbi_raise_type(vm, "String.asInteger: trailing garbage", out);
    *out = val_int((int64_t)v);
    return UEXEC_OK;
#else
    return urbi_raise_type(vm,
        "String.asInteger: freestanding strtoll not yet linked", out);
#endif
}

static int
str_asFloat(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "String.asFloat", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.asFloat: self must be String", out);

    const char *s = (const char *)self.v.p;
    if (s == NULL || s[0] == '\0')
        return urbi_raise_type(vm, "String.asFloat: empty / NULL string", out);

#if __STDC_HOSTED__
    char *endptr = NULL;
    double v = strtod(s, &endptr);
    if (endptr == s)
        return urbi_raise_type(vm, "String.asFloat: not a number", out);
    while (*endptr == ' ' || *endptr == '\t') endptr++;
    if (*endptr != '\0')
        return urbi_raise_type(vm, "String.asFloat: trailing garbage", out);
    *out = val_float(v);
    return UEXEC_OK;
#else
    return urbi_raise_type(vm,
        "String.asFloat: freestanding strtod not yet linked", out);
#endif
}

static int
str_asBoolean(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "String.asBoolean", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.asBoolean: self must be String", out);

    const char *s = (const char *)self.v.p;
    if (s == NULL) return urbi_raise_type(vm, "String.asBoolean: NULL string", out);

    /* Case-sensitive byte compare against "true" / "false". */
    if (s[0] == 't' && s[1] == 'r' && s[2] == 'u' && s[3] == 'e' && s[4] == '\0') {
        *out = val_bool(1);
        return UEXEC_OK;
    }
    if (s[0] == 'f' && s[1] == 'a' && s[2] == 'l' && s[3] == 's' && s[4] == 'e' && s[5] == '\0') {
        *out = val_bool(0);
        return UEXEC_OK;
    }
    return urbi_raise_type(vm,
        "String.asBoolean: only \"true\" / \"false\" recognized", out);
}

/* === String.asciiAt (T50) ================================================
 *
 * Byte-level codepoint access — returns the byte at the given index as
 * Integer (0..255).  charAt (T45) returns a 1-byte string slice; asciiAt
 * returns the numeric byte value.  Codepoint-aware variants (codePointAt
 * etc.) are deferred to Wave 2 Unicode follow-up (delta §3.2). */

static int
str_asciiAt(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "String.asciiAt", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.asciiAt: self must be String", out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "String.asciiAt: index must be Integer", out);

    const char *s = (const char *)self.v.p;
    if (s == NULL) return urbi_raise_type(vm, "String.asciiAt: NULL string", out);
    size_t n = urbi_strlen(s);
    int64_t i = args[0].v.i;
    if (i < 0 || (size_t)i >= n)
        return urbi_raise_type(vm, "String.asciiAt: index out of range", out);

    *out = val_int((int64_t)(unsigned char)s[i]);
    return UEXEC_OK;
}

/* === Per-family method tables (filled across T36-T54) ===================== */

static const AtomMethodEntry BOOL_METHODS[] = {
    { "negate", bool_negate }
};
static const AtomMethodEntry INT_METHODS[] = {
    { "asString",  int_asString  },
    { "asFloat",   int_asFloat   },
    { "asBoolean", int_asBoolean },
    { "asInteger", int_asInteger },
    { "bitand",    int_bitand    },
    { "bitor",     int_bitor     },
    { "bitxor",    int_bitxor    },
    { "bitnot",    int_bitnot    },
    { "shl",       int_shl       },
    { "shr",       int_shr       }
};
static const AtomMethodEntry FLOAT_METHODS[] = {
    { "sqrt",  flt_sqrt  },
    { "sin",   flt_sin   },
    { "cos",   flt_cos   },
    { "tan",   flt_tan   },
    { "asin",  flt_asin  },
    { "acos",  flt_acos  },
    { "atan",  flt_atan  },
    { "atan2", flt_atan2 },
    { "log",   flt_log   },
    { "log10", flt_log10 },
    { "exp",   flt_exp   },
    { "pow",   flt_pow   },
    { "floor", flt_floor },
    { "ceil",  flt_ceil  },
    { "abs",   flt_abs   },
    { "round", flt_round },
    { "isNaN",      flt_isNaN      },
    { "isInfinite", flt_isInfinite },
    { "asString",   flt_asString   },
    { "asInteger",  flt_asInteger  },
    { "asBoolean",  flt_asBoolean  }
};
static const AtomMethodEntry STR_METHODS[] = {
    { "size",    str_size    },
    { "isEmpty", str_isEmpty },
    { "charAt",  str_charAt  },
    { "toUpper", str_toUpper },
    { "toLower", str_toLower },
    { "indexOf",    str_indexOf    },
    { "contains",   str_contains   },
    { "startsWith", str_startsWith },
    { "endsWith",   str_endsWith   },
    { "asInteger",  str_asInteger  },
    { "asFloat",    str_asFloat    },
    { "asBoolean",  str_asBoolean  },
    { "asciiAt",    str_asciiAt    }
};

/* Empty tables retain a `{NULL, NULL}` sentinel so the array has at
 * least one element (C99 forbids zero-size arrays).  Tables with real
 * entries omit the sentinel.  COUNT macros use the sentinel form when
 * needed; populated tables use straight sizeof. */
#define BOOL_METHODS_COUNT    (sizeof(BOOL_METHODS)  / sizeof(BOOL_METHODS[0]))
#define INT_METHODS_COUNT     (sizeof(INT_METHODS)   / sizeof(INT_METHODS[0]))
#define FLOAT_METHODS_COUNT   (sizeof(FLOAT_METHODS) / sizeof(FLOAT_METHODS[0]))
#define STR_METHODS_COUNT     (sizeof(STR_METHODS)   / sizeof(STR_METHODS[0]))

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
