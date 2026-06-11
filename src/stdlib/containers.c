/* SPDX-License-Identifier: BSD-3-Clause */
/* containers.c — M6 Phase 6: C-native container types.
 *
 * Pair / Triplet / Tuple / List / Dict — see banner in containers.h.
 *
 * Storage strategy at v1.0:
 *   - Pair / Triplet are stateless: each instance is a clone of the
 *     proto with `first` / `second` (/ `third`) installed as ordinary
 *     UObject slots.  Lookup goes through OP_GETSLOT → walks proto
 *     chain → finds the slot.
 *   - Tuple / List / Dict carry a heap-allocated backing buffer
 *     (UList / UDict struct).  The pointer is stashed in a hidden
 *     `_storage` slot as UVAL_INT (cast through uintptr_t) so the
 *     GC walker treats it as a leaf scalar.  Each backing buffer
 *     starts with a void *next header threading onto vm->stdlib_-
 *     containers; freed at urbi_vm_destroy via
 *     urbi_stdlib_containers_destroy.
 *
 * VM-lifetime backing buffers are intentional at v1.0 (tracked at
 * docs/urbi-embedded-design-risks.md "stdlib container backing buffers
 * vm-lifetime"); proper UTYPE_LIST / UTYPE_DICT GC types land at v1.x
 * when the cross-cutting walker plumbing arrives.
 *
 * Method registration mirrors atom_protos.c / atoms.c: each method
 * tabulates {name, fn} pairs, install_methods walks the table and
 * binds via urbi_native_closure_create + urbi_object_set_local_slot.
 *
 * Pair / Triplet / Tuple are exposed as fresh UObjects via
 * urbi_realm_set_global (the realm-populate registry has no row for
 * them; see src/realm/urealm_globals.c).  List and Dict reuse the
 * existing URBI_ATOM_LIST / URBI_ATOM_DICT atom-proto singletons —
 * the registry already exposes "List" / "Dict" as realm globals
 * pointing at those protos, so installing methods on the protos is
 * sufficient.
 */

#include "stdlib/containers.h"
#include "stdlib/object_root.h"        /* urbi_native_closure_create + raise helpers */
#ifdef URBI_ENABLE_ROS2
#include "value/ulist_build.h"         /* declaration cross-check for the C-builder wrappers */
#endif

#include "chunk/uchunk.h"            /* UValue / UVAL_* */
#include "gc/ugc_incremental.h"        /* GC_PHASE_*, uvalue_is_heap_white, gc_shade_gray */
#include "object/uobject.h"            /* urbi_object_alloc / atom / clone / set_local_slot */
#include "realm/urealm.h"              /* URealm + global_object */
#include "runtime/uclosure.h"          /* urbi_native_method_fn */
#include "runtime/umacros.h"           /* urbi_strlen, urbi_zero */
#include "sched/ustrand.h"             /* UEXEC_OK / UEXEC_THROW */
#include "urbi/object.h"               /* URBI_ATOM_LIST / DICT / OBJECT */
#include "urbi/types.h"                /* urbi_make_nil */
#include "urbi/urbi.h"                 /* URBI_OK / URBI_ERR_* / urbi_realm_set_global */
#include "value/uintern.h"             /* ustr_intern + USymbol */
#include "value/uvalue.h"              /* uvalue_equal */
#include "vm/uvm.h"                    /* UVM */

#include <stdint.h>
#include <stddef.h>

/* === Backing-buffer header ================================================
 *
 * Every UList / UDict allocation begins with this header so urbi_stdlib_-
 * containers_destroy can walk the chain at VM teardown.  `next` threads onto
 * vm->stdlib_containers (declared as void * in uvm.h to keep that header
 * decoupled from this struct).  `kind` distinguishes UList vs UDict for
 * teardown (UDict carries a separately-allocated entries[] array). */

typedef enum {
    UCONTAINER_LIST = 1,
    UCONTAINER_DICT = 2
} UContainerKind;

typedef struct UContainerHdr {
    struct UContainerHdr *next;
    uint8_t               kind;
    uint8_t               _pad[7];
} UContainerHdr;

/* === UList — backing for List, Tuple ===================================== */

typedef struct UList {
    UContainerHdr hdr;
    UValue       *items;         /* heap array, len * sizeof(UValue) */
    size_t        len;
    size_t        cap;
} UList;

/* === UDict — backing for Dict (open-address linear-probe) ================ */

#define UDICT_EMPTY 0
#define UDICT_USED  1
#define UDICT_TOMB  2

typedef struct UDictEntry {
    UValue   key;     /* UVAL_STR only at v1.0 */
    UValue   val;
    uint32_t hash;
    uint8_t  state;
    uint8_t  _pad[3];
} UDictEntry;

typedef struct UDict {
    UContainerHdr hdr;
    UDictEntry   *entries;       /* heap array, cap entries; cap is power of two */
    size_t        cap;
    size_t        len;           /* USED entries (TOMB excluded) */
} UDict;

/* === Container-list helpers ============================================== */

static void
container_register(UVM *vm, UContainerHdr *hdr, uint8_t kind)
{
    hdr->kind = kind;
    hdr->next = (UContainerHdr *)vm->stdlib_containers;
    vm->stdlib_containers = hdr;
}

void
urbi_stdlib_containers_destroy(UVM *vm)
{
    if (vm == NULL || vm->alloc_fn == NULL) return;
    UContainerHdr *h = (UContainerHdr *)vm->stdlib_containers;
    while (h != NULL) {
        UContainerHdr *next = h->next;
        if (h->kind == (uint8_t)UCONTAINER_LIST) {
            UList *l = (UList *)h;
            if (l->items != NULL) {
                vm->alloc_fn(l->items, 0, vm->alloc_ud);
                l->items = NULL;
            }
        } else if (h->kind == (uint8_t)UCONTAINER_DICT) {
            UDict *d = (UDict *)h;
            if (d->entries != NULL) {
                vm->alloc_fn(d->entries, 0, vm->alloc_ud);
                d->entries = NULL;
            }
        }
        vm->alloc_fn(h, 0, vm->alloc_ud);
        h = next;
    }
    vm->stdlib_containers = NULL;
}

/* refactor-3 B2/GC-01/STD-01: container elements are GC roots.
 *
 * UList/UDict backing stores are raw vm->alloc_fn buffers (not GC cells)
 * threaded onto vm->stdlib_containers by container_register; the script-
 * visible object's `_storage` slot is deliberately UVAL_INT so the object
 * walker treats it as a leaf.  Elements therefore need a dedicated root
 * provider: walk every registered container and yield every element slot.
 * Tuple backing buffers are UCONTAINER_LIST too (tuple_new builds via
 * list_alloc), so the LIST arm covers Tuples.  NULL backing is only
 * possible with len == 0 / cap == 0, so the loop bounds already guard
 * the dereferences.  Cost: lists are O(len); the dict arm is O(cap), not
 * O(len) — bounded <= 4x len by the load factor.  Paid at MARK_ROOTS plus
 * the GC-02 ATOMIC_FINISH re-scan (twice per cycle).  Container backings
 * are vm-lifetime (container_register has no inverse), so root-scan cost
 * grows monotonically with container churn and elements of unreachable
 * containers stay pinned until VM destroy — a soundness-over-precision
 * tradeoff; the full fix is the v1.x UTYPE_LIST promotion (deferred; see
 * design-risks). */
void
urbi_stdlib_containers_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx)
{
    UContainerHdr *h = (UContainerHdr *)vm->stdlib_containers;
    while (h != NULL) {
        if (h->kind == (uint8_t)UCONTAINER_LIST) {
            UList *l = (UList *)h;
            size_t i;
            for (i = 0U; i < l->len; i++) {
                cb(vm, &l->items[i], ctx);
            }
        } else if (h->kind == (uint8_t)UCONTAINER_DICT) {
            UDict *d = (UDict *)h;
            size_t i;
            for (i = 0U; i < d->cap; i++) {
                if (d->entries[i].state == UDICT_USED) {
                    cb(vm, &d->entries[i].key, ctx);
                    cb(vm, &d->entries[i].val, ctx);
                }
            }
        }
        h = h->next;
    }
}

/* refactor-3 B2: incremental-marking insertion barrier for container
 * element stores.  Containers have no parent gc_byte (raw buffers), so the
 * Dijkstra parent-is-BLACK check is unavailable; instead shade the stored
 * child whenever a mark phase is in flight.  Stores while the GC is IDLE
 * or SWEEPing need no barrier (IDLE: next cycle's root scan sees the
 * element; SWEEP: marking is complete and mid-sweep allocations are
 * current_white by construction).  A slice can also end with phase ==
 * ATOMIC_FINISH (gray list drained exactly at budget exhaustion), so the
 * mutator may store in that phase too and this barrier no-ops — that is
 * sound because the GC-02 full root re-scan runs at the START of the next
 * ATOMIC_FINISH slice, after the store, and re-discovers the element.
 *
 * Usage contract: call immediately before the store; no allocation may
 * intervene between barrier and store. */
static void
container_element_pre_store(UVM *vm, UValue child)
{
    if (UNLIKELY((vm->gc_phase == GC_PHASE_MARK_ROOTS
                  || vm->gc_phase == GC_PHASE_MARK_INCREMENTAL)
                 && uvalue_is_heap_white(vm, child))) {
        gc_shade_gray(vm, uvalue_as_cell(child));
    }
}

/* === UList / UDict alloc helpers ========================================= */

static UList *
list_alloc(UVM *vm, size_t initial_cap)
{
    if (vm->alloc_fn == NULL) return NULL;
    UList *l = (UList *)vm->alloc_fn(NULL, sizeof(UList), vm->alloc_ud);
    if (l == NULL) return NULL;
    urbi_zero(l, sizeof(UList));
    if (initial_cap > 0U) {
        l->items = (UValue *)vm->alloc_fn(NULL, initial_cap * sizeof(UValue), vm->alloc_ud);
        if (l->items == NULL) {
            vm->alloc_fn(l, 0, vm->alloc_ud);
            return NULL;
        }
        urbi_zero(l->items, initial_cap * sizeof(UValue));
    }
    l->len = 0U;
    l->cap = initial_cap;
    container_register(vm, &l->hdr, (uint8_t)UCONTAINER_LIST);
    return l;
}

static int
list_grow(UVM *vm, UList *l, size_t need)
{
    size_t new_cap = l->cap > 0U ? l->cap : 4U;
    while (new_cap < need) new_cap *= 2U;
    if (new_cap == l->cap) return 0;
    UValue *fresh = (UValue *)vm->alloc_fn(NULL, new_cap * sizeof(UValue), vm->alloc_ud);
    if (fresh == NULL) return -1;
    urbi_zero(fresh, new_cap * sizeof(UValue));
    size_t i;
    for (i = 0U; i < l->len; i++) fresh[i] = l->items[i];
    if (l->items != NULL) vm->alloc_fn(l->items, 0, vm->alloc_ud);
    l->items = fresh;
    l->cap   = new_cap;
    return 0;
}

static UDict *
dict_alloc(UVM *vm, size_t initial_cap)
{
    if (vm->alloc_fn == NULL) return NULL;
    UDict *d = (UDict *)vm->alloc_fn(NULL, sizeof(UDict), vm->alloc_ud);
    if (d == NULL) return NULL;
    urbi_zero(d, sizeof(UDict));
    if (initial_cap > 0U) {
        d->entries = (UDictEntry *)vm->alloc_fn(NULL, initial_cap * sizeof(UDictEntry), vm->alloc_ud);
        if (d->entries == NULL) {
            vm->alloc_fn(d, 0, vm->alloc_ud);
            return NULL;
        }
        urbi_zero(d->entries, initial_cap * sizeof(UDictEntry));
    }
    d->cap = initial_cap;
    d->len = 0U;
    container_register(vm, &d->hdr, (uint8_t)UCONTAINER_DICT);
    return d;
}

/* === Hidden _storage slot helpers ========================================
 *
 * The UObject visible to scripts holds the underlying UList* / UDict* in a
 * UVAL_INT slot named `_storage`.  The pointer is round-tripped through
 * uintptr_t.  GC sees a UVAL_INT and treats it as a scalar leaf — the
 * backing buffer's lifetime is owned by vm->stdlib_containers, not by GC. */

static UValue
val_from_ptr(void *p)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_INT;
    v.v.i = (int64_t)(intptr_t)p;
    return v;
}

static void *
ptr_from_val(UValue v)
{
    if (v.kind != (uint8_t)UVAL_INT) return NULL;
    /* NOLINT(performance-no-int-to-ptr) — UList/UDict backing pointer
     * stashed as int64 in a hidden _storage slot.  See file banner; the
     * cast is the inverse of val_from_ptr above. */
    return (void *)(intptr_t)v.v.i;  /* NOLINT(performance-no-int-to-ptr) */
}

static int
attach_storage(UVM *vm, UObject *o, void *storage)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, "_storage", 8);
    if (sym == NULL) return -1;
    return urbi_object_set_local_slot(vm, o, sym, val_from_ptr(storage));
}

static void *
fetch_storage_ptr(UVM *vm, UObject *o)
{
    if (o == NULL) return NULL;
    USymbol *sym = (USymbol *)ustr_intern(vm, "_storage", 8);
    if (sym == NULL) return NULL;
    UObject *holder = NULL;
    uint32_t idx = 0U;
    int rc = urbi_object_resolve_slot(vm, o, sym, &holder, &idx);
    if (rc != 1 || holder == NULL || holder->slots == NULL) return NULL;
    return ptr_from_val(holder->slots[idx]);
}

static UList *
list_storage(UVM *vm, UValue self)
{
    if (self.kind != (uint8_t)UVAL_OBJECT || self.v.p == NULL) return NULL;
    return (UList *)fetch_storage_ptr(vm, (UObject *)self.v.p);
}

static UDict *
dict_storage(UVM *vm, UValue self)
{
    if (self.kind != (uint8_t)UVAL_OBJECT || self.v.p == NULL) return NULL;
    return (UDict *)fetch_storage_ptr(vm, (UObject *)self.v.p);
}

/* === UValue construction helpers ========================================= */

static UValue
val_int(int64_t i)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_INT;
    v.v.i  = i;
    return v;
}

static UValue
val_bool(int b)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_BOOL;
    v.v.i  = b ? 1 : 0;
    return v;
}

static UValue
val_obj(UObject *o)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_OBJECT;
    v.v.p  = o;
    return v;
}

/* === Method-table install helper ========================================= */

typedef struct {
    const char           *name;
    urbi_native_method_fn fn;
} ContainerMethodEntry;

static int
install_methods(UVM *vm, UObject *proto,
                const ContainerMethodEntry *table, size_t count)
{
    if (proto == NULL) return URBI_ERR_OOM;
    size_t i;
    for (i = 0U; i < count; i++) {
        UClosure *cl = urbi_native_closure_create(vm, table[i].fn);
        if (cl == NULL) return URBI_ERR_OOM;

        USymbol *sym = (USymbol *)ustr_intern(vm, table[i].name,
                                              urbi_strlen(table[i].name));
        if (sym == NULL) return URBI_ERR_OOM;

        UValue v = urbi_make_nil();
        v.kind = (uint8_t)UVAL_CLOSURE;
        v.v.p  = cl;
        int rc = urbi_object_set_local_slot(vm, proto, sym, v);
        if (rc != 0) return URBI_ERR_OOM;
    }
    return URBI_OK;
}

/* === Pair (immutable 2-tuple) ============================================
 *
 * Pair.new(a, b) clones the Pair proto (urbi_object_clone) and installs
 * `first` + `second` as ordinary slots.  No backing storage. */

static int
pair_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 2) return urbi_raise_arity(vm, "Pair.new", 2, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "Pair.new: self must be Pair proto", out);

    UObject *p = urbi_object_clone(vm, (UObject *)self.v.p);
    if (p == NULL) return urbi_raise_oom(vm, out);

    USymbol *sym_first  = (USymbol *)ustr_intern(vm, "first",  5);
    USymbol *sym_second = (USymbol *)ustr_intern(vm, "second", 6);
    if (sym_first == NULL || sym_second == NULL) return urbi_raise_oom(vm, out);
    if (urbi_object_set_local_slot(vm, p, sym_first,  args[0]) != 0)
        return urbi_raise_oom(vm, out);
    if (urbi_object_set_local_slot(vm, p, sym_second, args[1]) != 0)
        return urbi_raise_oom(vm, out);

    *out = val_obj(p);
    return UEXEC_OK;
}

/* === Triplet (immutable 3-tuple) ========================================= */

static int
triplet_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 3) return urbi_raise_arity(vm, "Triplet.new", 3, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "Triplet.new: self must be Triplet proto", out);

    UObject *t = urbi_object_clone(vm, (UObject *)self.v.p);
    if (t == NULL) return urbi_raise_oom(vm, out);

    USymbol *sf = (USymbol *)ustr_intern(vm, "first",  5);
    USymbol *ss = (USymbol *)ustr_intern(vm, "second", 6);
    USymbol *st = (USymbol *)ustr_intern(vm, "third",  5);
    if (sf == NULL || ss == NULL || st == NULL) return urbi_raise_oom(vm, out);
    if (urbi_object_set_local_slot(vm, t, sf, args[0]) != 0) return urbi_raise_oom(vm, out);
    if (urbi_object_set_local_slot(vm, t, ss, args[1]) != 0) return urbi_raise_oom(vm, out);
    if (urbi_object_set_local_slot(vm, t, st, args[2]) != 0) return urbi_raise_oom(vm, out);

    *out = val_obj(t);
    return UEXEC_OK;
}

/* === Tuple (variadic immutable n-tuple) ==================================
 *
 * Backed by a UList that's populated at construction and never grown.
 * Methods: length, at(i). */

static int
tuple_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "Tuple.new: self must be Tuple proto", out);

    UList *l = list_alloc(vm, (size_t)nargs > 0U ? (size_t)nargs : 1U);
    if (l == NULL) return urbi_raise_oom(vm, out);

    uint8_t i;
    for (i = 0U; i < nargs; i++) {
        container_element_pre_store(vm, args[i]);
        l->items[i] = args[i];
    }
    l->len = (size_t)nargs;

    UObject *t = urbi_object_clone(vm, (UObject *)self.v.p);
    if (t == NULL) return urbi_raise_oom(vm, out);
    if (attach_storage(vm, t, l) != 0) return urbi_raise_oom(vm, out);

    *out = val_obj(t);
    return UEXEC_OK;
}

static int
list_or_tuple_length(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "length", 0, nargs, out);
    UList *l = list_storage(vm, self);
    if (l == NULL) return urbi_raise_type(vm, "length: missing _storage", out);
    *out = val_int((int64_t)l->len);
    return UEXEC_OK;
}

static int
list_or_tuple_get(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "get", 1, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "get: index must be Integer", out);
    UList *l = list_storage(vm, self);
    if (l == NULL) return urbi_raise_type(vm, "get: missing _storage", out);
    int64_t i = args[0].v.i;
    if (i < 0 || (size_t)i >= l->len)
        return urbi_raise_type(vm, "get: index out of range", out);
    *out = l->items[(size_t)i];
    return UEXEC_OK;
}

/* === List ================================================================
 *
 * Mutable, growable.  `List.new(...)` constructs from variadic args.
 * Methods: new, length, isEmpty, at(i), add(v), set(i, v), concat(other),
 * diff(other), contains(v). */

static int
list_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "List.new: self must be List proto", out);

    size_t cap = (size_t)nargs > 0U ? (size_t)nargs : 4U;
    UList *l = list_alloc(vm, cap);
    if (l == NULL) return urbi_raise_oom(vm, out);

    uint8_t i;
    for (i = 0U; i < nargs; i++) {
        container_element_pre_store(vm, args[i]);
        l->items[i] = args[i];
    }
    l->len = (size_t)nargs;

    UObject *o = urbi_object_clone(vm, (UObject *)self.v.p);
    if (o == NULL) return urbi_raise_oom(vm, out);
    if (attach_storage(vm, o, l) != 0) return urbi_raise_oom(vm, out);

    *out = val_obj(o);
    return UEXEC_OK;
}

static int
list_isEmpty(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "isEmpty", 0, nargs, out);
    UList *l = list_storage(vm, self);
    if (l == NULL) return urbi_raise_type(vm, "isEmpty: missing _storage", out);
    *out = val_bool(l->len == 0U);
    return UEXEC_OK;
}

static int
list_add(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "add", 1, nargs, out);
    UList *l = list_storage(vm, self);
    if (l == NULL) return urbi_raise_type(vm, "add: missing _storage", out);
    if (l->len == l->cap) {
        if (list_grow(vm, l, l->len + 1U) != 0)
            return urbi_raise_oom(vm, out);
    }
    container_element_pre_store(vm, args[0]);
    l->items[l->len++] = args[0];
    *out = self;   /* allow chaining */
    return UEXEC_OK;
}

static int
list_set(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 2) return urbi_raise_arity(vm, "set", 2, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "set: index must be Integer", out);
    UList *l = list_storage(vm, self);
    if (l == NULL) return urbi_raise_type(vm, "set: missing _storage", out);
    int64_t i = args[0].v.i;
    if (i < 0 || (size_t)i >= l->len)
        return urbi_raise_type(vm, "set: index out of range", out);
    container_element_pre_store(vm, args[1]);
    l->items[(size_t)i] = args[1];
    *out = self;
    return UEXEC_OK;
}

static int
list_contains(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "contains", 1, nargs, out);
    UList *l = list_storage(vm, self);
    if (l == NULL) return urbi_raise_type(vm, "contains: missing _storage", out);
    size_t i;
    for (i = 0U; i < l->len; i++) {
        if (uvalue_equal(&l->items[i], &args[0])) {
            *out = val_bool(1);
            return UEXEC_OK;
        }
    }
    *out = val_bool(0);
    return UEXEC_OK;
}

static int
list_concat(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "concat", 1, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "concat: argument must be a List", out);
    UList *a = list_storage(vm, self);
    UList *b = list_storage(vm, args[0]);
    if (a == NULL || b == NULL)
        return urbi_raise_type(vm, "concat: missing _storage", out);

    /* Allocate a fresh List proto-clone backed by a new UList. */
    UList *o = list_alloc(vm, a->len + b->len > 0U ? a->len + b->len : 1U);
    if (o == NULL) return urbi_raise_oom(vm, out);
    /* No element barrier on these copy loops (same for diff / reverse /
     * sort below): every value copied is already reachable from a
     * registered source container, which the root provider re-yields. */
    size_t i;
    for (i = 0U; i < a->len; i++) o->items[o->len++] = a->items[i];
    for (i = 0U; i < b->len; i++) o->items[o->len++] = b->items[i];

    UObject *ret = urbi_object_clone(vm, (UObject *)self.v.p);
    if (ret == NULL) return urbi_raise_oom(vm, out);
    if (attach_storage(vm, ret, o) != 0) return urbi_raise_oom(vm, out);

    *out = val_obj(ret);
    return UEXEC_OK;
}

static int
list_diff(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "diff", 1, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "diff: argument must be a List", out);
    UList *a = list_storage(vm, self);
    UList *b = list_storage(vm, args[0]);
    if (a == NULL || b == NULL)
        return urbi_raise_type(vm, "diff: missing _storage", out);

    /* Allocate a fresh List backed by a UList with capacity a->len. */
    UList *o = list_alloc(vm, a->len > 0U ? a->len : 1U);
    if (o == NULL) return urbi_raise_oom(vm, out);

    size_t i, j;
    for (i = 0U; i < a->len; i++) {
        int present = 0;
        for (j = 0U; j < b->len; j++) {
            if (uvalue_equal(&a->items[i], &b->items[j])) { present = 1; break; }
        }
        if (!present) o->items[o->len++] = a->items[i];
    }

    UObject *ret = urbi_object_clone(vm, (UObject *)self.v.p);
    if (ret == NULL) return urbi_raise_oom(vm, out);
    if (attach_storage(vm, ret, o) != 0) return urbi_raise_oom(vm, out);

    *out = val_obj(ret);
    return UEXEC_OK;
}

/* reverse(): return a fresh List with the elements in reverse order. */
static int
list_reverse(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "reverse", 0, nargs, out);
    UList *a = list_storage(vm, self);
    if (a == NULL) return urbi_raise_type(vm, "reverse: missing _storage", out);
    UList *o = list_alloc(vm, a->len > 0U ? a->len : 1U);
    if (o == NULL) return urbi_raise_oom(vm, out);
    size_t i;
    for (i = 0U; i < a->len; i++) o->items[i] = a->items[a->len - 1U - i];
    o->len = a->len;
    UObject *ret = urbi_object_clone(vm, (UObject *)self.v.p);
    if (ret == NULL) return urbi_raise_oom(vm, out);
    if (attach_storage(vm, ret, o) != 0) return urbi_raise_oom(vm, out);
    *out = val_obj(ret);
    return UEXEC_OK;
}

/* Lexicographic byte compare of two NUL-terminated interned strings.
 * Returns <0, 0, >0 like strcmp (no <string.h> dependency). */
static int
str_lex_cmp(const char *x, const char *y)
{
    size_t i = 0U;
    while (x[i] != '\0' && x[i] == y[i]) i++;
    return (int)(unsigned char)x[i] - (int)(unsigned char)y[i];
}

/* Three-way compare of two UValues for sort.  Supports Integer, Float
 * (mixed numeric), and String.  Sets *ok = 0 when incomparable. */
static int
uval_cmp(const UValue *x, const UValue *y, int *ok)
{
    *ok = 1;
    if (x->kind == (uint8_t)UVAL_INT && y->kind == (uint8_t)UVAL_INT)
        return (x->v.i < y->v.i) ? -1 : (x->v.i > y->v.i) ? 1 : 0;
    if ((x->kind == (uint8_t)UVAL_INT || x->kind == (uint8_t)UVAL_FLOAT) &&
        (y->kind == (uint8_t)UVAL_INT || y->kind == (uint8_t)UVAL_FLOAT)) {
        double xd = (x->kind == (uint8_t)UVAL_FLOAT) ? x->v.f : (double)x->v.i;
        double yd = (y->kind == (uint8_t)UVAL_FLOAT) ? y->v.f : (double)y->v.i;
        return (xd < yd) ? -1 : (xd > yd) ? 1 : 0;
    }
    if (x->kind == (uint8_t)UVAL_STR && y->kind == (uint8_t)UVAL_STR)
        return str_lex_cmp((const char *)x->v.p, (const char *)y->v.p);
    *ok = 0;
    return 0;
}

/* sort(): return a fresh List sorted ascending (insertion sort).  All
 * elements must be mutually comparable (numeric or all String). */
static int
list_sort(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "sort", 0, nargs, out);
    UList *a = list_storage(vm, self);
    if (a == NULL) return urbi_raise_type(vm, "sort: missing _storage", out);
    UList *o = list_alloc(vm, a->len > 0U ? a->len : 1U);
    if (o == NULL) return urbi_raise_oom(vm, out);
    size_t i;
    for (i = 0U; i < a->len; i++) o->items[i] = a->items[i];
    o->len = a->len;
    for (i = 1U; i < o->len; i++) {
        UValue key = o->items[i];
        size_t j = i;
        while (j > 0U) {
            int ok;
            int c = uval_cmp(&o->items[j - 1U], &key, &ok);
            if (!ok) return urbi_raise_type(vm, "sort: elements not comparable", out);
            if (c <= 0) break;
            o->items[j] = o->items[j - 1U];
            j--;
        }
        o->items[j] = key;
    }
    UObject *ret = urbi_object_clone(vm, (UObject *)self.v.p);
    if (ret == NULL) return urbi_raise_oom(vm, out);
    if (attach_storage(vm, ret, o) != 0) return urbi_raise_oom(vm, out);
    *out = val_obj(ret);
    return UEXEC_OK;
}

/* join(sep): concatenate String elements separated by the String sep.
 * Raises TypeError if any element is not a String. */
static int
list_join(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "join", 1, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "join: separator must be String", out);
    UList *a = list_storage(vm, self);
    if (a == NULL) return urbi_raise_type(vm, "join: missing _storage", out);
    const char *sep = (const char *)args[0].v.p;
    size_t seplen = urbi_strlen(sep);
    size_t i, total = 0U;
    for (i = 0U; i < a->len; i++) {
        if (a->items[i].kind != (uint8_t)UVAL_STR)
            return urbi_raise_type(vm, "join: all elements must be String", out);
        total += urbi_strlen((const char *)a->items[i].v.p);
        if (i + 1U < a->len) total += seplen;
    }
    char *buf = (char *)vm->alloc_fn(NULL, total > 0U ? total : 1U, vm->alloc_ud);
    if (buf == NULL) return urbi_raise_oom(vm, out);
    size_t off = 0U;
    for (i = 0U; i < a->len; i++) {
        const char *s = (const char *)a->items[i].v.p;
        size_t k, sl = urbi_strlen(s);
        for (k = 0U; k < sl; k++) buf[off + k] = s[k];
        off += sl;
        if (i + 1U < a->len) {
            for (k = 0U; k < seplen; k++) buf[off + k] = sep[k];
            off += seplen;
        }
    }
    const char *interned = ustr_intern(vm, buf, total);
    vm->alloc_fn(buf, 0, vm->alloc_ud);
    if (interned == NULL) return urbi_raise_oom(vm, out);
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_STR;
    v.v.p = (void *)interned;
    *out = v;
    return UEXEC_OK;
}

/* === Dict ================================================================
 *
 * Mutable, string-keyed open-address hash table.  Methods: new, length,
 * isEmpty, get(key), set(key, value), has(key), remove(key).
 *
 * Iteration order is unspecified at v1.0 (Dict iteration order joins
 * Lua/Ruby<1.9 in declining the insertion-order guarantee).
 *
 * Hash: FNV-1a over string bytes.  Capacity grows by doubling when load
 * factor crosses 0.5. */

static uint32_t
dict_hash_bytes(const char *s, size_t n)
{
    uint32_t h = 0x811C9DC5U;
    size_t i;
    for (i = 0U; i < n; i++) {
        h ^= (uint32_t)(unsigned char)s[i];
        h *= 0x01000193U;
    }
    return h;
}

static int
dict_key_check(UVM *vm, UValue key, UValue *out, const char *fn_name)
{
    (void)fn_name;
    if (key.kind != (uint8_t)UVAL_STR || key.v.p == NULL) {
        return urbi_raise_type(vm, "Dict op: key must be String", out);
    }
    return UEXEC_OK;
}

/* dict_lookup walks the open-address probe sequence and returns either the
 * matching USED entry or the first EMPTY/TOMB slot suitable for insert.
 * Returns NULL only when the table is full of USED entries (caller must
 * grow first).  d->cap must be a power of two. */
static UDictEntry *
dict_lookup(UDict *d, const char *ks, size_t kn, uint32_t h)
{
    if (d->cap == 0U) return NULL;
    UDictEntry *first_avail = NULL;
    size_t mask = d->cap - 1U;
    size_t start = (size_t)h & mask;
    size_t probe;
    for (probe = 0U; probe < d->cap; probe++) {
        UDictEntry *e = &d->entries[(start + probe) & mask];
        if (e->state == UDICT_EMPTY) {
            return first_avail != NULL ? first_avail : e;
        }
        if (e->state == UDICT_TOMB) {
            if (first_avail == NULL) first_avail = e;
            continue;
        }
        /* USED — compare. */
        if (e->hash == h && e->key.kind == (uint8_t)UVAL_STR
            && e->key.v.p != NULL) {
            const char *sk = (const char *)e->key.v.p;
            size_t sklen = urbi_strlen(sk);
            if (sklen == kn) {
                size_t i;
                int eq = 1;
                for (i = 0U; i < kn; i++) {
                    if (sk[i] != ks[i]) { eq = 0; break; }
                }
                if (eq) return e;
            }
        }
    }
    return first_avail;   /* table full of USED + TOMB */
}

static int
dict_grow(UVM *vm, UDict *d)
{
    size_t new_cap = d->cap > 0U ? d->cap * 2U : 8U;
    UDictEntry *fresh = (UDictEntry *)vm->alloc_fn(NULL,
        new_cap * sizeof(UDictEntry), vm->alloc_ud);
    if (fresh == NULL) return -1;
    urbi_zero(fresh, new_cap * sizeof(UDictEntry));
    UDictEntry *old = d->entries;
    size_t old_cap = d->cap;
    d->entries = fresh;
    d->cap     = new_cap;
    d->len     = 0U;
    if (old != NULL) {
        size_t i;
        for (i = 0U; i < old_cap; i++) {
            if (old[i].state != UDICT_USED) continue;
            const char *sk = (const char *)old[i].key.v.p;
            size_t sklen = urbi_strlen(sk);
            UDictEntry *slot = dict_lookup(d, sk, sklen, old[i].hash);
            if (slot == NULL) {
                /* should never happen — fresh table has empty slots */
                vm->alloc_fn(old, 0, vm->alloc_ud);
                return -1;
            }
            *slot = old[i];
            d->len++;
        }
        vm->alloc_fn(old, 0, vm->alloc_ud);
    }
    return 0;
}

static int
dict_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Dict.new", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "Dict.new: self must be Dict proto", out);

    UDict *d = dict_alloc(vm, 8U);
    if (d == NULL) return urbi_raise_oom(vm, out);

    UObject *o = urbi_object_clone(vm, (UObject *)self.v.p);
    if (o == NULL) return urbi_raise_oom(vm, out);
    if (attach_storage(vm, o, d) != 0) return urbi_raise_oom(vm, out);

    *out = val_obj(o);
    return UEXEC_OK;
}

static int
dict_length(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "length", 0, nargs, out);
    UDict *d = dict_storage(vm, self);
    if (d == NULL) return urbi_raise_type(vm, "length: missing _storage", out);
    *out = val_int((int64_t)d->len);
    return UEXEC_OK;
}

static int
dict_isEmpty(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "isEmpty", 0, nargs, out);
    UDict *d = dict_storage(vm, self);
    if (d == NULL) return urbi_raise_type(vm, "isEmpty: missing _storage", out);
    *out = val_bool(d->len == 0U);
    return UEXEC_OK;
}

static int
dict_set(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 2) return urbi_raise_arity(vm, "set", 2, nargs, out);
    int rc = dict_key_check(vm, args[0], out, "set");
    if (rc != UEXEC_OK) return rc;

    UDict *d = dict_storage(vm, self);
    if (d == NULL) return urbi_raise_type(vm, "set: missing _storage", out);

    /* Grow if load factor would exceed 0.5 after potential insert. */
    if ((d->len + 1U) * 2U > d->cap) {
        if (dict_grow(vm, d) != 0) return urbi_raise_oom(vm, out);
    }

    const char *ks = (const char *)args[0].v.p;
    size_t kn = urbi_strlen(ks);
    uint32_t h = dict_hash_bytes(ks, kn);
    UDictEntry *e = dict_lookup(d, ks, kn, h);
    if (e == NULL) return urbi_raise_oom(vm, out);

    if (e->state != UDICT_USED) {
        container_element_pre_store(vm, args[0]);
        e->key   = args[0];
        e->hash  = h;
        e->state = UDICT_USED;
        d->len++;
    }
    container_element_pre_store(vm, args[1]);
    e->val = args[1];
    *out = self;
    return UEXEC_OK;
}

static int
dict_get(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "get", 1, nargs, out);
    int rc = dict_key_check(vm, args[0], out, "get");
    if (rc != UEXEC_OK) return rc;

    UDict *d = dict_storage(vm, self);
    if (d == NULL) return urbi_raise_type(vm, "get: missing _storage", out);
    if (d->cap == 0U) { *out = urbi_make_nil(); return UEXEC_OK; }

    const char *ks = (const char *)args[0].v.p;
    size_t kn = urbi_strlen(ks);
    UDictEntry *e = dict_lookup(d, ks, kn, dict_hash_bytes(ks, kn));
    if (e == NULL || e->state != UDICT_USED) {
        *out = urbi_make_nil();
        return UEXEC_OK;
    }
    *out = e->val;
    return UEXEC_OK;
}

static int
dict_has(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "has", 1, nargs, out);
    int rc = dict_key_check(vm, args[0], out, "has");
    if (rc != UEXEC_OK) return rc;

    UDict *d = dict_storage(vm, self);
    if (d == NULL) return urbi_raise_type(vm, "has: missing _storage", out);
    if (d->cap == 0U) { *out = val_bool(0); return UEXEC_OK; }

    const char *ks = (const char *)args[0].v.p;
    size_t kn = urbi_strlen(ks);
    UDictEntry *e = dict_lookup(d, ks, kn, dict_hash_bytes(ks, kn));
    *out = val_bool(e != NULL && e->state == UDICT_USED);
    return UEXEC_OK;
}

static int
dict_remove(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "remove", 1, nargs, out);
    int rc = dict_key_check(vm, args[0], out, "remove");
    if (rc != UEXEC_OK) return rc;

    UDict *d = dict_storage(vm, self);
    if (d == NULL) return urbi_raise_type(vm, "remove: missing _storage", out);
    if (d->cap == 0U) { *out = self; return UEXEC_OK; }

    const char *ks = (const char *)args[0].v.p;
    size_t kn = urbi_strlen(ks);
    UDictEntry *e = dict_lookup(d, ks, kn, dict_hash_bytes(ks, kn));
    if (e != NULL && e->state == UDICT_USED) {
        /* No element barrier: deletion stores nil — an insertion barrier
         * only guards new black→white edges. */
        e->state = UDICT_TOMB;
        e->key   = urbi_make_nil();
        e->val   = urbi_make_nil();
        d->len--;
    }
    *out = self;
    return UEXEC_OK;
}

/* keys(): return a fresh List of the dict's keys.  Order is unspecified
 * (matches the v1.0 Dict iteration-order contract). */
static int
dict_keys(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "keys", 0, nargs, out);
    UDict *d = dict_storage(vm, self);
    if (d == NULL) return urbi_raise_type(vm, "keys: missing _storage", out);
    UObject *lst = urbi_stdlib_list_new_empty(vm);
    if (lst == NULL) return urbi_raise_oom(vm, out);
    size_t i;
    for (i = 0U; i < d->cap; i++) {
        if (d->entries[i].state != UDICT_USED) continue;
        if (urbi_stdlib_list_append_value(vm, lst, d->entries[i].key) != 0)
            return urbi_raise_oom(vm, out);
    }
    *out = val_obj(lst);
    return UEXEC_OK;
}

/* values(): return a fresh List of the dict's values.  Order is
 * unspecified and parallels keys() for a given dict instance. */
static int
dict_values(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "values", 0, nargs, out);
    UDict *d = dict_storage(vm, self);
    if (d == NULL) return urbi_raise_type(vm, "values: missing _storage", out);
    UObject *lst = urbi_stdlib_list_new_empty(vm);
    if (lst == NULL) return urbi_raise_oom(vm, out);
    size_t i;
    for (i = 0U; i < d->cap; i++) {
        if (d->entries[i].state != UDICT_USED) continue;
        if (urbi_stdlib_list_append_value(vm, lst, d->entries[i].val) != 0)
            return urbi_raise_oom(vm, out);
    }
    *out = val_obj(lst);
    return UEXEC_OK;
}

/* === Method tables ======================================================= */

static const ContainerMethodEntry PAIR_METHODS[] = {
    { "new", pair_new }
};

static const ContainerMethodEntry TRIPLET_METHODS[] = {
    { "new", triplet_new }
};

static const ContainerMethodEntry TUPLE_METHODS[] = {
    { "new",    tuple_new            },
    { "length", list_or_tuple_length },
    { "get",    list_or_tuple_get     }
};

static const ContainerMethodEntry LIST_METHODS[] = {
    { "new",      list_new             },
    { "length",   list_or_tuple_length },
    { "isEmpty",  list_isEmpty         },
    { "get",      list_or_tuple_get     },
    { "add",      list_add             },
    { "set",      list_set             },
    { "contains", list_contains        },
    { "concat",   list_concat          },
    { "diff",     list_diff            },
    { "sort",     list_sort            },
    { "reverse",  list_reverse         },
    { "join",     list_join            }
};

static const ContainerMethodEntry DICT_METHODS[] = {
    { "new",     dict_new     },
    { "length",  dict_length  },
    { "isEmpty", dict_isEmpty },
    { "set",     dict_set     },
    { "get",     dict_get     },
    { "has",     dict_has     },
    { "remove",  dict_remove  },
    { "keys",    dict_keys    },
    { "values",  dict_values  }
};

#define PAIR_METHODS_COUNT    (sizeof(PAIR_METHODS)    / sizeof(PAIR_METHODS[0]))
#define TRIPLET_METHODS_COUNT (sizeof(TRIPLET_METHODS) / sizeof(TRIPLET_METHODS[0]))
#define TUPLE_METHODS_COUNT   (sizeof(TUPLE_METHODS)   / sizeof(TUPLE_METHODS[0]))
#define LIST_METHODS_COUNT    (sizeof(LIST_METHODS)    / sizeof(LIST_METHODS[0]))
#define DICT_METHODS_COUNT    (sizeof(DICT_METHODS)    / sizeof(DICT_METHODS[0]))

/* === urbi_stdlib_register_containers ====================================
 *
 * Boot phase (called from urbi_stdlib_boot AFTER atom_protos_register and
 * atoms.c register_atom_methods, BEFORE the realm-populate registry loop):
 *
 *   1. Install List / Dict methods on the existing URBI_ATOM_LIST /
 *      URBI_ATOM_DICT atom-proto singletons.  The realm-populate registry
 *      already publishes these as "List" / "Dict" globals.
 *   2. Allocate fresh Pair / Triplet / Tuple proto UObjects, install
 *      their methods, and stash the pointers in vm fields.
 *      Realm-global binding for these names is deferred to
 *      urbi_stdlib_register_container_globals (called after the registry
 *      loop) so the registry's stable slot 0..7 layout for the v1.0
 *      packed-flag CONSTANT enforcement range stays intact.
 *
 * Idempotent — guarded by vm->stdlib_booted upstream. */

int
urbi_stdlib_register_containers(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;

    int rc;

    /* 1. List / Dict atom protos (existing singletons; realm-populate
     *    registry already binds the names). */
    UObject *list_proto = urbi_object_atom(vm, URBI_ATOM_LIST);
    if (list_proto == NULL) return URBI_ERR_OOM;
    rc = install_methods(vm, list_proto, LIST_METHODS, LIST_METHODS_COUNT);
    if (rc != URBI_OK) return rc;

    UObject *dict_proto = urbi_object_atom(vm, URBI_ATOM_DICT);
    if (dict_proto == NULL) return URBI_ERR_OOM;
    rc = install_methods(vm, dict_proto, DICT_METHODS, DICT_METHODS_COUNT);
    if (rc != URBI_OK) return rc;

    /* 2. Pair / Triplet / Tuple fresh protos.  Each is a vanilla
     *    URBI_ATOM_OBJECT-family UObject with the proper methods installed.
     *    GC reachability comes from object_roots_walker (uobject.c) which
     *    shades vm->container_*_proto during MARK_ROOTS. */
    if (vm->container_pair_proto == NULL) {
        UObject *p = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
        if (p == NULL) return URBI_ERR_OOM;
        vm->container_pair_proto = p;
    }
    rc = install_methods(vm, vm->container_pair_proto,
                         PAIR_METHODS, PAIR_METHODS_COUNT);
    if (rc != URBI_OK) return rc;

    if (vm->container_triplet_proto == NULL) {
        UObject *t = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
        if (t == NULL) return URBI_ERR_OOM;
        vm->container_triplet_proto = t;
    }
    rc = install_methods(vm, vm->container_triplet_proto,
                         TRIPLET_METHODS, TRIPLET_METHODS_COUNT);
    if (rc != URBI_OK) return rc;

    if (vm->container_tuple_proto == NULL) {
        UObject *t = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
        if (t == NULL) return URBI_ERR_OOM;
        vm->container_tuple_proto = t;
    }
    rc = install_methods(vm, vm->container_tuple_proto,
                         TUPLE_METHODS, TUPLE_METHODS_COUNT);
    if (rc != URBI_OK) return rc;

    return URBI_OK;
}

/* === urbi_stdlib_register_container_globals =============================
 *
 * Post-registry hook: installs Pair / Triplet / Tuple as realm globals on
 * `realm`.  Called by urbi_populate_realm_globals AFTER the 15-row
 * registry loop completes, so these names occupy slots 15+ and don't
 * displace the registry's slot 0..7 CONSTANT-enforcement layout.
 *
 * The protos themselves are allocated by urbi_stdlib_register_containers
 * (which runs at BOOT TIME, before this function); this hook just binds
 * names to the existing protos. */

int
urbi_stdlib_register_container_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;

    int rc;
    if (vm->container_pair_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "Pair", 4,
                                   val_obj(vm->container_pair_proto));
        if (rc != URBI_OK) return rc;
    }
    if (vm->container_triplet_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "Triplet", 7,
                                   val_obj(vm->container_triplet_proto));
        if (rc != URBI_OK) return rc;
    }
    if (vm->container_tuple_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "Tuple", 5,
                                   val_obj(vm->container_tuple_proto));
        if (rc != URBI_OK) return rc;
    }
    return URBI_OK;
}

/* === Host-side List mutators (v0.9.1 Phase 5 / lobby.lobbies) ===========
 *
 * The Lobby proto's `lobbies` slot is created by lobby.u as a fresh List
 * (`var Lobby.lobbies = []`).  The C-side dispatcher (urepl_session_*)
 * needs to push/remove session global-objects from that List as sessions
 * come and go.  Reaching directly into the script-side List from urbi_-
 * repl_eval is too heavyweight for a lifecycle event; instead we expose
 * two thin C helpers that operate on the UList backing buffer the same
 * way list_add / list_contains do.
 *
 * Both helpers accept a List UObject* and a UValue item; they no-op
 * (returning URBI_OK) when list_obj is NULL or carries no _storage slot
 * — the early-call scenario where urbi_lobby_register_session fires
 * before the .u overlay has populated Lobby.lobbies.  Out-of-memory
 * surfaces as URBI_ERR_OOM.
 *
 * Scope: only the Lobby dispatcher should call these; user-facing List
 * mutation goes through list_add / list_set / list_contains. */

int
urbi_stdlib_list_append_value(UVM *vm, UObject *list_obj, UValue item)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    if (list_obj == NULL) return URBI_OK;
    UList *l = (UList *)fetch_storage_ptr(vm, list_obj);
    if (l == NULL) return URBI_OK;  /* Lobby.lobbies not yet initialized */
    if (l->len == l->cap) {
        if (list_grow(vm, l, l->len + 1U) != 0) return URBI_ERR_OOM;
    }
    container_element_pre_store(vm, item);
    l->items[l->len++] = item;
    return URBI_OK;
}

/* Ungated list-read accessors mirroring the ROS2-gated urbi_list_len/get,
 * but taking a UObject* (like urbi_stdlib_list_append_value) so stdlib code
 * outside the ROS2 component (e.g. String.join/format) can read List backing
 * without the URBI_ENABLE_ROS2 gate.  Internal; not public ABI. */
size_t
urbi_stdlib_list_len(UVM *vm, UObject *list_obj)
{
    if (vm == NULL || list_obj == NULL) return 0U;
    UList *l = (UList *)fetch_storage_ptr(vm, list_obj);
    if (l == NULL) return 0U;
    return l->len;
}

UValue
urbi_stdlib_list_get(UVM *vm, UObject *list_obj, size_t i)
{
    if (vm == NULL || list_obj == NULL) return urbi_make_nil();
    UList *l = (UList *)fetch_storage_ptr(vm, list_obj);
    if (l == NULL || i >= l->len) return urbi_make_nil();
    return l->items[i];
}

int
urbi_stdlib_list_remove_first_equal(UVM *vm, UObject *list_obj, UValue item)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    if (list_obj == NULL) return URBI_OK;
    UList *l = (UList *)fetch_storage_ptr(vm, list_obj);
    if (l == NULL) return URBI_OK;
    size_t i;
    for (i = 0U; i < l->len; i++) {
        if (uvalue_equal(&l->items[i], &item)) {
            /* Shift tail left by one; len decrements. */
            size_t j;
            for (j = i; j + 1U < l->len; j++) {
                l->items[j] = l->items[j + 1U];
            }
            l->len--;
            return URBI_OK;
        }
    }
    return URBI_OK;  /* not found — silent no-op (mirrors Lobby spec) */
}

UObject *
urbi_stdlib_list_new_empty(UVM *vm)
{
    if (vm == NULL) return NULL;
    UObject *list_proto = urbi_object_atom(vm, URBI_ATOM_LIST);
    if (list_proto == NULL) return NULL;
    UList *l = list_alloc(vm, 4U);
    if (l == NULL) return NULL;
    UObject *o = urbi_object_clone(vm, list_proto);
    if (o == NULL) return NULL;
    if (attach_storage(vm, o, l) != 0) return NULL;
    return o;
}

#ifdef URBI_ENABLE_ROS2
/* === Internal List C-builder (src/value/ulist_build.h) ===================
 *
 * Thin wrappers over the file-static helpers above, exposed so the ROS2
 * bridge can build List objects from incoming sequence fields without
 * duplicating storage logic.  Bodies live here (not in a separate .c)
 * because list_storage / list_grow / urbi_object_atom / urbi_object_clone
 * / attach_storage are all file-static.  Declarations: src/value/ulist_build.h.
 *
 * All three are INTERNAL; they are NOT part of the public ABI surface. */

/* Create a new empty List object backed by a fresh UList.
 * Returns a UVAL_OBJECT UValue, or urbi_make_nil() on OOM. */
UValue
urbi_list_create(UVM *vm)
{
    if (vm == NULL) return urbi_make_nil();
    UObject *o = urbi_stdlib_list_new_empty(vm);
    if (o == NULL) return urbi_make_nil();
    return val_obj(o);
}

/* Append value `v` to List object `lst`.
 * Returns 0 on success, -1 on allocation failure or invalid argument. */
int
urbi_list_append(UVM *vm, UValue lst, UValue v)
{
    if (vm == NULL) return -1;
    UList *l = list_storage(vm, lst);
    if (l == NULL) return -1;
    if (l->len == l->cap) {
        if (list_grow(vm, l, l->len + 1U) != 0) return -1;
    }
    container_element_pre_store(vm, v);
    l->items[l->len++] = v;
    return 0;
}

/* Return the number of elements in List object `lst`, or -1 if invalid. */
int
urbi_list_len(UVM *vm, UValue lst)
{
    if (vm == NULL) return -1;
    UList *l = list_storage(vm, lst);
    if (l == NULL) return -1;
    return (int)l->len;
}

/* Return the element at index `i` (0-based) from List object `lst`.
 * Returns urbi_make_nil() if `i` is out of range or `lst` is invalid. */
UValue
urbi_list_get(UVM *vm, UValue lst, int i)
{
    if (vm == NULL || i < 0) return urbi_make_nil();
    UList *l = list_storage(vm, lst);
    if (l == NULL) return urbi_make_nil();
    if ((size_t)i >= l->len) return urbi_make_nil();
    return l->items[(size_t)i];
}
#endif /* URBI_ENABLE_ROS2 */
