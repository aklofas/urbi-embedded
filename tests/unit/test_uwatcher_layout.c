/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UWatcher layout additions from spec #1 §4.1.
 *
 * Checks:
 *   - realm and body_strand fields are present and pointer-sized.
 *   - pending_refire_count / max_refire_queue uint8_t fields are present
 *     and addressable (formerly a single URBI_WATCHER_PENDING_REFIRE flag
 *     bit; widened in v0.7.x to a bounded counter to fix the per-drain
 *     event-loss cap — see docs/superpowers/specs/.../m5-reactive.md §3.2).
 *   - sizeof(UWatcher) >= 216 at the default preset (READSET_MAX=16). */

#include "utest.h"
#include "watcher/uwatcher.h"

static void uwatcher_spec1_fields(void)
{
    UWatcher w = {0};
    /* Compile-time field presence checks: assignment must compile. */
    w.realm       = NULL;
    w.body_strand = NULL;
    /* Refire-queue field assignments must compile. */
    w.pending_refire_count = 0U;
    w.max_refire_queue     = URBI_WATCHER_REFIRE_QUEUE_DEFAULT;
    /* Default cap value per uwatcher.h URBI_WATCHER_REFIRE_QUEUE_DEFAULT comment. */
    UASSERT_EQ((unsigned)URBI_WATCHER_REFIRE_QUEUE_DEFAULT, 15U);
    /* spec #1 §4.1: two pointer fields added → sizeof grows by 16 B.
     * Pre-#1 default layout was 200 B + 8 B padding = 208 B; post-#1 ≥ 216 B. */
#if URBI_WATCHER_READSET_MAX >= 16
    UASSERT(sizeof(UWatcher) >= 216U);
#endif
    (void)w;
}

static void uwatcher_refire_counter_fields(void)
{
    /* pending_refire_count must be uint8_t (0..255).  max_refire_queue
     * caps it; verify counter cannot exceed cap by simulating the spawn-
     * gate increment (without invoking the runtime). */
    UWatcher w = {0};
    w.max_refire_queue = 4;
    for (int i = 0; i < 10; i++) {
        if (w.pending_refire_count < w.max_refire_queue) {
            w.pending_refire_count++;
        }
    }
    UASSERT_EQ((unsigned)w.pending_refire_count, 4U);
}

static void uwatcher_spec2_fields(void)
{
    UWatcher w = {0};
    /* Compile-time field presence check: assignment must compile. */
    w.waiter_strand = NULL;
    /* Flag assignment must compile. */
    w.flags |= URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE;
    /* Flag value per spec #2 §5.1. */
    UASSERT_EQ((unsigned)URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE, 0x10U);
    /* Mode constant per spec #2 §5.1. */
    UASSERT_EQ((int)UWATCHER_WAITUNTIL, 4);
    /* spec #2 §5.1: one pointer field added → sizeof grows by 8 B.
     * Pre-#2 default layout was 216 B; post-#2 ≥ 224 B. */
#if URBI_WATCHER_READSET_MAX >= 16
    UASSERT(sizeof(UWatcher) >= 224U);
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
    UASSERT(sizeof(UWatcher) >= 240U);
#endif
    (void)w;
}

void
test_uwatcher_layout_suite(void)
{
    printf("test_uwatcher_layout\n");
    utest_run("uwatcher_spec1_fields",
              uwatcher_spec1_fields);
    utest_run("uwatcher_refire_counter_fields",
              uwatcher_refire_counter_fields);
    utest_run("uwatcher_spec2_fields",
              uwatcher_spec2_fields);
    utest_run("uwatcher_spec3_fields",
              uwatcher_spec3_fields);
}
