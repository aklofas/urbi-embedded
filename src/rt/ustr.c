/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/ustr.c — see rt/ustr.h. */

#include "rt/ustr.h"

uint32_t ustr_hash(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

int ustrtab_init(struct UVM *vm, UStrTab *t) {
    t->nbuckets = 64;
    t->count = 0;
    t->buckets = (USym **)ugc_raw_alloc(vm, (size_t)t->nbuckets * sizeof(USym *));
    if (!t->buckets) { t->nbuckets = 0; return -1; }
    return 0;
}

void ustrtab_destroy(struct UVM *vm, UStrTab *t) {
    for (uint32_t i = 0; i < t->nbuckets; i++) {
        USym *s = t->buckets[i];
        while (s) {
            USym *next = s->next;
            ugc_raw_free(vm, s, sizeof(USym) + s->len);
            s = next;
        }
    }
    ugc_raw_free(vm, (void *)t->buckets, (size_t)t->nbuckets * sizeof(USym *));
    t->buckets = NULL; t->nbuckets = 0; t->count = 0;
}

/* Doubles the bucket array and rehashes every chain into it. On OOM the
 * table is left unchanged (still correct, just denser) -- growth is a
 * pacing optimization, not a correctness requirement. */
static void ustrtab_grow(struct UVM *vm, UStrTab *t) {
    uint32_t old_n = t->nbuckets;
    USym **old_buckets = t->buckets;
    uint32_t new_n = old_n * 2;
    USym **new_buckets = (USym **)ugc_raw_alloc(vm, (size_t)new_n * sizeof(USym *));
    if (!new_buckets) return;
    for (uint32_t i = 0; i < old_n; i++) {
        USym *s = old_buckets[i];
        while (s) {
            USym *next = s->next;
            uint32_t idx = s->hash % new_n;
            s->next = new_buckets[idx];
            new_buckets[idx] = s;
            s = next;
        }
    }
    ugc_raw_free(vm, (void *)old_buckets, (size_t)old_n * sizeof(USym *));
    t->buckets = new_buckets;
    t->nbuckets = new_n;
}

USym *usym_intern(struct UVM *vm, const char *s, size_t n) {
    UStrTab *t = uvm_strings(vm);
    uint32_t h = ustr_hash(s, n);
    uint32_t idx = h % t->nbuckets;
    for (USym *e = t->buckets[idx]; e; e = e->next) {
        if (e->hash == h && e->len == n && memcmp(e->bytes, s, n) == 0) return e;
    }
    USym *sym = (USym *)ugc_raw_alloc(vm, sizeof(USym) + n);
    if (!sym) return NULL;
    sym->hash = h; sym->len = (uint32_t)n;
    memcpy(sym->bytes, s, n);
    sym->bytes[n] = '\0';
    sym->next = t->buckets[idx];
    t->buckets[idx] = sym;
    t->count++;
    if (t->count > t->nbuckets) ustrtab_grow(vm, t);
    return sym;
}

UStr *ustr_new(struct UVM *vm, const char *s, size_t n) {
    UStr *r = (UStr *)ugc_alloc(vm, UCELL_STR, sizeof(UStr) + n);
    if (!r) return NULL;
    memcpy(r->bytes, s, n);
    r->bytes[n] = '\0';
    r->len = (uint32_t)n;
    r->hash = ustr_hash(s, n);
    return r;
}

UStr *ustr_concat(struct UVM *vm, const char *a, size_t na, const char *b, size_t nb) {
    size_t n = na + nb;
    UStr *r = (UStr *)ugc_alloc(vm, UCELL_STR, sizeof(UStr) + n);
    if (!r) return NULL;
    memcpy(r->bytes, a, na);
    memcpy(r->bytes + na, b, nb);
    r->bytes[n] = '\0';
    r->len = (uint32_t)n;
    r->hash = ustr_hash(r->bytes, n);
    return r;
}

bool uv_str_equal(UValue a, UValue b) {
    if (a.kind == UV_SYM && b.kind == UV_SYM) return a.v.p == b.v.p;
    uint32_t la, lb;
    const char *pa = uv_str_bytes(a, &la);
    const char *pb = uv_str_bytes(b, &lb);
    return la == lb && memcmp(pa, pb, la) == 0;
}

USym *uv_to_sym(struct UVM *vm, UValue v) {
    if (v.kind == UV_SYM) return (USym *)v.v.p;
    const UStr *s = (const UStr *)v.v.p;
    return usym_intern(vm, s->bytes, s->len);
}
