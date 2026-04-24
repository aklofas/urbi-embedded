/* SPDX-License-Identifier: BSD-3-Clause */
/* UValue formatter — hosted-only (snprintf + <stdio.h>). */

#include "uvalue.h"

#if __STDC_HOSTED__

#include <stdio.h>
#include <string.h>

#if URBI_FLOAT_TYPE == 8
#  define UVALUE_FLOAT_FMT "%.14g"
#else
#  define UVALUE_FLOAT_FMT "%.7g"
#endif

size_t uvalue_format(const UValue *v, char *buf, size_t cap) {
    if (cap == 0) return 0;
    int n = 0;
    switch ((UValKind)v->kind) {
    case UVAL_NIL:
        n = snprintf(buf, cap, "nil");
        break;
    case UVAL_BOOL:
        n = snprintf(buf, cap, "%s", v->v.i ? "true" : "false");
        break;
    case UVAL_INT:
        n = snprintf(buf, cap, "%lld", (long long)v->v.i);
        break;
    case UVAL_FLOAT: {
        n = snprintf(buf, cap, UVALUE_FLOAT_FMT, (double)v->v.f);
        if (n < 0) break;
        /* If snprintf would have produced more than cap-1 bytes, skip the
           trailing-.0 logic — there's no room left for it anyway. */
        if ((size_t)n >= cap) break;
        /* Lua 5.4 rule: append ".0" if the result looks like an integer
           (no '.', 'e', 'E', 'n' for nan, 'i' for inf). */
        int needs_dot_zero = 1;
        for (int k = 0; k < n; k++) {
            char c = buf[k];
            if (c == '.' || c == 'e' || c == 'E' || c == 'n' || c == 'i') {
                needs_dot_zero = 0;
                break;
            }
        }
        if (needs_dot_zero && (size_t)(n + 2) < cap) {
            buf[n++] = '.';
            buf[n++] = '0';
            buf[n] = '\0';
        }
        break;
    }
    default:
        n = snprintf(buf, cap, "<?>");
        break;
    }
    if (n < 0) { buf[0] = '\0'; return 0; }
    if ((size_t)n >= cap) return cap - 1;
    return (size_t)n;
}

#endif /* __STDC_HOSTED__ */
