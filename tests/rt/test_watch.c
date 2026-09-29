/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/rt/test_watch.c — the reactive runtime: condition watchers and the
 * dirty set, event subscriptions, slot-change events, and the two host
 * entry points (urbi_watch, a host slot write).
 *
 * Every case drives the PUBLIC entry points.  The CADENCE the file depends
 * on: urbi_run spawns the chunk as a scheduled strand and pumps until
 * nothing is READY, and a drain runs after every strand slice -- so by the
 * time urbi_run returns, every watcher a chunk armed has been evaluated
 * and every body it fired has finished, unless something is parked or a
 * timer is pending. */

#include <stdlib.h>
#include <string.h>

#include "rtest.h"
#include "rt/uwatch.h"
#include "urbi/urbi.h"

/* --- fixture ------------------------------------------------------------ */

typedef struct { size_t live, peak; } WatchAlloc;

static void *watch_alloc(void *ptr, size_t n, void *ud)
{
    WatchAlloc *ca = (WatchAlloc *)ud;
    size_t *hdr = ptr ? ((size_t *)ptr) - 2 : NULL;
    size_t old = hdr ? hdr[0] : 0;
    if (n == 0) {
        if (hdr) { ca->live -= old; free(hdr); }
        return NULL;
    }
    size_t *nh = (size_t *)realloc(hdr, n + 2 * sizeof(size_t));
    if (!nh) return NULL;
    nh[0] = n;
    ca->live += n - old;
    if (ca->live > ca->peak) ca->peak = ca->live;
    return (void *)(nh + 2);
}

typedef struct {
    UVM        *vm;
    WatchAlloc  ca;
    uint64_t    now_us;
    int         diag_hits;
    char        diag_last[256];
} Fix;

static uint64_t fake_clock(void *ud) { return ((const Fix *)ud)->now_us; }

static void diag_sink(UVM *vm, void *ud, int level, const char *msg, size_t len)
{
    (void)vm; (void)level;
    Fix *fx = (Fix *)ud;
    fx->diag_hits++;
    size_t n = len < sizeof fx->diag_last - 1 ? len : sizeof fx->diag_last - 1;
    memcpy(fx->diag_last, msg, n);
    fx->diag_last[n] = '\0';
}

static void fix_open(Fix *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->vm = urbi_open(watch_alloc, &fx->ca, NULL);
    urbi_set_clock(fx->vm, fake_clock, fx);
    urbi_set_diag(fx->vm, diag_sink, fx);
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

/* A value's rendering, for the order-sensitive string accumulators.  The
 * public formatter is what the REPL prints, so a String comes back
 * quoted; the cases compare against the quoted form. */
static const char *shown(Fix *fx, UValue v, char *buf, size_t cap)
{
    buf[0] = '\0';
    (void)urbi_value_to_string(fx->vm, v, buf, cap);
    return buf;
}

static int64_t global_int(Fix *fx, const char *name)
{
    UValue v = urbi_make_nil();
    if (urbi_global_get(fx->vm, urbi_realm_main(fx->vm), name, &v) != URBI_OK) return -1;
    return v.kind == UVAL_INT ? v.v.i : -1;
}

/* --- (a) at (cond): one body per rising edge ----------------------------- */

static void at_fires_once_per_rising_edge(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var x = 0");
    run(&fx, "var hit = 0");
    run(&fx, "at (x == 3) { hit = hit + 1 }");
    RT_EQ(global_int(&fx, "hit"), 0);          /* the condition does not hold yet */

    run(&fx, "x = 3");
    RT_EQ(global_int(&fx, "hit"), 1);

    /* A write that does not change the condition's truth is not an edge. */
    run(&fx, "x = 3");
    RT_EQ(global_int(&fx, "hit"), 1);

    /* Down and up again is a second edge. */
    run(&fx, "x = 0");
    RT_EQ(global_int(&fx, "hit"), 1);
    run(&fx, "x = 3");
    RT_EQ(global_int(&fx, "hit"), 2);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* A condition that already holds at install fires on the first drain: the
 * watcher has never been evaluated, so `false -> true` is the edge it
 * sees. */
static void at_fires_when_the_condition_already_holds(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var x = 9");
    run(&fx, "var hit = 0");
    run(&fx, "at (x > 5) { hit = 1 }");
    RT_EQ(global_int(&fx, "hit"), 1);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (b) whenever is a reactive loop ------------------------------------- */

static void whenever_refires_until_the_condition_falls(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var n = 0");
    run(&fx, "var runs = 0");
    /* The body takes the condition down one step at a time: `whenever`
     * re-fires on body completion while the condition still holds, so the
     * loop runs exactly n times and then stops. */
    run(&fx, "whenever (n > 0) { n = n - 1; runs = runs + 1 }");
    RT_EQ(global_int(&fx, "runs"), 0);

    run(&fx, "n = 3");
    RT_EQ(global_int(&fx, "runs"), 3);
    RT_EQ(global_int(&fx, "n"), 0);

    /* A second rising edge runs it again, from the top. */
    run(&fx, "n = 2");
    RT_EQ(global_int(&fx, "runs"), 5);
    RT_EQ(global_int(&fx, "n"), 0);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (c) a body that flips its own condition fires once ------------------ */

static void whenever_does_not_double_fire_on_its_own_write(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var go = false");
    run(&fx, "var runs = 0");
    run(&fx, "whenever (go) { runs = runs + 1; go = false }");
    run(&fx, "go = true");
    RT_EQ(global_int(&fx, "runs"), 1);
    /* And the level is back down, so nothing else is pending. */
    RT_CHECK(urbi_step(fx.vm, 0, NULL) == URBI_STEP_QUIESCENT);
    RT_EQ(global_int(&fx, "runs"), 1);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- at sync runs before the next statement of the installing strand ----- */

static void at_sync_body_completes_before_the_next_statement(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var log = \"\"");
    run(&fx, "var go = false");
    run(&fx, "at sync (go) { log = log + \"1\"; log = log + \"2\" }");
    /* The `;` after the flip is the sequence point the sync body runs at,
     * so "M" lands after both halves of the body. */
    char buf[64];
    UValue v = run(&fx, "go = true; log = log + \"M\"");
    RT_STREQ(shown(&fx, v, buf, sizeof buf), "\"12M\"");

    /* The async form gets only as far as its first `;` before the
     * installing strand resumes. */
    run(&fx, "var alog = \"\"");
    run(&fx, "var ago = false");
    run(&fx, "at (ago) { alog = alog + \"1\"; alog = alog + \"2\" }");
    v = run(&fx, "ago = true; alog = alog + \"M\"");
    RT_STREQ(shown(&fx, v, buf, sizeof buf), "\"1M\"");
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- onleave fires once on the falling edge, and only after a body ------- */

static void onleave_fires_on_the_falling_edge_only_after_a_body(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var x = 0");
    run(&fx, "var n = 0");
    run(&fx, "at (x > 5) { n = n + 1 } onleave { n = n + 10 }");
    /* Down before ever being up: no body has run, so no onleave. */
    run(&fx, "x = 1");
    RT_EQ(global_int(&fx, "n"), 0);
    run(&fx, "x = 10");
    RT_EQ(global_int(&fx, "n"), 1);
    run(&fx, "x = 0");
    RT_EQ(global_int(&fx, "n"), 11);
    /* And not a second time for a write that stays below. */
    run(&fx, "x = 1");
    RT_EQ(global_int(&fx, "n"), 11);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (d) waituntil parks the strand and wakes it ------------------------- */

static void waituntil_parks_until_the_condition_holds(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var x = 0");
    run(&fx, "var done = 0");
    /* The chunk's strand parks inside waituntil, so the chunk's value is
     * nil and the write has not happened. */
    UValue v = run(&fx, "waituntil (x == 1); done = 42");
    RT_EQ(v.kind, (uint8_t)UVAL_NIL);
    RT_EQ(global_int(&fx, "done"), 0);
    /* A wait is not live work, however it is spelled -- and the step says
     * so too: there is nothing runnable and no timer. */
    RT_CHECK(!urbi_has_live_work(fx.vm));
    RT_EQ(urbi_step(fx.vm, 0, NULL), URBI_STEP_QUIESCENT);

    run(&fx, "x = 1");
    RT_EQ(global_int(&fx, "done"), 42);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* A condition that already holds does not park, and the rest of the line
 * runs on the same strand. */
static void waituntil_true_at_install_does_not_park(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var flag = true");
    UValue v = run(&fx, "waituntil (flag); 7");
    RT_EQ(v.kind, (uint8_t)UVAL_INT);
    RT_EQ(v.v.i, 7);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* A HOST write between two steps reaches a parked waituntil: urbi_global_set
 * goes through the same notification OP_SETSLOT does. */
static void a_host_write_wakes_a_parked_waituntil(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var go = false");
    run(&fx, "var done = 0");
    run(&fx, "waituntil (go); done = 42");
    RT_EQ(global_int(&fx, "done"), 0);

    RT_EQ(urbi_global_set(fx.vm, urbi_realm_main(fx.vm), "go", urbi_make_bool(true)), URBI_OK);
    while (urbi_step(fx.vm, 0, NULL) == URBI_STEP_RAN) { }
    RT_EQ(global_int(&fx, "done"), 42);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* Neither spelling of a wait is live work: `waituntil (cond)` parks on a
 * watcher's wait list and `waituntil (e?)` on an event's, and spec section
 * 8 says wait lists do not count.  An armed `at` does count, which is the
 * line between the two. */
static void neither_waituntil_form_is_live_work(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var x = 0");
    run(&fx, "waituntil (x == 1)");
    RT_CHECK(!urbi_has_live_work(fx.vm));
    RT_EQ(urbi_step(fx.vm, 0, NULL), URBI_STEP_QUIESCENT);

    run(&fx, "var e = Event.new()");
    run(&fx, "var t = Tag.new()");
    run(&fx, "t: { waituntil(e?) }");
    RT_CHECK(!urbi_has_live_work(fx.vm));
    RT_EQ(urbi_step(fx.vm, 0, NULL), URBI_STEP_QUIESCENT);

    /* An armed `at` is the thing that IS live: a host write between two
     * steps is what it exists to notice. */
    run(&fx, "at (x == 7) { x = 0 }");
    RT_CHECK(urbi_has_live_work(fx.vm));
    RT_EQ(urbi_step(fx.vm, 0, NULL), URBI_STEP_QUIESCENT);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (e) event subscriptions -------------------------------------------- */

static void at_event_fires_per_emission_in_registration_order(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var e = Event.new()");
    run(&fx, "var seq = \"\"");
    run(&fx, "at (e?) { seq = seq + \"A\" }");
    run(&fx, "at (e?) { seq = seq + \"B\" }");
    run(&fx, "e!");
    char buf[64];
    UValue v = run(&fx, "seq");
    RT_STREQ(shown(&fx, v, buf, sizeof buf), "\"AB\"");

    /* Five emissions on one line drive five bodies each, not two. */
    run(&fx, "var hits = 0");
    run(&fx, "at (e?) { hits = hits + 1 }");
    run(&fx, "e!; e!; e!");
    RT_EQ(global_int(&fx, "hits"), 3);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

static void at_event_binds_the_payload(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var e = Event.new()");
    run(&fx, "var got = 0");
    run(&fx, "at (e?(var p)) { got = p }");
    run(&fx, "e!(42)");
    RT_EQ(global_int(&fx, "got"), 42);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* syncEmit runs an `at sync (e?)` subscriber inline; a plain emit does
 * not, which is what "synchronous" is a property OF. */
static void sync_emit_runs_a_sync_subscriber_inline(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var e = Event.new()");
    run(&fx, "var order = \"\"");
    run(&fx, "at sync (e?) { order = order + \"s\" }");
    char buf[64];
    UValue v = run(&fx, "e.syncEmit(1); order");
    RT_STREQ(shown(&fx, v, buf, sizeof buf), "\"s\"");
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (f) waituntil (e?) delivers the payload ----------------------------- */

static void waituntil_event_resumes_with_the_payload(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var e = Event.new()");
    run(&fx, "var got = 0");
    run(&fx, "var t = Tag.new()");
    run(&fx, "t: { got = waituntil(e?) }");
    RT_EQ(global_int(&fx, "got"), 0);
    run(&fx, "e!(99)");
    RT_EQ(global_int(&fx, "got"), 99);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (g) x.changed? ------------------------------------------------------ */

static void changed_event_fires_on_a_write_but_not_on_the_install(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var fired = 0");
    /* Subscribed BEFORE the slot exists: declaring it is not a change. */
    run(&fx, "at (Realm.x.changed?) { fired = fired + 1 }");
    run(&fx, "var Realm.x = 0");
    RT_EQ(global_int(&fx, "fired"), 0);
    run(&fx, "Realm.x = 1");
    RT_EQ(global_int(&fx, "fired"), 1);
    run(&fx, "Realm.x = 2");
    RT_EQ(global_int(&fx, "fired"), 2);

    /* The hidden slot the event lives in is not part of the object's
     * script-visible surface. */
    char buf[512];
    UValue v = run(&fx, "Realm.localSlotNames().join(\",\")");
    RT_CHECK(strstr(shown(&fx, v, buf, sizeof buf), "\x01") == NULL);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* Taking `x.changed?` on an object has to hold that object across two
 * allocations.  It used to borrow UCELL_F_PINNED -- the HOST's pin bit --
 * and clear it unconditionally on the way out, so an embedder that had
 * urbi_ref'd the object lost its hold the first time script subscribed to
 * one of its slots, and the next collection was free to free a value the
 * host still held.  The runtime has its own pin bit now (UCELL_F_RTPIN).
 *
 * The object is reachable by name only while the subscription is set up;
 * dropping the name afterwards leaves the host's pin as the one thing
 * keeping it alive, which is what makes the collect below load-bearing. */
static void a_host_pin_survives_a_changed_subscription(void)
{
    Fix fx; fix_open(&fx);
    URealm *realm = urbi_realm_main(fx.vm);

    UValue ov = run(&fx, "Object.new()");
    RT_EQ(ov.kind, (uint8_t)UV_OBJ);
    urbi_ref(fx.vm, ov);

    RT_EQ(urbi_global_set(fx.vm, realm, "Probe", ov), URBI_OK);
    run(&fx, "var fired = 0");
    run(&fx, "var Probe.x = 0");
    run(&fx, "at (Probe.x.changed?) { fired = fired + 1 }");
    run(&fx, "Probe.x = 1");
    RT_EQ(global_int(&fx, "fired"), 1);

    /* The direct regression: the subscription must not have cleared the
     * host's bit. */
    RT_CHECK((((UCell *)ov.v.p)->flags & UCELL_F_PINNED) != 0);

    /* And the pin must still do its job with nothing else referring to
     * the object. */
    RT_EQ(urbi_global_set(fx.vm, realm, "Probe", urbi_make_nil()), URBI_OK);
    urbi_gc_collect(fx.vm);
    RT_EQ(((UCell *)ov.v.p)->type, (uint8_t)UCELL_OBJ);
    UValue x = urbi_make_nil();
    RT_EQ(urbi_slot_get(fx.vm, ov, "x", &x), URBI_OK);
    RT_EQ(x.v.i, 1);

    urbi_unref(fx.vm, ov);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (h) a tag cancels the watchers installed inside its scope ----------- */

static void tag_stop_removes_the_watcher(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var x = 0");
    run(&fx, "var hits = 0");
    run(&fx, "var mytag = Tag.new()");
    run(&fx, "mytag: at (x > 0) { hits = hits + 1 }");
    run(&fx, "x = 1");
    RT_EQ(global_int(&fx, "hits"), 1);

    run(&fx, "mytag.stop()");
    run(&fx, "x = 0");
    run(&fx, "x = 2");
    RT_EQ(global_int(&fx, "hits"), 1);
    /* And it is gone, not merely quiet: nothing is pending any more. */
    RT_CHECK(!urbi_has_live_work(fx.vm));
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* An event subscription made inside a tag scope is cancelled the same
 * way. */
static void tag_stop_unsubscribes_an_event_watcher(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var e = Event.new()");
    run(&fx, "var hits = 0");
    run(&fx, "var t = Tag.new()");
    run(&fx, "t: at (e?) { hits = hits + 1 }");
    run(&fx, "e!");
    RT_EQ(global_int(&fx, "hits"), 1);
    run(&fx, "t.stop()");
    run(&fx, "e!");
    RT_EQ(global_int(&fx, "hits"), 1);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* Cancelling a watcher from INSIDE its own condition.  The condition is
 * arbitrary script and it runs while the reactive runtime is holding the
 * watcher, so the cancel may only clear `armed` -- unlinking it from
 * vm->watch.all, its one GC root, would let the next allocation inside
 * that same condition free the cell the caller is still using.  Under
 * URBI_GC_STRESS every allocation collects, which is what turns "may" into
 * "does"; the trailing list literal is that allocation. */
static void a_condition_may_cancel_its_own_watcher(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var t = Tag.new()");
    run(&fx, "var n = 0");
    run(&fx, "var k = 0");
    run(&fx, "var c = function() { k = k + 1; if (k > 2) { t.stop(); var z = [1, 2, 3, 4, 5] }; true }");
    /* A `whenever` re-asks its condition on body death, which is the path
     * that runs a condition outside a drain. */
    run(&fx, "t: whenever (c()) n = n + 1");
    RT_EQ(global_int(&fx, "n"), 1);
    RT_CHECK(!urbi_has_live_work(fx.vm));

    /* The same from inside a drain-time condition, for the `at` path. */
    run(&fx, "var t2 = Tag.new()");
    run(&fx, "var m = 0");
    run(&fx, "var j = 0");
    run(&fx, "var c2 = function() { j = j + 1; if (j > 1) { t2.stop(); var z2 = [1, 2, 3] }; j > 1 }");
    run(&fx, "t2: at (c2()) m = m + 1");
    run(&fx, "j = j");
    urbi_gc_collect(fx.vm);
    RT_EQ(global_int(&fx, "m"), 0);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (i) a condition that throws kills nothing --------------------------- */

static void a_condition_that_throws_is_reported_once_and_disarmed(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var x = 0");
    run(&fx, "var ok = 0");
    /* A second, healthy watcher on the same slot proves the raise took
     * nothing else down with it. */
    run(&fx, "at (x > 0) { ok = ok + 1 }");
    /* The drain at the end of the installing chunk is the raise: the
     * condition is evaluated once, reported once, and disarmed. */
    run(&fx, "at (nosuchname > 0) { ok = ok + 100 }");
    RT_EQ(fx.diag_hits, 1);
    RT_CHECK(strstr(fx.diag_last, "at condition raised") != NULL);

    /* The healthy watcher on the same slot is untouched, and the dead one
     * never raises again. */
    run(&fx, "x = 1");
    RT_EQ(global_int(&fx, "ok"), 1);
    RT_EQ(fx.diag_hits, 1);
    run(&fx, "x = 0");
    run(&fx, "x = 2");
    RT_EQ(global_int(&fx, "ok"), 2);
    RT_EQ(fx.diag_hits, 1);

    /* And the host's error channel is not left holding the watcher's
     * private failure. */
    UErrorInfo info;
    RT_EQ(urbi_last_error(fx.vm, &info), URBI_OK);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- the host watch ------------------------------------------------------ */

typedef struct { int hits; int64_t last; } HostWatch;

static int host_watch_cb(UVM *vm, void *ud, UValue v)
{
    (void)vm;
    HostWatch *hw = (HostWatch *)ud;
    hw->hits++;
    hw->last = (v.kind == UVAL_INT) ? v.v.i : -1;
    return 0;
}

static void urbi_watch_calls_back_on_each_rising_edge(void)
{
    Fix fx; fix_open(&fx);
    HostWatch hw = { 0, 0 };
    run(&fx, "var x = 0");
    RT_EQ(urbi_watch(fx.vm, urbi_realm_main(fx.vm), "x", host_watch_cb, &hw), URBI_OK);

    run(&fx, "x = 7");
    RT_EQ(hw.hits, 1);
    RT_EQ(hw.last, 7);
    /* Level, not repetition: staying truthy is not another edge. */
    run(&fx, "x = 8");
    RT_EQ(hw.hits, 1);
    run(&fx, "x = 0");
    run(&fx, "x = 9");
    RT_EQ(hw.hits, 2);
    RT_EQ(hw.last, 9);

    /* And a host can cancel its own watch on the MAIN realm, which
     * urbi_realm_free refuses to touch: the watch is scoped to the realm's
     * connection tag and urbi_realm_tag is how the host names it. */
    UValue tag = urbi_realm_tag(fx.vm, urbi_realm_main(fx.vm));
    RT_EQ(tag.kind, (uint8_t)UVAL_CELL);
    RT_EQ(urbi_tag_stop(fx.vm, tag), URBI_OK);
    RT_CHECK(!urbi_has_live_work(fx.vm));
    run(&fx, "x = 0");
    run(&fx, "x = 11");
    RT_EQ(hw.hits, 2);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- shape and footprint ------------------------------------------------- */

/* What the reactive runtime costs a VM that never uses it: nothing on the
 * heap, and one cell per watcher when it does.  The VM's own boot-heap
 * budget is measured once, by tests/rt/test_realm.c's
 * t_boot_heap_is_small -- duplicating the headline number here only
 * produced two figures that drift apart under different build variants. */
static void the_reactive_runtime_costs_an_unused_vm_nothing(void)
{
    Fix fx; fix_open(&fx);
    (void)urbi_realm_main(fx.vm);
    size_t booted = fx.ca.live;
    printf("    sizeof(UWatcher) = %lu; watcher list empty at boot\n",
           (unsigned long)sizeof(UWatcher));
    RT_EQ(sizeof(UWatcher), 136u);
    /* UWatchState is four words inside UVM and vm->watch.all starts NULL,
     * so running a script that installs nothing adds no watcher bytes. */
    run(&fx, "var x = 1");
    RT_CHECK(!urbi_has_live_work(fx.vm));
    urbi_gc_collect(fx.vm);

    /* One `at` is one cell plus its two closures; a thousand of them stay
     * proportional rather than quadratic. */
    run(&fx, "var t = Tag.new()");
    run(&fx, "var i = 0");
    run(&fx, "t: { while (i < 200) { at (x == 99) { x = x } ; i = i + 1 } }");
    RT_CHECK(urbi_has_live_work(fx.vm));
    size_t with_watchers = fx.ca.live;
    RT_CHECK(with_watchers > booted);
    RT_CHECK(with_watchers - booted < 200u * 1024u);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* A hundred watchers on one condition, all cancelled by one tag: the list
 * really is emptied rather than merely disarmed. */
static void a_hundred_watchers_are_all_reclaimed(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var x = 0");
    run(&fx, "var t = Tag.new()");
    run(&fx, "var i = 0");
    run(&fx, "t: { while (i < 100) { at (x > 0) { x = x } ; i = i + 1 } }");
    RT_CHECK(urbi_has_live_work(fx.vm));
    run(&fx, "t.stop()");
    RT_CHECK(!urbi_has_live_work(fx.vm));
    urbi_gc_collect(fx.vm);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- whenever: the arms are serialised ------------------------------------
 *
 * These bodies sleep and do not take their own condition down, so while
 * the condition holds nothing here may pump the scheduler without a
 * budget: urbi_run and an unbudgeted urbi_step would keep a runaway loop
 * running for as long as it keeps re-firing.  The condition is therefore
 * raised by a host write, the scheduler is driven in bounded slices, and
 * `run` is only ever called while the condition is false. */

static UValue run_ok(Fix *fx, const char *src)
{
    UValue out = urbi_make_nil();
    char err[256] = { 0 };
    int rc = urbi_run(fx->vm, urbi_realm_main(fx->vm), src, strlen(src), "<test>",
                      &out, err, sizeof err);
    RT_EQ(rc, URBI_OK);
    return out;
}

static void set_int(Fix *fx, const char *name, int64_t v)
{
    RT_EQ(urbi_global_set(fx->vm, urbi_realm_main(fx->vm), name, urbi_make_int(v)), URBI_OK);
}

/* A bounded number of bounded slices, after moving the clock by `us`. */
static void slices(Fix *fx, uint64_t us)
{
    fx->now_us += us;
    for (int k = 0; k < 8; k++) (void)urbi_step(fx->vm, 1000, NULL);
}

static const char *global_shown(Fix *fx, const char *name, char *buf, size_t cap)
{
    UValue v = urbi_make_nil();
    RT_EQ(urbi_global_get(fx->vm, urbi_realm_main(fx->vm), name, &v), URBI_OK);
    return shown(fx, v, buf, cap);
}

static void the_else_arm_waits_for_a_running_body(void)
{
    Fix fx; fix_open(&fx);
    char buf[64];
    run_ok(&fx, "var x = 0 | var log = \"\"");
    run_ok(&fx, "whenever (x > 0) { log = log + \"b1 \"; sleep(10ms); log = log + \"b2 \" }"
                " else { log = log + \"else \" }");
    set_int(&fx, "x", 1);
    slices(&fx, 0);                                   /* body runs to its sleep */
    set_int(&fx, "x", 0);
    slices(&fx, 0);                                   /* falling edge, body still asleep */
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 \"");
    slices(&fx, 20000);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 b2 else \"");
    RT_EQ(urbi_step(fx.vm, 1000, NULL), URBI_STEP_QUIESCENT);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* Falls and rises again while the body is still running: the else arm
 * the fall earned runs first, then the body the rise earned.  The second
 * body takes the condition down itself. */
static void a_fall_and_rise_during_the_body_keep_edge_order(void)
{
    Fix fx; fix_open(&fx);
    char buf[64];
    run_ok(&fx, "var x = 0 | var n = 0 | var log = \"\"");
    run_ok(&fx, "whenever (x > 0) { n = n + 1;"
                " if (n == 1) log = log + \"b1 \" else log = log + \"b2 \"; sleep(10ms);"
                " if (n >= 2) x = 0 } else { log = log + \"else \" }");
    set_int(&fx, "x", 1); slices(&fx, 0);
    set_int(&fx, "x", 0); slices(&fx, 0);
    set_int(&fx, "x", 1); slices(&fx, 0);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 \"");
    for (int k = 0; k < 6; k++) slices(&fx, 20000);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 else b2 else \"");
    RT_EQ(global_int(&fx, "n"), 2);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* Falls, rises and falls again while the body runs: one body, one else. */
static void a_fall_rise_fall_during_the_body_owes_one_else(void)
{
    Fix fx; fix_open(&fx);
    char buf[64];
    run_ok(&fx, "var x = 0 | var log = \"\"");
    run_ok(&fx, "whenever (x > 0) { log = log + \"b1 \"; sleep(10ms); log = log + \"b2 \" }"
                " else { log = log + \"else \" }");
    set_int(&fx, "x", 1); slices(&fx, 0);
    set_int(&fx, "x", 0); slices(&fx, 0);
    set_int(&fx, "x", 1); slices(&fx, 0);
    set_int(&fx, "x", 0); slices(&fx, 0);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 \"");
    slices(&fx, 20000);
    slices(&fx, 20000);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 b2 else \"");
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

static void a_cancelled_watcher_drops_its_pending_else(void)
{
    Fix fx; fix_open(&fx);
    char buf[64];
    run_ok(&fx, "var x = 0 | var log = \"\" | var t = Tag.new()");
    run_ok(&fx, "t: whenever (x > 0) { log = log + \"b1 \"; sleep(10ms); log = log + \"b2 \" }"
                " else { log = log + \"else \" }");
    set_int(&fx, "x", 1); slices(&fx, 0);
    set_int(&fx, "x", 0); slices(&fx, 0);
    run_ok(&fx, "t.stop()");                          /* the condition is false here */
    slices(&fx, 20000);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 \"");
    RT_CHECK(!urbi_has_live_work(fx.vm));
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* A body that dies by a throw still ends its turn: the else arm owed to
 * the fall runs after it. */
static void a_body_that_throws_still_hands_over_to_its_else(void)
{
    Fix fx; fix_open(&fx);
    char buf[64];
    run_ok(&fx, "var x = 0 | var log = \"\"");
    run_ok(&fx, "whenever (x > 0) { log = log + \"b1 \"; sleep(10ms); throw \"boom\" }"
                " else { log = log + \"else \" }");
    set_int(&fx, "x", 1); slices(&fx, 0);
    set_int(&fx, "x", 0); slices(&fx, 0);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 \"");
    slices(&fx, 20000);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 else \"");
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* A raise disarms a watcher for the edges still to come; it does not take
 * back an edge already seen.  The fall is seen while the body sleeps, the
 * body's last statement makes the condition raise, and the re-evaluation
 * at the body's death is the raise: the else arm the fall earned still
 * runs. */
static void a_raising_condition_still_serves_the_else_it_owed(void)
{
    Fix fx; fix_open(&fx);
    char buf[64];
    run_ok(&fx, "var x = 0 | var boom = 0 | var log = \"\"");
    run_ok(&fx, "var c = function() { if (boom > 0) throw \"raised\"; x > 0 }");
    run_ok(&fx, "whenever (c()) { log = log + \"b1 \"; sleep(10ms); log = log + \"b2 \"; boom = 1 }"
                " else { log = log + \"else \" }");
    set_int(&fx, "x", 1); slices(&fx, 0);
    set_int(&fx, "x", 0); slices(&fx, 0);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 \"");
    RT_EQ(fx.diag_hits, 0);
    slices(&fx, 20000);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 b2 else \"");
    RT_EQ(fx.diag_hits, 1);
    RT_CHECK(!urbi_has_live_work(fx.vm));
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* The same raise, but in a drain while the body is still asleep: the
 * watcher is disarmed at once, yet the else arm it owes still waits for
 * the body and runs when it finishes. */
static void a_raise_in_the_drain_keeps_the_owed_else_for_the_body_to_finish(void)
{
    Fix fx; fix_open(&fx);
    char buf[64];
    run_ok(&fx, "var x = 0 | var boom = 0 | var log = \"\"");
    run_ok(&fx, "var c = function() { if (boom > 0) throw \"raised\"; x > 0 }");
    run_ok(&fx, "whenever (c()) { log = log + \"b1 \"; sleep(10ms); log = log + \"b2 \" }"
                " else { log = log + \"else \" }");
    set_int(&fx, "x", 1); slices(&fx, 0);
    set_int(&fx, "x", 0); slices(&fx, 0);
    set_int(&fx, "boom", 1); slices(&fx, 0);          /* the drain raises */
    RT_EQ(fx.diag_hits, 1);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 \"");
    slices(&fx, 20000);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 b2 else \"");
    RT_EQ(fx.diag_hits, 1);
    RT_CHECK(!urbi_has_live_work(fx.vm));
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* A cancel after such a raise still drops the owed arm: cancelling is the
 * one thing that takes an earned else arm back.  The raise has already
 * disarmed the watcher, so it is the cancel's clearing of the owed arm,
 * not the disarm, that keeps the body's death from serving it. */
static void a_cancel_after_a_raise_drops_the_owed_else(void)
{
    Fix fx; fix_open(&fx);
    char buf[64];
    run_ok(&fx, "var x = 0 | var boom = 0 | var log = \"\" | var t = Tag.new()");
    run_ok(&fx, "var c = function() { if (boom > 0) throw \"raised\"; x > 0 }");
    run_ok(&fx, "t: whenever (c()) { log = log + \"b1 \"; sleep(10ms); log = log + \"b2 \" }"
                " else { log = log + \"else \" }");
    set_int(&fx, "x", 1); slices(&fx, 0);
    set_int(&fx, "x", 0); slices(&fx, 0);
    set_int(&fx, "boom", 1); slices(&fx, 0);          /* disarmed, arm still owed */
    run_ok(&fx, "t.stop()");
    slices(&fx, 20000);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b1 \"");
    RT_CHECK(!urbi_has_live_work(fx.vm));
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* THE RECORDED RESIDUAL, pinned so that a change to it is deliberate.  The
 * else arm is a strand of its own and is not tracked as the body, so an
 * else arm that sleeps is overtaken by the body the next rise earned: the
 * arms start in edge order but still overlap here. */
static void a_sleeping_else_arm_is_overtaken_by_the_next_body(void)
{
    Fix fx; fix_open(&fx);
    char buf[64];
    run_ok(&fx, "var x = 0 | var n = 0 | var log = \"\"");
    run_ok(&fx, "whenever (x > 0) { n = n + 1; log = log + \"b \"; sleep(10ms); log = log + \"B \";"
                " if (n >= 2) x = 0 } else { log = log + \"e \"; sleep(5ms); log = log + \"E \" }");
    set_int(&fx, "x", 1); slices(&fx, 0);
    set_int(&fx, "x", 0); slices(&fx, 0);
    set_int(&fx, "x", 1); slices(&fx, 0);
    slices(&fx, 11000);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b B e b \"");
    for (int k = 0; k < 3; k++) slices(&fx, 11000);
    RT_STREQ(global_shown(&fx, "log", buf, sizeof buf), "\"b B e b E B e E \"");
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* The arm that never overlapped is unchanged: a body that has already
 * finished when the condition falls gets its else arm at once.  This body
 * takes its own condition down, so an unbudgeted pump is bounded. */
static void an_else_arm_with_no_body_in_flight_is_immediate(void)
{
    Fix fx; fix_open(&fx);
    run_ok(&fx, "var x = 10 | var on = 0 | var off = 0");
    run_ok(&fx, "whenever (x > 5) { on = on + 1; x = x - 1 } else { off = off + 1 }");
    for (int k = 0; k < 64 && urbi_step(fx.vm, 1000, NULL) == URBI_STEP_RAN; k++) { }
    RT_EQ(global_int(&fx, "on"), 5);
    RT_EQ(global_int(&fx, "off"), 1);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

RT_SUITE(rt_watch_suite) {
    rt_run("at_fires_once_per_rising_edge", at_fires_once_per_rising_edge);
    rt_run("at_fires_when_the_condition_already_holds", at_fires_when_the_condition_already_holds);
    rt_run("whenever_refires_until_the_condition_falls", whenever_refires_until_the_condition_falls);
    rt_run("whenever_does_not_double_fire_on_its_own_write", whenever_does_not_double_fire_on_its_own_write);
    rt_run("at_sync_body_completes_before_the_next_statement", at_sync_body_completes_before_the_next_statement);
    rt_run("onleave_fires_on_the_falling_edge_only_after_a_body", onleave_fires_on_the_falling_edge_only_after_a_body);
    rt_run("waituntil_parks_until_the_condition_holds", waituntil_parks_until_the_condition_holds);
    rt_run("waituntil_true_at_install_does_not_park", waituntil_true_at_install_does_not_park);
    rt_run("a_host_write_wakes_a_parked_waituntil", a_host_write_wakes_a_parked_waituntil);
    rt_run("neither_waituntil_form_is_live_work", neither_waituntil_form_is_live_work);
    rt_run("at_event_fires_per_emission_in_registration_order", at_event_fires_per_emission_in_registration_order);
    rt_run("at_event_binds_the_payload", at_event_binds_the_payload);
    rt_run("sync_emit_runs_a_sync_subscriber_inline", sync_emit_runs_a_sync_subscriber_inline);
    rt_run("waituntil_event_resumes_with_the_payload", waituntil_event_resumes_with_the_payload);
    rt_run("changed_event_fires_on_a_write_but_not_on_the_install", changed_event_fires_on_a_write_but_not_on_the_install);
    rt_run("a_host_pin_survives_a_changed_subscription", a_host_pin_survives_a_changed_subscription);
    rt_run("tag_stop_removes_the_watcher", tag_stop_removes_the_watcher);
    rt_run("tag_stop_unsubscribes_an_event_watcher", tag_stop_unsubscribes_an_event_watcher);
    rt_run("a_condition_may_cancel_its_own_watcher", a_condition_may_cancel_its_own_watcher);
    rt_run("a_condition_that_throws_is_reported_once_and_disarmed", a_condition_that_throws_is_reported_once_and_disarmed);
    rt_run("urbi_watch_calls_back_on_each_rising_edge", urbi_watch_calls_back_on_each_rising_edge);
    rt_run("the_reactive_runtime_costs_an_unused_vm_nothing", the_reactive_runtime_costs_an_unused_vm_nothing);
    rt_run("a_hundred_watchers_are_all_reclaimed", a_hundred_watchers_are_all_reclaimed);
    rt_run("the_else_arm_waits_for_a_running_body", the_else_arm_waits_for_a_running_body);
    rt_run("a_fall_and_rise_during_the_body_keep_edge_order", a_fall_and_rise_during_the_body_keep_edge_order);
    rt_run("a_fall_rise_fall_during_the_body_owes_one_else", a_fall_rise_fall_during_the_body_owes_one_else);
    rt_run("a_cancelled_watcher_drops_its_pending_else", a_cancelled_watcher_drops_its_pending_else);
    rt_run("a_body_that_throws_still_hands_over_to_its_else", a_body_that_throws_still_hands_over_to_its_else);
    rt_run("an_else_arm_with_no_body_in_flight_is_immediate", an_else_arm_with_no_body_in_flight_is_immediate);
    rt_run("a_raising_condition_still_serves_the_else_it_owed", a_raising_condition_still_serves_the_else_it_owed);
    rt_run("a_raise_in_the_drain_keeps_the_owed_else_for_the_body_to_finish", a_raise_in_the_drain_keeps_the_owed_else_for_the_body_to_finish);
    rt_run("a_cancel_after_a_raise_drops_the_owed_else", a_cancel_after_a_raise_drops_the_owed_else);
    rt_run("a_sleeping_else_arm_is_overtaken_by_the_next_body", a_sleeping_else_arm_is_overtaken_by_the_next_body);
}
