/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_arith.h — VM arithmetic primitives, inlined into the dispatch loop.
 * Consumed by src/vm/ translation units. */

#ifndef UVM_ARITH_H
#define UVM_ARITH_H

#include <stdbool.h>
#include <stdint.h>

#include "vm/uvm.h"
#include "value/uvalue.h"

/* --- Arithmetic helpers.
       Each returns UVM_OK with result written into *a, or UVM_TYPE_ERROR
       leaving *a untouched. Integer overflow uses the unsigned-cast
       trick for portable two's-complement wrap (defined behavior; UBSan
       clean). Float promotion follows LANG-CONVENTIONS §1.3. --- */

/* Convenience: promote an Int/Float UValue to the target Float type. */
static inline double uvalue_to_double(const UValue *v) {
    return v->kind == UVAL_INT ? (double)v->v.i : (double)v->v.f;
}

static inline void uvalue_set_float(UValue *a, const double val) {
    a->kind = UVAL_FLOAT;
#if URBI_FLOAT_TYPE == 8
    a->v.f = val;
#else
    a->v.f = (float)val;
#endif
}

static inline bool is_number(const UValue *v) {
    return v->kind == UVAL_INT || v->kind == UVAL_FLOAT;
}

static inline UVMError arith_add(UValue *a, const UValue *b, const UValue *c) {
    if (!is_number(b) || !is_number(c)) return UVM_TYPE_ERROR;
    if (b->kind == UVAL_INT && c->kind == UVAL_INT) {
        a->kind = UVAL_INT;
        a->v.i = (int64_t)((uint64_t)b->v.i + (uint64_t)c->v.i);
        return UVM_OK;
    }
    uvalue_set_float(a, uvalue_to_double(b) + uvalue_to_double(c));
    return UVM_OK;
}

static inline UVMError arith_sub(UValue *a, const UValue *b, const UValue *c) {
    if (!is_number(b) || !is_number(c)) return UVM_TYPE_ERROR;
    if (b->kind == UVAL_INT && c->kind == UVAL_INT) {
        a->kind = UVAL_INT;
        a->v.i = (int64_t)((uint64_t)b->v.i - (uint64_t)c->v.i);
        return UVM_OK;
    }
    uvalue_set_float(a, uvalue_to_double(b) - uvalue_to_double(c));
    return UVM_OK;
}

static inline UVMError arith_mul(UValue *a, const UValue *b, const UValue *c) {
    if (!is_number(b) || !is_number(c)) return UVM_TYPE_ERROR;
    if (b->kind == UVAL_INT && c->kind == UVAL_INT) {
        a->kind = UVAL_INT;
        a->v.i = (int64_t)((uint64_t)b->v.i * (uint64_t)c->v.i);
        return UVM_OK;
    }
    uvalue_set_float(a, uvalue_to_double(b) * uvalue_to_double(c));
    return UVM_OK;
}

/* Signalled by arith_div when the divisor is zero, so OP_DIV raises a
   catchable DivByZero (legacy-conformant) instead of leaking an IEEE
   inf/NaN.  Positive so it stays distinct from UVM_OK (0) and the negative
   URBI_ERR_* codes; arith_div is only consumed at OP_DIV, which already
   branches on `rc != UVM_OK`. */
#define UVM_DIV_ZERO 1

static inline UVMError arith_div(UValue *a, const UValue *b, const UValue *c) {
    if (!is_number(b) || !is_number(c)) return UVM_TYPE_ERROR;
    /* DIV always produces Float per LANG-CONVENTIONS §1.3.  Legacy
       conformance (float.cc BOUNCE_OP `/`: `if (!rhs) RAISE("division by
       0")`): legacy urbi is all-Float, so a zero divisor raises for BOTH
       integer and float operands — and `!0.0` is true, so 0.0/0.0 raises
       too (no NaN result).  Follow that: any zero divisor → DivByZero. */
    double cd = uvalue_to_double(c);
    if (cd == 0.0) return UVM_DIV_ZERO;
    uvalue_set_float(a, uvalue_to_double(b) / cd);
    return UVM_OK;
}

static inline UVMError arith_neg(UValue *a, const UValue *b) {
    if (!is_number(b)) return UVM_TYPE_ERROR;
    if (b->kind == UVAL_INT) {
        a->kind = UVAL_INT;
        /* (int64_t)(-(uint64_t)v) wraps INT64_MIN to INT64_MIN.
           Defined behavior; UBSan clean. */
        a->v.i = (int64_t)(-(uint64_t)b->v.i);
        return UVM_OK;
    }
    /* Float negation; IEEE 754 flips the sign bit, defined for NaN/Inf. */
    uvalue_set_float(a, -uvalue_to_double(b));
    return UVM_OK;
}

#endif /* UVM_ARITH_H */
