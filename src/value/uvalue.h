/* SPDX-License-Identifier: BSD-3-Clause */
/* UValue → printable-string formatter.  Hosted-only. */

#ifndef UVALUE_H
#define UVALUE_H

#include <stdbool.h>
#include <stddef.h>

#include "chunk/umodule.h"  /* UValue, UValKind */

#ifdef __cplusplus
extern "C" {
#endif

/* Format *v as a printable string into buf (NUL-terminated).
   Returns number of bytes written excluding the terminator.
   If cap is too small, result is truncated but buf is always NUL-terminated
   whenever cap > 0.  Minimum recommended cap: 64 bytes.
   Caller-supplied buffer; no allocation. */
size_t uvalue_format(const UValue *v, char *buf, size_t cap);

/* Truthiness rule: nil/false/void → false; everything else → true.
   Note: int 0 and float 0.0 are TRUTHY (only nil, bool-false, void are falsy). */
bool uvalue_truthy(const UValue *v);

/* Equality with cross-kind numeric promotion.
   INT==FLOAT (and reverse) promotes INT to double for comparison.
   NIL==NIL → true.  VOID==anything → false (void is never equal, per spec).
   Different kinds with no promotion path → false. */
bool uvalue_equal(const UValue *a, const UValue *b);

/* Ordered comparison helpers — numeric only at M2.
   Returns UVAL_CMP_OK with *out set, or UVAL_CMP_TYPE_ERROR for non-numeric. */
typedef enum { UVAL_CMP_OK = 0, UVAL_CMP_TYPE_ERROR } UValCmpResult;
UValCmpResult uvalue_lt(const UValue *a, const UValue *b, bool *out);
UValCmpResult uvalue_le(const UValue *a, const UValue *b, bool *out);

#ifdef __cplusplus
}
#endif

#endif /* UVALUE_H */
