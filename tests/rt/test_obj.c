#include "rtest.h"
#include "fakevm.h"
#include "rt/uobj.h"
static void local_slots_grow(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    UObject *o = uobj_new(&vm, NULL);
    char buf[8];
    for (int i = 0; i < 40; i++) { snprintf(buf, sizeof buf, "s%d", i); RT_CHECK(uobj_set_local(&vm, o, usym_cstr(&vm, buf), uv_int(i), 0) >= 0); }
    RT_EQ(o->count, 40); RT_CHECK(o->cap >= 40 && o->cap <= 64);
    RT_EQ(uobj_find_local(o, usym_cstr(&vm, "s17")), 17);
    RT_CHECK(uobj_set_local(&vm, o, usym_cstr(&vm, "s17"), uv_int(99), 0) == 17 && o->values[17].v.i == 99);
    RT_CHECK(uobj_remove_local(&vm, o, usym_cstr(&vm, "s0")) && o->count == 39 && uobj_find_local(o, usym_cstr(&vm, "s0")) == -1);
    fakevm_destroy(&vm);
}
static void proto_resolution_and_diamond(void) {
    /* Every UObject created here must be rooted before the *next*
     * uobj_new call: under URBI_GC_STRESS every ugc_alloc collects first,
     * and none of root/a/b/d is reachable from anywhere but a local C
     * variable until it's linked in as somebody's proto. */
    UCell *roots[4] = { NULL, NULL, NULL, NULL };
    struct UVM vm; fakevm_init(&vm, roots, 4);
    UObject *root = uobj_new(&vm, NULL); roots[0] = &root->cell;
    UObject *a = uobj_new(&vm, root);    roots[1] = &a->cell;
    UObject *b = uobj_new(&vm, root);    roots[2] = &b->cell;
    UObject *d = uobj_new(&vm, a);       roots[3] = &d->cell;
    uobj_add_proto(&vm, d, b);
    uobj_set_local(&vm, root, usym_cstr(&vm, "x"), uv_int(1), 0);
    uobj_set_local(&vm, b, usym_cstr(&vm, "y"), uv_int(2), 0);
    UObjSlotRef r;
    RT_CHECK(uobj_resolve(&vm, d, usym_cstr(&vm, "x"), &r) && r.owner == root && uobj_slot_value(&r).v.i == 1);
    RT_CHECK(uobj_resolve(&vm, d, usym_cstr(&vm, "y"), &r) && r.owner == b);
    RT_CHECK(!uobj_resolve(&vm, d, usym_cstr(&vm, "z"), &r));
    RT_CHECK(uobj_is_a(&vm, d, root) && !uobj_is_a(&vm, root, d));
    fakevm_destroy(&vm);
}
static void gc_traces_values_and_protos(void) {
    /* root must be rooted before `o` is allocated (same stress-mode
     * hazard as above): o's own root slot isn't set until after both
     * exist, so root would otherwise be unreachable garbage during the
     * `uobj_new(&vm, root)` call that creates o. */
    UCell *roots[2] = { NULL, NULL };
    struct UVM vm; fakevm_init(&vm, roots, 2);
    UObject *root = uobj_new(&vm, NULL); roots[0] = &root->cell;
    UObject *o = uobj_new(&vm, root);    roots[1] = &o->cell;
    uobj_set_local(&vm, o, usym_cstr(&vm, "s"), uv_str(ustr_new(&vm, "keep", 4)), 0);
    ugc_alloc(&vm, UCELL_STR, 32);   /* garbage */
    ugc_collect(&vm);
    RT_EQ(vm.gc.cells_live, 3u);     /* o, root, the string */
    fakevm_destroy(&vm);
}
static void set_protos_three(void) {
    UCell *roots[4] = { NULL, NULL, NULL, NULL };
    struct UVM vm; fakevm_init(&vm, roots, 4);
    UObject *p0 = uobj_new(&vm, NULL); roots[0] = &p0->cell;
    UObject *p1 = uobj_new(&vm, NULL); roots[1] = &p1->cell;
    UObject *p2 = uobj_new(&vm, NULL); roots[2] = &p2->cell;
    UObject *o  = uobj_new(&vm, NULL); roots[3] = &o->cell;
    uobj_set_local(&vm, p0, usym_cstr(&vm, "k"), uv_int(0), 0);
    uobj_set_local(&vm, p1, usym_cstr(&vm, "k"), uv_int(1), 0);
    uobj_set_local(&vm, p2, usym_cstr(&vm, "k"), uv_int(2), 0);
    UObject *ps[3] = { p0, p1, p2 };
    RT_CHECK(uobj_set_protos(&vm, o, ps, 3) == 0);
    RT_EQ(o->nprotos, 3); RT_CHECK(o->protos != NULL && o->proto0 == p0);
    UObjSlotRef r;
    /* "k" lives on all three protos; resolution must pick protos[0] first. */
    RT_CHECK(uobj_resolve(&vm, o, usym_cstr(&vm, "k"), &r) && r.owner == p0);
    fakevm_destroy(&vm);
}
/* addProto PREPENDS: the most recently added prototype wins the
 * depth-first walk.  Legacy urbiscript does the same (Object::proto_add's
 * push_front), and the whole `Target.addProto(TargetMethods)` pattern in
 * the standard-library overlay depends on it -- under append, the Object
 * root would answer `new` and `clone` ahead of every overlay. */
static void add_proto_prepends(void) {
    UCell *roots[4] = { NULL, NULL, NULL, NULL };
    struct UVM vm; fakevm_init(&vm, roots, 4);
    UObject *base = uobj_new(&vm, NULL);  roots[0] = &base->cell;
    UObject *mid  = uobj_new(&vm, NULL);  roots[1] = &mid->cell;
    UObject *top  = uobj_new(&vm, NULL);  roots[2] = &top->cell;
    UObject *o    = uobj_new(&vm, NULL);  roots[3] = &o->cell;

    USym *k = usym_cstr(&vm, "which");
    uobj_set_local(&vm, base, k, uv_int(1), 0);
    uobj_set_local(&vm, mid,  k, uv_int(2), 0);
    uobj_set_local(&vm, top,  k, uv_int(3), 0);

    /* First add: the single-proto representation. */
    uobj_add_proto(&vm, o, base);
    RT_EQ(o->nprotos, 1);
    RT_EQ(o->proto0, base);

    /* Second add: promotes to the array, newest FIRST. */
    uobj_add_proto(&vm, o, mid);
    RT_EQ(o->nprotos, 2);
    RT_EQ(o->protos[0], mid);
    RT_EQ(o->protos[1], base);
    RT_EQ(o->proto0, mid);

    /* Third add: still newest first, the rest shifted down. */
    uobj_add_proto(&vm, o, top);
    RT_EQ(o->nprotos, 3);
    RT_EQ(o->protos[0], top);
    RT_EQ(o->protos[1], mid);
    RT_EQ(o->protos[2], base);

    /* And resolution follows that order, which is the point. */
    UObjSlotRef ref;
    RT_CHECK(uobj_resolve(&vm, o, k, &ref));
    RT_EQ(ref.owner, top);
    RT_EQ(uobj_slot_value(&ref).v.i, 3);

    fakevm_destroy(&vm);
}
static void remove_proto(void) {
    UCell *roots[3] = { NULL, NULL, NULL };
    struct UVM vm; fakevm_init(&vm, roots, 3);
    UObject *p0 = uobj_new(&vm, NULL); roots[0] = &p0->cell;
    UObject *p1 = uobj_new(&vm, NULL); roots[1] = &p1->cell;
    UObject *o  = uobj_new(&vm, NULL); roots[2] = &o->cell;
    uobj_add_proto(&vm, o, p0);
    uobj_add_proto(&vm, o, p1);
    RT_EQ(o->nprotos, 2);
    RT_CHECK(uobj_remove_proto(&vm, o, p0) == 0);
    RT_CHECK(o->nprotos == 1 && o->proto0 == p1 && o->protos == NULL);
    RT_CHECK(uobj_remove_proto(&vm, o, p1) == 0);
    RT_CHECK(o->nprotos == 0 && o->proto0 == NULL);
    RT_CHECK(uobj_remove_proto(&vm, o, p0) == -1);   /* already gone */
    fakevm_destroy(&vm);
}
static void getter_slot_survives_gc(void) {
    UCell *roots[1] = { NULL };
    struct UVM vm; fakevm_init(&vm, roots, 1);
    UObject *o = uobj_new(&vm, NULL);
    roots[0] = &o->cell;
    int idx = uobj_set_local(&vm, o, usym_cstr(&vm, "g"), uv_int(42), USLOT_GETTER);
    RT_CHECK(idx >= 0 && uv_is(o->values[idx], UV_CELL));
    ugc_collect(&vm);
    RT_EQ(vm.gc.cells_live, 2u);    /* o (rooted) + its UProps cell (reached via uobj_trace) */
    UProps *props = (UProps *)o->values[idx].v.p;
    RT_CHECK(props->value.v.i == 42);
    fakevm_destroy(&vm);
}
static void resolve_depth_cap(void) {
    /* Build a 70-level proto chain where every level also protos a shared
     * dead-end object (protos = [next-in-chain, dummy], so the chain link
     * is searched first). uobj_resolve's DFS pushes both children of each
     * level and only pops the chain link immediately -- the dummy sibling
     * stays buried on the stack while the walk keeps descending, adding
     * one stack slot net per level. That overflows the 64-slot cap well
     * before the chain bottoms out, so this exercises the cap itself
     * rather than a legitimate not-found search. */
    UCell *roots[2] = { NULL, NULL };
    struct UVM vm; fakevm_init(&vm, roots, 2);
    UObject *dummy = uobj_new(&vm, NULL); roots[0] = &dummy->cell;
    UObject *chain = uobj_new(&vm, NULL); roots[1] = &chain->cell;
    for (int i = 0; i < 70; i++) {
        UObject *next = uobj_new(&vm, chain);
        uobj_add_proto(&vm, next, dummy);
        chain = next;
        roots[1] = &chain->cell;
    }
    UObjSlotRef r;
    RT_CHECK(!uobj_resolve(&vm, chain, usym_cstr(&vm, "nope"), &r));
    fakevm_destroy(&vm);
}
RT_SUITE(rt_obj_suite) {
    rt_run("local_slots_grow", local_slots_grow);
    rt_run("proto_resolution_and_diamond", proto_resolution_and_diamond);
    rt_run("gc_traces_values_and_protos", gc_traces_values_and_protos);
    rt_run("set_protos_three", set_protos_three);
    rt_run("add_proto_prepends", add_proto_prepends);
    rt_run("remove_proto", remove_proto);
    rt_run("getter_slot_survives_gc", getter_slot_survives_gc);
    rt_run("resolve_depth_cap", resolve_depth_cap);
}
