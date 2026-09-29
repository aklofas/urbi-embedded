/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/usched.h — the scheduler for the refound/core runtime: one run
 * queue, one timer heap, park/wake, tags and events.
 *
 * WHY THIS HEADER SITS BELOW uexec.  `USched` is a by-value member of
 * `struct UVM`, so its shape has to be complete before uexec.h completes
 * the VM.  The scheduler's BEHAVIOUR, though, drives the exec core
 * (usched_step calls uexec_run), so usched.c / utag.c / usched_natives.c
 * are exec-rank implementation files and include rt/uexec.h.  The
 * layering gate (tests/scripts/check_rt_layering.sh) names them
 * explicitly for that reason.
 *
 * STATES.  READY (on the run queue, FIFO through UStrand.link), RUNNING
 * (the one strand in dispatch), PARKED (on the timer heap, on one wait
 * list, or nowhere at all when only a gate holds it), DEAD (on the dead
 * list until the next step reaps it).  There is no SUSPENDED state and
 * no reason nibble: block and freeze are gate BITS on the strand, and a
 * gated strand is simply PARKED with nothing to wait for.
 *
 * LIVENESS is derived, never counted: a VM has live work when the run
 * queue is non-empty or the timer heap holds a timer.  Wait lists do not
 * count — a program whose every strand is waiting for an event that can
 * no longer arrive is quiescent, which is the ratified rule. */

#ifndef URT_SCHED_H
#define URT_SCHED_H
#include "rt/ustrand.h"

struct URealm;
struct UWatcher;   /* completed by rt/uwatch.h, which sits above this header */

/* --- tags and events --------------------------------------------------
 *
 * Both are GC cells with no per-cell prototype pointer: uv_dispatch_proto
 * already maps UCELL_TAG / UCELL_EVENT / UCELL_STRAND onto vm->protos[],
 * so a second copy on every cell would only be a way for the two to
 * drift apart. */

/* A tag is an identity plus two gate bits.  It owns no member list: the
 * strands in its scope are found by walking every strand's scope chain
 * (see utag_covers), which keeps membership correct for free when a
 * strand pushes, pops, unwinds or dies. */
typedef struct UTag {
    UCell    cell;
    struct UEvent *enter, *leave;   /* allocated on first use, not at PUSH_TAG */
    UValue   name;                  /* a string, or nil for an anonymous tag */
    uint8_t  flags;                 /* UTAG_F_* */
} UTag;

#define UTAG_F_BLOCKED 0x1
#define UTAG_F_FROZEN  0x2

/* An event is an identity plus one intrusive wait list.  Strands park on
 * `waiters` threaded through their own UStrand.link. */
typedef struct UEvent {
    UCell    cell;
    UStrand *waiters;               /* strands parked in `waituntil (e?)` */
    /* Watchers subscribed through `at (e?)` and friends, threaded via
     * UWatcher.next_on_event.  The watchers are rooted by the VM's watch
     * list, so this list is a membership record and not a trace edge. */
    struct UWatcher *watchers;
    UValue   name;                  /* a string, or nil */
} UEvent;

/* --- timers -----------------------------------------------------------
 *
 * One array-backed binary min-heap on due_us carries both kinds:
 *
 *   period_us == 0 — a SLEEPER.  `strand` is the parked strand; firing
 *                    wakes it and drops the record.
 *   period_us != 0 — a PERIODIC.  Firing spawns `body` under `tag` in
 *                    `realm` and re-arms at due += period, skipping
 *                    missed ticks (the ratified cadence: a clock jump
 *                    produces one fire, not one per elapsed period).
 *
 * The record is not a GC cell; usched_mark traces its fields. */
typedef struct UTimer {
    uint64_t     due_us, period_us;
    UStrand     *strand;
    UClosure    *body;
    UTag        *tag;
    struct URealm *realm;
} UTimer;

/* --- the ISR ring -----------------------------------------------------
 *
 * urbi_inject_event is the one entry point an interrupt handler may
 * call.  It is allocation-free and lock-free for a single producer: the
 * handler publishes into ring[head] and releases `head`; usched_step
 * consumes from `tail`.  A full ring drops the newest injection, which
 * is the only behaviour available without blocking an ISR. */
#define USCHED_ISR_SLOTS   16
#define USCHED_MAX_EVENTS  32    /* ids handed out by urbi_event_register */

typedef struct UIsrRec { uint16_t id; uint8_t n; urbi_event_payload_t payload; } UIsrRec;

typedef struct USched {
    struct UStrand *run_head, *run_tail;   /* FIFO run queue, linked via UStrand.link */
    struct UStrand *current;               /* the one strand in dispatch, or NULL */
    struct UStrand *dead;                  /* died since the last reap, linked via .link */

    UTimer  *heap; uint32_t heap_len, heap_cap;

    /* Events reachable by id from an ISR.  Only urbi_event_register puts
     * an event here; a script's `Event.new()` stays anonymous and
     * collectable. */
    UEvent  *events[USCHED_MAX_EVENTS];
    uint16_t event_count;

    UIsrRec  isr[USCHED_ISR_SLOTS];
    uint32_t isr_head, isr_tail;

    /* The strand a caller is waiting on for the length of a pump.  Its
     * death IS the caller's return code, so it is the one strand whose
     * uncaught throw does not also go to the diag hook -- that channel
     * exists for strands nobody is waiting on. */
    struct UStrand *awaited;

    /* A strand nobody awaited died on an uncaught throw during this step.
     * It keeps the clean-run clear in uexec_finish_run from wiping the
     * message before the host reads it, and the next usched_step clears
     * both. */
    uint8_t  threw;

    /* Monotonic fallback for a VM with no clock hook installed: one
     * microsecond per usched_step.  Timers still fire, just slowly; a
     * host that cares installs urbi_set_clock. */
    uint64_t fallback_now_us;

    /* UVMConfig.step_budget: what urbi_step spends when its caller passes
     * 0.  Zero here keeps the original meaning of 0 -- run until nothing
     * is runnable -- so a host that never sets it sees no change.  Read
     * only by urbi_step; the internal pumps pass their own budget. */
    uint32_t default_budget;
} USched;

/* Instructions handed to one strand before the scheduler looks at the
 * queue again.  Only backward jumps consume budget (one decrement, in
 * uexec_ops.c), so a straight-line strand runs to its next park or death
 * regardless. */
#define USCHED_SLICE 256u

/* Accessor into the owning VM, defined by the layer above (uexec.c) —
 * same pattern as uvm_gc/uvm_strings/uvm_objstats. */
USched *uvm_sched(struct UVM *vm);

/* --- queue and park/wake ---------------------------------------------- */

/* Put a strand at the tail of the run queue.  A strand that is already
 * queued, or DEAD, is left alone; one held by a gate parks instead, so
 * this is also the last line of defence against a blocked strand being
 * scheduled. */
void usched_enqueue(UStrand *s);

/* Take a READY strand back off the run queue without changing its state
 * — what a gate does to a strand that has not had its turn yet. */
void usched_unqueue(UStrand *s);

/* Park the running strand.
 *
 *   waitlist  — address of an intrusive `UStrand *` head (an event's
 *               `waiters`, a strand's `joiners`), or NULL.
 *   wake_us   — absolute deadline for a timed park, or 0 for none.
 *
 * WHAT KEEPS THE WAITED-ON OBJECT ALIVE.  Nothing here does: `waiting_on`
 * is a raw pointer into a cell's interior and the trace edge runs the
 * other way (an event marks its waiters, not the reverse).  The rule in
 * force is that the caller must hold the object somewhere ustrand_trace
 * reaches -- in practice a native's argument register, which is inside
 * the parked strand's marked register window.  Anything parked on an
 * object held only by a C local is a dangling wait.
 *
 * NULL waitlist with wake_us == 0 is a gate park: the strand waits for
 * utag_block/freeze to be cleared.  Returns 0 when the strand parked and
 * -1 when it may not: a spare strand and any strand inside a synchronous
 * uexec_call boundary run to completion by contract, so for them a park
 * request is a no-op the caller reports as an immediate return. */
int  usched_park(UStrand *s, UStrand **waitlist, uint64_t wake_us);

/* Whether `s` may be handed back to the scheduler at all right now.
 * False for a spare strand and for any strand inside a synchronous
 * uexec_call boundary: a C frame is waiting for that call's value, and
 * returning to the scheduler mid-call would deliver a stale one.  Both
 * park and OP_YIELD ask this -- a `;` inside a comparator, a getter or
 * an operator overload has to be a plain sequence point, not a
 * deschedule. */
bool usched_may_deschedule(const UStrand *s);

/* Take a strand off whatever it is waiting on and make it READY — unless
 * a gate bit still holds it, in which case it stays PARKED with nothing
 * to wait for and the gate's release enqueues it.  A non-nil payload is
 * delivered through s->transfer when no unwind is pending; NOTHING READS
 * IT BACK yet, so the reactive task choosing where a woken watcher picks
 * its value up is designing that seam, not inheriting it.  Safe on a DEAD
 * or already-running strand (no-op). */
void usched_wake(UStrand *s, UValue payload);

/* The one spawn path: allocate a strand in `realm`, give it `tag` as its
 * ambient tag, push `cl`'s frame with `recv` and `argv`, and enqueue it.
 * NULL on OOM.  May collect — root `cl` and `recv` first. */
UStrand *usched_spawn(struct UVM *vm, struct URealm *realm, UClosure *cl, UTag *tag,
                      UValue recv, const UValue *argv, uint8_t argc);

/* --- timers ------------------------------------------------------------ */

int      usched_timer_add(struct UVM *vm, UTimer t);      /* 0 ok, -1 OOM */
uint64_t usched_now(struct UVM *vm);
/* True while `s` still has an unfired sleeper on the heap — the one
 * thing that distinguishes "parked only by a gate" from "asleep". */
bool     usched_has_timer(struct UVM *vm, const UStrand *s);
/* Drops every periodic owned by `tag`.  Sleepers are untouched: a
 * sleeping member is stopped through its own unwind, not by deleting its
 * wake-up. */
void     usched_timers_drop_tag(struct UVM *vm, const UTag *tag);
/* Drops every record belonging to `realm`, sleepers included.  Realm
 * teardown needs this: stopping the connection tag only reaches
 * connection-tag periodics, and one armed under a USER tag in that realm
 * would keep firing bodies into a realm that no longer has globals. */
void     usched_timers_drop_realm(struct UVM *vm, const struct URealm *realm);

/* --- stepping ---------------------------------------------------------- */

/* Named apart from the public UStepResult in <urbi/urbi.h>: same three
 * values, but the core does not include the public header. */
typedef enum { USTEP_RAN = 0, USTEP_IDLE_UNTIL = 1, USTEP_QUIESCENT = 2 } USchedStep;

/* One scheduler slice: reap the dead, drain the ISR ring, fire every
 * timer due at the clock reading taken on entry, then run the run queue
 * until it empties or `budget` instructions have been handed out
 * (budget 0 = unbounded).
 *
 * USTEP_RAN        — the budget ran out with strands still READY.
 * USTEP_IDLE_UNTIL — nothing READY, a timer pending; *next_wake_us gets
 *                    its deadline when the pointer is non-NULL.
 * USTEP_QUIESCENT  — no runnable strand and no timer. */
USchedStep usched_step(struct UVM *vm, uint32_t budget, uint64_t *next_wake_us);

/* Run queue non-empty || timer heap non-empty.  Wait lists do not count. */
bool usched_has_live_work(struct UVM *vm);

/* Drive `s` to death right here, on this C stack.  The one caller is
 * JOIN_WAIT reached from a context that may not park (a spare strand, or
 * inside a synchronous uexec_call): the join still has to mean "wait", so
 * the child runs nested instead.  Returns early if the child parks on
 * something this nested run cannot deliver. */
void usched_run_inline(struct UVM *vm, UStrand *s);

/* GC: the run queue, the dispatching strand, the dead list, every timer
 * record's fields, and the id-registered events. */
void usched_mark(struct UVM *vm);

/* --- tags -------------------------------------------------------------- */

UTag   *utag_new(struct UVM *vm, UValue name);
/* True when `t` is anywhere on `s`'s scope chain: its ambient tag plus
 * the tag every TAG_SCOPE cleanup entry saved on its way in. */
bool    utag_covers(const UStrand *s, const UTag *t);
/* Mark every strand in `t`'s scope for a STOP unwind and wake the parked
 * ones, then drop `t`'s periodics.  Never unwinds another strand on this
 * strand's stack: each one runs its own cleanup at its next safepoint.
 * Returns 1 when the calling strand itself was marked (its caller must
 * return UEXEC_THROW so the dispatch loop unwinds), 0 otherwise. */
int     utag_stop(struct UVM *vm, UTag *t);
void    utag_gate(struct UVM *vm, UTag *t, uint8_t bit, bool on);
/* The strand gate bits a newcomer to `t`'s scope inherits.  UTAG_F_* and
 * USTRAND_GATE_* share their values, but the two live in different
 * headers and this is the one place that relies on it. */
static inline uint8_t utag_gate_bits(const UTag *t)
{ return t ? (uint8_t)(t->flags & (UTAG_F_BLOCKED | UTAG_F_FROZEN)) : 0u; }
/* Every gate bit `s` is under right now, read off the tags that cover
 * it -- its ambient tag plus each tag an enclosing scope displaced. */
uint8_t utag_strand_gate_bits(const UStrand *s);
/* The tag's enter / leave event, allocated on first ask.  NULL on OOM. */
UEvent *utag_enter_event(struct UVM *vm, UTag *t);
UEvent *utag_leave_event(struct UVM *vm, UTag *t);
/* Emits `ev` if it exists; a tag nobody subscribed to costs nothing. */
void    utag_fire(struct UVM *vm, UEvent *ev);
void    utag_trace(struct UVM *vm, UTag *t);

/* --- events ------------------------------------------------------------ */

UEvent *uevent_new(struct UVM *vm, UValue name);
/* Fans `payload` out to every watcher on `e`, then wakes every waiter
 * with it.  `sync` makes an AT_SYNC subscriber run its body inline before
 * this returns (what `e.syncEmit(p)` means); every other subscriber, and
 * every subscriber of a plain `e.emit(p)`, gets a spawned body strand. */
void    uevent_emit_to(struct UVM *vm, UEvent *e, UValue payload, bool sync);
static inline void uevent_emit(struct UVM *vm, UEvent *e, UValue payload)
{ uevent_emit_to(vm, e, payload, false); }
void    uevent_trace(struct UVM *vm, UEvent *e);

/* --- provided by the layer above (uexec) ------------------------------- */

/* Run a strand until it parks, dies, or the budget is spent; returns its
 * new state.  Declared here as well as in rt/uexec.h because the
 * scheduler calls it from below that header — the same arrangement as
 * uvm_sched above, in the other direction. */
int uexec_run(struct UVM *vm, UStrand *s, uint32_t budget);

#endif
