/* SPDX-License-Identifier: BSD-3-Clause */
/* UValue semantic helpers + formatter (formatter: hosted-only). */

#include "value/uvalue.h"
#include "gc/ugc.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"          /* URBI_ASSERT_NOT_ISR */
#include "runtime/umacros.h"    /* URBI_INTERNAL_ASSERT */
#include "module/umodule.h"

/* --- Value semantic helpers (freestanding-safe). --- */

bool uvalue_truthy(const UValue *v) {
    if (v == NULL) return false;
    switch ((UValKind)v->kind) {
        case UVAL_NIL:    return false;
        case UVAL_BOOL:   return v->v.i != 0;
        case UVAL_VOID:   return false;
        case UVAL_STRAND: return true;   /* strand handle is truthy (matches closure pattern) */
        case UVAL_OBJECT: return true;   /* object reference is truthy (matches closure pattern) */
        default:          return true;   /* int 0, float 0.0, etc. → truthy */
    }
}

bool uvalue_equal(const UValue *a, const UValue *b) {
    if (a == NULL || b == NULL) return false;

    /* Same kind: direct compare. */
    if (a->kind == b->kind) {
        switch ((UValKind)a->kind) {
            case UVAL_NIL:     return true;
            case UVAL_INT:     return a->v.i == b->v.i;
            case UVAL_FLOAT:   return a->v.f == b->v.f;
            case UVAL_BOOL:    return a->v.i == b->v.i;
            case UVAL_STR:     return a->v.p == b->v.p;     /* interned ptr eq */
            case UVAL_CLOSURE: return a->v.p == b->v.p;     /* identity */
            case UVAL_VOID:    return false;                 /* void != void per spec */
            case UVAL_STRAND:  return a->v.p == b->v.p;     /* strand identity */
            case UVAL_OBJECT:  return a->v.p == b->v.p;     /* object identity */
            case UVAL_EVENT:   return a->v.p == b->v.p;     /* event identity */
            case UVAL_HOST_FN: return a->v.p == b->v.p;     /* fn-pointer identity */
        }
    }

    /* Cross-kind numeric promotion: INT vs FLOAT. */
    if (a->kind == (uint8_t)UVAL_INT && b->kind == (uint8_t)UVAL_FLOAT) {
        return (double)a->v.i == b->v.f;
    }
    if (a->kind == (uint8_t)UVAL_FLOAT && b->kind == (uint8_t)UVAL_INT) {
        return a->v.f == (double)b->v.i;
    }

    /* Different kinds, no promotion path → not equal. */
    return false;
}

UValCmpResult uvalue_lt(const UValue *a, const UValue *b, bool *out) {
    if (a == NULL || b == NULL) return UVAL_CMP_TYPE_ERROR;
    if (a->kind == (uint8_t)UVAL_INT && b->kind == (uint8_t)UVAL_INT) {
        *out = a->v.i < b->v.i;
        return UVAL_CMP_OK;
    }
    if (a->kind == (uint8_t)UVAL_FLOAT && b->kind == (uint8_t)UVAL_FLOAT) {
        *out = a->v.f < b->v.f;
        return UVAL_CMP_OK;
    }
    if (a->kind == (uint8_t)UVAL_INT && b->kind == (uint8_t)UVAL_FLOAT) {
        *out = (double)a->v.i < b->v.f;
        return UVAL_CMP_OK;
    }
    if (a->kind == (uint8_t)UVAL_FLOAT && b->kind == (uint8_t)UVAL_INT) {
        *out = a->v.f < (double)b->v.i;
        return UVAL_CMP_OK;
    }
    return UVAL_CMP_TYPE_ERROR;
}

UValCmpResult uvalue_le(const UValue *a, const UValue *b, bool *out) {
    bool lt = false;
    UValCmpResult r = uvalue_lt(a, b, &lt);
    if (r != UVAL_CMP_OK) return r;
    *out = lt || uvalue_equal(a, b);
    return UVAL_CMP_OK;
}

/* --- Host type registration (folded from utype.c — FOUND-020) ---
 * urbi_register_type is declared in ugc.h (row 10 §7). */

uint8_t
urbi_register_type(UVM *vm, const UType *type)
{
    URBI_ASSERT_NOT_ISR(vm);

    uint8_t tag = type->type_tag;

    if (tag == 0U) {
        /* Auto-assign next free host slot. */
        URBI_INTERNAL_ASSERT(
            vm->host_type_count < (uint8_t)(UTYPE_HOST_MAX - UTYPE_HOST_BASE + 1U));
        tag = (uint8_t)(UTYPE_HOST_BASE + vm->host_type_count);
        vm->host_type_count++;
    } else if (tag >= UTYPE_HOST_BASE /* && tag <= UTYPE_HOST_MAX */) {
        /* Host-allocated explicit tag — use as-is. */
    } else {
        /* Tags 1..(UTYPE_HOST_BASE-1) are reserved for built-in types and
         * must not be registered via this API.
         * M4 NOTE: built-in types (UTYPE_OBJECT/CLOSURE/STRING/etc., tags 1..63)
         * cannot be registered through urbi_register_type — they must write
         * vm->type_table[tag] directly via an internal init function
         * (e.g., builtin_types_init(vm) called from urbi_vm_init). This guard exists
         * to catch accidental host misuse of those slots.
         * URBI_INTERNAL_ASSERT fires in URBI_DEBUG builds; returns 0 in release. */
        URBI_INTERNAL_ASSERT(0);
        return 0U;
    }

    /* Detect collision: explicit-tag must point at a free slot.  Auto-assign
     * doesn't bump host_type_count past explicit registrations, so mixing the
     * two patterns can collide.  Catch in URBI_DEBUG. */
    URBI_INTERNAL_ASSERT(vm->type_table[tag] == NULL);

    vm->type_table[tag] = (UType *)type;
    return tag;
}

#if __STDC_HOSTED__

#include <stdint.h>
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
    case UVAL_STR: {
        /* FOUND-004: read interned-string pointer via v.p (the union member
         * that semantically owns the pointer), not via v.i + uintptr_t cast. */
        const char *s = (const char *)v->v.p;
        size_t w = 0;
        if (w + 1 >= cap) { buf[0] = '\0'; return 0; }
        buf[w++] = '"';
        for (size_t k = 0; s[k] != '\0'; k++) {
            unsigned char c = (unsigned char)s[k];
            const char *esc = NULL;
            char single = 0;
            switch (c) {
            case '\\': esc = "\\\\"; break;
            case '"':  esc = "\\\""; break;
            case '\n': esc = "\\n"; break;
            case '\t': esc = "\\t"; break;
            case '\r': esc = "\\r"; break;
            /* Unreachable during normal iteration — the outer for-loop
               terminates at '\0'. Kept as defensive coverage for
               callers that ever iterate past the terminator. */
            case '\0': esc = "\\0"; break;
            default:
                if (c >= 0x20 && c < 0x7f) { single = (char)c; }
                break;
            }
            /* FOUND-005: every escape branch must reserve room for the
             * trailing closing quote.  Reserved bytes = escape length + 1
             * (for the trailing '"'). */
            if (esc) {
                if (w + 2 + 1 >= cap) break;   /* 2 escape + 1 trailing quote */
                buf[w++] = esc[0];
                buf[w++] = esc[1];
            } else if (single) {
                if (w + 1 + 1 >= cap) break;   /* 1 char + 1 trailing quote */
                buf[w++] = single;
            } else {
                /* \xNN hex escape for non-printable bytes */
                if (w + 4 + 1 >= cap) break;   /* 4 escape + 1 trailing quote */
                static const char hex[] = "0123456789abcdef";
                buf[w++] = '\\';
                buf[w++] = 'x';
                buf[w++] = hex[(c >> 4) & 0xf];
                buf[w++] = hex[c & 0xf];
            }
        }
        if (w + 1 >= cap) { buf[w] = '\0'; return w; }
        buf[w++] = '"';
        buf[w] = '\0';
        return w;
    }
    case UVAL_STRAND:
        n = snprintf(buf, cap, "<strand>");
        break;
    case UVAL_OBJECT:
        n = snprintf(buf, cap, "<object %p>", v->v.p);
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
