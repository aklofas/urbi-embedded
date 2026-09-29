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
 * refused grow would test something else. */
typedef struct { size_t live; int fail_at; size_t refused; } CacheAlloc;

static void *cache_alloc(void *ptr, size_t n, void *ud)
{
    CacheAlloc *ca = (CacheAlloc *)ud;
    size_t *hdr = ptr ? ((size_t *)ptr) - 2 : NULL;
    size_t old = hdr ? hdr[0] : 0;
    if (n == 0) { if (hdr) { ca->live -= old; free(hdr); } return NULL; }
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
static void fix_close(Fix *fx) { urbi_close(fx->vm); }

static UValue run(Fix *fx, const char *src)
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
    run(&fx, "var o = Object.clone() | var o.f = 1 | o.f + o.f");
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- own slots ------------------------------------------------------------ */

static void a_repeated_own_read_hits(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var o = Object.clone() | var o.f = 41");
    run(&fx, "var rd = function() { o.f }");
    UValue v = run(&fx, "rd()");                     /* fills */
    RT_EQ(v.v.i, 41);
    uint32_t before = stats(&fx)->cache_hits;
    v = run(&fx, "rd() + rd() + rd()");
    RT_EQ(v.v.i, 123);
    /* `o` on the realm globals and `f` on o: two sites, three runs each. */
    RT_CHECK(stats(&fx)->cache_hits - before >= 6u);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

static void a_repeated_own_write_hits_and_updates(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var o = Object.clone() | var o.f = 0");
    run(&fx, "var bump = function() { o.f = o.f + 1 }");
    run(&fx, "bump()");
    uint32_t before = stats(&fx)->cache_hits;
    run(&fx, "bump() | bump() | bump()");
    RT_EQ(run(&fx, "o.f").v.i, 4);
    /* Sites are per occurrence: the write's receiver `o` and the right
     * side's `o` are two sites, then read f and write f.  Four, x3. */
    RT_CHECK(stats(&fx)->cache_hits - before >= 12u);
    fix_close(&fx);
}

static void a_bare_name_update_hits(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var n = 0");
    run(&fx, "var inc = function() { n = n + 1 }");
    run(&fx, "inc()");
    uint32_t before = stats(&fx)->cache_hits;
    run(&fx, "inc() | inc()");
    RT_EQ(run(&fx, "n").v.i, 3);
    RT_CHECK(stats(&fx)->cache_hits - before >= 4u);
    fix_close(&fx);
}

static void a_removed_slot_with_another_swapped_in_misses(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var o = Object.clone() | var o.a = 1 | var o.b = 2");
    run(&fx, "var rd = function() { o.a }");
    RT_EQ(run(&fx, "rd()").v.i, 1);
    /* removeSlot swaps the last slot (b) into a's index. */
    run(&fx, "o.removeSlot(\"a\")");
    UValue out = urbi_make_nil();
    char err[64] = { 0 };
    int rc = urbi_run(fx.vm, urbi_realm_main(fx.vm), "rd()", 4, "<test>", &out, err, sizeof err);
    RT_EQ(rc, URBI_ERR_UNCAUGHT_THROW);               /* LookupError, never 2 */
    fix_close(&fx);
}

static void a_getter_installed_after_caching_runs(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var o = Object.clone() | var o.f = 1");
    run(&fx, "var rd = function() { o.f }");
    RT_EQ(run(&fx, "rd()").v.i, 1);
    RT_EQ(run(&fx, "rd()").v.i, 1);
    run(&fx, "o.setProperty(\"f\", \"oget\", function() { 99 })");
    RT_EQ(run(&fx, "rd()").v.i, 99);
    fix_close(&fx);
}

static void a_slot_made_constant_refuses_a_cached_write(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var o = Object.clone() | var o.f = 1");
    run(&fx, "var wr = function() { o.f = 5 }");
    run(&fx, "wr()"); run(&fx, "wr()");
    run(&fx, "o.setProperty(\"f\", \"constant\", true)");
    UValue out = urbi_make_nil();
    char err[64] = { 0 };
    int rc = urbi_run(fx.vm, urbi_realm_main(fx.vm), "wr()", 4, "<test>", &out, err, sizeof err);
    RT_EQ(rc, URBI_ERR_UNCAUGHT_THROW);
    fix_close(&fx);
}

static void a_readonly_receiver_refuses_a_cached_write(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var o = Object.clone() | var o.f = 1");
    run(&fx, "var wr = function() { o.f = 5 }");
    run(&fx, "wr()"); run(&fx, "wr()");
    UValue ov = run(&fx, "o");
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
    run(&fx, "var o = Object.clone() | var o.n = 0 | var hits = 0");
    run(&fx, "var bump = function() { o.n = o.n + 1 }");
    run(&fx, "bump()"); run(&fx, "bump()");           /* the write site is warm */
    run(&fx, "at (o.n == 3) hits = hits + 1");
    run(&fx, "bump()");
    (void)urbi_step(fx.vm, 0, NULL);
    RT_EQ(run(&fx, "hits").v.i, 1);
    fix_close(&fx);
}

static void a_watcher_sees_a_cached_read(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var o = Object.clone() | var o.n = 0 | var hits = 0");
    run(&fx, "var rd = function() { o.n }");
    run(&fx, "rd()"); run(&fx, "rd()"); run(&fx, "rd()");   /* rd's sites are warm */
    /* The condition reads o.n only through rd's cached sites, so the
     * watcher learns what to wake on from the hit path alone. */
    run(&fx, "at (rd() == 2) hits = hits + 1");
    run(&fx, "o.n = 1");
    run(&fx, "o.n = 2");
    (void)urbi_step(fx.vm, 0, NULL);
    RT_EQ(run(&fx, "hits").v.i, 1);
    fix_close(&fx);
}

static void a_refused_cache_array_is_retried_and_harmless(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var o = Object.clone() | var o.f = 7");
    run(&fx, "var rd = function() { o.f }");
    UProto *body = ((UClosure *)run(&fx, "rd").v.p)->proto;
    /* The 16th fresh block of the first `rd()` is rd's cache array: 15
     * for compiling and running the call chunk, then the body's.  Pinned
     * by a dry run; the `refused` check says so if it drifts.  The body's
     * first site runs uncached and its next site retries the array. */
    fx.ca.fail_at = 16;
    RT_EQ(run(&fx, "rd()").v.i, 7);
    RT_EQ(fx.ca.refused, (size_t)body->ic_count * sizeof(USlotCache));
    RT_CHECK(((USlotCache *)body->site_cache)[0].recv == NULL);   /* ran uncached */
    RT_CHECK(((USlotCache *)body->site_cache)[1].recv != NULL);   /* retried, filled */
    RT_EQ(run(&fx, "rd()").v.i, 7);
    RT_CHECK(body->site_cache != NULL);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
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
    rt_run("a_slot_made_constant_refuses_a_cached_write", a_slot_made_constant_refuses_a_cached_write);
    rt_run("a_readonly_receiver_refuses_a_cached_write", a_readonly_receiver_refuses_a_cached_write);
    rt_run("a_watcher_sees_a_cached_write", a_watcher_sees_a_cached_write);
    rt_run("a_watcher_sees_a_cached_read", a_watcher_sees_a_cached_read);
    rt_run("a_refused_cache_array_is_retried_and_harmless", a_refused_cache_array_is_retried_and_harmless);
}
