/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UWatcher layout additions from spec #1 §4.1.
 *
 * Checks:
 *   - realm and body_strand fields are present and pointer-sized.
 *   - URBI_WATCHER_PENDING_REFIRE == 0x08u.
 *   - sizeof(UWatcher) >= 216 at the default preset (READSET_MAX=16). */

#include "utest.h"
#include "watcher/uwatcher.h"

static void uwatcher_spec1_fields(void)
{
    UWatcher w = {0};
    /* Compile-time field presence checks: assignment must compile. */
    w.realm       = NULL;
    w.body_strand = NULL;
    /* Flag assignment must compile. */
    w.flags |= URBI_WATCHER_PENDING_REFIRE;
    /* Flag value per spec #1 §3.2. */
    UASSERT_EQ((unsigned)URBI_WATCHER_PENDING_REFIRE, 0x08u);
    /* spec #1 §4.1: two pointer fields added → sizeof grows by 16 B.
     * Pre-#1 default layout was 200 B + 8 B padding = 208 B; post-#1 ≥ 216 B. */
#if URBI_WATCHER_READSET_MAX >= 16
    UASSERT(sizeof(UWatcher) >= 216u);
#endif
    (void)w;
}

static void uwatcher_pending_refire_does_not_collide(void)
{
    /* PENDING_REFIRE must not overlap with any existing flag bit. */
    unsigned existing = URBI_WATCHER_ACTIVE
                      | URBI_WATCHER_PENDING_UNREGISTER
                      | URBI_WATCHER_FIRED_DURING_EVAL;
    UASSERT((existing & URBI_WATCHER_PENDING_REFIRE) == 0u);
}

static void uwatcher_spec2_fields(void)
{
    UWatcher w = {0};
    /* Compile-time field presence check: assignment must compile. */
    w.waiter_strand = NULL;
    /* Flag assignment must compile. */
    w.flags |= URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE;
    /* Flag value per spec #2 §5.1. */
    UASSERT_EQ((unsigned)URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE, 0x10u);
    /* Mode constant per spec #2 §5.1. */
    UASSERT_EQ((int)UWATCHER_WAITUNTIL, 4);
    /* spec #2 §5.1: one pointer field added → sizeof grows by 8 B.
     * Pre-#2 default layout was 216 B; post-#2 ≥ 224 B. */
#if URBI_WATCHER_READSET_MAX >= 16
    UASSERT(sizeof(UWatcher) >= 224u);
#endif
    (void)w;
}

static void uwatcher_spec3_fields(void)
{
    UWatcher w = {0};
    /* Compile-time field presence checks: assignment must compile. */
    w.next_in_event = NULL;
    w.event         = NULL;
    /* Mode constants per spec #3 §3.2. */
    UASSERT_EQ((int)UWATCHER_AT_EVENT,      5);
    UASSERT_EQ((int)UWATCHER_AT_EVENT_SYNC, 6);
    /* spec #3 §3.2: two pointer fields added → sizeof grows by 16 B.
     * Pre-#3 default layout was 224 B; post-#3 ≥ 240 B. */
#if URBI_WATCHER_READSET_MAX >= 16
    UASSERT(sizeof(UWatcher) >= 240u);
#endif
    (void)w;
}

void
test_uwatcher_layout_suite(void)
{
    printf("test_uwatcher_layout\n");
    utest_run("uwatcher_spec1_fields",
              uwatcher_spec1_fields);
    utest_run("uwatcher_pending_refire_does_not_collide",
              uwatcher_pending_refire_does_not_collide);
    utest_run("uwatcher_spec2_fields",
              uwatcher_spec2_fields);
    utest_run("uwatcher_spec3_fields",
              uwatcher_spec3_fields);
}
