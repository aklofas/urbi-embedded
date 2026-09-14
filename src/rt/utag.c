/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/utag.c — tags, events, and cross-strand stop.
 *
 * A tag is an identity.  It keeps no member list: the strands in its
 * scope are exactly those whose scope chain contains it, and that chain
 * is read off the strand itself (its ambient tag plus the tag every
 * TAG_SCOPE cleanup entry saved on the way in).  Nothing has to be
 * unlinked when a strand pushes, pops, unwinds or dies, which is what
 * made the old per-tag member lists a source of dangling entries.
 *
 * Cross-strand stop never runs on another strand's stack.  utag_stop
 * marks every member `unwind = STOP` with the tag in `transfer` and wakes
 * the parked ones; each strand consumes that at its own next safepoint
 * (uexec_run_inner checks on entry), walks its own cleanup stack, runs
 * its own finallys, and resumes after the matching tag scope.
 *
 * Exec-rank (see rt/usched.h): it reaches vm->realms and the exec core. */

#include "rt/uexec.h"

/* --- construction --------------------------------------------------------- */

UTag *utag_new(UVM *vm, UValue name)
{
    UTag *t = (UTag *)ugc_alloc(vm, UCELL_TAG, sizeof(UTag));
    if (!t) return NULL;
    t->name = name;
    return t;
}

UEvent *uevent_new(UVM *vm, UValue name)
{
    UEvent *e = (UEvent *)ugc_alloc(vm, UCELL_EVENT, sizeof(UEvent));
    if (!e) return NULL;
    e->name = name;
    return e;
}

void utag_trace(UVM *vm, UTag *t)
{
    if (t->enter) ugc_mark(vm, &t->enter->cell);
    if (t->leave) ugc_mark(vm, &t->leave->cell);
    ugc_mark_value(vm, t->name);
}

void uevent_trace(UVM *vm, UEvent *e)
{
    for (UStrand *s = e->waiters; s; s = s->link) ugc_mark(vm, &s->cell);
    ugc_mark_value(vm, e->name);
}

/* --- events ---------------------------------------------------------------- */

void uevent_emit(UVM *vm, UEvent *e, UValue payload)
{
    if (e == NULL) return;
    (void)vm;
    /* Detach the whole list first: a woken strand must not be able to
     * re-park onto the list being walked. */
    UStrand *w = e->waiters;
    e->waiters = NULL;
    while (w) {
        UStrand *next = w->link;
        w->link = NULL;
        w->waiting_on = NULL;
        usched_wake(w, payload);
        w = next;
    }
    /* The reactive task fans out to condition/event watchers here. */
}

UEvent *utag_enter_event(UVM *vm, UTag *t)
{
    if (t->enter == NULL) t->enter = uevent_new(vm, uv_nil());
    return t->enter;
}

UEvent *utag_leave_event(UVM *vm, UTag *t)
{
    if (t->leave == NULL) t->leave = uevent_new(vm, uv_nil());
    return t->leave;
}

void utag_fire(UVM *vm, UEvent *ev)
{
    /* A tag nobody ever asked for an enter/leave event on has none, so
     * entering and leaving its scope costs nothing at all. */
    if (ev) uevent_emit(vm, ev, uv_nil());
}

/* --- scope membership -------------------------------------------------------
 *
 * The chain is `s->tag` (the innermost active tag) plus the tag each
 * TAG_SCOPE entry saved when it took over — which is the enclosing tag at
 * that depth, down to the one the strand inherited at spawn.  Together
 * those are every tag currently covering the strand, with no duplicates
 * to reason about. */
bool utag_covers(const UStrand *s, const UTag *t)
{
    if (t == NULL || s == NULL) return false;
    if (s->tag == t) return true;
    for (uint16_t i = 0; i < s->ncleanup; i++) {
        const UCleanup *c = &s->cleanup[i];
        if (c->kind != (uint8_t)UCLEAN_TAG_SCOPE) continue;
        if (c->saved.kind == UV_CELL && (const UTag *)c->saved.v.p == t) return true;
    }
    return false;
}

/* --- stop -------------------------------------------------------------------- */

int utag_stop(UVM *vm, UTag *t)
{
    USched *sc = uvm_sched(vm);
    int hit_current = 0;
    if (t == NULL) return 0;

    for (URealm *r = vm->realms; r; r = r->next) {
        for (UStrand *s = r->strands; s; s = s->next_in_realm) {
            if (s->state == USTRAND_DEAD || !utag_covers(s, t)) continue;
            s->unwind = (uint8_t)UUNWIND_STOP;
            s->transfer = uv_ptr(UV_CELL, t);
            if (s == sc->current) { hit_current = 1; continue; }
            /* A gate must not outlive a cancellation: the point of stop
             * is that the strand gets to run its cleanup. */
            s->gates = 0;
            if (s->state == USTRAND_PARKED) usched_wake(s, uv_nil());
        }
    }
    usched_timers_drop_tag(vm, t);
    return hit_current;
}

/* --- gates --------------------------------------------------------------------
 *
 * block and freeze are independent bits on each member strand.  A gated
 * strand sits PARKED with nothing to wait for; clearing the LAST bit is
 * what puts it back on the run queue, and only when it was not also
 * waiting for something else (a wait list, or a timer that has not
 * fired). */
void utag_gate(UVM *vm, UTag *t, uint8_t bit, bool on)
{
    USched *sc = uvm_sched(vm);
    if (t == NULL) return;
    t->flags = on ? (uint8_t)(t->flags | bit) : (uint8_t)(t->flags & ~bit);

    for (URealm *r = vm->realms; r; r = r->next) {
        for (UStrand *s = r->strands; s; s = s->next_in_realm) {
            if (s->state == USTRAND_DEAD || !utag_covers(s, t)) continue;
            if (on) {
                s->gates = (uint8_t)(s->gates | bit);
                /* Through the primitives, not by writing the state here:
                 * park is the one way a strand leaves the run queue.  A
                 * queued strand is never a spare and never inside a call
                 * boundary, so the park cannot be refused. */
                if (s->state == USTRAND_READY) {
                    usched_unqueue(s);
                    (void)usched_park(s, NULL, 0);
                } else if (s == sc->current) {
                    (void)usched_park(s, NULL, 0);
                }
            } else {
                s->gates = (uint8_t)(s->gates & ~bit);
                if (s->gates == 0 && s->state == USTRAND_PARKED
                    && s->waiting_on == NULL && !usched_has_timer(vm, s))
                    usched_enqueue(s);
            }
        }
    }
}
