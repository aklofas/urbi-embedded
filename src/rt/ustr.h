/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/ustr.h — symbol table (immortal, interned) and GC-managed string
 * cells for the refound/core runtime. */

#ifndef URT_STR_H
#define URT_STR_H
#include <string.h>
#include "rt/ugc.h"

typedef struct USym  { uint32_t hash; uint32_t len; struct USym *next; char bytes[1]; } USym;   /* immortal, interned */
typedef struct UStr  { UCell cell; uint32_t hash; uint32_t len; char bytes[1]; } UStr;          /* GC cell */

typedef struct UStrTab { USym **buckets; uint32_t nbuckets, count; } UStrTab;
UStrTab *uvm_strings(struct UVM *vm);                      /* defined in uexec.c */

int      ustrtab_init(struct UVM *vm, UStrTab *t);
void     ustrtab_destroy(struct UVM *vm, UStrTab *t);
USym    *usym_intern(struct UVM *vm, const char *s, size_t n);   /* NULL on OOM */
static inline USym *usym_cstr(struct UVM *vm, const char *s) { return usym_intern(vm, s, strlen(s)); }
UStr    *ustr_new(struct UVM *vm, const char *s, size_t n);      /* GC cell; NULL on OOM */
/* ustr_concat's inputs are raw byte pointers by design: ugc_alloc may run a
 * collection before allocating the result cell, so a and b must not be
 * pointers into a UStr/USym that the caller hasn't rooted -- root any
 * source cell (via the VM's C-root stack, landing in Task 7) before
 * calling this with bytes that live inside it. */
UStr    *ustr_concat(struct UVM *vm, const char *a, size_t na, const char *b, size_t nb);
uint32_t ustr_hash(const char *s, size_t n);                     /* FNV-1a 32 */
/* Uniform access for SYM and STR values. */
static inline const char *uv_str_bytes(UValue v, uint32_t *len) {
    if (v.kind == UV_SYM) { const USym *s = (const USym *)v.v.p; *len = s->len; return s->bytes; }
    const UStr *s = (const UStr *)v.v.p; *len = s->len; return s->bytes;
}
bool     uv_str_equal(UValue a, UValue b);       /* both SYM/STR; byte compare, SYM/SYM by pointer */
USym    *uv_to_sym(struct UVM *vm, UValue v);    /* SYM → itself; STR → interned */
static inline UValue uv_sym(USym *s) { return uv_ptr(UV_SYM, s); }
static inline UValue uv_str(UStr *s) { return uv_ptr(UV_STR, s); }
#endif
