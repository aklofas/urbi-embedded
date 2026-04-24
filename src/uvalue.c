/* SPDX-License-Identifier: BSD-3-Clause */
/* UValue formatter — hosted-only (snprintf + <stdio.h>). */

#include "uvalue.h"

#if __STDC_HOSTED__

#include <stdio.h>
#include <string.h>

size_t uvalue_format(const UValue *v, char *buf, size_t cap) {
    if (cap == 0) return 0;
    int n = 0;
    switch ((UValKind)v->kind) {
    case UVAL_NIL:
        n = snprintf(buf, cap, "nil");
        break;
    default:
        n = snprintf(buf, cap, "<?>");
        break;
    }
    if (n < 0) { buf[0] = '\0'; return 0; }
    if ((size_t)n >= cap) return cap - 1;
    return (size_t)n;
}

#endif /* __STDC_HOSTED__ */
