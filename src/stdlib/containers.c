/* SPDX-License-Identifier: BSD-3-Clause */
/* containers.c — List, Dict, Tuple, Pair and Triplet.  See containers.h.
 *
 * RECEIVERS.  A List, a Tuple and a Dict arrive as UV_CELL values, not as
 * objects: `self.v.p` IS the UList / UDict.  Every body therefore starts
 * with uv_is_list / uv_is_dict rather than with a slot lookup, and a bare
 * `List.clone()` — an ordinary object that merely inherits the List
 * prototype — is rejected by those guards the way the old core's missing
 * `_storage` slot rejected it.
 *
 * WHICH PROTOTYPE A RESULT CARRIES.  concat / diff / reverse / sort hand
 * back a fresh list carrying the RECEIVER's prototype, which is the new
 * spelling of the old "clone the receiver's proto object" step.
 *
 * EQUALITY AND ORDER.  Membership (contains, diff, Dict keys) is uv_equal,
 * the runtime's own structural comparison.  Order exists in exactly one
 * place — the no-argument sort — so there is exactly one comparator,
 * container_cmp.  The old file carried a private uval_cmp and a private
 * str_lex_cmp that between them duplicated both.
 *
 * CALLING BACK INTO SCRIPT.  sort(f) is the only native here that runs
 * user code.  Two hazards come with that: a nested call can grow the
 * strand's register stack, so the `args` window a native was handed may
 * be a dangling pointer afterwards (every value needed across a call is
 * copied into a local first), and a collection can run between calls, so
 * the snapshot list and the element lifted out of it are rooted.
 * map / filter / each and the rest of the higher-order surface stay in
 * the script overlay (stdlib.u), as they were before.
 *
 * ARITY is the boot table's (min_args / max_args in the tables below);
 * uexec rejects a mis-counted call before the body runs. */

#include "rt/ustdlib_glue.h"
#include "stdlib/containers.h"
#include "stdlib/stdlib_join_core.h"

/* === shared helpers ====================================================== */

/* Three-way order for the no-argument sort: number against number,
 * string against string.  *ok goes 0 for anything else, which the caller
 * turns into "elements not comparable". */
static int container_cmp(UValue a, UValue b, int *ok)
{
    *ok = 1;
    if (uv_is_number(a) && uv_is_number(b)) {
        if (a.kind == UV_INT && b.kind == UV_INT)
            return a.v.i < b.v.i ? -1 : (a.v.i > b.v.i ? 1 : 0);
        double x = uv_as_double(a), y = uv_as_double(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (urbi_is_str(a) && urbi_is_str(b)) {
        size_t na = urbi_str_size(a), nb = urbi_str_size(b);
        size_t n = na < nb ? na : nb;
        int d = n ? memcmp(urbi_str_cstr(a), urbi_str_cstr(b), n) : 0;
        if (d != 0) return d < 0 ? -1 : 1;
        return na < nb ? -1 : (na > nb ? 1 : 0);
    }
    *ok = 0;
    return 0;
}

/* A fresh list carrying the receiver's prototype, so a derived list stays
 * whatever the receiver was.  NULL on OOM. */
static UList *fresh_like(UVM *vm, UValue self, uint32_t cap)
{
    UObject *proto = ((const UList *)self.v.p)->proto;
    return ulist_new(vm, proto ? proto : vm->protos[UP_LIST], cap);
}

/* Installs `n` values as `first` / `second` / `third` on a fresh clone of
 * `proto`.  Used by Pair and Triplet, which carry their payload as
 * ordinary slots rather than as a cell. */
static int tuple_object_new(UVM *vm, UObject *proto, const UValue *args,
                            uint8_t n, UValue *out)
{
    static const char *const kNames[3] = { "first", "second", "third" };
    UObject *o = urbi_object_clone(vm, proto);
    if (!o) return urbi_raise_oom(vm, out);

    /* Rooted for the rest of the body: interning a name and growing the
     * slot block both allocate, and nothing else points at `o` yet. */
    UValue ov = uv_obj(o);
    URBI_ROOT(vm, ov);
    int rc = 0;
    for (uint8_t i = 0; i < n && rc == 0; i++) {
        USym *nm = usym_cstr(vm, kNames[i]);
        rc = (nm && urbi_object_set_local_slot(vm, o, nm, args[i]) == 0) ? 0 : -1;
    }
    URBI_UNROOT(vm, ov);
    if (rc != 0) return urbi_raise_oom(vm, out);
    *out = ov;
    return UEXEC_OK;
}

/* A fresh list cell on `proto` holding args[0..nargs). */
static int list_cell_new(UVM *vm, UObject *proto, const UValue *args,
                         uint8_t nargs, UValue *out)
{
    UList *l = ulist_new(vm, proto, nargs);
    if (!l) return urbi_raise_oom(vm, out);
    /* ulist_push only touches the raw items array, so nothing between
     * here and the last push can collect; the root is the cheap way to
     * keep that true if ulist_push ever grows a cell allocation. */
    UValue lv = uv_list(l);
    URBI_ROOT(vm, lv);
    int rc = 0;
    for (uint8_t i = 0; i < nargs && rc == 0; i++) rc = ulist_push(vm, l, args[i]);
    URBI_UNROOT(vm, lv);
    if (rc != 0) return urbi_raise_oom(vm, out);
    *out = lv;
    return UEXEC_OK;
}

/* === Pair and Triplet ==================================================== */

static int pair_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)nargs;
    return tuple_object_new(vm, vm->protos[UP_PAIR], args, 2, out);
}

static int triplet_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)nargs;
    return tuple_object_new(vm, vm->protos[UP_TRIPLET], args, 3, out);
}

/* === Tuple ===============================================================
 *
 * A Tuple is a UList cell whose prototype is Tuple, so `length` and `get`
 * are the very same bodies List uses.  Immutability is by omission: the
 * Tuple table has no mutator. */

static int tuple_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self;
    return list_cell_new(vm, vm->protos[UP_TUPLE], args, nargs, out);
}

/* === List and Tuple readers ============================================== */

static int list_length(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    if (!uv_is_list(self)) return urbi_raise_type(vm, "length: self must be a List", out);
    *out = uv_int((int64_t)((const UList *)self.v.p)->len);
    return UEXEC_OK;
}

static int list_isEmpty(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    if (!uv_is_list(self)) return urbi_raise_type(vm, "isEmpty: self must be a List", out);
    *out = uv_bool(((const UList *)self.v.p)->len == 0);
    return UEXEC_OK;
}

static int list_get(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (args[0].kind != UV_INT) return urbi_raise_type(vm, "get: index must be an Integer", out);
    if (!uv_is_list(self)) return urbi_raise_type(vm, "get: self must be a List", out);
    const UList *l = (const UList *)self.v.p;
    int64_t i = args[0].v.i;
    if (i < 0 || (uint64_t)i >= l->len) return urbi_raise_index(vm, "get: index out of range", out);
    *out = l->items[i];
    return UEXEC_OK;
}

static int list_contains(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (!uv_is_list(self)) return urbi_raise_type(vm, "contains: self must be a List", out);
    const UList *l = (const UList *)self.v.p;
    bool found = false;
    for (uint32_t i = 0; i < l->len && !found; i++) found = uv_equal(l->items[i], args[0]);
    *out = uv_bool(found);
    return UEXEC_OK;
}

/* === List mutators ======================================================= */

static int list_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self;
    return list_cell_new(vm, vm->protos[UP_LIST], args, nargs, out);
}

/* add(v) / insertBack(v) / `<<` — appends and returns the RECEIVER, which
 * is what makes the corpus's `l << a << b << c` chain work. */
static int list_add(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (!uv_is_list(self)) return urbi_raise_type(vm, "add: self must be a List", out);
    if (ulist_push(vm, (UList *)self.v.p, args[0]) != 0) return urbi_raise_oom(vm, out);
    *out = self;
    return UEXEC_OK;
}

static int list_set(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (args[0].kind != UV_INT) return urbi_raise_type(vm, "set: index must be an Integer", out);
    if (!uv_is_list(self)) return urbi_raise_type(vm, "set: self must be a List", out);
    UList *l = (UList *)self.v.p;
    int64_t i = args[0].v.i;
    if (i < 0 || (uint64_t)i >= l->len) return urbi_raise_index(vm, "set: index out of range", out);
    l->items[i] = args[1];
    *out = self;
    return UEXEC_OK;
}

/* === List derivations ====================================================
 *
 * Each builds a fresh list and never mutates the receiver.  `self` and the
 * argument are both rooted by uexec's native call arm, so the one
 * allocation in each body (the result) cannot sweep them. */

static int list_concat(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (!uv_is_list(self)) return urbi_raise_type(vm, "concat: self must be a List", out);
    if (!uv_is_list(args[0])) return urbi_raise_type(vm, "concat: argument must be a List", out);
    const UList *a = (const UList *)self.v.p;
    const UList *b = (const UList *)args[0].v.p;

    UList *o = fresh_like(vm, self, a->len + b->len);
    if (!o) return urbi_raise_oom(vm, out);
    int rc = 0;
    for (uint32_t i = 0; i < a->len && rc == 0; i++) rc = ulist_push(vm, o, a->items[i]);
    for (uint32_t i = 0; i < b->len && rc == 0; i++) rc = ulist_push(vm, o, b->items[i]);
    if (rc != 0) return urbi_raise_oom(vm, out);
    *out = uv_list(o);
    return UEXEC_OK;
}

static int list_diff(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (!uv_is_list(self)) return urbi_raise_type(vm, "diff: self must be a List", out);
    if (!uv_is_list(args[0])) return urbi_raise_type(vm, "diff: argument must be a List", out);
    const UList *a = (const UList *)self.v.p;
    const UList *b = (const UList *)args[0].v.p;

    UList *o = fresh_like(vm, self, a->len);
    if (!o) return urbi_raise_oom(vm, out);
    for (uint32_t i = 0; i < a->len; i++) {
        bool present = false;
        for (uint32_t j = 0; j < b->len && !present; j++) present = uv_equal(a->items[i], b->items[j]);
        if (!present && ulist_push(vm, o, a->items[i]) != 0) return urbi_raise_oom(vm, out);
    }
    *out = uv_list(o);
    return UEXEC_OK;
}

static int list_reverse(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    if (!uv_is_list(self)) return urbi_raise_type(vm, "reverse: self must be a List", out);
    const UList *a = (const UList *)self.v.p;
    UList *o = fresh_like(vm, self, a->len);
    if (!o) return urbi_raise_oom(vm, out);
    for (uint32_t i = a->len; i > 0; i--)
        if (ulist_push(vm, o, a->items[i - 1]) != 0) return urbi_raise_oom(vm, out);
    *out = uv_list(o);
    return UEXEC_OK;
}

/* Insertion sort over `o`'s own items.  `cmp` is nil for the default
 * order, otherwise a strict less-than predicate: cmp(a, b) truthy means
 * `a` sorts before `b` (the legacy convention — a descending sort is
 * written `function(a, b) { a > b }`).
 *
 * `o` and `cmp` are rooted by the caller.  `key` is rooted here because
 * the shift lifts it out of the array, leaving nothing else pointing at
 * it while the comparator allocates. */
static int sort_items(UVM *vm, UList *o, UValue cmp, UValue *out)
{
    for (uint32_t i = 1; i < o->len; i++) {
        UValue key = o->items[i];
        uint32_t j = i;
        URBI_ROOT(vm, key);
        while (j > 0) {
            bool before;
            if (cmp.kind == UV_NIL) {
                int ok;
                int c = container_cmp(o->items[j - 1], key, &ok);
                if (!ok) {
                    URBI_UNROOT(vm, key);
                    return urbi_raise_type(vm, "sort: elements not comparable", out);
                }
                before = c > 0;
            } else {
                UValue cargs[2], cres = uv_nil();
                cargs[0] = key;
                cargs[1] = o->items[j - 1];
                int rc = urbi_call_closure(vm, cmp, uv_nil(), cargs, 2, &cres);
                if (rc != UEXEC_OK) { URBI_UNROOT(vm, key); return rc; }
                before = uv_truthy(cres);
            }
            if (!before) break;
            o->items[j] = o->items[j - 1];
            j--;
        }
        URBI_UNROOT(vm, key);
        o->items[j] = key;
    }
    return UEXEC_OK;
}

/* sort() / sort(f) — a fresh sorted list; the receiver is untouched even
 * when the comparator throws.
 *
 * The snapshot is taken before any comparator runs and is reachable from
 * nothing the script can name, so a comparator that mutates the source
 * list cannot disturb the sort in progress. */
static int list_sort(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (!uv_is_list(self)) return urbi_raise_type(vm, "sort: self must be a List", out);

    /* Copied out of the argument window up front: running the comparator
     * can grow the strand's register stack, and `args` points into it. */
    UValue cmp = nargs ? args[0] : uv_nil();
    if (nargs == 1) {
        if (!urbi_is_closure(cmp))
            return urbi_raise_type(vm, "sort: comparator must be a function", out);
        const UClosure *cl = (const UClosure *)cmp.v.p;
        /* A native comparator has no bytecode body to run per comparison,
         * and an under-2-parameter body would read a window the 2-argument
         * call has already overwritten and silently mis-sort.  Both are the
         * same guard.  A 3-parameter body is left to its own arity
         * prologue, which rejects the 2-argument call correctly. */
        if (cl->proto == NULL || cl->proto->nparams < 2)
            return urbi_raise_type(vm, "sort: comparator must accept 2 arguments", out);
    }

    const UList *src = (const UList *)self.v.p;
    UList *o = fresh_like(vm, self, src->len);
    if (!o) return urbi_raise_oom(vm, out);
    UValue ov = uv_list(o);
    URBI_ROOT(vm, ov);
    URBI_ROOT(vm, cmp);
    int rc = 0;
    for (uint32_t i = 0; i < src->len && rc == 0; i++) rc = ulist_push(vm, o, src->items[i]);
    if (rc == 0) rc = sort_items(vm, o, cmp, out);
    else rc = urbi_raise_oom(vm, out);
    URBI_UNROOT(vm, cmp);
    URBI_UNROOT(vm, ov);
    if (rc != UEXEC_OK) return rc;
    *out = ov;
    return UEXEC_OK;
}

/* join(sep) — the shared join_core, the same one String.join uses. */
static int list_join(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (!urbi_is_str(args[0])) return urbi_raise_type(vm, "join: separator must be a String", out);
    if (!uv_is_list(self)) return urbi_raise_type(vm, "join: self must be a List", out);
    return join_core(vm, args[0], self, out);
}

/* === Dict ================================================================
 *
 * Keys are compared with uv_equal, so a number, a boolean or nil is as
 * good a key as a string; the old core restricted them to String because
 * its hash table hashed bytes, and nothing in the corpus asked for the
 * restriction. */

static int dict_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args; (void)nargs;
    UDict *d = udict_new(vm, vm->protos[UP_DICT]);
    if (!d) return urbi_raise_oom(vm, out);
    *out = uv_dict(d);
    return UEXEC_OK;
}

static int dict_length(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    if (!uv_is_dict(self)) return urbi_raise_type(vm, "length: self must be a Dictionary", out);
    *out = uv_int((int64_t)((const UDict *)self.v.p)->len);
    return UEXEC_OK;
}

static int dict_isEmpty(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    if (!uv_is_dict(self)) return urbi_raise_type(vm, "isEmpty: self must be a Dictionary", out);
    *out = uv_bool(((const UDict *)self.v.p)->len == 0);
    return UEXEC_OK;
}

static int dict_set(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (!uv_is_dict(self)) return urbi_raise_type(vm, "set: self must be a Dictionary", out);
    if (udict_set(vm, (UDict *)self.v.p, args[0], args[1]) != 0) return urbi_raise_oom(vm, out);
    *out = self;
    return UEXEC_OK;
}

/* get(key) — nil for an absent key rather than a raise, which is the
 * legacy contract.  has(key) is how a nil VALUE is told from a missing
 * one. */
static int dict_get(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (!uv_is_dict(self)) return urbi_raise_type(vm, "get: self must be a Dictionary", out);
    if (!udict_get((const UDict *)self.v.p, args[0], out)) *out = uv_nil();
    return UEXEC_OK;
}

static int dict_has(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (!uv_is_dict(self)) return urbi_raise_type(vm, "has: self must be a Dictionary", out);
    UValue ignored;
    *out = uv_bool(udict_get((const UDict *)self.v.p, args[0], &ignored));
    return UEXEC_OK;
}

static int dict_remove(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (!uv_is_dict(self)) return urbi_raise_type(vm, "remove: self must be a Dictionary", out);
    (void)udict_remove((UDict *)self.v.p, args[0]);
    *out = self;
    return UEXEC_OK;
}

/* keys() / values() — a fresh List.  Iteration order is unspecified, and
 * the two agree with each other for a dict nothing has touched in
 * between.  The scripted Dict.each iterates over a keys() snapshot, so an
 * entry added under it is not visited and one removed under it reads back
 * as nil. */
static int dict_collect(UVM *vm, UValue self, bool want_keys, UValue *out)
{
    const UDict *d = (const UDict *)self.v.p;
    UList *l = ulist_new(vm, vm->protos[UP_LIST], d->len);
    if (!l) return urbi_raise_oom(vm, out);
    UValue lv = uv_list(l);
    URBI_ROOT(vm, lv);
    int rc = 0;
    for (uint32_t i = 0; i < d->len && rc == 0; i++)
        rc = ulist_push(vm, l, want_keys ? d->keys[i] : d->vals[i]);
    URBI_UNROOT(vm, lv);
    if (rc != 0) return urbi_raise_oom(vm, out);
    *out = lv;
    return UEXEC_OK;
}

static int dict_keys(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    if (!uv_is_dict(self)) return urbi_raise_type(vm, "keys: self must be a Dictionary", out);
    return dict_collect(vm, self, true, out);
}

static int dict_values(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    if (!uv_is_dict(self)) return urbi_raise_type(vm, "values: self must be a Dictionary", out);
    return dict_collect(vm, self, false, out);
}

/* === the tables ==========================================================
 *
 * The legacy names each alias carries, with the source it came from:
 *   List.size       list.cc:167 BINDG(size)
 *   List.insertBack list.cc:92  BIND(insertBack)
 *   List.<<         list.u:35   copySlot("insertBack", "<<")
 *   List.+          list.cc:88  BIND(PLUS, operator+)
 *   Dict.size       dictionary.cc:91 BINDG(size)
 * `head` and the higher-order surface are the script overlay's. */

const UMethodDef ustdlib_pair_methods[USTDLIB_PAIR_NMETHODS] = {
    { "new", pair_new, 2, 2 }
};

const UMethodDef ustdlib_triplet_methods[USTDLIB_TRIPLET_NMETHODS] = {
    { "new", triplet_new, 3, 3 }
};

const UMethodDef ustdlib_tuple_methods[USTDLIB_TUPLE_NMETHODS] = {
    { "new",    tuple_new,   0, UMETHOD_VARARGS },
    { "length", list_length, 0, 0 },
    { "get",    list_get,    1, 1 }
};

const UMethodDef ustdlib_list_methods[USTDLIB_LIST_NMETHODS] = {
    { "new",        list_new,     0, UMETHOD_VARARGS },
    { "length",     list_length,  0, 0 },
    { "isEmpty",    list_isEmpty, 0, 0 },
    { "get",        list_get,     1, 1 },
    { "add",        list_add,     1, 1 },
    { "set",        list_set,     2, 2 },
    { "contains",   list_contains, 1, 1 },
    { "concat",     list_concat,  1, 1 },
    { "diff",       list_diff,    1, 1 },
    { "sort",       list_sort,    0, 1 },
    { "reverse",    list_reverse, 0, 0 },
    { "join",       list_join,    1, 1 },
    { "size",       list_length,  0, 0 },
    { "insertBack", list_add,     1, 1 },
    { "<<",         list_add,     1, 1 },
    { "+",          list_concat,  1, 1 }
};

const UMethodDef ustdlib_dict_methods[USTDLIB_DICT_NMETHODS] = {
    { "new",     dict_new,     0, 0 },
    { "length",  dict_length,  0, 0 },
    { "isEmpty", dict_isEmpty, 0, 0 },
    { "set",     dict_set,     2, 2 },
    { "get",     dict_get,     1, 1 },
    { "has",     dict_has,     1, 1 },
    { "remove",  dict_remove,  1, 1 },
    { "keys",    dict_keys,    0, 0 },
    { "values",  dict_values,  0, 0 },
    { "size",    dict_length,  0, 0 }
};
