/* SPDX-License-Identifier: BSD-3-Clause */
/* UValue → printable-string formatter.  Hosted-only. */

#ifndef UVALUE_H
#define UVALUE_H

#include <stddef.h>

#include "umodule.h"  /* UValue, UValKind */

#ifdef __cplusplus
extern "C" {
#endif

/* Format *v as a printable string into buf (NUL-terminated).
   Returns number of bytes written excluding the terminator.
   If cap is too small, result is truncated but buf is always NUL-terminated
   whenever cap > 0.  Minimum recommended cap: 64 bytes.
   Caller-supplied buffer; no allocation. */
size_t uvalue_format(const UValue *v, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* UVALUE_H */
