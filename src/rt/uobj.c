/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uobj.c — see rt/uobj.h. */

#include "rt/uobj.h"

#define UOBJ_INITIAL_CAP  4
#define URESOLVE_STACK_CAP 64

UObject *uobj_new(struct UVM *vm, UObject *proto) {
    UObject *o = (UObject *)ugc_alloc(vm, UCELL_OBJ, sizeof(UObject));
    if (!o) return NULL;
    if (proto) { o->proto0 = proto; o->nprotos = 1; }
    o->id = uvm_objstats(vm)->next_id++;
    return o;
}

int uobj_find_local(const UObject *o, const USym *name) {
    for (uint16_t i = 0; i < o->count; i++) if (o->names[i] == name) return (int)i;
    return -1;
}

/* Bytes the three parallel slot arrays occupy at a given capacity.  They
 * share ONE block: values first (the strictest alignment), then names,
 * then attrs.  Every capacity is a power of two at least
 * UOBJ_INITIAL_CAP, so each array starts on a multiple of its own
 * alignment on both 32- and 64-bit targets. */
static size_t uobj_slots_bytes(uint16_t cap)
{ return (size_t)cap * (sizeof(UValue) + sizeof(USym *) + 1u); }

static void uobj_point_arrays(UObject *o, void *block, uint16_t cap)
{
    UValue *values = (UValue *)block;
    o->values = values;
    o->names  = (USym **)(values + cap);
    o->attrs  = (uint8_t *)(o->names + cap);
}

/* Doubles the slot capacity, starting at UOBJ_INITIAL_CAP.
 *
 * ONE allocation, not three.  Three separate arrays cost an object with
 * any slot at all three allocations and three headers, which at boot is
 * most of the heap: the whole standard library is objects with a handful
 * of slots each.  It also had a partial-failure path, where one array
 * grew and the next did not; a single block cannot fail halfway.
 *
 * Growth cannot use realloc: the three regions move relative to each
 * other as the capacity changes, so the old contents are copied into
 * their new offsets explicitly, highest offset first is unnecessary
 * because the blocks are disjoint. */
static int uobj_grow(struct UVM *vm, UObject *o) {
    uint16_t old_cap = o->cap;
    uint16_t new_cap = old_cap ? (uint16_t)(old_cap * 2) : UOBJ_INITIAL_CAP;
    void *block = ugc_raw_alloc(vm, uobj_slots_bytes(new_cap));
    if (!block) return -1;

    UValue *old_values = o->values;
    if (o->count > 0) {
        USym *const *old_names = o->names;
        const uint8_t *old_attrs = o->attrs;
        uobj_point_arrays(o, block, new_cap);
        memcpy(o->values, old_values, (size_t)o->count * sizeof(UValue));
        memcpy((void *)o->names, (const void *)old_names, (size_t)o->count * sizeof(USym *));
        memcpy(o->attrs, old_attrs, (size_t)o->count);
    } else {
        uobj_point_arrays(o, block, new_cap);
    }
    if (old_values) ugc_raw_free(vm, old_values, uobj_slots_bytes(old_cap));
    o->cap = new_cap;
    return 0;
}

int uobj_set_local(struct UVM *vm, UObject *o, USym *name, UValue v, uint8_t attrs) {
    UValue stored = v;
    if (attrs & (USLOT_GETTER | USLOT_SETTER)) {
        /* Allocate before touching the slot arrays: this may collect, and
         * the caller is responsible for `o` already being rooted. */
        UProps *props = (UProps *)ugc_alloc(vm, UCELL_PROPS, sizeof(UProps));
        if (!props) return -1;
        props->value = v;
        stored = uv_ptr(UV_CELL, props);
    }
    int idx = uobj_find_local(o, name);
    if (idx >= 0) {
        o->values[idx] = stored;
        o->attrs[idx] = attrs;
        return idx;
    }
    if (o->count == o->cap && uobj_grow(vm, o) != 0) return -1;
    o->names[o->count] = name;
    o->values[o->count] = stored;
    o->attrs[o->count] = attrs;
    return (int)o->count++;
}

bool uobj_remove_local(struct UVM *vm, UObject *o, const USym *name) {
    (void)vm;
    int idx = uobj_find_local(o, name);
    if (idx < 0) return false;
    uint16_t last = (uint16_t)(o->count - 1);
    if ((uint16_t)idx != last) {
        o->names[idx] = o->names[last];
        o->values[idx] = o->values[last];
        o->attrs[idx] = o->attrs[last];
    }
    o->count = last;
    return true;
}

/* PREPENDS.  The most recently added prototype takes priority in the
 * depth-first walk, which is what both legacy urbi (Object::proto_add's
 * push_front) and every `Target.addProto(TargetMethods)` line in the
 * stdlib overlay rely on: an overlay method has to shadow the Object
 * root's, not sit behind it. */
int uobj_add_proto(struct UVM *vm, UObject *o, UObject *p) {
    if (o->nprotos == 0) { o->proto0 = p; o->nprotos = 1; return 0; }
    if (o->nprotos == 1) {
        UObject **arr = (UObject **)ugc_raw_alloc(vm, 2 * sizeof(UObject *));
        if (!arr) return -1;
        arr[0] = p; arr[1] = o->proto0;
        o->protos = arr; o->proto0 = arr[0]; o->nprotos = 2;
        return 0;
    }
    UObject **arr = (UObject **)ugc_raw_realloc(vm, (void *)o->protos, (size_t)o->nprotos * sizeof(UObject *), (size_t)(o->nprotos + 1) * sizeof(UObject *));
    if (!arr) return -1;
    for (uint16_t i = o->nprotos; i > 0; i--) arr[i] = arr[i - 1];
    arr[0] = p;
    o->protos = arr; o->proto0 = arr[0]; o->nprotos++;
    return 0;
}

int uobj_remove_proto(struct UVM *vm, UObject *o, const UObject *p) {
    if (o->nprotos == 1 && o->proto0 == p) { o->proto0 = NULL; o->nprotos = 0; return 0; }
    if (o->nprotos <= 1) return -1;   /* not present (protos[] doesn't exist yet) */
    int idx = -1;
    for (uint16_t i = 0; i < o->nprotos; i++) if (o->protos[i] == p) { idx = (int)i; break; }
    if (idx < 0) return -1;
    uint16_t n = (uint16_t)(o->nprotos - 1);
    if (n <= 1) {
        /* Collapses back to the proto0-only representation: protos[] is
         * NULL whenever nprotos <= 1. */
        UObject *remaining = o->protos[idx == 0 ? 1 : 0];
        ugc_raw_free(vm, (void *)o->protos, (size_t)o->nprotos * sizeof(UObject *));
        o->protos = NULL; o->proto0 = remaining; o->nprotos = 1;
        return 0;
    }
    o->protos[idx] = o->protos[n];   /* swap the last slot into the hole */
    UObject **arr = (UObject **)ugc_raw_realloc(vm, (void *)o->protos, (size_t)o->nprotos * sizeof(UObject *), (size_t)n * sizeof(UObject *));
    if (arr) o->protos = arr;        /* shrink failure just leaves it oversized -- harmless */
    o->proto0 = o->protos[0];
    o->nprotos = n;
    return 0;
}

int uobj_set_protos(struct UVM *vm, UObject *o, UObject **ps, uint16_t n) {
    if (n <= 1) {
        if (o->protos) { ugc_raw_free(vm, (void *)o->protos, (size_t)o->nprotos * sizeof(UObject *)); o->protos = NULL; }
        o->proto0 = n ? ps[0] : NULL;
        o->nprotos = n;
        return 0;
    }
    UObject **arr = (UObject **)ugc_raw_alloc(vm, (size_t)n * sizeof(UObject *));
    if (!arr) return -1;
    memcpy((void *)arr, (const void *)ps, (size_t)n * sizeof(UObject *));
    if (o->protos) ugc_raw_free(vm, (void *)o->protos, (size_t)o->nprotos * sizeof(UObject *));
    o->protos = arr; o->proto0 = arr[0]; o->nprotos = n;
    return 0;
}

/* Shared per-call state for the two DFS proto walks below: a fresh visit
 * stamp (0 is reserved as "never visited", so a wrap skips back over it)
 * and the push-in-reverse helper that puts protos[0]/proto0 on top of the
 * stack so it's the first one popped and searched. */
static uint32_t uobj_next_stamp(struct UVM *vm) {
    UObjStats *st = uvm_objstats(vm);
    st->visit_stamp++;
    if (st->visit_stamp == 0) st->visit_stamp++;
    return st->visit_stamp;
}
static bool uobj_push_protos(const UObject *cur, UObject **stack, int *sp) {
    if (cur->nprotos == 1) {
        if (*sp >= URESOLVE_STACK_CAP) return false;
        stack[(*sp)++] = cur->proto0;
    } else {
        for (int i = (int)cur->nprotos - 1; i >= 0; i--) {
            if (*sp >= URESOLVE_STACK_CAP) return false;
            stack[(*sp)++] = cur->protos[i];
        }
    }
    return true;
}

bool uobj_resolve(struct UVM *vm, UObject *o, const USym *name, UObjSlotRef *out) {
    uint32_t stamp = uobj_next_stamp(vm);
    UObject *stack[URESOLVE_STACK_CAP];
    int sp = 0;
    stack[sp++] = o;
    while (sp > 0) {
        UObject *cur = stack[--sp];
        if (cur->visit == stamp) continue;   /* diamond: already searched */
        cur->visit = stamp;
        int idx = uobj_find_local(cur, name);
        if (idx >= 0) { out->owner = cur; out->index = idx; return true; }
        if (cur->nprotos > 0 && !uobj_push_protos(cur, stack, &sp)) return false;   /* proto graph too deep */
    }
    return false;
}

bool uobj_is_a(struct UVM *vm, UObject *o, UObject *proto) {
    uint32_t stamp = uobj_next_stamp(vm);
    UObject *stack[URESOLVE_STACK_CAP];
    int sp = 0;
    stack[sp++] = o;
    while (sp > 0) {
        UObject *cur = stack[--sp];
        if (cur->visit == stamp) continue;
        cur->visit = stamp;
        if (cur == proto) return true;
        if (cur->nprotos > 0 && !uobj_push_protos(cur, stack, &sp)) return false;   /* proto graph too deep */
    }
    return false;
}

void uobj_trace(struct UVM *vm, UObject *o) {
    for (uint16_t i = 0; i < o->count; i++) ugc_mark_value(vm, o->values[i]);
    if (o->nprotos == 1) {
        ugc_mark(vm, &o->proto0->cell);
    } else {
        for (uint16_t i = 0; i < o->nprotos; i++) ugc_mark(vm, &o->protos[i]->cell);
    }
}

void uobj_finalize(struct UVM *vm, UObject *o) {
    /* One block behind all three arrays; `values` is its base. */
    ugc_raw_free(vm, o->values, uobj_slots_bytes(o->cap));
    ugc_raw_free(vm, (void *)o->protos, (size_t)o->nprotos * sizeof(UObject *));
}
