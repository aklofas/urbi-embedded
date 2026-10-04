/* SPDX-License-Identifier: BSD-3-Clause */
/* src/host/uformat.c — hosted-only public API helpers.
 *
 * src/rt/ is held to the freestanding rule (spec section 3): no libc
 * beyond <stdint.h>, <stddef.h>, <stdbool.h>, <string.h> and <math.h>,
 * enforced by tests/scripts/check_rt_layering.sh.  Rendering a Float the
 * way the .chk corpus pins it needs snprintf's "%.14g", which no
 * hand-rolled formatter reproduces, so urbi_value_to_string lives here
 * instead of in src/rt/uapi.c.
 *
 * This directory is for exactly that: public API whose implementation is
 * inherently hosted.  A freestanding build omits it and the symbol is
 * absent, the same way urbi_compile is absent without the frontend. */

#include <stdio.h>

#include "urbi/urbi.h"
#include "rt/uexec.h"

size_t urbi_value_to_string(UVM *vm, UValue v, char *buf, size_t cap)
{
    (void)vm;
    if (cap == 0 || buf == NULL) return 0;
    int n = 0;
    switch (v.kind) {
    case UV_NIL:  n = snprintf(buf, cap, "nil"); break;
    case UV_BOOL: n = snprintf(buf, cap, "%s", v.v.i ? "true" : "false"); break;
    case UV_INT:  n = snprintf(buf, cap, "%lld", (long long)v.v.i); break;
    case UV_FLOAT: {
        n = snprintf(buf, cap, "%.14g", v.v.f);
        if (n < 0 || (size_t)n >= cap) break;
        /* Lua's rule: append ".0" when the result reads as an integer, so
         * 4/2 shows as 2.0 rather than 2.  The corpus pins this. */
        int needs_dot_zero = 1;
        for (int k = 0; k < n; k++) {
            char c = buf[k];
            if (c == '.' || c == 'e' || c == 'E' || c == 'n' || c == 'i') { needs_dot_zero = 0; break; }
        }
        if (needs_dot_zero && (size_t)n + 2U < cap) {
            buf[n++] = '.'; buf[n++] = '0'; buf[n] = '\0';
        }
        break;
    }
    case UV_SYM: case UV_STR: {
        uint32_t len;
        const char *s = uv_str_bytes(v, &len);
        size_t w = 0;
        if (w + 1 >= cap) { buf[0] = '\0'; return 0; }
        buf[w++] = '"';
        for (uint32_t k = 0; k < len; k++) {
            unsigned char c = (unsigned char)s[k];
            const char *esc = NULL;
            switch (c) {
            case '\\': esc = "\\\\"; break;
            case '"':  esc = "\\\""; break;
            case '\n': esc = "\\n"; break;
            case '\t': esc = "\\t"; break;
            case '\r': esc = "\\r"; break;
            default: break;
            }
            if (esc) {
                if (w + 3 >= cap) break;
                buf[w++] = esc[0]; buf[w++] = esc[1];
            } else if (c >= 0x20 && c < 0x7f) {
                if (w + 2 >= cap) break;
                buf[w++] = (char)c;
            } else {
                static const char hex[] = "0123456789abcdef";
                if (w + 5 >= cap) break;
                buf[w++] = '\\'; buf[w++] = 'x';
                buf[w++] = hex[(c >> 4) & 0xf]; buf[w++] = hex[c & 0xf];
            }
        }
        if (w + 1 >= cap) { buf[w] = '\0'; return w; }
        buf[w++] = '"';
        buf[w] = '\0';
        return w;
    }
    case UV_OBJ: n = snprintf(buf, cap, "<object %p>", v.v.p); break;
    case UV_CELL:
        /* The scheduler's cells print as themselves.  A tag shows its
         * name when it has one, because a named tag is something the
         * reader wrote down; everything else would only be an address. */
        switch (((const UCell *)v.v.p)->type) {
        case UCELL_TAG: {
            const UTag *t = (const UTag *)v.v.p;
            uint32_t nl = 0;
            const char *nb = (t->name.kind == UV_STR || t->name.kind == UV_SYM)
                           ? uv_str_bytes(t->name, &nl) : NULL;
            n = (nb && nl) ? snprintf(buf, cap, "<Tag: %.*s>", (int)nl, nb)
                           : snprintf(buf, cap, "<Tag>");
            break;
        }
        case UCELL_EVENT:  n = snprintf(buf, cap, "<event>"); break;
        case UCELL_STRAND: n = snprintf(buf, cap, "<Job %u>", (unsigned)((const UStrand *)v.v.p)->id); break;
        default:           n = snprintf(buf, cap, "<?>"); break;
        }
        break;
    default:
        /* Closures and void render as "<?>" — the spelling the corpus has
         * pinned since the first REPL fixtures. */
        n = snprintf(buf, cap, "<?>");
        break;
    }
    if (n < 0) { buf[0] = '\0'; return 0; }
    if ((size_t)n >= cap) return cap - 1;
    return (size_t)n;
}
