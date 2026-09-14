#include <string.h>
#include "rtest.h"
#include "fakevm.h"
#include "rt/ulist.h"

static void push_grows_and_reports_len_cap(void) {
    /* No rooting needed: ulist_push only touches the raw items array via
     * ugc_raw_realloc, never ugc_alloc, so it can never trigger a
     * collection that would sweep an unrooted list out from under it. */
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    UList *l = ulist_new(&vm, NULL, 0);
    for (int i = 0; i < 1000; i++) RT_CHECK(ulist_push(&vm, l, uv_int(i)) == 0);
    RT_EQ(l->len, 1000u);
    RT_CHECK(l->cap >= 1000 && (l->cap & (l->cap - 1)) == 0);   /* power of two */
    for (int i = 0; i < 1000; i++) RT_CHECK(l->items[i].v.i == i);
    fakevm_destroy(&vm);
}

static void insert_at_zero_and_middle(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    UList *l = ulist_new(&vm, NULL, 0);
    ulist_push(&vm, l, uv_int(1));
    ulist_push(&vm, l, uv_int(2));
    ulist_push(&vm, l, uv_int(3));
    RT_CHECK(ulist_insert(&vm, l, 0, uv_int(0)) == 0);   /* -> 0 1 2 3 */
    RT_EQ(l->len, 4u);
    RT_EQ(l->items[0].v.i, 0); RT_EQ(l->items[1].v.i, 1);
    RT_CHECK(ulist_insert(&vm, l, 2, uv_int(99)) == 0);  /* -> 0 1 99 2 3 */
    RT_EQ(l->len, 5u);
    RT_EQ(l->items[0].v.i, 0); RT_EQ(l->items[1].v.i, 1); RT_EQ(l->items[2].v.i, 99);
    RT_EQ(l->items[3].v.i, 2); RT_EQ(l->items[4].v.i, 3);
    RT_CHECK(ulist_insert(&vm, l, l->len, uv_int(100)) == 0);   /* at == len appends */
    RT_EQ(l->len, 6u); RT_EQ(l->items[5].v.i, 100);
    RT_CHECK(ulist_insert(&vm, l, l->len + 1, uv_int(101)) == -1);   /* at > len fails */
    RT_EQ(l->len, 6u);
    fakevm_destroy(&vm);
}

static void remove_at_first_middle_last(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    UList *l = ulist_new(&vm, NULL, 0);
    for (int i = 0; i < 5; i++) ulist_push(&vm, l, uv_int(i));   /* 0 1 2 3 4 */
    RT_CHECK(ulist_remove_at(l, 0));                             /* -> 1 2 3 4 */
    RT_EQ(l->len, 4u); RT_EQ(l->items[0].v.i, 1);
    RT_CHECK(ulist_remove_at(l, 1));                             /* -> 1 3 4 */
    RT_EQ(l->len, 3u);
    RT_EQ(l->items[0].v.i, 1); RT_EQ(l->items[1].v.i, 3); RT_EQ(l->items[2].v.i, 4);
    RT_CHECK(ulist_remove_at(l, 2));                             /* -> 1 3 */
    RT_EQ(l->len, 2u); RT_EQ(l->items[0].v.i, 1); RT_EQ(l->items[1].v.i, 3);
    RT_CHECK(!ulist_remove_at(l, 5));                            /* out of range */
    fakevm_destroy(&vm);
}

static void dict_set_get_remove_mixed_keys(void) {
    /* d is rooted for the whole test: ustr_new below is a real ugc_alloc
     * and, under URBI_GC_STRESS, could collect before storing the result
     * -- an unrooted d would be swept out from under itself. */
    UCell *roots[1] = { NULL };
    struct UVM vm; fakevm_init(&vm, roots, 1);
    UDict *d = udict_new(&vm, NULL); roots[0] = &d->cell;
    RT_CHECK(udict_set(&vm, d, uv_int(1), uv_int(100)) == 0);
    UStr *str_a = ustr_new(&vm, "a", 1);
    RT_CHECK(udict_set(&vm, d, uv_str(str_a), uv_int(200)) == 0);
    USym *sym_b = usym_cstr(&vm, "b");
    RT_CHECK(udict_set(&vm, d, uv_sym(sym_b), uv_int(300)) == 0);
    RT_EQ(d->len, 3u);
    UValue out;
    RT_CHECK(udict_get(d, uv_int(1), &out) && out.v.i == 100);
    /* stored under a STR key "a"; looked up via a SYM "a" -- cross-kind. */
    RT_CHECK(udict_get(d, uv_sym(usym_cstr(&vm, "a")), &out) && out.v.i == 200);
    RT_CHECK(udict_get(d, uv_sym(sym_b), &out) && out.v.i == 300);
    RT_CHECK(!udict_get(d, uv_int(999), &out));
    RT_CHECK(udict_remove(d, uv_int(1)));
    RT_EQ(d->len, 2u);
    RT_CHECK(!udict_get(d, uv_int(1), &out));
    RT_CHECK(udict_get(d, uv_sym(usym_cstr(&vm, "a")), &out) && out.v.i == 200);
    RT_CHECK(udict_get(d, uv_sym(sym_b), &out) && out.v.i == 300);
    RT_CHECK(!udict_remove(d, uv_int(1)));   /* already gone */
    fakevm_destroy(&vm);
}

static void dict_overwrite_keeps_len(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    UDict *d = udict_new(&vm, NULL);
    udict_set(&vm, d, uv_int(1), uv_int(10));
    udict_set(&vm, d, uv_int(2), uv_int(20));
    RT_EQ(d->len, 2u);
    RT_CHECK(udict_set(&vm, d, uv_int(1), uv_int(99)) == 0);
    RT_EQ(d->len, 2u);
    UValue out;
    RT_CHECK(udict_get(d, uv_int(1), &out) && out.v.i == 99);
    fakevm_destroy(&vm);
}

static void gc_rooted_list_keeps_strings_alive(void) {
    UCell *roots[1] = { NULL };
    struct UVM vm; fakevm_init(&vm, roots, 1);
    UList *l = ulist_new(&vm, NULL, 0);
    roots[0] = &l->cell;
    for (int i = 0; i < 100; i++) {
        char buf[16];
        snprintf(buf, sizeof buf, "s%d", i);
        UStr *s = ustr_new(&vm, buf, strlen(buf));
        RT_CHECK(ulist_push(&vm, l, uv_str(s)) == 0);
    }
    ugc_collect(&vm);
    RT_EQ(l->len, 100u);
    RT_EQ(vm.gc.cells_live, 101u);   /* l itself + 100 string items */
    for (int i = 0; i < 100; i++) {
        char expect[16]; snprintf(expect, sizeof expect, "s%d", i);
        uint32_t len; const char *bytes = uv_str_bytes(l->items[i], &len);
        RT_CHECK(len == strlen(expect) && memcmp(bytes, expect, len) == 0);
    }
    fakevm_destroy(&vm);
}

static void gc_unrooted_list_reclaimed(void) {
    /* Rooted during construction (ustr_new may collect), then dropped
     * from the root set before the check collect -- that's what makes it
     * "the unrooted list" at the moment reclamation is verified. */
    UCell *roots[1] = { NULL };
    struct UVM vm; fakevm_init(&vm, roots, 1);
    UList *l = ulist_new(&vm, NULL, 0);
    roots[0] = &l->cell;
    for (int i = 0; i < 1000; i++) {
        char buf[16]; snprintf(buf, sizeof buf, "x%d", i);
        UStr *s = ustr_new(&vm, buf, strlen(buf));
        RT_CHECK(ulist_push(&vm, l, uv_str(s)) == 0);
    }
    roots[0] = NULL;
    ugc_collect(&vm);
    RT_EQ(vm.gc.cells_live, 0u);
    fakevm_destroy(&vm);
}

static void uv_equal_table(void) {
    UCell *roots[2] = { NULL, NULL };
    struct UVM vm; fakevm_init(&vm, roots, 2);
    RT_CHECK(uv_equal(uv_int(1), uv_float(1.0)));
    RT_CHECK(uv_equal(uv_float(1.0), uv_int(1)));
    RT_CHECK(!uv_equal(uv_int(1), uv_int(2)));
    USym *sym_a = usym_cstr(&vm, "a");
    UStr *str_a = ustr_new(&vm, "a", 1);
    RT_CHECK(uv_equal(uv_sym(sym_a), uv_str(str_a)));
    RT_CHECK(uv_equal(uv_str(str_a), uv_sym(sym_a)));
    UObject *o1 = uobj_new(&vm, NULL); roots[0] = &o1->cell;
    UObject *o2 = uobj_new(&vm, NULL); roots[1] = &o2->cell;
    RT_CHECK(!uv_equal(uv_obj(o1), uv_obj(o2)));
    RT_CHECK(uv_equal(uv_obj(o1), uv_obj(o1)));
    RT_CHECK(!uv_equal(uv_nil(), uv_void()));
    RT_CHECK(uv_equal(uv_nil(), uv_nil()));
    RT_CHECK(uv_equal(uv_bool(true), uv_bool(true)));
    RT_CHECK(!uv_equal(uv_bool(true), uv_bool(false)));
    fakevm_destroy(&vm);
}

RT_SUITE(rt_list_suite) {
    rt_run("push_grows_and_reports_len_cap", push_grows_and_reports_len_cap);
    rt_run("insert_at_zero_and_middle", insert_at_zero_and_middle);
    rt_run("remove_at_first_middle_last", remove_at_first_middle_last);
    rt_run("dict_set_get_remove_mixed_keys", dict_set_get_remove_mixed_keys);
    rt_run("dict_overwrite_keeps_len", dict_overwrite_keeps_len);
    rt_run("gc_rooted_list_keeps_strings_alive", gc_rooted_list_keeps_strings_alive);
    rt_run("gc_unrooted_list_reclaimed", gc_unrooted_list_reclaimed);
    rt_run("uv_equal_table", uv_equal_table);
}
