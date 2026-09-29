/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/rt/test_slotcache.c — the per-site slot cache: when it fills,
 * when it hits, and every way an entry stops being true.
 *
 * Every case asserts on cache_hits / cache_fills as well as on the value.
 * A cache that never engages passes every value assertion, so the
 * counters are what make these tests about the cache at all. */

#include <stdlib.h>
#include <string.h>

#include "rtest.h"
#include "urbi/urbi.h"
#include "rt/uexec.h"
#include "rt/uobj.h"
#include "rt/uslotcache.h"

/* fail_at > 0 refuses exactly the fail_at'th fresh block from here on,
 * records its size in `refused`, then disarms.  Fresh blocks only: a
 * refused grow would test something else.
 *
 * `hold` names a block whose free is kept back in `held` instead; with
 * `give_back` set, the next fresh block of the same size is that one, so
 * a test can put a new object at a freed object's address. */
typedef struct {
    size_t live; int fail_at; size_t refused;
    void *hold, *held; bool give_back;
} CacheAlloc;

static void *cache_alloc(void *ptr, size_t n, void *ud)
{
    CacheAlloc *ca = (CacheAlloc *)ud;
    size_t *hdr = ptr ? ((size_t *)ptr) - 2 : NULL;
    size_t old = hdr ? hdr[0] : 0;
    if (n == 0 && ptr != NULL && ptr == ca->hold) {
        ca->live -= old; ca->held = ptr; ca->hold = NULL; return NULL;
    }
    if (n == 0) { if (hdr) { ca->live -= old; free(hdr); } return NULL; }
    if (hdr == NULL && ca->give_back && ca->held && ((size_t *)ca->held)[-2] == n) {
        void *p = ca->held;
        ca->held = NULL; ca->give_back = false; ca->live += n;
        return p;
    }
    if (hdr == NULL && ca->fail_at > 0 && --ca->fail_at == 0) { ca->refused = n; return NULL; }
    size_t *nh = (size_t *)realloc(hdr, n + 2 * sizeof(size_t));
    if (!nh) return NULL;
    nh[0] = n;
    ca->live += n - old;
    return (void *)(nh + 2);
}

typedef struct { UVM *vm; CacheAlloc ca; } Fix;

static void fix_open(Fix *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->vm = urbi_open(cache_alloc, &fx->ca, NULL);
}
static void fix_close(Fix *fx)
{
    urbi_close(fx->vm);
    if (fx->ca.held) free(((size_t *)fx->ca.held) - 2);
}

static UValue run_rc(Fix *fx, const char *src, int *rc_out)
{
    UValue out = urbi_make_nil();
    char err[256] = { 0 };
    int rc = urbi_run(fx->vm, urbi_realm_main(fx->vm), src, strlen(src), "<test>",
                      &out, err, sizeof err);
    if (rc != URBI_OK) {
        UErrorInfo info;
        urbi_last_error(fx->vm, &info);
        printf("    run(%s) rc=%d err=%s last=%s\n", src, rc, err,
               info.message ? info.message : "");
    }
    *rc_out = rc;
    return out;
}

/* Every call that is not meant to throw goes through here, so a setup
 * statement that fails to compile or throws fails the test instead of
 * leaving it to assert on a world that was never built. */
static UValue run_ok(Fix *fx, const char *src)
{
    int rc;
    UValue out = run_rc(fx, src, &rc);
    RT_EQ(rc, URBI_OK);
    return out;
}

static UObjStats *stats(Fix *fx) { return uvm_objstats(fx->vm); }

/* --- storage -------------------------------------------------------------- */

static void the_epoch_skips_zero(void)
{
    Fix fx; fix_open(&fx);
    stats(&fx)->slot_epoch = 0xFFFFFFFFu;
    uobj_epoch_bump(fx.vm);
    RT_EQ(stats(&fx)->slot_epoch, 1u);
    fix_close(&fx);
}

static void an_entry_is_empty_until_filled(void)
{
    UObjStats st; memset(&st, 0, sizeof st);
    USlotCache e; memset(&e, 0, sizeof e);
    UObject o; memset(&o, 0, sizeof o);
    RT_CHECK(!uslotcache_hit(&st, &e, &o, NULL));
}

static void cache_arrays_are_freed_with_the_vm(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone() | var o.f = 1 | o.f + o.f");
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- own slots ------------------------------------------------------------ */

static void a_repeated_own_read_hits(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone() | var o.f = 41");
    run_ok(&fx, "var rd = function() { o.f }");
    UValue v = run_ok(&fx, "rd()");                     /* fills */
    RT_EQ(v.v.i, 41);
    uint32_t before = stats(&fx)->cache_hits;
    v = run_ok(&fx, "rd() + rd() + rd()");
    RT_EQ(v.v.i, 123);
    /* `o` on the realm globals and `f` on o: two sites, three runs each. */
    RT_CHECK(stats(&fx)->cache_hits - before >= 6u);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

static void a_repeated_own_write_hits_and_updates(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone() | var o.f = 0");
    run_ok(&fx, "var bump = function() { o.f = o.f + 1 }");
    run_ok(&fx, "bump()");
    uint32_t before = stats(&fx)->cache_hits;
    run_ok(&fx, "bump() | bump() | bump()");
    RT_EQ(run_ok(&fx, "o.f").v.i, 4);
    /* Sites are per occurrence: the write's receiver `o` and the right
     * side's `o` are two sites, then read f and write f.  Four, x3. */
    RT_CHECK(stats(&fx)->cache_hits - before >= 12u);
    fix_close(&fx);
}

static void a_bare_name_update_hits(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var n = 0");
    run_ok(&fx, "var inc = function() { n = n + 1 }");
    run_ok(&fx, "inc()");
    uint32_t before = stats(&fx)->cache_hits;
    run_ok(&fx, "inc() | inc()");
    RT_EQ(run_ok(&fx, "n").v.i, 3);
    RT_CHECK(stats(&fx)->cache_hits - before >= 4u);
    fix_close(&fx);
}

static void a_removed_slot_with_another_swapped_in_misses(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone() | var o.a = 1 | var o.b = 2");
    run_ok(&fx, "var rd = function() { o.a }");
    RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    /* removeSlot swaps the last slot (b) into a's index. */
    run_ok(&fx, "o.removeSlot(\"a\")");
    UValue out = urbi_make_nil();
    char err[64] = { 0 };
    int rc = urbi_run(fx.vm, urbi_realm_main(fx.vm), "rd()", 4, "<test>", &out, err, sizeof err);
    RT_EQ(rc, URBI_ERR_UNCAUGHT_THROW);               /* LookupError, never 2 */
    fix_close(&fx);
}

static void a_getter_installed_after_caching_runs(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone() | var o.f = 1");
    run_ok(&fx, "var rd = function() { o.f }");
    RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    run_ok(&fx, "o.setProperty(\"f\", \"oget\", function() { 99 })");
    RT_EQ(run_ok(&fx, "rd()").v.i, 99);
    fix_close(&fx);
}

/* A slot carrying an accessor holds a property cell, not its value.  A
 * setter-only slot reads back the stored value, never the cell. */
static void a_setter_installed_after_caching_a_read_reads_the_value(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone() | var o.f = 1 | var St = Object.clone() | var St.n = 0");
    run_ok(&fx, "var rd = function() { o.f }");
    RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    run_ok(&fx, "o.setProperty(\"f\", \"oset\", function(v) { St.n = St.n + 1 })");
    UValue v = run_ok(&fx, "rd()");
    RT_EQ(v.kind, UV_INT);
    RT_EQ(v.v.i, 1);
    fix_close(&fx);
}

static void a_setter_installed_after_caching_a_write_runs(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone() | var o.f = 0 | var St = Object.clone() | var St.n = 0");
    run_ok(&fx, "var wr = function() { o.f = 5 }");
    run_ok(&fx, "wr()"); run_ok(&fx, "wr()");
    run_ok(&fx, "o.setProperty(\"f\", \"oset\", function(v) { St.n = St.n + v })");
    run_ok(&fx, "wr()");
    RT_EQ(run_ok(&fx, "St.n").v.i, 5);
    fix_close(&fx);
}

/* A getter-only slot has no setter to call, so a write stores into the
 * property cell; writing over the cell itself would leave the getter bit
 * pointing at an integer. */
static void a_getter_installed_after_caching_a_write_keeps_its_cell(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone() | var o.f = 0");
    run_ok(&fx, "var wr = function() { o.f = 5 }");
    run_ok(&fx, "wr()"); run_ok(&fx, "wr()");
    run_ok(&fx, "o.setProperty(\"f\", \"oget\", function() { 99 })");
    run_ok(&fx, "wr()");
    UObject *o = (UObject *)run_ok(&fx, "o").v.p;
    int idx = uobj_find_local(o, usym_cstr(fx.vm, "f"));
    RT_CHECK(idx >= 0);
    RT_EQ(o->values[idx].kind, UV_CELL);
    RT_EQ(run_ok(&fx, "o.f").v.i, 99);
    fix_close(&fx);
}

static void a_slot_made_constant_refuses_a_cached_write(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone() | var o.f = 1");
    run_ok(&fx, "var wr = function() { o.f = 5 }");
    run_ok(&fx, "wr()"); run_ok(&fx, "wr()");
    run_ok(&fx, "o.setProperty(\"f\", \"constant\", true)");
    UValue out = urbi_make_nil();
    char err[64] = { 0 };
    int rc = urbi_run(fx.vm, urbi_realm_main(fx.vm), "wr()", 4, "<test>", &out, err, sizeof err);
    RT_EQ(rc, URBI_ERR_UNCAUGHT_THROW);
    fix_close(&fx);
}

static void a_readonly_receiver_refuses_a_cached_write(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone() | var o.f = 1");
    run_ok(&fx, "var wr = function() { o.f = 5 }");
    run_ok(&fx, "wr()"); run_ok(&fx, "wr()");
    UValue ov = run_ok(&fx, "o");
    ((UObject *)ov.v.p)->cell.flags |= UOBJ_F_READONLY;
    UValue out = urbi_make_nil();
    char err[64] = { 0 };
    int rc = urbi_run(fx.vm, urbi_realm_main(fx.vm), "wr()", 4, "<test>", &out, err, sizeof err);
    RT_EQ(rc, URBI_ERR_UNCAUGHT_THROW);
    ((UObject *)ov.v.p)->cell.flags &= (uint16_t)~UOBJ_F_READONLY;
    fix_close(&fx);
}

static void a_watcher_sees_a_cached_write(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone() | var o.n = 0 | var hits = 0");
    run_ok(&fx, "var bump = function() { o.n = o.n + 1 }");
    run_ok(&fx, "bump()"); run_ok(&fx, "bump()");           /* the write site is warm */
    run_ok(&fx, "at (o.n == 3) hits = hits + 1");
    run_ok(&fx, "bump()");
    (void)urbi_step(fx.vm, 0, NULL);
    RT_EQ(run_ok(&fx, "hits").v.i, 1);
    fix_close(&fx);
}

static void a_watcher_sees_a_cached_read(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone() | var o.n = 0 | var hits = 0");
    run_ok(&fx, "var rd = function() { o.n }");
    run_ok(&fx, "rd()"); run_ok(&fx, "rd()"); run_ok(&fx, "rd()");   /* rd's sites are warm */
    /* The condition reads o.n only through rd's cached sites, so the
     * watcher learns what to wake on from the hit path alone. */
    run_ok(&fx, "at (rd() == 2) hits = hits + 1");
    run_ok(&fx, "o.n = 1");
    run_ok(&fx, "o.n = 2");
    (void)urbi_step(fx.vm, 0, NULL);
    RT_EQ(run_ok(&fx, "hits").v.i, 1);
    fix_close(&fx);
}

static void a_refused_cache_array_is_retried_and_harmless(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone() | var o.f = 7");
    run_ok(&fx, "var rd = function() { o.f }");
    UProto *body = ((UClosure *)run_ok(&fx, "rd").v.p)->proto;
    /* The 16th fresh block of the first `rd()` is rd's cache array: 15
     * for compiling and running the call chunk, then the body's.  Pinned
     * by a dry run; the `refused` check says so if it drifts.  The body's
     * first site runs uncached and its next site retries the array. */
    fx.ca.fail_at = 16;
    RT_EQ(run_ok(&fx, "rd()").v.i, 7);
    RT_EQ(fx.ca.refused, (size_t)body->ic_count * sizeof(USlotCache));
    RT_CHECK(((USlotCache *)body->site_cache)[0].recv == NULL);   /* ran uncached */
    RT_CHECK(((USlotCache *)body->site_cache)[1].recv != NULL);   /* retried, filled */
    RT_EQ(run_ok(&fx, "rd()").v.i, 7);
    RT_CHECK(body->site_cache != NULL);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- inherited slots ------------------------------------------------------ */

/* P <- M <- o, `v` on P, read through one site. */
static void chain(Fix *fx)
{
    run_ok(fx, "var P = Object.clone() | var P.v = 1");
    run_ok(fx, "var M = P.clone() | var o = M.clone()");
    run_ok(fx, "var rd = function() { o.v }");
}

static void a_repeated_inherited_read_hits(void)
{
    Fix fx; fix_open(&fx); chain(&fx);
    RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    uint32_t before = stats(&fx)->cache_hits;
    RT_EQ(run_ok(&fx, "rd() + rd()").v.i, 2);
    /* Under URBI_GC_STRESS every allocation collects, and every
     * collection bumps the epoch: the count only holds in paced mode. */
#ifndef URBI_GC_STRESS
    RT_CHECK(stats(&fx)->cache_hits - before >= 4u);
#else
    (void)before;
#endif
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

static void a_shadow_on_the_receiver_wins(void)
{
    Fix fx; fix_open(&fx); chain(&fx);
    run_ok(&fx, "rd()"); run_ok(&fx, "rd()");
    run_ok(&fx, "var o.v = 3");
    RT_EQ(run_ok(&fx, "rd()").v.i, 3);
    fix_close(&fx);
}

static void a_shadow_on_an_intermediate_proto_wins(void)
{
    Fix fx; fix_open(&fx); chain(&fx);
    run_ok(&fx, "rd()"); run_ok(&fx, "rd()");
    run_ok(&fx, "var M.v = 2");
    RT_EQ(run_ok(&fx, "rd()").v.i, 2);
    fix_close(&fx);
}

static void remove_slot_native_invalidates(void)
{
    Fix fx; fix_open(&fx); chain(&fx);
    run_ok(&fx, "var M.v = 2");
    run_ok(&fx, "rd()"); run_ok(&fx, "rd()");               /* cached on M */
    run_ok(&fx, "M.removeSlot(\"v\")");
    RT_EQ(run_ok(&fx, "rd()").v.i, 1);                   /* falls through to P */
    run_ok(&fx, "P.removeSlot(\"v\")");
    UValue out = urbi_make_nil();
    char err[64] = { 0 };
    int rc = urbi_run(fx.vm, urbi_realm_main(fx.vm), "rd()", 4, "<test>", &out, err, sizeof err);
    RT_EQ(rc, URBI_ERR_UNCAUGHT_THROW);
    fix_close(&fx);
}

static void a_proto_list_change_re_resolves(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var o = Object.clone()");
    run_ok(&fx, "var Q = Object.clone() | var Q.w = 7");
    run_ok(&fx, "var R = Object.clone() | var R.w = 8");
    run_ok(&fx, "var rd = function() { o.w }");
    run_ok(&fx, "o.addProto(Q)");
    RT_EQ(run_ok(&fx, "rd()").v.i, 7);
    RT_EQ(run_ok(&fx, "rd()").v.i, 7);
    run_ok(&fx, "o.addProto(R)");                        /* prepends: R wins */
    RT_EQ(run_ok(&fx, "rd()").v.i, 8);
    run_ok(&fx, "o.removeProto(R)");
    RT_EQ(run_ok(&fx, "rd()").v.i, 7);
    fix_close(&fx);
}

static void a_value_write_on_the_owner_does_not_bump(void)
{
    Fix fx; fix_open(&fx); chain(&fx);
    run_ok(&fx, "rd()");
    uint32_t epoch = stats(&fx)->slot_epoch;
    run_ok(&fx, "P.v = 50");
    /* Under URBI_GC_STRESS every allocation collects, and every
     * collection bumps the epoch: this only holds in paced mode. */
#ifndef URBI_GC_STRESS
    RT_EQ(stats(&fx)->slot_epoch, epoch);
#else
    (void)epoch;
#endif
    RT_EQ(run_ok(&fx, "rd()").v.i, 50);
    fix_close(&fx);
}

static void building_fresh_objects_does_not_bump(void)
{
    Fix fx; fix_open(&fx); chain(&fx);
    run_ok(&fx, "rd()");
    uint32_t epoch = stats(&fx)->slot_epoch;
    UObject *a = uobj_new(fx.vm, NULL);
    (void)uobj_set_local(fx.vm, a, usym_cstr(fx.vm, "k"), uv_int(1), 0);
    /* Under URBI_GC_STRESS every allocation collects, and every
     * collection bumps the epoch: this only holds in paced mode. */
#ifndef URBI_GC_STRESS
    RT_EQ(stats(&fx)->slot_epoch, epoch);
#else
    (void)epoch;
#endif
    fix_close(&fx);
}

static void a_collection_bumps_the_epoch(void)
{
    Fix fx; fix_open(&fx); chain(&fx);
    run_ok(&fx, "rd()");
    uint32_t epoch = stats(&fx)->slot_epoch;
    ugc_collect(fx.vm);
    RT_CHECK(stats(&fx)->slot_epoch != epoch);
    RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    fix_close(&fx);
}

/* Entries are weak, so a warm entry can outlive its receiver and meet a
 * new object built at the same address.  The own entry must re-check the
 * live object (b has `j` where a had `k`), and the inherited entry must
 * be retired by the collection that freed a (b inherits from D, not C). */
static void a_receiver_freed_and_its_address_reused_misses(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var C = Object.clone() | var C.m = 10 | var D = Object.clone() | var D.m = 30");
    run_ok(&fx, "var a = C.clone() | var a.k = 1 | var b = nil");
    run_ok(&fx, "var rd = function(x) { x.k } | var rdi = function(x) { x.m }");
    RT_EQ(run_ok(&fx, "rd(a) + rdi(a)").v.i, 11);
    RT_EQ(run_ok(&fx, "rd(a) + rdi(a)").v.i, 11);          /* both sites hold a */
    UObject *a = (UObject *)run_ok(&fx, "a").v.p;
    UObject *D = (UObject *)run_ok(&fx, "D").v.p;
    fx.ca.hold = a;
    run_ok(&fx, "a = nil");
    ugc_collect(fx.vm);
    RT_CHECK(fx.ca.held == a);                              /* a was swept */
    fx.ca.give_back = true;
    UObject *b = uobj_new(fx.vm, D);
    RT_CHECK(b == a);
    RT_EQ(urbi_global_set(fx.vm, urbi_realm_main(fx.vm), "b", uv_obj(b)), URBI_OK);
    RT_CHECK(uobj_set_local(fx.vm, b, usym_cstr(fx.vm, "j"), uv_int(7), 0) == 0);
    RT_CHECK(uobj_set_local(fx.vm, b, usym_cstr(fx.vm, "k"), uv_int(2), 0) == 1);
    RT_EQ(run_ok(&fx, "rd(b)").v.i, 2);
    RT_EQ(run_ok(&fx, "rdi(b)").v.i, 30);
    fix_close(&fx);
}

static void a_host_write_is_seen_through_a_cached_site(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var g = 1");
    run_ok(&fx, "var rd = function() { g }");
    run_ok(&fx, "rd()"); run_ok(&fx, "rd()");
    RT_EQ(urbi_global_set(fx.vm, urbi_realm_main(fx.vm), "g", urbi_make_int(9)), URBI_OK);
    RT_EQ(run_ok(&fx, "rd()").v.i, 9);
    fix_close(&fx);
}

static void a_site_with_alternating_receivers_stays_correct(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var C = Object.clone() | var C.k = 1");
    run_ok(&fx, "var a = C.clone() | var b = C.clone() | var b.k = 2");
    run_ok(&fx, "var rd = function(x) { x.k }");
    RT_EQ(run_ok(&fx, "rd(a) * 1000 + rd(b) * 100 + rd(a) * 10 + rd(b)").v.i, 1212);
    fix_close(&fx);
}

static void an_overflowed_walk_is_not_cached(void)
{
    Fix fx; fix_open(&fx);
    /* 70 protos on one object: wider than the 64-entry walk stack.  Added
     * from C, because from script `o.addProto` itself stops resolving
     * once the list outgrows the stack.  `o` is rooted as a global, and
     * uobj_add_proto's raw allocation never collects, so each new proto
     * is reachable before anything can sweep it. */
    run_ok(&fx, "var o = Object.clone()");
    UObject *o = (UObject *)run_ok(&fx, "o").v.p;
    for (int k = 0; k < 70; k++) {
        UObject *p = uobj_new(fx.vm, NULL);
        RT_CHECK(p != NULL && uobj_add_proto(fx.vm, o, p) == 0);
    }
    RT_EQ(o->nprotos, 71);        /* 70 in front of Object */
    run_ok(&fx, "var rd = function() { o.nothingHere }");
    UValue out = urbi_make_nil();
    char err[256] = { 0 };
    (void)urbi_run(fx.vm, urbi_realm_main(fx.vm), "rd()", 4, "<test>", &out, err, sizeof err);
    /* The first call warmed rd's site for `o`.  On the second, the fresh
     * call chunk's site for `rd` may fill; the failed one must not. */
    uint32_t fills = stats(&fx)->cache_fills;
    int rc = urbi_run(fx.vm, urbi_realm_main(fx.vm), "rd()", 4, "<test>", &out, err, sizeof err);
    RT_EQ(rc, URBI_ERR_UNCAUGHT_THROW);
    UErrorInfo info;
    urbi_last_error(fx.vm, &info);
    RT_CHECK(info.message != NULL && strstr(info.message, "exceeds the 64-entry") != NULL);
    RT_CHECK(stats(&fx)->cache_fills - fills <= 1u);
    fix_close(&fx);
}

/* A value write on the owner does not bump the epoch, so the condition's
 * read stays a cache hit: the watcher has to learn about the owner from
 * the hit path itself. */
static void a_watcher_sees_a_write_to_an_inherited_cached_slot(void)
{
    Fix fx; fix_open(&fx); chain(&fx);
    run_ok(&fx, "var hits = 0");
    run_ok(&fx, "rd()"); run_ok(&fx, "rd()");               /* the site holds an inherited entry */
    run_ok(&fx, "at (rd() == 5) hits = hits + 1");
    run_ok(&fx, "P.v = 5");
    (void)urbi_step(fx.vm, 0, NULL);
    RT_EQ(run_ok(&fx, "hits").v.i, 1);
    fix_close(&fx);
}

/* The epoch is reset at a collection long before it could wrap back to a
 * value an idle entry still carries; the reset clears every entry. */
static void an_epoch_near_the_top_is_reset_and_entries_cleared(void)
{
    Fix fx; fix_open(&fx); chain(&fx);
    run_ok(&fx, "rd()"); run_ok(&fx, "rd()");
    UProto *body = ((UClosure *)run_ok(&fx, "rd").v.p)->proto;
    USlotCache *a = (USlotCache *)body->site_cache;
    RT_CHECK(a != NULL);
    bool inherited = false;
    for (uint16_t k = 0; k < body->ic_count; k++)
        if (a[k].recv != NULL && a[k].owner != a[k].recv) inherited = true;
    RT_CHECK(inherited);
    stats(&fx)->slot_epoch = 0x80000001u;
    ugc_collect(fx.vm);
    RT_EQ(stats(&fx)->slot_epoch, 1u);
    for (uint16_t k = 0; k < body->ic_count; k++) RT_CHECK(a[k].recv == NULL);
    uint32_t fills = stats(&fx)->cache_fills;
    RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    RT_CHECK(stats(&fx)->cache_fills > fills);
    fix_close(&fx);
}

/* --- proto-list changes on an intermediate object --------------------------
 *
 * r's protos are [o, Q] with `k` on Q, so a read of r.k passes over o.
 * Each case warms that inherited entry, then changes o's proto list along
 * exactly one path in uobj.c and reads again. */

static UObject *obj(Fix *fx, const char *name) { return (UObject *)run_ok(fx, name).v.p; }

static void two_protos_below_r(Fix *fx)
{
    run_ok(fx, "var o = Object.clone() | var Q = Object.clone() | var Q.k = 1");
    run_ok(fx, "var X = Object.clone() | var X.k = 2 | var W = Object.clone() | var W.k = 5");
    run_ok(fx, "var r = Object.clone() | r.setProtos([o, Q])");
    run_ok(fx, "var rd = function() { r.k }");
}

static void an_intermediate_gaining_its_first_proto_re_resolves(void)
{
    Fix fx; fix_open(&fx); two_protos_below_r(&fx);
    UObject *o = obj(&fx, "o");
    RT_EQ(uobj_set_protos(fx.vm, o, NULL, 0), 0);
    RT_EQ(run_ok(&fx, "rd()").v.i, 1); RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    RT_CHECK(o->cell.flags & UOBJ_F_CACHED);
    RT_EQ(uobj_add_proto(fx.vm, o, obj(&fx, "X")), 0);          /* 0 -> 1 */
    RT_EQ(o->nprotos, 1);
    RT_EQ(run_ok(&fx, "rd()").v.i, 2);
    fix_close(&fx);
}

static void an_intermediate_gaining_a_second_proto_re_resolves(void)
{
    Fix fx; fix_open(&fx); two_protos_below_r(&fx);
    UObject *o = obj(&fx, "o");
    RT_EQ(run_ok(&fx, "rd()").v.i, 1); RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    RT_EQ(o->nprotos, 1);
    RT_EQ(uobj_add_proto(fx.vm, o, obj(&fx, "X")), 0);          /* 1 -> 2 */
    RT_EQ(run_ok(&fx, "rd()").v.i, 2);
    fix_close(&fx);
}

static void an_intermediate_losing_its_only_proto_re_resolves(void)
{
    Fix fx; fix_open(&fx); two_protos_below_r(&fx);
    UObject *o = obj(&fx, "o"); UObject *w = obj(&fx, "W");
    RT_EQ(uobj_set_protos(fx.vm, o, &w, 1), 0);
    RT_EQ(run_ok(&fx, "rd()").v.i, 5); RT_EQ(run_ok(&fx, "rd()").v.i, 5);
    RT_EQ(uobj_remove_proto(fx.vm, o, w), 0);                   /* 1 -> 0 */
    RT_EQ(o->nprotos, 0);
    RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    fix_close(&fx);
}

static void an_intermediate_dropping_to_one_proto_re_resolves(void)
{
    Fix fx; fix_open(&fx); two_protos_below_r(&fx);
    UObject *o = obj(&fx, "o"); UObject *w = obj(&fx, "W");
    RT_EQ(uobj_add_proto(fx.vm, o, w), 0);
    RT_EQ(run_ok(&fx, "rd()").v.i, 5); RT_EQ(run_ok(&fx, "rd()").v.i, 5);
    RT_EQ(uobj_remove_proto(fx.vm, o, w), 0);                   /* 2 -> 1 */
    RT_EQ(o->nprotos, 1);
    RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    fix_close(&fx);
}

static void an_intermediate_given_one_proto_wholesale_re_resolves(void)
{
    Fix fx; fix_open(&fx); two_protos_below_r(&fx);
    UObject *o = obj(&fx, "o"); UObject *x = obj(&fx, "X");
    RT_EQ(run_ok(&fx, "rd()").v.i, 1); RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    RT_EQ(uobj_set_protos(fx.vm, o, &x, 1), 0);                 /* n <= 1 */
    RT_EQ(run_ok(&fx, "rd()").v.i, 2);
    fix_close(&fx);
}

static void an_intermediate_given_two_protos_wholesale_re_resolves(void)
{
    Fix fx; fix_open(&fx); two_protos_below_r(&fx);
    UObject *o = obj(&fx, "o");
    UObject *ps[2] = { obj(&fx, "X"), obj(&fx, "W") };
    RT_EQ(run_ok(&fx, "rd()").v.i, 1); RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    RT_EQ(uobj_set_protos(fx.vm, o, ps, 2), 0);                 /* n > 1 */
    RT_EQ(run_ok(&fx, "rd()").v.i, 2);
    fix_close(&fx);
}

static void setProtos_from_script_re_resolves(void)
{
    Fix fx; fix_open(&fx); two_protos_below_r(&fx);
    RT_EQ(run_ok(&fx, "rd()").v.i, 1); RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    run_ok(&fx, "o.setProtos([X])");
    RT_EQ(run_ok(&fx, "rd()").v.i, 2);
    fix_close(&fx);
}

/* P is reached only as the object the slot is found on, never passed
 * over, so only the fill's walk can flag it.  Removing `v` swaps `w` into
 * its index: an unflagged owner would leave the entry naming `w`. */
static void an_owner_found_directly_is_flagged(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var P = Object.clone() | var P.v = 1 | var P.w = 2");
    run_ok(&fx, "var o = Object.clone() | o.addProto(P)");
    run_ok(&fx, "var rd = function() { o.v }");
    UObject *P = obj(&fx, "P");
    RT_CHECK((P->cell.flags & UOBJ_F_CACHED) == 0);
    RT_EQ(run_ok(&fx, "rd()").v.i, 1); RT_EQ(run_ok(&fx, "rd()").v.i, 1);
    RT_CHECK(P->cell.flags & UOBJ_F_CACHED);
    RT_CHECK(uobj_remove_local(fx.vm, P, usym_cstr(fx.vm, "v")));
    UValue out = urbi_make_nil();
    char err[64] = { 0 };
    int rc = urbi_run(fx.vm, urbi_realm_main(fx.vm), "rd()", 4, "<test>", &out, err, sizeof err);
    RT_EQ(rc, URBI_ERR_UNCAUGHT_THROW);
    fix_close(&fx);
}

RT_SUITE(rt_slotcache_suite)
{
    rt_run("the_epoch_skips_zero", the_epoch_skips_zero);
    rt_run("an_entry_is_empty_until_filled", an_entry_is_empty_until_filled);
    rt_run("cache_arrays_are_freed_with_the_vm", cache_arrays_are_freed_with_the_vm);
    rt_run("a_repeated_own_read_hits", a_repeated_own_read_hits);
    rt_run("a_repeated_own_write_hits_and_updates", a_repeated_own_write_hits_and_updates);
    rt_run("a_bare_name_update_hits", a_bare_name_update_hits);
    rt_run("a_removed_slot_with_another_swapped_in_misses", a_removed_slot_with_another_swapped_in_misses);
    rt_run("a_getter_installed_after_caching_runs", a_getter_installed_after_caching_runs);
    rt_run("a_setter_installed_after_caching_a_read_reads_the_value", a_setter_installed_after_caching_a_read_reads_the_value);
    rt_run("a_setter_installed_after_caching_a_write_runs", a_setter_installed_after_caching_a_write_runs);
    rt_run("a_getter_installed_after_caching_a_write_keeps_its_cell", a_getter_installed_after_caching_a_write_keeps_its_cell);
    rt_run("a_slot_made_constant_refuses_a_cached_write", a_slot_made_constant_refuses_a_cached_write);
    rt_run("a_readonly_receiver_refuses_a_cached_write", a_readonly_receiver_refuses_a_cached_write);
    rt_run("a_watcher_sees_a_cached_write", a_watcher_sees_a_cached_write);
    rt_run("a_watcher_sees_a_cached_read", a_watcher_sees_a_cached_read);
    rt_run("a_refused_cache_array_is_retried_and_harmless", a_refused_cache_array_is_retried_and_harmless);
    rt_run("a_repeated_inherited_read_hits", a_repeated_inherited_read_hits);
    rt_run("a_shadow_on_the_receiver_wins", a_shadow_on_the_receiver_wins);
    rt_run("a_shadow_on_an_intermediate_proto_wins", a_shadow_on_an_intermediate_proto_wins);
    rt_run("remove_slot_native_invalidates", remove_slot_native_invalidates);
    rt_run("a_proto_list_change_re_resolves", a_proto_list_change_re_resolves);
    rt_run("a_value_write_on_the_owner_does_not_bump", a_value_write_on_the_owner_does_not_bump);
    rt_run("building_fresh_objects_does_not_bump", building_fresh_objects_does_not_bump);
    rt_run("a_collection_bumps_the_epoch", a_collection_bumps_the_epoch);
    rt_run("a_receiver_freed_and_its_address_reused_misses", a_receiver_freed_and_its_address_reused_misses);
    rt_run("a_host_write_is_seen_through_a_cached_site", a_host_write_is_seen_through_a_cached_site);
    rt_run("a_site_with_alternating_receivers_stays_correct", a_site_with_alternating_receivers_stays_correct);
    rt_run("an_overflowed_walk_is_not_cached", an_overflowed_walk_is_not_cached);
    rt_run("a_watcher_sees_a_write_to_an_inherited_cached_slot", a_watcher_sees_a_write_to_an_inherited_cached_slot);
    rt_run("an_epoch_near_the_top_is_reset_and_entries_cleared", an_epoch_near_the_top_is_reset_and_entries_cleared);
    rt_run("an_intermediate_gaining_its_first_proto_re_resolves", an_intermediate_gaining_its_first_proto_re_resolves);
    rt_run("an_intermediate_gaining_a_second_proto_re_resolves", an_intermediate_gaining_a_second_proto_re_resolves);
    rt_run("an_intermediate_losing_its_only_proto_re_resolves", an_intermediate_losing_its_only_proto_re_resolves);
    rt_run("an_intermediate_dropping_to_one_proto_re_resolves", an_intermediate_dropping_to_one_proto_re_resolves);
    rt_run("an_intermediate_given_one_proto_wholesale_re_resolves", an_intermediate_given_one_proto_wholesale_re_resolves);
    rt_run("an_intermediate_given_two_protos_wholesale_re_resolves", an_intermediate_given_two_protos_wholesale_re_resolves);
    rt_run("setProtos_from_script_re_resolves", setProtos_from_script_re_resolves);
    rt_run("an_owner_found_directly_is_flagged", an_owner_found_directly_is_flagged);
}
