/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/rt/test_sched.c — the scheduler: the run queue, the timer heap,
 * park/wake, fork/join, gates and cross-strand stop.
 *
 * Every case drives the PUBLIC entry points (urbi_run, urbi_step) over a
 * FAKE CLOCK the test advances by hand.  That is the whole point: with a
 * real clock "did the sleeper wake at the right time" is a race, and with
 * a fake one it is an assertion.  A handful of cases reach into the
 * runtime headers afterwards to read scheduler state (the heap length,
 * a strand's park state) that no public call exposes -- tests/rt is the
 * one place allowed to.
 *
 * The CADENCE the whole file depends on: urbi_run spawns the chunk as a
 * scheduled strand and pumps until nothing is READY, but never waits for
 * a timer.  A chunk that slept hands back nil and resumes on a later
 * urbi_step, which is what makes the clock the test's to control. */

#include <stdlib.h>
#include <string.h>

#include "rtest.h"
#include "rt/uboot.h"
#include "urbi/urbi.h"

/* --- fixture ------------------------------------------------------------ */

typedef struct { size_t live, peak; unsigned long allocs; } SchedAlloc;

static void *sched_alloc(void *ptr, size_t n, void *ud)
{
    SchedAlloc *ca = (SchedAlloc *)ud;
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
    ca->allocs++;
    if (ca->live > ca->peak) ca->peak = ca->live;
    return (void *)(nh + 2);
}

typedef struct {
    UVM        *vm;
    SchedAlloc  ca;
    uint64_t    now_us;
} Fix;

static uint64_t fake_clock(void *ud) { return ((const Fix *)ud)->now_us; }

static void fix_open(Fix *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->vm = urbi_open(sched_alloc, &fx->ca, NULL);
    urbi_set_clock(fx->vm, fake_clock, fx);
}

static void fix_close(Fix *fx) { urbi_close(fx->vm); }

/* Advance the fake clock by `us`, then step once. */
static int tick(Fix *fx, uint64_t us)
{
    fx->now_us += us;
    return urbi_step(fx->vm, 0, NULL);
}

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

static UValue run_on(Fix *fx, URealm *r, const char *src)
{
    UValue out = urbi_make_nil();
    char err[256] = { 0 };
    int rc = urbi_run(fx->vm, r, src, strlen(src), "<test>", &out, err, sizeof err);
    if (rc != URBI_OK) printf("    run_on(%s) rc=%d err=%s\n", src, rc, err);
    return out;
}

/* The value of a global, as an integer (-1 when it is not one). */
static int64_t global_int(Fix *fx, const char *name)
{
    UValue v = urbi_make_nil();
    if (urbi_global_get(fx->vm, urbi_realm_main(fx->vm), name, &v) != URBI_OK) return -1;
    return v.kind == UVAL_INT ? v.v.i : -1;
}

static USched *sched(Fix *fx) { return uvm_sched(fx->vm); }

/* --- (a) the separators ------------------------------------------------- */

static void separators_run_both_arms(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var a = 0");
    /* `,` detaches the LEFT arm and runs the right one inline as the
     * parent's continuation, so the chunk's value is the right arm's and
     * the detached write lands afterwards -- fire and forget, in that
     * order.  Both have run by the time the pump stops. */
    UValue v = run(&fx, "{ Realm.a = 1 } , { Realm.a = 2 }");
    RT_EQ(v.kind, (uint8_t)UVAL_INT);
    RT_EQ(v.v.i, 2);
    RT_EQ(global_int(&fx, "a"), 1);
    RT_CHECK(sched(&fx)->run_head == NULL);
    RT_CHECK(!urbi_has_live_work(fx.vm));

    /* The ordering is observable from inside: the inline arm reads the
     * value the detached one has not written yet. */
    run(&fx, "var x = 0");
    v = run(&fx, "{ Realm.x = 1 } , { Realm.x + 10 }");
    RT_EQ(v.kind, (uint8_t)UVAL_INT);
    RT_EQ(v.v.i, 10);
    RT_EQ(global_int(&fx, "x"), 1);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (b) `&` joins, and the join really waits ---------------------------- */

static void join_waits_for_the_child(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var x = 0");
    run(&fx, "var y = 0");
    /* The left arm sleeps 10 ms before writing, the right one writes at
     * once.  The emitter runs the LEFT arm inline and only reaches the
     * spawn-and-join once it finishes, so nothing at all has happened
     * while the sleeper is parked. */
    UValue v = run(&fx, "{ sleep(10ms); Realm.x = 1 } & { Realm.y = 2 }");
    RT_EQ(v.kind, (uint8_t)UVAL_NIL);   /* the chunk's strand is parked */
    RT_EQ(global_int(&fx, "x"), 0);
    RT_EQ(global_int(&fx, "y"), 0);
    RT_CHECK(urbi_has_live_work(fx.vm));

    /* Not yet due. */
    RT_EQ(tick(&fx, 5000), URBI_STEP_IDLE_UNTIL);
    RT_EQ(global_int(&fx, "x"), 0);
    /* Due: the sleeper wakes and finishes the left arm, the right arm is
     * spawned and joined, and the chunk runs off the end. */
    RT_EQ(tick(&fx, 6000), URBI_STEP_QUIESCENT);
    RT_EQ(global_int(&fx, "x"), 1);
    RT_EQ(global_int(&fx, "y"), 2);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (c) sleep parks with a deadline ------------------------------------- */

static void sleep_parks_until_its_deadline(void)
{
    Fix fx; fix_open(&fx);
    fx.now_us = 1000;
    run(&fx, "var done = 0");
    run(&fx, "sleep(5ms) | Realm.done = 1");
    RT_EQ(global_int(&fx, "done"), 0);
    /* One sleeper on the heap, due exactly now + 5 ms. */
    RT_EQ(sched(&fx)->heap_len, 1u);
    RT_EQ(sched(&fx)->heap[0].due_us, 1000u + 5000u);
    RT_EQ(sched(&fx)->heap[0].period_us, 0u);
    RT_EQ(sched(&fx)->heap[0].strand->state, (uint8_t)USTRAND_PARKED);

    uint64_t wake = 0;
    RT_EQ(urbi_step(fx.vm, 0, &wake), URBI_STEP_IDLE_UNTIL);
    RT_EQ(wake, 6000u);
    RT_EQ(global_int(&fx, "done"), 0);

    RT_EQ(tick(&fx, 5000), URBI_STEP_QUIESCENT);
    RT_EQ(global_int(&fx, "done"), 1);
    RT_EQ(sched(&fx)->heap_len, 0u);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (d) every() fires on cadence and skips missed ticks ----------------- */

static void every_fires_on_cadence_and_skips_missed_ticks(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var n = 0");
    run(&fx, "every (10ms) { Realm.n = Realm.n + 1 }");
    RT_EQ(global_int(&fx, "n"), 0);
    RT_EQ(sched(&fx)->heap_len, 1u);

    for (int i = 1; i <= 3; i++) {
        RT_EQ(tick(&fx, 10000), URBI_STEP_IDLE_UNTIL);
        RT_EQ(global_int(&fx, "n"), i);
    }
    /* A 100 ms jump is ONE fire, not ten: the periodic re-arms past the
     * clock rather than replaying every window it slept through. */
    RT_EQ(tick(&fx, 100000), URBI_STEP_IDLE_UNTIL);
    RT_EQ(global_int(&fx, "n"), 4);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (e) + (i) a stopped tag takes its periodic with it ------------------ */

static void tag_stop_cancels_its_periodic(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var n = 0");
    run(&fx, "var mytag = Tag.new(\"beat\")");
    run(&fx, "mytag: every (10ms) { Realm.n = Realm.n + 1 }");
    RT_EQ(sched(&fx)->heap_len, 1u);
    RT_EQ(tick(&fx, 10000), URBI_STEP_IDLE_UNTIL);
    RT_EQ(global_int(&fx, "n"), 1);

    run(&fx, "mytag.stop()");
    /* The heap is empty, so the VM has no live work at all -- which is
     * the observable difference between "cancelled" and "still armed but
     * doing nothing". */
    RT_EQ(sched(&fx)->heap_len, 0u);
    RT_CHECK(!urbi_has_live_work(fx.vm));
    RT_EQ(tick(&fx, 100000), URBI_STEP_QUIESCENT);
    RT_EQ(global_int(&fx, "n"), 1);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (f) a gate outranks a fired timer ----------------------------------- */

static void block_holds_a_woken_sleeper(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var done = 0");
    run(&fx, "var t = Tag.new()");
    run(&fx, "t: { sleep(5ms); Realm.done = 1 }");
    RT_EQ(sched(&fx)->heap_len, 1u);
    UStrand *sleeper = sched(&fx)->heap[0].strand;
    RT_CHECK(sleeper != NULL);

    run(&fx, "t.block()");
    /* Past the deadline: the timer fires, but the block gate keeps the
     * strand parked with nothing left to wait for. */
    RT_EQ(tick(&fx, 10000), URBI_STEP_QUIESCENT);
    RT_EQ(global_int(&fx, "done"), 0);
    RT_EQ(sleeper->state, (uint8_t)USTRAND_PARKED);
    RT_EQ(sleeper->gates, (uint8_t)USTRAND_GATE_BLOCKED);

    /* Clearing the last gate is what puts it back on the run queue. */
    run(&fx, "t.unblock()");
    RT_EQ(global_int(&fx, "done"), 1);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* Block and freeze are independent: neither release alone resumes a
 * strand both are holding. */
static void block_and_freeze_are_independent(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var done = 0");
    run(&fx, "var t = Tag.new()");
    run(&fx, "t: { sleep(5ms); Realm.done = 1 }");
    run(&fx, "t.block() | t.freeze()");
    RT_EQ(tick(&fx, 10000), URBI_STEP_QUIESCENT);
    RT_EQ(global_int(&fx, "done"), 0);
    run(&fx, "t.unblock()");
    RT_EQ(global_int(&fx, "done"), 0);
    run(&fx, "t.unfreeze()");
    RT_EQ(global_int(&fx, "done"), 1);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (g) + (k) what a parked strand costs -------------------------------- */

static void a_hundred_sleepers_stay_small(void)
{
    Fix fx; fix_open(&fx);
    size_t before = fx.ca.live;
    /* __detach_strand rather than the `detach` wrapper: the wrapper takes
     * a LAZY argument, and a lazy-taking function called twice in one
     * frame miscompiles today (see the task report).  The primitive is
     * what this case is about anyway. */
    run(&fx, "var mk = function(n) { var i = 0;"
             " while (i < n) { __detach_strand(function() { sleep(1s) }); i = i + 1 } }");
    run(&fx, "mk(100)");
    /* Each detached arm ran far enough to park on its own sleeper. */
    RT_EQ(sched(&fx)->heap_len, 100u);
    size_t cost = fx.ca.live - before;
    printf("    100 sleeping strands: %lu bytes (%lu each)\n",
           (unsigned long)cost, (unsigned long)(cost / 100u));
    RT_CHECK(cost < 100u * 1024u);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (h) + (m) liveness is derived, and wait lists do not count ---------- */

static void wait_list_parkers_are_not_live_work(void)
{
    Fix fx; fix_open(&fx);
    /* A strand parked on an event nobody will ever emit: the run queue is
     * empty and the timer heap is empty, so the VM is quiescent even
     * though a strand exists and will never finish. */
    run(&fx, "var e = Event.new()");
    run(&fx, "__detach_strand(function() { __wait_event(Realm.e) })");
    RT_CHECK(!urbi_has_live_work(fx.vm));
    RT_EQ(urbi_step(fx.vm, 0, NULL), URBI_STEP_QUIESCENT);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (j) a stop from a sibling strand runs the target's finally ---------- */

static void stop_from_a_sibling_runs_the_finally(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var log = \"\"");
    run(&fx, "var t = Tag.new()");
    /* The target parks in a long sleep inside a try/finally; a separate
     * strand stops the tag.  The finally has to run on the TARGET's own
     * stack, at its next safepoint -- not on the stopper's. */
    run(&fx, "__detach_strand(function() {"
             " t: { try { sleep(1h); Realm.log = Realm.log + \"body\" }"
             " finally { Realm.log = Realm.log + \"F\" } } })");
    RT_EQ(sched(&fx)->heap_len, 1u);
    run(&fx, "__detach_strand(function() { t.stop() })");
    RT_EQ(urbi_step(fx.vm, 0, NULL), URBI_STEP_QUIESCENT);

    UValue v = urbi_make_nil();
    RT_EQ(urbi_global_get(fx.vm, urbi_realm_main(fx.vm), "log", &v), URBI_OK);
    RT_CHECK(v.kind == UVAL_STR || v.kind == UVAL_SYM);
    {
        char buf[64];
        urbi_value_to_string(fx.vm, v, buf, sizeof buf);
        RT_STREQ(buf, "\"F\"");     /* the finally ran; the body did not */
    }
    /* The sleeper's timer went with it. */
    RT_EQ(sched(&fx)->heap_len, 0u);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* A stop aimed at a scope the CALLING strand is inside unwinds that
 * strand right where it stands, and execution resumes after the scope. */
static void stop_inside_its_own_scope_resumes_after_it(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var log = \"\"");
    run(&fx, "var t = Tag.new()");
    UValue v = run(&fx, "t: { Realm.log = \"in\"; t.stop(); Realm.log = \"unreached\" } | Realm.log = Realm.log + \"-after\" | 7");
    RT_EQ(v.kind, (uint8_t)UVAL_INT);
    RT_EQ(v.v.i, 7);
    {
        UValue lv = urbi_make_nil();
        char buf[64];
        RT_EQ(urbi_global_get(fx.vm, urbi_realm_main(fx.vm), "log", &lv), URBI_OK);
        urbi_value_to_string(fx.vm, lv, buf, sizeof buf);
        RT_STREQ(buf, "\"in-after\"");
    }
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- (l) an injection from a "fake ISR" wakes a waiting strand ----------- */

/* `waituntil(e?)` is the reactive task's watcher, so the strand parks
 * through a test-registered native instead: __wait_event(e) parks the
 * caller on the event's wait list, which is exactly what an event
 * watcher will do once it exists. */
static int wait_event_native(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)nargs;
    *out = urbi_make_nil();
    if (args[0].kind != UVAL_CELL || ((UCell *)args[0].v.p)->type != UCELL_EVENT)
        return urbi_throw(vm, "TypeError", "__wait_event: not an Event");
    UEvent *e = (UEvent *)args[0].v.p;
    (void)usched_park(vm->sched.current, &e->waiters, 0);
    return URBI_OK;
}

static void an_isr_injection_wakes_a_waiter(void)
{
    Fix fx; fix_open(&fx);
    RT_EQ(urbi_register(fx.vm, "__wait_event", wait_event_native, 1, 1), URBI_OK);

    urbi_event_id_t id = URBI_EVENT_ID_INVALID;
    RT_EQ(urbi_event_register(fx.vm, NULL, "button", &id), URBI_OK);
    /* The registered event has to be reachable from script to be waited
     * on; the id and the value name the same cell. */
    UValue ev = urbi_make_nil();
    RT_EQ(urbi_event_new(fx.vm, NULL, "ignored", &ev), URBI_OK);
    RT_CHECK(uvm_sched(fx.vm)->events[id] != NULL);
    RT_EQ(urbi_global_set(fx.vm, urbi_realm_main(fx.vm), "button",
                          uv_ptr(UV_CELL, uvm_sched(fx.vm)->events[id])), URBI_OK);

    run(&fx, "var woke = 0");
    run(&fx, "__detach_strand(function() { __wait_event(Realm.button); Realm.woke = 1 })");
    RT_EQ(global_int(&fx, "woke"), 0);
    RT_CHECK(!urbi_has_live_work(fx.vm));   /* a wait-list parker is not live work */

    /* The "interrupt": no allocation, no VM state touched beyond the ring. */
    urbi_event_payload_t p;
    memset(&p, 0, sizeof p);
    p.u32[0] = 1;
    RT_EQ(urbi_inject_event(fx.vm, id, &p, sizeof p.u32[0]), URBI_OK);
    RT_EQ(urbi_step(fx.vm, 0, NULL), URBI_STEP_QUIESCENT);
    RT_EQ(global_int(&fx, "woke"), 1);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- yield keeps the queue FIFO ------------------------------------------ */

static void yield_round_robins_in_fifo_order(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var log = \"\"");
    /* Three detached strands, each writing twice with a `;` (which is a
     * yield) between.  FIFO round-robin interleaves them as ABCABC. */
    run(&fx, "__detach_strand(function() { Realm.log = Realm.log + \"A\"; Realm.log = Realm.log + \"A\" })"
             " | __detach_strand(function() { Realm.log = Realm.log + \"B\"; Realm.log = Realm.log + \"B\" })"
             " | __detach_strand(function() { Realm.log = Realm.log + \"C\"; Realm.log = Realm.log + \"C\" })");
    {
        UValue lv = urbi_make_nil();
        char buf[64];
        RT_EQ(urbi_global_get(fx.vm, urbi_realm_main(fx.vm), "log", &lv), URBI_OK);
        urbi_value_to_string(fx.vm, lv, buf, sizeof buf);
        RT_STREQ(buf, "\"ABCABC\"");
    }
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- a realm's connection tag ------------------------------------------- */

static void a_chunk_runs_under_the_connection_tag(void)
{
    Fix fx; fix_open(&fx);
    UValue v = run(&fx, "scopeTag() == Lobby.connectionTag");
    RT_EQ(v.kind, (uint8_t)UVAL_BOOL);
    RT_CHECK(v.v.i != 0);
    /* Stopping it kills the realm's strands: the sleeper never resumes. */
    run(&fx, "var done = 0");
    run(&fx, "__detach_strand(function() { sleep(5ms); Realm.done = 1 })");
    run(&fx, "Lobby.connectionTag.stop()");
    RT_EQ(tick(&fx, 10000), URBI_STEP_QUIESCENT);
    RT_EQ(global_int(&fx, "done"), 0);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- C1/M8: a stopped tag drops EVERY periodic it owns ------------------
 *
 * One entry per tag cannot see this.  A scan that removed records one at a
 * time filled each hole with the heap's last record and sifted it, and a
 * sift UP carries that record above the scan position -- to an index
 * already read -- so a victim landing there survived and went on firing.
 * Seven periodics in this arming order, three of them under `t`, is the
 * shape that produces the sift-up. */
static void tag_stop_drops_every_periodic_it_owns(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var t = Tag.new(\"t\")");
    run(&fx, "var u = Tag.new(\"u\")");
    run(&fx, "var nt = 0");
    run(&fx, "var nu = 0");
    run(&fx, "u: every (12ms) { Realm.nu = Realm.nu + 1 }");
    run(&fx, "t: every (50ms) { Realm.nt = Realm.nt + 1 }");
    run(&fx, "t: every (29ms) { Realm.nt = Realm.nt + 1 }");
    run(&fx, "u: every (23ms) { Realm.nu = Realm.nu + 1 }");
    run(&fx, "u: every (51ms) { Realm.nu = Realm.nu + 1 }");
    run(&fx, "u: every (10ms) { Realm.nu = Realm.nu + 1 }");
    run(&fx, "t: every (14ms) { Realm.nt = Realm.nt + 1 }");
    RT_EQ(sched(&fx)->heap_len, 7u);

    run(&fx, "t.stop()");
    /* Exactly the three t owns leave, and the four u owns stay. */
    RT_EQ(sched(&fx)->heap_len, 4u);
    for (uint32_t i = 0; i < sched(&fx)->heap_len; i++)
        RT_CHECK(sched(&fx)->heap[i].tag != NULL);

    /* The heap property survived the bulk removal, so the next deadline is
     * u's 10 ms one and not whichever record happened to land at the root. */
    uint64_t wake = 0;
    RT_EQ(urbi_step(fx.vm, 0, &wake), URBI_STEP_IDLE_UNTIL);
    RT_EQ(wake, 10000u);

    /* Far past every original deadline: nothing t owned may fire. */
    RT_EQ(tick(&fx, 200000), URBI_STEP_IDLE_UNTIL);
    RT_EQ(global_int(&fx, "nt"), 0);
    RT_EQ(global_int(&fx, "nu"), 4);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- I1: freeing a realm takes its timers with it ----------------------- */
static void freeing_a_realm_drops_its_timers(void)
{
    Fix fx; fix_open(&fx);
    /* The main realm first: the FIRST realm a VM creates becomes it, and
     * freeing the main realm is refused. */
    RT_CHECK(urbi_realm_main(fx.vm) != NULL);
    URealm *r = urbi_realm_new(fx.vm);
    RT_CHECK(r != NULL && r != urbi_realm_main(fx.vm));
    /* Under a USER tag, which is the case realm teardown used to miss:
     * stopping the connection tag reaches connection-tag periodics only. */
    run_on(&fx, r, "var t = Tag.new()");
    run_on(&fx, r, "var n = 0");
    run_on(&fx, r, "t: every (10ms) { Realm.n = Realm.n + 1 }");
    RT_EQ(sched(&fx)->heap_len, 1u);

    urbi_realm_free(fx.vm, r);
    RT_EQ(sched(&fx)->heap_len, 0u);
    RT_CHECK(!urbi_has_live_work(fx.vm));
    /* No body spawns into the dismantled realm, and the VM is done. */
    RT_EQ(tick(&fx, 100000), URBI_STEP_QUIESCENT);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- M11 + I4: a newcomer to a gated scope is gated too ----------------- */

/* A periodic's body is spawned under the tag that armed it, so blocking
 * that tag has to hold the bodies as well as the members already in the
 * scope -- otherwise `t.block()` stops nothing that matters. */
static void spawning_into_a_gated_scope_parks_the_newcomer(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var t = Tag.new()");
    run(&fx, "var n = 0");
    run(&fx, "t: every (10ms) { Realm.n = Realm.n + 1 }");
    run(&fx, "t.block()");

    RT_EQ(tick(&fx, 10000), URBI_STEP_IDLE_UNTIL);
    RT_EQ(global_int(&fx, "n"), 0);          /* the body spawned, gated */
    UStrand *body = urbi_realm_main(fx.vm)->strands;
    RT_CHECK(body != NULL);
    RT_EQ(body->state, (uint8_t)USTRAND_PARKED);
    RT_EQ(body->gates, (uint8_t)USTRAND_GATE_BLOCKED);

    /* I4: asking for it to be queued anyway must not queue it.  This is
     * the last line of defence for the one strand utag_gate cannot reach
     * -- one that is RUNNING but not the dispatching one, the parent of a
     * join whose child ran inline. */
    usched_enqueue(body);
    RT_EQ(body->state, (uint8_t)USTRAND_PARKED);
    RT_CHECK(sched(&fx)->run_head == NULL);
    RT_EQ(global_int(&fx, "n"), 0);

    /* I4, the other guard: a strand already queued is not queued twice. */
    body->gates = 0;
    usched_enqueue(body);
    usched_enqueue(body);
    RT_CHECK(sched(&fx)->run_head == body);
    RT_CHECK(sched(&fx)->run_tail == body);
    RT_CHECK(body->link == NULL);

    run(&fx, "t.unblock()");
    RT_EQ(global_int(&fx, "n"), 1);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* Entering the scope through OP_PUSH_TAG is the other way in. */
static void entering_a_gated_scope_parks_the_entrant(void)
{
    Fix fx; fix_open(&fx);
    run(&fx, "var t = Tag.new()");
    run(&fx, "var mark = 0");
    run(&fx, "t.block()");
    /* The chunk's own strand parks at the PUSH_TAG and hands back nil. */
    UValue v = run(&fx, "t: { Realm.mark = 1 } | 9");
    RT_EQ(v.kind, (uint8_t)UVAL_NIL);
    RT_EQ(global_int(&fx, "mark"), 0);
    RT_CHECK(!urbi_has_live_work(fx.vm));   /* gated, so not runnable */

    run(&fx, "t.unblock()");
    RT_EQ(global_int(&fx, "mark"), 1);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

/* --- I2: a detached strand's uncaught throw reaches both channels ------- */
static int diag_hits;
static char diag_last[128];
static void count_diag(UVM *vm, void *ud, int level, const char *msg, size_t len)
{
    (void)vm; (void)ud; (void)level;
    diag_hits++;
    size_t n = len < sizeof diag_last - 1 ? len : sizeof diag_last - 1;
    memcpy(diag_last, msg, n);
    diag_last[n] = '\0';
}

static void a_detached_throw_reaches_both_channels(void)
{
    Fix fx; fix_open(&fx);
    diag_hits = 0;
    diag_last[0] = '\0';
    urbi_set_diag(fx.vm, count_diag, NULL);
    run(&fx, "var b = 0");

    /* The chunk itself succeeds -- its value is the inline arm's -- so the
     * return code cannot carry the detached arm's failure.  Both other
     * channels must. */
    UValue v = run(&fx, "{ throw 1 } , { Realm.b = 2 }");
    RT_EQ(v.kind, (uint8_t)UVAL_INT);
    RT_EQ(v.v.i, 2);
    RT_EQ(diag_hits, 1);
    RT_STREQ(diag_last, "1");
    UErrorInfo info;
    RT_EQ(urbi_last_error(fx.vm, &info), URBI_ERR_UNCAUGHT_THROW);
    RT_STREQ(info.message, "1");

    /* And the channel describes the CURRENT step, so the next one clears
     * it rather than leaving a stale failure standing. */
    RT_EQ(urbi_step(fx.vm, 0, NULL), URBI_STEP_QUIESCENT);
    RT_EQ(urbi_last_error(fx.vm, &info), URBI_OK);

    /* A chunk's OWN throw is not double-reported: its caller is handed the
     * same failure as a return code. */
    diag_hits = 0;
    char err[256] = { 0 };
    UValue out = urbi_make_nil();
    const char *src = "throw 2";
    RT_EQ(urbi_run(fx.vm, urbi_realm_main(fx.vm), src, strlen(src), NULL, &out, err, sizeof err),
          URBI_ERR_UNCAUGHT_THROW);
    RT_EQ(diag_hits, 0);
    fix_close(&fx);
    RT_EQ(fx.ca.live, 0u);
}

RT_SUITE(rt_sched_suite) {
    rt_run("separators_run_both_arms", separators_run_both_arms);
    rt_run("join_waits_for_the_child", join_waits_for_the_child);
    rt_run("sleep_parks_until_its_deadline", sleep_parks_until_its_deadline);
    rt_run("every_fires_on_cadence_and_skips_missed_ticks", every_fires_on_cadence_and_skips_missed_ticks);
    rt_run("tag_stop_cancels_its_periodic", tag_stop_cancels_its_periodic);
    rt_run("tag_stop_drops_every_periodic_it_owns", tag_stop_drops_every_periodic_it_owns);
    rt_run("freeing_a_realm_drops_its_timers", freeing_a_realm_drops_its_timers);
    rt_run("spawning_into_a_gated_scope_parks_the_newcomer", spawning_into_a_gated_scope_parks_the_newcomer);
    rt_run("entering_a_gated_scope_parks_the_entrant", entering_a_gated_scope_parks_the_entrant);
    rt_run("a_detached_throw_reaches_both_channels", a_detached_throw_reaches_both_channels);
    rt_run("block_holds_a_woken_sleeper", block_holds_a_woken_sleeper);
    rt_run("block_and_freeze_are_independent", block_and_freeze_are_independent);
    rt_run("a_hundred_sleepers_stay_small", a_hundred_sleepers_stay_small);
    rt_run("wait_list_parkers_are_not_live_work", wait_list_parkers_are_not_live_work);
    rt_run("stop_from_a_sibling_runs_the_finally", stop_from_a_sibling_runs_the_finally);
    rt_run("stop_inside_its_own_scope_resumes_after_it", stop_inside_its_own_scope_resumes_after_it);
    rt_run("an_isr_injection_wakes_a_waiter", an_isr_injection_wakes_a_waiter);
    rt_run("yield_round_robins_in_fifo_order", yield_round_robins_in_fifo_order);
    rt_run("a_chunk_runs_under_the_connection_tag", a_chunk_runs_under_the_connection_tag);
}
