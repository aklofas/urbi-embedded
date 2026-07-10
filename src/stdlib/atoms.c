/* SPDX-License-Identifier: BSD-3-Clause */
/* atoms.c — C-native methods on atom protos.
 *
 * Methods implemented here (one section per group):
 *   Boolean.negate
 *   Integer.asString / asFloat / asBoolean
 *   Integer.and / or / xor / inv / shl / shr / ushr (Kotlin-named)
 *   Float.sqrt / sin / cos / tan / asin / acos / atan / atan2 /
 *         log / log10 / exp / pow / floor / ceil / abs / round
 *   Float.isNaN / isInfinite
 *   Float.asString / asInteger / asBoolean
 *   String.size / isEmpty / charAt / asciiAt
 *   String.toUpper / toLower
 *   String.indexOf / contains / startsWith / endsWith
 *   String.asInteger / asFloat
 *
 * Note: Integer arithmetic, Integer comparison, Float arith, and String
 * concat are inline VM opcodes (OP_ADD / OP_LT / OP_EQ / etc. in
 * src/vm/uvm.c), not slot lookups, so registering them as slots would
 * VM opcodes (OP_ADD / OP_LT / OP_EQ / etc. in src/vm/uvm.c), not slot
 * lookups, so registering them as slots would have no effect on the
 * `1 + 2` source form.  The plan templated against an atom-method-only
 * dispatch model the shipped VM does not use.
 */

#include "stdlib/atoms.h"
#include "stdlib/containers.h"         /* urbi_stdlib_list_new_empty/append/len/get */
#include "stdlib/object_root.h"        /* urbi_native_closure_create + raise helpers */
#include "stdlib/stdlib_join_core.h"   /* join_core: shared String/List join logic */

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

/* Method tables use UNativeMethodDef from stdlib/object_root.h;
 * urbi_install_native_methods / URBI_REGISTER_METHODS do the install loop. */

/* === Numeric helpers (freestanding-safe) ================================== */

/* fmod_portable — floating modulo.  Hosted builds defer to libm fmod;
 * freestanding builds open-code a truncating remainder.  The int path
 * guards against a zero divisor before calling this; the float path passes
 * any divisor through (matching libm fmod, which returns NaN on b==0). */
#if __STDC_HOSTED__
static double fmod_portable(double a, double b) { return fmod(a, b); }
#else
static double fmod_portable(double a, double b)
{
    if (b == 0.0) return 0.0;
    double q = a / b;
    /* Truncate q toward zero without libm trunc(). */
    double t = (q < 0.0) ? -(double)(uint64_t)(-q) : (double)(uint64_t)q;
    return a - t * b;
}
#endif

/* xorshift64 PRNG — file-static, seeded with a fixed nonzero constant.
 * Float.random() returns a value in [0, 1).  Deterministic by default
 * (fixed seed): the determinism checksum gate runs the unit suite, not
 * random scripts, so a fixed seed keeps test reproducibility while still
 * providing variation within a run.  Non-deterministic across-run seeding
 * is a v1.x follow-up (no host time source is wired here). */
static uint64_t s_prng_state = 0x9E3779B97F4A7C15ULL;  /* NOLINT(cppcoreguidelines-avoid-non-const-global-variables) — deliberate fixed-seed process-global (doc above); per-VM relocation is filed for the v0.13.x hardening arc; audit-globals-allow: deliberate fixed-seed PRNG */

static uint64_t
prng_next(void)
{
    uint64_t x = s_prng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    s_prng_state = x;
    return x;
}

/* === Boolean.negate — return the unary inverse ============================
 *
 * Legacy `var '!' = false` (in share/urbi/boolean.u) installs the negation
 * as a slot value, not a method.  The v1.0 runtime uses the named-method
 * form `negate()` because slot-name dispatch through OP_GETSLOT requires a
 * UClosure value, not a UVAL_BOOL leaf.  The plan's `!` slot would not
 * dispatch from the v1.0 source `true.'!'` form (no quoted-name lex). */

static int
bool_negate(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Boolean.negate", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_BOOL, "Boolean.negate: self must be Boolean", out);

    *out = urbi_make_bool(self.v.i == 0);
    return UEXEC_OK;
}

/* === Integer.asString / asFloat / asBoolean / asInteger ==================
 *
 * asString prints base-10 via snprintf into a stack buffer, then interns.
 * Buffer 24 B is large enough for any int64_t (worst case 20 chars +
 * sign + NUL).  Freestanding builds without snprintf raise TypeError; a
 * dedicated decimal formatter for that path is a deferred follow-up, to
 * land when the freestanding path becomes load-bearing (urbi-embedded
 * targets Cortex-M7 with newlib-nano,
 * which does provide snprintf — the freestanding fallback is reserved
 * for STM32 stripped-libc configurations). */

static int
int_asString(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Integer.asString", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_INT, "Integer.asString: self must be Integer", out);

#if __STDC_HOSTED__
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)self.v.i);
    if (n <= 0 || (size_t)n >= sizeof(buf))
        return urbi_raise_type(vm, "Integer.asString: format failure", out);
    int oom = 0;
    UValue v = urbi_val_str_intern(vm, buf, (size_t)n, &oom);
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
    URBI_CHECK_ARITY(vm, "Integer.asFloat", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_INT, "Integer.asFloat: self must be Integer", out);

    *out = urbi_make_float((double)self.v.i);
    return UEXEC_OK;
}

static int
int_asBoolean(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Integer.asBoolean", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_INT, "Integer.asBoolean: self must be Integer", out);

    *out = urbi_make_bool(self.v.i != 0);
    return UEXEC_OK;
}

static int
int_asInteger(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Integer.asInteger", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_INT, "Integer.asInteger: self must be Integer", out);

    *out = self;
    return UEXEC_OK;
}

/* === Integer bitops =======================================================
 *
 * Bitwise ops are NAMED methods (no symbolic-operator lex tokens reserve
 * `&` / `|` for bitwise — `&` is the parallel-join concurrency separator).
 *
 * Shift count semantics (Kotlin Long): all three ops mask the count to
 * [0, 63] via effective = raw_count & 63.  ushr had this from the start;
 * shl and shr now match. */

#define DEF_INT_BINOP(name, op)                                              \
    static int                                                               \
    int_##name(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out) \
    {                                                                        \
        if (nargs != 1) return urbi_raise_arity(vm, "Integer." #name, 1, nargs, out); \
        if (self.kind != (uint8_t)UVAL_INT)                                  \
            return urbi_raise_type(vm, "Integer." #name ": self must be Integer", out); \
        if (args[0].kind != (uint8_t)UVAL_INT)                               \
            return urbi_raise_type(vm, "Integer." #name ": argument must be Integer", out); \
        *out = urbi_make_int(self.v.i op args[0].v.i);                             \
        return UEXEC_OK;                                                     \
    }

DEF_INT_BINOP(and, &)
DEF_INT_BINOP(or,  |)
DEF_INT_BINOP(xor, ^)

#undef DEF_INT_BINOP

static int
int_inv(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Integer.inv", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_INT, "Integer.inv: self must be Integer", out);

    *out = urbi_make_int(~self.v.i);
    return UEXEC_OK;
}

static int
int_shl(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "Integer.shl", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_INT, "Integer.shl: self must be Integer", out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Integer.shl: argument must be Integer", out);

    /* Kotlin Long.shl mask: effective = raw & 63, always in [0, 63]. */
    int64_t n = args[0].v.i & (int64_t)63;
    *out = urbi_make_int((int64_t)((uint64_t)self.v.i << (uint64_t)n));
    return UEXEC_OK;
}

static int
int_shr(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "Integer.shr", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_INT, "Integer.shr: self must be Integer", out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Integer.shr: argument must be Integer", out);

    /* Kotlin Long.shr mask: effective = raw & 63, always in [0, 63].
     * Implementation uses uint64_t cast (logical right shift on the raw
     * i64 bit pattern) matching the pre-existing shr semantic. */
    int64_t n = args[0].v.i & (int64_t)63;
    *out = urbi_make_int((int64_t)((uint64_t)self.v.i >> (uint64_t)n));
    return UEXEC_OK;
}

/* ushr — unsigned (logical) right shift on the i64 bit pattern.  Zero-fills
 * from the left regardless of sign; the Kotlin `ushr` semantic.  Shift amount
 * is masked to [0, 63] (Kotlin masks). */
static int
int_ushr(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "Integer.ushr", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_INT, "Integer.ushr: self must be Integer", out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Integer.ushr: argument must be Integer", out);

    int64_t n = args[0].v.i & 63;
    *out = urbi_make_int((int64_t)((uint64_t)self.v.i >> (uint64_t)n));
    return UEXEC_OK;
}

/* === Integer / Float `%` modulo ===========================================
 *
 * `a % b` desugars (in the parser) to `a.'%'(b)`.  Integer%Integer yields an
 * Integer (zero divisor raises DivByZero — legacy "modulo by 0"; INT64_MIN %
 * -1 returns 0 to avoid signed-division overflow UB); any Float operand
 * promotes to fmod (zero divisor also raises DivByZero, per legacy). */

static int
int_mod(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "Integer.%", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_INT, "%: self must be Integer", out);
    if (args[0].kind == (uint8_t)UVAL_FLOAT) {
        /* v0.13.5: legacy-conformant modulo-by-zero (float.cc
         * operator%: `if (rhs) fmod(...) else RAISE("modulo by 0")`). */
        if ((double)args[0].v.f == 0.0)
            return urbi_raise_divzero(vm, "modulo by 0", out);
        *out = urbi_make_float(fmod_portable((double)self.v.i, (double)args[0].v.f));
        return UEXEC_OK;
    }
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "%: argument must be Integer or Float", out);
    if (args[0].v.i == 0) return urbi_raise_divzero(vm, "modulo by 0", out);
    if (self.v.i == INT64_MIN && args[0].v.i == -1) { *out = urbi_make_int(0); return UEXEC_OK; }
    *out = urbi_make_int(self.v.i % args[0].v.i);
    return UEXEC_OK;
}

static int
flt_mod(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "Float.%", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_FLOAT, "%: self must be Float", out);
    double b;
    if (args[0].kind == (uint8_t)UVAL_FLOAT) b = (double)args[0].v.f;
    else if (args[0].kind == (uint8_t)UVAL_INT) b = (double)args[0].v.i;
    else return urbi_raise_type(vm, "%: argument must be Integer or Float", out);
    /* v0.13.5: legacy-conformant modulo-by-zero (float.cc
     * operator%: `if (rhs) fmod(...) else RAISE("modulo by 0")`). */
    if (b == 0.0) return urbi_raise_divzero(vm, "modulo by 0", out);
    *out = urbi_make_float(fmod_portable((double)self.v.f, b));
    return UEXEC_OK;
}

/* Float.random() — pseudo-random Float in [0, 1).  Receiver is ignored
 * (called as `Float.random()` or on any Float).  53-bit mantissa draw. */
static int
flt_random(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args;
    URBI_CHECK_ARITY(vm, "Float.random", 0, nargs, out);
    uint64_t bits = prng_next() >> 11;          /* top 53 bits */
    *out = urbi_make_float((double)bits * (1.0 / 9007199254740992.0)); /* / 2^53 */
    return UEXEC_OK;
}

/* === Float math + conversion ==============================================
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
        *out = urbi_make_float(libm_call((double)self.v.f));                       \
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
    URBI_CHECK_ARITY(vm, "Float.abs", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_FLOAT, "Float.abs: self must be Float", out);
    double x = (double)self.v.f;
    *out = urbi_make_float(x < 0.0 ? -x : x);
    return UEXEC_OK;
}

static int
flt_floor(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Float.floor", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_FLOAT, "Float.floor: self must be Float", out);
    double x = (double)self.v.f;
    int64_t t = (int64_t)x;
    double tf = (double)t;
    /* For negatives where x != tf, truncation rounded TOWARD zero; floor
     * needs to round DOWN, so subtract 1.  Edge case: huge values that
     * overflow int64_t fall through unchanged — acceptable for v0.8.2. */
    if (x < 0.0 && tf != x) tf -= 1.0;
    *out = urbi_make_float(tf);
    return UEXEC_OK;
}

static int
flt_ceil(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Float.ceil", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_FLOAT, "Float.ceil: self must be Float", out);
    double x = (double)self.v.f;
    int64_t t = (int64_t)x;
    double tf = (double)t;
    if (x > 0.0 && tf != x) tf += 1.0;
    *out = urbi_make_float(tf);
    return UEXEC_OK;
}

static int
flt_round(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Float.round", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_FLOAT, "Float.round: self must be Float", out);
    double x = (double)self.v.f;
    /* Round half-away-from-zero (matches glibc round()). */
    double biased = x < 0.0 ? x - 0.5 : x + 0.5;
    *out = urbi_make_float((double)(int64_t)biased);
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
    URBI_CHECK_ARITY(vm, "Float.atan2", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_FLOAT, "Float.atan2: self must be Float", out);

    double x = FLOAT_OF_VALUE(args[0]);
#if __STDC_HOSTED__
    *out = urbi_make_float(atan2((double)self.v.f, x));
    return UEXEC_OK;
#else
    (void)x;
    return urbi_raise_type(vm, "Float.atan2: libm not linked", out);
#endif
}

/* === Float.asString / asInteger / asBoolean ================================
 *
 * asString uses the same UVALUE_FLOAT_FMT (%.14g + Lua trailing-.0) as the
 * REPL printer for round-trip parity.  Alternate format selectors are
 * not exposed; %g is canonical.
 *
 * asInteger truncates toward zero (C99 (int64_t) cast).  Inf/NaN
 * conversions are implementation-defined in C; we explicitly raise
 * TypeError on those so the v1.0 surface is well-defined. */

static int
flt_asString(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Float.asString", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_FLOAT, "Float.asString: self must be Float", out);

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
    UValue v = urbi_val_str_intern(vm, buf, (size_t)n, &oom);
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
    URBI_CHECK_ARITY(vm, "Float.asInteger", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_FLOAT, "Float.asInteger: self must be Float", out);

    double f = (double)self.v.f;
    /* Reject NaN / Inf — C99 conversion is implementation-defined; we
     * raise TypeError so callers get a clear failure mode. */
    if (f != f) return urbi_raise_type(vm, "Float.asInteger: NaN", out);
    if (f != 0.0 && (f - f) != 0.0) return urbi_raise_type(vm, "Float.asInteger: infinite", out);
    /* Out-of-range conversion is also implementation-defined; clamp at
     * INT64_MIN / INT64_MAX for safety. */
    if (f >= (double)INT64_MAX) { *out = urbi_make_int(INT64_MAX); return UEXEC_OK; }
    if (f <= (double)INT64_MIN) { *out = urbi_make_int(INT64_MIN); return UEXEC_OK; }
    *out = urbi_make_int((int64_t)f);
    return UEXEC_OK;
}

static int
flt_asBoolean(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Float.asBoolean", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_FLOAT, "Float.asBoolean: self must be Float", out);

    double f = (double)self.v.f;
    /* Legacy semantics: NaN is truthy (non-comparable but not zero).
     * Inf is also truthy.  Only +/- zero is falsy. */
    *out = urbi_make_bool(f != 0.0);
    return UEXEC_OK;
}

/* === Float.isNaN / isInfinite ============================================ */

static int
flt_isNaN(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Float.isNaN", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_FLOAT, "Float.isNaN: self must be Float", out);

    /* IEEE-754 NaN-detection: x != x is true iff x is NaN.  Avoids the
     * isnan() macro dependency on freestanding builds. */
    double f = (double)self.v.f;
    *out = urbi_make_bool(f != f);
    return UEXEC_OK;
}

static int
flt_isInfinite(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Float.isInfinite", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_FLOAT, "Float.isInfinite: self must be Float", out);

    double f = (double)self.v.f;
    /* +/- inf detection: NaN compares unordered, so subtraction yields
     * NaN — guards the f - f == 0 trick.  inf - inf = NaN, so the
     * predicate excludes NaN.  Finite values: f - f = 0.  Infinity:
     * f - f = NaN (NaN != 0), and f != 0.
     *
     * Equivalent to isinf() under POSIX; we open-code to keep the
     * freestanding path identical. */
    *out = urbi_make_bool(f != 0.0 && (f - f) != 0.0 && f == f);
    return UEXEC_OK;
}

/* pow(self, exponent) — two-arg method */
static int
flt_pow(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "Float.pow", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_FLOAT, "Float.pow: self must be Float", out);

    double e = FLOAT_OF_VALUE(args[0]);
#if __STDC_HOSTED__
    *out = urbi_make_float(pow((double)self.v.f, e));
    return UEXEC_OK;
#else
    (void)e;
    return urbi_raise_type(vm, "Float.pow: libm not linked", out);
#endif
}

/* === String basic methods ==================================================
 *
 * UVAL_STR.v.p is a NUL-terminated `const char *` from ustr_intern.
 * Boolean.toString + String.length already use urbi_strlen;
 * the runtime guarantees no embedded NULs in v1.0 strings (escape
 * `\0` is rejected by the lex; FUTURE backlog item LEX-035
 * extension).
 *
 * Strings are BYTE-counted at v1.0 (delta §3.2): length / size return
 * byte count, charAt indexes by byte.  Unicode-aware code-point indexing
 * is a later follow-up. */

static int
str_size(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "String.size", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "String.size: self must be String", out);

    const char *s = (const char *)self.v.p;
    if (s == NULL) return urbi_raise_type(vm, "String.size: NULL string", out);
    *out = urbi_make_int((int64_t)urbi_strlen(s));
    return UEXEC_OK;
}

static int
str_isEmpty(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "String.isEmpty", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "String.isEmpty: self must be String", out);

    const char *s = (const char *)self.v.p;
    *out = urbi_make_bool(s == NULL || s[0] == '\0');
    return UEXEC_OK;
}

static int
str_charAt(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "String.charAt", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "String.charAt: self must be String", out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "String.charAt: index must be Integer", out);

    const char *s = (const char *)self.v.p;
    if (s == NULL) return urbi_raise_type(vm, "String.charAt: NULL string", out);
    size_t n = urbi_strlen(s);
    int64_t i = args[0].v.i;
    if (i < 0 || (size_t)i >= n)
        return urbi_raise_range(vm, "String.charAt: index out of range", out);

    /* Single-byte slice — interns into a 1-byte string. */
    char tmp[2];
    tmp[0] = s[i];
    tmp[1] = '\0';
    int oom = 0;
    UValue v = urbi_val_str_intern(vm, tmp, 1U, &oom);
    if (oom) return urbi_raise_oom(vm, out);
    *out = v;
    return UEXEC_OK;
}

/* === String case methods ===================================================
 *
 * ASCII-only conversion at v1.0.  Non-ASCII bytes (>= 0x80) pass through
 * unchanged.  Unicode-aware case folding will land when libicu /
 * the embedded NFC tables do — tracked as a stdlib backlog item.
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
    URBI_CHECK_ARITY(vm, fn_name, 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "String case op: self must be String", out);

    const char *s = (const char *)self.v.p;
    if (s == NULL) return urbi_raise_type(vm, "String case op: NULL string", out);
    size_t n = urbi_strlen(s);

    if (n == 0) {
        int oom = 0;
        UValue v = urbi_val_str_intern(vm, "", 0U, &oom);
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
    UValue v = urbi_val_str_intern(vm, buf, n, &oom);
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

/* === String search methods =================================================
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
    URBI_CHECK_ARITY(vm, "String.indexOf", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "String.indexOf: self must be String", out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.indexOf: argument must be String", out);

    const char *h = (const char *)self.v.p;
    const char *n = (const char *)args[0].v.p;
    if (h == NULL || n == NULL)
        return urbi_raise_type(vm, "String.indexOf: NULL string", out);

    int64_t idx;
    (void)strs_find(h, urbi_strlen(h), n, urbi_strlen(n), &idx);
    *out = urbi_make_int(idx);
    return UEXEC_OK;
}

static int
str_contains(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "String.contains", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "String.contains: self must be String", out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String.contains: argument must be String", out);

    const char *h = (const char *)self.v.p;
    const char *n = (const char *)args[0].v.p;
    if (h == NULL || n == NULL)
        return urbi_raise_type(vm, "String.contains: NULL string", out);

    int64_t idx;
    int found = strs_find(h, urbi_strlen(h), n, urbi_strlen(n), &idx);
    *out = urbi_make_bool(found);
    return UEXEC_OK;
}

static int
str_starts_or_ends(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                   UValue *out, int starts, const char *fn_name)
{
    URBI_CHECK_ARITY(vm, fn_name, 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "String prefix/suffix op: self must be String", out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "String prefix/suffix op: argument must be String", out);

    const char *h = (const char *)self.v.p;
    const char *n = (const char *)args[0].v.p;
    if (h == NULL || n == NULL)
        return urbi_raise_type(vm, "String prefix/suffix op: NULL string", out);

    size_t hlen = urbi_strlen(h);
    size_t nlen = urbi_strlen(n);
    if (nlen == 0) { *out = urbi_make_bool(1); return UEXEC_OK; }
    if (nlen > hlen) { *out = urbi_make_bool(0); return UEXEC_OK; }

    const char *base = starts ? h : (h + (hlen - nlen));
    size_t k;
    for (k = 0; k < nlen; k++) {
        if (base[k] != n[k]) { *out = urbi_make_bool(0); return UEXEC_OK; }
    }
    *out = urbi_make_bool(1);
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

/* === String parse methods ==================================================
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
    URBI_CHECK_ARITY(vm, "String.asInteger", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "String.asInteger: self must be String", out);

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
    *out = urbi_make_int((int64_t)v);
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
    URBI_CHECK_ARITY(vm, "String.asFloat", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "String.asFloat: self must be String", out);

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
    *out = urbi_make_float(v);
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
    URBI_CHECK_ARITY(vm, "String.asBoolean", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "String.asBoolean: self must be String", out);

    const char *s = (const char *)self.v.p;
    if (s == NULL) return urbi_raise_type(vm, "String.asBoolean: NULL string", out);

    /* Case-sensitive byte compare against "true" / "false". */
    if (s[0] == 't' && s[1] == 'r' && s[2] == 'u' && s[3] == 'e' && s[4] == '\0') {
        *out = urbi_make_bool(1);
        return UEXEC_OK;
    }
    if (s[0] == 'f' && s[1] == 'a' && s[2] == 'l' && s[3] == 's' && s[4] == 'e' && s[5] == '\0') {
        *out = urbi_make_bool(0);
        return UEXEC_OK;
    }
    return urbi_raise_type(vm,
        "String.asBoolean: only \"true\" / \"false\" recognized", out);
}

/* === String.asciiAt =======================================================
 *
 * Byte-level codepoint access — returns the byte at the given index as
 * Integer (0..255).  charAt returns a 1-byte string slice; asciiAt
 * returns the numeric byte value.  Codepoint-aware variants (codePointAt
 * etc.) are deferred to a later Unicode follow-up (delta §3.2). */

static int
str_asciiAt(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "String.asciiAt", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "String.asciiAt: self must be String", out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "String.asciiAt: index must be Integer", out);

    const char *s = (const char *)self.v.p;
    if (s == NULL) return urbi_raise_type(vm, "String.asciiAt: NULL string", out);
    size_t n = urbi_strlen(s);
    int64_t i = args[0].v.i;
    if (i < 0 || (size_t)i >= n)
        return urbi_raise_range(vm, "String.asciiAt: index out of range", out);

    *out = urbi_make_int((int64_t)(unsigned char)s[i]);
    return UEXEC_OK;
}

/* === String split / join / format =========================================
 *
 * split(sep) -> List of String pieces (empty sep splits per byte).
 * join(list) -> String, with `self` as the separator between elements.
 * format(list) -> printf-style %s / %d / %f / %% substitution (minimal).
 *
 * All build results via the allocate-fill-intern-free pattern (str_caseop
 * template) using byte loops (no <string.h> memcpy/memcmp dependency). */

/* byte-equality at s[0..n) vs t[0..n) */
static int
bytes_eq(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int
str_split(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "String.split", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_STR || args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "split: self and separator must be String", out);

    const char *s = (const char *)self.v.p;
    const char *sep = (const char *)args[0].v.p;
    if (s == NULL || sep == NULL)
        return urbi_raise_type(vm, "split: NULL string", out);
    size_t n = urbi_strlen(s), seplen = urbi_strlen(sep);

    UObject *lst = urbi_stdlib_list_new_empty(vm);
    if (lst == NULL) return urbi_raise_oom(vm, out);

    if (seplen == 0U) {   /* empty sep -> per-byte split (legacy string.cc:385-391).
                           * Per-BYTE, not per-character: multi-byte UTF-8
                           * characters split into byte fragments — matches
                           * the reference implementation (foreach char c). */
        size_t j;
        for (j = 0U; j < n; j++) {
            int oom = 0;
            UValue ch = urbi_val_str_intern(vm, s + j, 1U, &oom);
            if (oom) return urbi_raise_oom(vm, out);
            if (urbi_stdlib_list_append_value(vm, lst, ch) != 0)
                return urbi_raise_oom(vm, out);
        }
        *out = urbi_make_object(lst);
        return UEXEC_OK;
    }

    size_t start = 0U, i = 0U;
    while (i + seplen <= n) {
        if (bytes_eq(s + i, sep, seplen)) {
            UValue piece = urbi_make_str_interned(vm, s + start, i - start);
            if (piece.kind == (uint8_t)UVAL_NIL) return urbi_raise_oom(vm, out);
            if (urbi_stdlib_list_append_value(vm, lst, piece) != 0)
                return urbi_raise_oom(vm, out);
            i += seplen;
            start = i;
        } else {
            i++;
        }
    }
    {
        UValue last = urbi_make_str_interned(vm, s + start, n - start);
        if (last.kind == (uint8_t)UVAL_NIL) return urbi_raise_oom(vm, out);
        if (urbi_stdlib_list_append_value(vm, lst, last) != 0)
            return urbi_raise_oom(vm, out);
    }
    *out = urbi_make_object(lst);
    return UEXEC_OK;
}

static int
str_join(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "String.join", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "join: self (separator) must be String", out);
    if (args[0].kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "join: argument must be a List", out);
    const char *sep = (const char *)self.v.p;
    size_t seplen = urbi_strlen(sep);
    return join_core(vm, sep, seplen, (UObject *)args[0].v.p, out);
}

/* format — minimal printf substitution.  Numeric specs (%d/%f) require the
 * hosted snprintf, mirroring Integer.asString; freestanding builds raise.
 * Output capped at 1024 bytes (raise on overflow).
 *
 * Arg-count contract (legacy share/urbi/string.u): spec count must equal
 * the list length; mismatch raises ArityError.  This matches legacy
 * `throw "invalid format: wrong number of arguments"`.
 *
 * Kind-mismatch coercion: mismatched argument kinds are silently coerced —
 * %s on a non-String produces "", %d on a non-Integer produces 0, %f on a
 * non-numeric produces 0.0.  This matches the legacy `%` operator which
 * uses `res += list.removeFront()` (implicit asString) without any kind
 * guard.  No TypeError is raised on kind mismatch. */
static size_t
count_format_specs(const char *fmt, size_t n)
{
    size_t count = 0U, i;
    for (i = 0U; i < n; i++) {
        if (fmt[i] == '%' && i + 1U < n) {
            char k = fmt[i + 1U];
            if (k == '%') { i++; continue; }
            if (k == 's' || k == 'd' || k == 'f') { count++; i++; continue; }
            /* unknown spec: the % is emitted literally and no arg consumed */
        }
    }
    return count;
}

static int
str_format(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "String.format", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "format: self must be String", out);
    if (args[0].kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "format: argument must be a List", out);

    const char *fmt = (const char *)self.v.p;
    if (fmt == NULL) return urbi_raise_type(vm, "format: NULL string", out);
    size_t n = urbi_strlen(fmt);
    UObject *list_obj = (UObject *)args[0].v.p;
    if (!urbi_stdlib_list_storage_present(vm, list_obj))
        return urbi_raise_type(vm, "format: argument must be a List", out);
    size_t argc = urbi_stdlib_list_len(vm, list_obj);

    /* Arg-count pre-check: raise before any substitution. */
    size_t spec_count = count_format_specs(fmt, n);
    if (spec_count != argc) {
        return urbi_raise_arity(vm, "String.format",
                                (uint8_t)(spec_count < 255U ? spec_count : 255U),
                                (uint8_t)(argc < 255U ? argc : 255U),
                                out);
    }

    char buf[1024];
    size_t off = 0U, i = 0U, ai = 0U;
    while (i < n) {
        char c = fmt[i];
        if (c == '%' && i + 1U < n) {
            char k = fmt[i + 1U];
            if (k == '%') {
                if (off + 1U >= sizeof buf) return urbi_raise_type(vm, "format: overflow", out);
                buf[off++] = '%'; i += 2U; continue;
            }
            UValue a = (ai < argc) ? urbi_stdlib_list_get(vm, list_obj, ai) : urbi_make_nil();
            ai++;
            if (k == 's') {
                const char *sv = (a.kind == (uint8_t)UVAL_STR) ? (const char *)a.v.p : "";
                size_t sl = urbi_strlen(sv);
                if (off + sl >= sizeof buf) return urbi_raise_type(vm, "format: overflow", out);
                urbi_memcpy(buf + off, sv, sl); off += sl;
                i += 2U; continue;
            }
#if __STDC_HOSTED__
            if (k == 'd') {
                char tmp[32];
                int tn = snprintf(tmp, sizeof tmp, "%lld",
                                  (long long)((a.kind == (uint8_t)UVAL_INT) ? a.v.i : 0));
                if (tn <= 0) return urbi_raise_type(vm, "format: int conversion failed", out);
                if (off + (size_t)tn >= sizeof buf) return urbi_raise_type(vm, "format: overflow", out);
                { int j; for (j = 0; j < tn; j++) buf[off++] = tmp[j]; }
                i += 2U; continue;
            }
            if (k == 'f') {
                char tmp[64];
                double dv = (a.kind == (uint8_t)UVAL_FLOAT) ? (double)a.v.f :
                            (a.kind == (uint8_t)UVAL_INT)   ? (double)a.v.i : 0.0;
                int tn = snprintf(tmp, sizeof tmp, "%g", dv);
                if (tn <= 0) return urbi_raise_type(vm, "format: float conversion failed", out);
                if (off + (size_t)tn >= sizeof buf) return urbi_raise_type(vm, "format: overflow", out);
                { int j; for (j = 0; j < tn; j++) buf[off++] = tmp[j]; }
                i += 2U; continue;
            }
#else
            if (k == 'd' || k == 'f')
                return urbi_raise_type(vm, "format: numeric specs need a hosted build", out);
#endif
            /* unknown spec: emit the '%' literally, rewind the arg consumed */
            ai--;
            if (off + 1U >= sizeof buf) return urbi_raise_type(vm, "format: overflow", out);
            buf[off++] = c; i++; continue;
        }
        if (off + 1U >= sizeof buf) return urbi_raise_type(vm, "format: overflow", out);
        buf[off++] = c; i++;
    }

    int oom = 0;
    UValue v = urbi_val_str_intern(vm, buf, off, &oom);
    if (oom) return urbi_raise_oom(vm, out);
    *out = v;
    return UEXEC_OK;
}

/* === String % — format operator ============================================
 *
 * `fmt % arg` — if arg is a List, delegate directly to str_format; otherwise
 * wrap arg in a one-element list first.  Matches legacy urbiscript where `%`
 * is the infix sugar for String.format with auto-wrapping of scalar operands.
 *
 * Constraint: do NOT rewrite str_format — the separate error-behavior task
 * touches it independently. */
static int
str_percent(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "String.%", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_STR, "String.%: self must be String", out);
    if (args[0].kind == (uint8_t)UVAL_OBJECT)
        return str_format(vm, self, args, 1U, out);
    /* Non-list scalar: wrap in a one-element list then delegate. */
    UObject *lst = urbi_stdlib_list_new_empty(vm);
    if (lst == NULL) return urbi_raise_oom(vm, out);
    if (urbi_stdlib_list_append_value(vm, lst, args[0]) != 0)
        return urbi_raise_oom(vm, out);
    UValue list_val = urbi_make_object(lst);
    return str_format(vm, self, &list_val, 1U, out);
}

/* === Per-family method tables ============================================= */

static const UNativeMethodDef BOOL_METHODS[] = {
    { "negate", bool_negate }
};
static const UNativeMethodDef INT_METHODS[] = {
    { "asString",  int_asString  },
    { "asFloat",   int_asFloat   },
    { "asBoolean", int_asBoolean },
    { "asInteger", int_asInteger },
    { "and",       int_and       },
    { "or",        int_or        },
    { "xor",       int_xor       },
    { "inv",       int_inv       },
    { "shl",       int_shl       },
    { "shr",       int_shr       },
    { "ushr",      int_ushr      },
    { "%",         int_mod       }
};
static const UNativeMethodDef FLOAT_METHODS[] = {
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
    { "asBoolean",  flt_asBoolean  },
    { "%",          flt_mod        },
    { "random",     flt_random     }
};
static const UNativeMethodDef STR_METHODS[] = {
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
    { "asciiAt",    str_asciiAt    },
    { "split",      str_split      },
    { "join",       str_join       },
    { "format",     str_format     },
    { "%",          str_percent    }   /* infix format sugar */
};

/* Empty tables retain a `{NULL, NULL}` sentinel so the array has at
 * least one element (C99 forbids zero-size arrays).  Tables with real
 * entries omit the sentinel. */

/* === urbi_stdlib_register_atom_methods =====================================
 *
 * This function lands the helper + boot wiring; the per-family method tables fill
 * in across sections.  At baseline all four tables are sentinel-only
 * (count == 0); URBI_REGISTER_METHODS is a no-op for those but the call
 * sites are wired so subsequent tasks only edit the table arrays. */

int
urbi_stdlib_register_atom_methods(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;

    int rc;
    rc = URBI_REGISTER_METHODS(vm, urbi_object_atom(vm, URBI_ATOM_BOOLEAN),
                               BOOL_METHODS);
    if (rc != URBI_OK) return rc;

    rc = URBI_REGISTER_METHODS(vm, urbi_object_atom(vm, URBI_ATOM_INTEGER),
                               INT_METHODS);
    if (rc != URBI_OK) return rc;

    rc = URBI_REGISTER_METHODS(vm, urbi_object_atom(vm, URBI_ATOM_FLOAT),
                               FLOAT_METHODS);
    if (rc != URBI_OK) return rc;

    rc = URBI_REGISTER_METHODS(vm, urbi_object_atom(vm, URBI_ATOM_STRING),
                               STR_METHODS);
    if (rc != URBI_OK) return rc;

    return URBI_OK;
}
