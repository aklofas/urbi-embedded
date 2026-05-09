/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: URealm + UNamespace lifecycle (T14, row 8).
 *
 * Tests cover:
 *  1.  round_trip: create + destroy frees cleanly; vm->realms_head returns NULL.
 *  2.  global_idempotent: urbi_realm_global returns the same pointer on repeated calls.
 *  3.  id_unique_monotonic: IDs are positive, unique, and increase across creates.
 *  4.  destroy_cascades_via_tag_stop: tag == NULL at M3; destroy completes without crash.
 *  5.  realm_flags_default_zero: fresh Realm has flags == 0.
 *  6.  realm_global_sets_global_flag: global Realm has REALM_GLOBAL set.
 *  7.  realm_user_data_round_trip: user_data survives round-trip through create.
 *  8.  realm_reflective_nil_at_m3: reflective.kind == UVAL_NIL after create.
 *  9.  realm_linked_list_invariants_after_create_destroy_create: list stitched correctly.
 * 10.  realm_destroy_unlinks_middle: destroy middle element fixes both neighbours.
 * 11.  realm_destroy_null_safe: urbi_realm_destroy(vm, NULL) is a no-op.
 * 12.  realm_namespace_set_get_round_trip: set then get returns stored value.
 * 13.  realm_namespace_grow_past_initial_cap: insert > 16 entries, all readable.
 * 14.  realm_walk_roots_invokes_callback_per_namespace_entry: walker visits each entry.
 * 15.  realm_create_oom_returns_null: allocator returning NULL yields NULL from create.
 * 16.  realm_destroy_does_not_free_strand_on_ready_queue: REALM-011 ready_queue splice.
 * 17.  realm_has_live_work_null_out_params: NULL out-params accepted (REALM-034). */

#include "utest.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "value/uintern.h"
#include "sched/ustrand.h"   /* T69: UStrand layout + ready_next/ready_prev fields */
#include "urbi/urbi.h"       /* T69: urbi_strand_create / urbi_strand_start (public API) */

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* === Helpers === */

static UValue
make_int(int64_t i)
{
    UValue v;
    v.kind = UVAL_INT;
    v.v.i  = i;
    return v;
}

/* Failing allocator: always returns NULL. */
static void *
null_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ptr; (void)nbytes; (void)ud;
    return NULL;
}

/* Count realm from realms_head. */
static int
count_realms(UVM *vm)
{
    int n = 0;
    URealm *r;
    for (r = vm->realms_head; r != NULL; r = r->next_in_vm) n++;
    return n;
}

/* GC root counter: ctx is int * incremented for each visited root. */
static void
root_count_cb(struct UVM *vm, UValue *root, void *ctx)
{
    (void)vm; (void)root;
    (*(int *)ctx)++;
}

/* ===== Tests ===== */

/* 1. round_trip: create + destroy leaves realms_head NULL. */
UTEST(realm_round_trip)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT(vm.realms_head == r);

    urbi_realm_destroy(&vm, r);
    UASSERT(vm.realms_head == NULL);

    urbi_vm_destroy(&vm);
}

/* 2. global_idempotent: repeated calls return the same pointer. */
UTEST(realm_global_idempotent)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *g1 = urbi_realm_global(&vm);
    URealm *g2 = urbi_realm_global(&vm);
    UASSERT(g1 != NULL);
    UASSERT(g1 == g2);

    urbi_vm_destroy(&vm);
}

/* 3. id_unique_monotonic: IDs are > 0, unique, and increasing. */
UTEST(realm_id_unique_monotonic)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *r1 = urbi_realm_create(&vm);
    URealm *r2 = urbi_realm_create(&vm);
    URealm *r3 = urbi_realm_create(&vm);

    UASSERT(r1 != NULL);
    UASSERT(r2 != NULL);
    UASSERT(r3 != NULL);

    UASSERT(r1->id > 0);
    UASSERT(r2->id > r1->id);
    UASSERT(r3->id > r2->id);

    urbi_realm_destroy(&vm, r1);
    urbi_realm_destroy(&vm, r2);
    urbi_realm_destroy(&vm, r3);
    urbi_vm_destroy(&vm);
}

/* 4. destroy_cascades_via_tag_stop: realm->tag is created at T29; destroy completes
 *    without crash.  urbi_tag_stop is a stub at M3; utag_destroy frees the tag. */
UTEST(realm_destroy_cascades_via_tag_stop)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    /* T29: realm->tag is now a real UTag (not NULL). */
    UASSERT(r->tag != NULL);

    /* Should not crash — urbi_tag_stop (stub) + utag_destroy handle the tag. */
    urbi_realm_destroy(&vm, r);
    UASSERT(vm.realms_head == NULL);

    urbi_vm_destroy(&vm);
}

/* 5. realm_flags_default_zero: fresh Realm has flags == 0. */
UTEST(realm_flags_default_zero)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT_EQ((unsigned)r->flags, 0U);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 6. realm_global_sets_global_flag: the global Realm has REALM_GLOBAL set. */
UTEST(realm_global_sets_global_flag)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *g = urbi_realm_global(&vm);
    UASSERT(g != NULL);
    UASSERT((g->flags & REALM_GLOBAL) != 0);

    urbi_vm_destroy(&vm);
}

/* 7. realm_user_data_round_trip: user_data survives round-trip through create. */
UTEST(realm_user_data_round_trip)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    int sentinel = 42;
    r->user_data = &sentinel;
    UASSERT(r->user_data == &sentinel);
    UASSERT(*(int *)r->user_data == 42);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 8. realm_reflective_nil_at_m3: reflective.kind == UVAL_NIL after create. */
UTEST(realm_reflective_nil_at_m3)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT(r->reflective.kind == UVAL_NIL);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 9. realm_linked_list_invariants_after_create_destroy_create:
 *    create A, destroy A, create B → list has exactly B, B->prev == NULL. */
UTEST(realm_linked_list_invariants_after_create_destroy_create)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *a = urbi_realm_create(&vm);
    UASSERT(a != NULL);
    UASSERT_EQ(count_realms(&vm), 1);

    urbi_realm_destroy(&vm, a);
    UASSERT_EQ(count_realms(&vm), 0);
    UASSERT(vm.realms_head == NULL);

    URealm *b = urbi_realm_create(&vm);
    UASSERT(b != NULL);
    UASSERT_EQ(count_realms(&vm), 1);
    UASSERT(vm.realms_head == b);
    UASSERT(b->prev_in_vm == NULL);
    UASSERT(b->next_in_vm == NULL);

    urbi_realm_destroy(&vm, b);
    urbi_vm_destroy(&vm);
}

/* 10. realm_destroy_unlinks_middle: A→B→C, destroy B → A↔C. */
UTEST(realm_destroy_unlinks_middle)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Create order: C first (head), then B (new head), then A (new head).
     * List order: A→B→C (most-recently-created at front). */
    URealm *c = urbi_realm_create(&vm);
    URealm *b = urbi_realm_create(&vm);
    URealm *a = urbi_realm_create(&vm);

    UASSERT(vm.realms_head == a);
    UASSERT_EQ(count_realms(&vm), 3);

    /* Destroy middle element B. */
    urbi_realm_destroy(&vm, b);
    UASSERT_EQ(count_realms(&vm), 2);
    UASSERT(vm.realms_head == a);
    UASSERT(a->next_in_vm == c);
    UASSERT(c->prev_in_vm == a);

    urbi_realm_destroy(&vm, a);
    urbi_realm_destroy(&vm, c);
    urbi_vm_destroy(&vm);
}

/* 11. realm_destroy_null_safe: no-op on NULL. */
UTEST(realm_destroy_null_safe)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Should not crash. */
    urbi_realm_destroy(&vm, NULL);

    urbi_vm_destroy(&vm);
}

/* 12. realm_namespace_set_get_round_trip: set a value, get it back. */
UTEST(realm_namespace_set_get_round_trip)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    /* Intern the key so it's a stable pointer. */
    const char *key = ustr_intern(&vm, "x", 1);
    UASSERT(key != NULL);

    UValue v = make_int(99);
    int rc = unamespace_set(&vm, r->bindings, key, v);
    UASSERT_EQ(rc, 0);

    UValue *got = unamespace_get(r->bindings, key);
    UASSERT(got != NULL);
    UASSERT(got->kind == UVAL_INT);
    UASSERT_EQ(got->v.i, 99LL);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 13. realm_namespace_grow_past_initial_cap: insert > 16 entries. */
UTEST(realm_namespace_grow_past_initial_cap)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    /* Insert 24 entries with interned single-char keys "a".."z" (minus 2). */
    static const char *labels[] = {
        "a","b","c","d","e","f","g","h",
        "i","j","k","l","m","n","o","p",
        "q","r","s","t","u","v","x"
    };
    unsigned n = sizeof(labels) / sizeof(labels[0]);
    unsigned i;

    for (i = 0; i < n; i++) {
        const char *k = ustr_intern(&vm, labels[i], strlen(labels[i]));
        UASSERT(k != NULL);
        UValue v = make_int((int64_t)(i + 1));
        int rc = unamespace_set(&vm, r->bindings, k, v);
        UASSERT_EQ(rc, 0);
    }

    /* Verify all entries readable after potential realloc. */
    for (i = 0; i < n; i++) {
        const char *k = ustr_intern(&vm, labels[i], strlen(labels[i]));
        UValue *got = unamespace_get(r->bindings, k);
        UASSERT(got != NULL);
        UASSERT_EQ(got->v.i, (long long)(i + 1));
    }

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 14. realm_walk_roots_invokes_callback_per_namespace_entry:
 *     N entries → callback fired at least N times. */
UTEST(realm_walk_roots_invokes_callback_per_namespace_entry)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    /* Insert 3 entries. */
    const char *k1 = ustr_intern(&vm, "p", 1);
    const char *k2 = ustr_intern(&vm, "q", 1);
    const char *k3 = ustr_intern(&vm, "r", 1);
    UASSERT(k1 && k2 && k3);

    unamespace_set(&vm, r->bindings, k1, make_int(1));
    unamespace_set(&vm, r->bindings, k2, make_int(2));
    unamespace_set(&vm, r->bindings, k3, make_int(3));

    int count = 0;
    realm_list_walk_roots(&vm, root_count_cb, &count);

    /* 3 namespace entries + 1 reflective UValue = 4 callbacks. */
    UASSERT(count >= 3);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 15. realm_create_oom_returns_null: null allocator → NULL from create. */
UTEST(realm_create_oom_returns_null)
{
    UVM vm;
    urbi_vm_init(&vm, null_alloc, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r == NULL);
    UASSERT(vm.realms_head == NULL);

    urbi_vm_destroy(&vm);
}

/* 16. realm_destroy_does_not_free_strand_on_ready_queue (T69 / REALM-011)
 *
 * Reproducer: create a realm, spawn two strands so they land on the
 * cooperative scheduler's ready_queue (state == READY), then destroy the
 * realm.  Pre-fix, urbi_strand_destroy would free each strand without
 * splicing it out of vm->ready_head / ready_tail's doubly-linked list,
 * leaving the queue head/tail and the surviving strand's ready_prev /
 * ready_next dangling — caught immediately by ASan on the next access.
 * Post-fix, sched_strand_unbind_from_ready_queue runs before each free,
 * so the queue is fully drained and ready_head / ready_tail are NULL.
 *
 * The test deliberately runs the destroy without invoking the dispatch
 * loop — the strand's entry_closure is NULL, but the strand never
 * actually runs.  Only its membership on the ready_queue matters here. */
UTEST(realm_destroy_does_not_free_strand_on_ready_queue)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s1 = urbi_strand_create(r, NULL);
    UASSERT(s1 != NULL);
    urbi_strand_start(s1);
    UStrand *s2 = urbi_strand_create(r, NULL);
    UASSERT(s2 != NULL);
    urbi_strand_start(s2);

    /* Sanity: both strands must be on the ready_queue before destroy. */
    UASSERT(vm.ready_head != NULL);
    UASSERT(vm.ready_tail != NULL);
    UASSERT_EQ((uint32_t)2, vm.strand_runnable_count);

    /* Destroy realm — under the bug, freed strand memory remains
     * referenced by vm->ready_head / ready_tail.  Under ASan this would
     * surface as use-after-free on any subsequent ready_queue access. */
    urbi_realm_destroy(&vm, r);

    /* Post-condition: queue head/tail cleared; runnable count drained. */
    UASSERT(vm.ready_head == NULL);
    UASSERT(vm.ready_tail == NULL);
    UASSERT_EQ((uint32_t)0, vm.strand_runnable_count);

    urbi_vm_destroy(&vm);
}

/* 17. realm_has_live_work_null_out_params (REALM-034)
 *
 * The has_live_work API documents that out_strands / out_watchers / out_wakes
 * may be NULL.  Coverage was missing — every existing caller passed all
 * three.  Pin the contract so future refactors that drop the NULL guard
 * (or shift the early-return ordering) trip a test rather than crash a
 * caller.
 *
 * Verified shapes:
 *   a) realm == NULL with all out-params NULL → returns false, no crash.
 *   b) realm != NULL with all out-params NULL → returns the bool result,
 *      no crash.
 *   c) realm != NULL with a mix of NULL / non-NULL out-params → only the
 *      non-NULL slots are written. */
UTEST(realm_has_live_work_null_out_params)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    /* a) NULL realm + all-NULL out-params → returns false. */
    UASSERT_EQ(false, urbi_realm_has_live_work(NULL, NULL, NULL, NULL));

    /* b) Live realm + all-NULL out-params → returns whatever the counters
     *    say, no crash.  At rest (no strands/watchers/wakes) → false. */
    UASSERT_EQ(false, urbi_realm_has_live_work(r, NULL, NULL, NULL));

    /* c) Mixed NULL / non-NULL → non-NULL slots are written, NULL slots
     *    are silently skipped.  Sentinel-fill the non-NULL receivers and
     *    confirm they got zeroed. */
    uint32_t strands  = 0xDEADBEEFu;
    uint32_t watchers = 0xDEADBEEFu;
    uint32_t wakes    = 0xDEADBEEFu;

    UASSERT_EQ(false, urbi_realm_has_live_work(r, &strands, NULL, NULL));
    UASSERT_EQ((uint32_t)0, strands);

    strands = 0xDEADBEEFu;
    UASSERT_EQ(false, urbi_realm_has_live_work(r, NULL, &watchers, NULL));
    UASSERT_EQ((uint32_t)0, watchers);

    watchers = 0xDEADBEEFu;
    UASSERT_EQ(false, urbi_realm_has_live_work(r, NULL, NULL, &wakes));
    UASSERT_EQ((uint32_t)0, wakes);

    /* d) NULL realm + non-NULL out-params → all out-params zeroed. */
    strands = watchers = wakes = 0xDEADBEEFu;
    UASSERT_EQ(false, urbi_realm_has_live_work(NULL, &strands, &watchers, &wakes));
    UASSERT_EQ((uint32_t)0, strands);
    UASSERT_EQ((uint32_t)0, watchers);
    UASSERT_EQ((uint32_t)0, wakes);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void
test_realm_suite(void)
{
    printf("test_realm\n");
    utest_run("realm_round_trip",                                   realm_round_trip);
    utest_run("realm_global_idempotent",                            realm_global_idempotent);
    utest_run("realm_id_unique_monotonic",                          realm_id_unique_monotonic);
    utest_run("realm_destroy_cascades_via_tag_stop",                realm_destroy_cascades_via_tag_stop);
    utest_run("realm_flags_default_zero",                           realm_flags_default_zero);
    utest_run("realm_global_sets_global_flag",                      realm_global_sets_global_flag);
    utest_run("realm_user_data_round_trip",                         realm_user_data_round_trip);
    utest_run("realm_reflective_nil_at_m3",                         realm_reflective_nil_at_m3);
    utest_run("realm_linked_list_invariants_after_create_destroy_create",
              realm_linked_list_invariants_after_create_destroy_create);
    utest_run("realm_destroy_unlinks_middle",                       realm_destroy_unlinks_middle);
    utest_run("realm_destroy_null_safe",                            realm_destroy_null_safe);
    utest_run("realm_namespace_set_get_round_trip",                 realm_namespace_set_get_round_trip);
    utest_run("realm_namespace_grow_past_initial_cap",              realm_namespace_grow_past_initial_cap);
    utest_run("realm_walk_roots_invokes_callback_per_namespace_entry",
              realm_walk_roots_invokes_callback_per_namespace_entry);
    utest_run("realm_create_oom_returns_null",                      realm_create_oom_returns_null);
    utest_run("realm_destroy_does_not_free_strand_on_ready_queue (T69)",
              realm_destroy_does_not_free_strand_on_ready_queue);
    utest_run("realm_has_live_work_null_out_params",
              realm_has_live_work_null_out_params);
}
