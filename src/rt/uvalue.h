/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uvalue.h — value representation for the refound/core runtime.
 *
 * UValue itself is NOT redefined here — it is the public struct declared
 * in include/urbi/types.h, shared with the kept frontend (src/chunk/) and
 * the old core so both sides agree on layout without a second definition.
 * This header adds the new core's own kind enum (aliasing the public
 * UValKind numeric values) and a small uv_* constructor/predicate API. */

#ifndef URT_VALUE_H
#define URT_VALUE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "urbi/types.h"

typedef enum {
    UV_NIL  = UVAL_NIL,
    UV_VOID = UVAL_VOID,
    UV_BOOL = UVAL_BOOL,
    UV_INT  = UVAL_INT,
    UV_FLOAT = UVAL_FLOAT,
    UV_SYM  = UVAL_SYM,
    UV_STR  = UVAL_STR,
    UV_OBJ  = UVAL_OBJECT,
    UV_CELL = UVAL_CELL
} UVKind;

static inline UValue uv_nil(void)          { UValue v; v.kind = UV_NIL;   v.v.i = 0; return v; }
static inline UValue uv_void(void)         { UValue v; v.kind = UV_VOID;  v.v.i = 0; return v; }
static inline UValue uv_bool(bool b)       { UValue v; v.kind = UV_BOOL;  v.v.i = b; return v; }
static inline UValue uv_int(int64_t i)     { UValue v; v.kind = UV_INT;   v.v.i = i; return v; }
static inline UValue uv_float(double f)    { UValue v; v.kind = UV_FLOAT; v.v.f = f; return v; }
static inline UValue uv_ptr(UVKind k, void *p) { UValue v; v.kind = (uint8_t)k; v.v.p = p; return v; }
static inline bool   uv_is(UValue v, UVKind k) { return v.kind == (uint8_t)k; }
static inline bool   uv_is_number(UValue v) { return v.kind == UV_INT || v.kind == UV_FLOAT; }
static inline double uv_as_double(UValue v) { return v.kind == UV_INT ? (double)v.v.i : v.v.f; }
/* Truthiness: nil, void, bool false, int 0, float 0.0 are false; every
 * other kind (including SYM/STR/OBJ/CELL and any legacy kind) is true.
 * Truthiness is pinned by the .chk corpus, which encodes what the old
 * core answered here; tests/chk/ is the specification. */
static inline bool uv_truthy(UValue v) {
    switch (v.kind) {
    case UV_NIL: case UV_VOID: return false;
    case UV_BOOL: case UV_INT: return v.v.i != 0;
    case UV_FLOAT: return v.v.f != 0.0;
    default: return true;
    }
}
#endif
