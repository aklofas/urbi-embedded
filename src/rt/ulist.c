/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/ulist.c — see rt/ulist.h. */

#include "rt/ulist.h"

UList *ulist_new(struct UVM *vm, UObject *proto, uint32_t cap_hint) {
    UList *l = (UList *)ugc_alloc(vm, UCELL_LIST, sizeof(UList));
    if (!l) return NULL;
    l->proto = proto;
    uint32_t cap = 4;
    while (cap < cap_hint) cap *= 2;
    UValue *items = (UValue *)ugc_raw_alloc(vm, (size_t)cap * sizeof(UValue));
    if (!items) return l;   /* leave cap 0 / items NULL; next push/insert retries growth */
    l->items = items;
    l->cap = cap;
    return l;
}

/* Doubles items[] from 4. On OOM l->cap is left at its old value (see
 * uobj_grow's identical partial-failure discipline in uobj.c), so
 * ulist_finalize still frees exactly what it's told it owns. */
static int ulist_grow(struct UVM *vm, UList *l) {
    uint32_t new_cap = l->cap ? l->cap * 2 : 4;
    UValue *items = (UValue *)ugc_raw_realloc(vm, l->items, (size_t)l->cap * sizeof(UValue), (size_t)new_cap * sizeof(UValue));
    if (!items) return -1;
    l->items = items;
    l->cap = new_cap;
    return 0;
}

int ulist_push(struct UVM *vm, UList *l, UValue v) {
    if (l->len == l->cap && ulist_grow(vm, l) != 0) return -1;
    l->items[l->len++] = v;
    return 0;
}

int ulist_insert(struct UVM *vm, UList *l, uint32_t at, UValue v) {
    if (at > l->len) return -1;
    if (l->len == l->cap && ulist_grow(vm, l) != 0) return -1;
    for (uint32_t i = l->len; i > at; i--) l->items[i] = l->items[i - 1];
    l->items[at] = v;
    l->len++;
    return 0;
}

bool ulist_remove_at(UList *l, uint32_t at) {
    if (at >= l->len) return false;
    for (uint32_t i = at; i + 1 < l->len; i++) l->items[i] = l->items[i + 1];
    l->len--;
    return true;
}

UDict *udict_new(struct UVM *vm, UObject *proto) {
    UDict *d = (UDict *)ugc_alloc(vm, UCELL_DICT, sizeof(UDict));
    if (!d) return NULL;
    d->proto = proto;
    return d;
}

static int udict_find(const UDict *d, UValue k) {
    for (uint32_t i = 0; i < d->len; i++) if (uv_equal(d->keys[i], k)) return (int)i;
    return -1;
}

/* Doubles keys[]/vals[] together from 4, same partial-failure discipline
 * as ulist_grow / uobj_grow: allocate keys first, then vals, and only
 * bump d->cap once both succeed. */
static int udict_grow(struct UVM *vm, UDict *d) {
    uint32_t new_cap = d->cap ? d->cap * 2 : 4;
    UValue *keys = (UValue *)ugc_raw_realloc(vm, d->keys, (size_t)d->cap * sizeof(UValue), (size_t)new_cap * sizeof(UValue));
    if (!keys) return -1;
    d->keys = keys;
    UValue *vals = (UValue *)ugc_raw_realloc(vm, d->vals, (size_t)d->cap * sizeof(UValue), (size_t)new_cap * sizeof(UValue));
    if (!vals) return -1;
    d->vals = vals;
    d->cap = new_cap;
    return 0;
}

int udict_set(struct UVM *vm, UDict *d, UValue k, UValue v) {
    int idx = udict_find(d, k);
    if (idx >= 0) { d->vals[idx] = v; return 0; }
    if (d->len == d->cap && udict_grow(vm, d) != 0) return -1;
    d->keys[d->len] = k;
    d->vals[d->len] = v;
    d->len++;
    return 0;
}

bool udict_get(const UDict *d, UValue k, UValue *out) {
    int idx = udict_find(d, k);
    if (idx < 0) return false;
    *out = d->vals[idx];
    return true;
}

bool udict_remove(UDict *d, UValue k) {
    int idx = udict_find(d, k);
    if (idx < 0) return false;
    uint32_t last = d->len - 1;
    if ((uint32_t)idx != last) {
        d->keys[idx] = d->keys[last];
        d->vals[idx] = d->vals[last];
    }
    d->len = last;
    return true;
}

bool uv_equal(UValue a, UValue b) {
    if (uv_is_number(a) && uv_is_number(b)) {
        if (a.kind == UV_INT && b.kind == UV_INT) return a.v.i == b.v.i;
        return uv_as_double(a) == uv_as_double(b);
    }
    bool a_str = a.kind == UV_SYM || a.kind == UV_STR;
    bool b_str = b.kind == UV_SYM || b.kind == UV_STR;
    if (a_str && b_str) return uv_str_equal(a, b);
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case UV_NIL: case UV_VOID: return true;
    case UV_BOOL: return a.v.i == b.v.i;
    default: return a.v.p == b.v.p;   /* OBJ, CELL: identity */
    }
}

void ulist_trace(struct UVM *vm, UList *l) {
    for (uint32_t i = 0; i < l->len; i++) ugc_mark_value(vm, l->items[i]);
    if (l->proto) ugc_mark(vm, &l->proto->cell);
}

void ulist_finalize(struct UVM *vm, UList *l) {
    ugc_raw_free(vm, l->items, (size_t)l->cap * sizeof(UValue));
}

void udict_trace(struct UVM *vm, UDict *d) {
    for (uint32_t i = 0; i < d->len; i++) {
        ugc_mark_value(vm, d->keys[i]);
        ugc_mark_value(vm, d->vals[i]);
    }
    if (d->proto) ugc_mark(vm, &d->proto->cell);
}

void udict_finalize(struct UVM *vm, UDict *d) {
    ugc_raw_free(vm, d->keys, (size_t)d->cap * sizeof(UValue));
    ugc_raw_free(vm, d->vals, (size_t)d->cap * sizeof(UValue));
}
