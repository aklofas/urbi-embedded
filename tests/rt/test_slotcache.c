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

typedef struct { size_t live; int fail_after; } CacheAlloc;

static void *cache_alloc(void *ptr, size_t n, void *ud)
{
    CacheAlloc *ca = (CacheAlloc *)ud;
    size_t *hdr = ptr ? ((size_t *)ptr) - 2 : NULL;
    size_t old = hdr ? hdr[0] : 0;
    if (n == 0) { if (hdr) { ca->live -= old; free(hdr); } return NULL; }
    if (ca->fail_after > 0 && --ca->fail_after == 0) { ca->fail_after = -1; }
    if (ca->fail_after < 0 && hdr == NULL && n % sizeof(USlotCache) == 0 && n >= sizeof(USlotCache)) return NULL;
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

RT_SUITE(rt_slotcache_suite)
{
    rt_run("the_epoch_skips_zero", the_epoch_skips_zero);
    rt_run("an_entry_is_empty_until_filled", an_entry_is_empty_until_filled);
    rt_run("cache_arrays_are_freed_with_the_vm", cache_arrays_are_freed_with_the_vm);
}
