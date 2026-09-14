/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/ulist.h — List and Dict cells for the refound/core runtime.
 *
 * Both are plain GC cells with a `proto` pointer so method dispatch
 * (`l.size`) resolves through the shared List/Dict prototype object.
 * UDict is a linear array of parallel key/value slots -- no hashing --
 * scanned with uv_equal; fine for the small dicts urbiscript programs
 * actually build, and it keeps the cell trivially traceable. */

#ifndef URT_LIST_H
#define URT_LIST_H
#include "rt/uobj.h"

typedef struct UList { UCell cell; UObject *proto; UValue *items; uint32_t len, cap; } UList;
typedef struct UDict { UCell cell; UObject *proto; UValue *keys; UValue *vals; uint32_t len, cap; } UDict;   /* linear; keys compared with uv_equal */

/* proto may be NULL. May collect (via ugc_alloc); root `proto` first if
 * it isn't otherwise reachable. cap_hint is rounded up to at least 4 and
 * preallocated; a failed preallocation just leaves the list at cap 0
 * (the next push/insert retries growth) rather than failing the whole
 * allocation. */
UList *ulist_new(struct UVM *vm, UObject *proto, uint32_t cap_hint);
int    ulist_push(struct UVM *vm, UList *l, UValue v);          /* 0 ok, -1 OOM */
int    ulist_insert(struct UVM *vm, UList *l, uint32_t at, UValue v);   /* at > len: -1; at == len: append */
bool   ulist_remove_at(UList *l, uint32_t at);                  /* shifts down; false if at >= len */

UDict *udict_new(struct UVM *vm, UObject *proto);
int    udict_set(struct UVM *vm, UDict *d, UValue k, UValue v);      /* add or overwrite; 0 ok, -1 OOM */
bool   udict_get(const UDict *d, UValue k, UValue *out);
bool   udict_remove(UDict *d, UValue k);                             /* swaps last into the hole */

bool   uv_equal(UValue a, UValue b);        /* structural for numbers/strings/bools/nil; identity otherwise */

void   ulist_trace(struct UVM *vm, UList *l); void ulist_finalize(struct UVM *vm, UList *l);
void   udict_trace(struct UVM *vm, UDict *d); void udict_finalize(struct UVM *vm, UDict *d);

static inline UValue uv_list(UList *l) { return uv_ptr(UV_CELL, l); }
static inline UValue uv_dict(UDict *d) { return uv_ptr(UV_CELL, d); }
static inline bool uv_is_list(UValue v) { return v.kind == UV_CELL && ((UCell *)v.v.p)->type == UCELL_LIST; }
static inline bool uv_is_dict(UValue v) { return v.kind == UV_CELL && ((UCell *)v.v.p)->type == UCELL_DICT; }
#endif
