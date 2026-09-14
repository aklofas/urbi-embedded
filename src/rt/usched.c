/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/usched.c — run queue, timer heap, park/wake, spawn and step.
 * See rt/usched.h for the model; rt/utag.c carries tags and events.
 *
 * This file is exec-rank: the scheduler's data lives below uexec (USched
 * is a by-value member of UVM) but its behaviour drives the exec core,
 * so it includes rt/uexec.h.  tests/scripts/check_rt_layering.sh names
 * it in the exec-rank list for exactly that reason. */

#include "rt/uexec.h"

/* --- the run queue ------------------------------------------------------ */

void usched_enqueue(UStrand *s)
{
    if (s == NULL || s->state == USTRAND_DEAD) return;
    USched *sc = uvm_sched(s->vm);
    /* Already queued?  The tail's link is NULL and every earlier entry's
     * points at its successor, and every path that takes a strand off a
     * queue or a wait list clears `link` -- so a strand that is NOT
     * queued always arrives here with it NULL.  O(1), no second copy of
     * the membership fact to keep in step. */
    if (s == sc->run_tail || s->link != NULL) return;
    /* Held by a gate.  utag_gate cannot reach a strand that is RUNNING
     * but not the DISPATCHING one -- the parent of a join whose child ran
     * inline -- so such a strand keeps its gate bit and comes back READY,
     * and this is the last place that can stop it running while blocked.
     * It parks instead; the gate's release is what enqueues it.  One that
     * is already PARKED is left exactly as it is, because it may be on a
     * wait list or a timer that this call knows nothing about. */
    if (s->gates != 0) {
        if (s->state != USTRAND_PARKED) {
            s->state = USTRAND_PARKED;
            s->waiting_on = NULL;
            s->wake_us = 0;
            s->link = NULL;
        }
        return;
    }
    s->state = USTRAND_READY;
    s->waiting_on = NULL;
    s->wake_us = 0;
    s->link = NULL;
    if (sc->run_tail) sc->run_tail->link = s; else sc->run_head = s;
    sc->run_tail = s;
}

void usched_unqueue(UStrand *s)
{
    if (s == NULL) return;
    USched *sc = uvm_sched(s->vm);
    UStrand *prev = NULL;
    for (UStrand *c = sc->run_head; c; prev = c, c = c->link) {
        if (c != s) continue;
        if (prev) prev->link = s->link; else sc->run_head = s->link;
        if (sc->run_tail == s) sc->run_tail = prev;
        s->link = NULL;
        return;
    }
}

static UStrand *usched_dequeue(USched *sc)
{
    UStrand *s = sc->run_head;
    if (!s) return NULL;
    sc->run_head = s->link;
    if (sc->run_head == NULL) sc->run_tail = NULL;
    s->link = NULL;
    return s;
}

/* --- the timer heap ------------------------------------------------------
 *
 * A plain array-backed binary min-heap on due_us.  Removal of an interior
 * entry (a sleeper woken early, a periodic whose tag was stopped) finds
 * the entry by linear scan: heaps here hold a handful of records, and the
 * alternative — a back-pointer from every strand and closure into the
 * array — would have to be rewritten on every sift. */

static void heap_swap(UTimer *a, UTimer *b) { UTimer t = *a; *a = *b; *b = t; }

static void heap_up(USched *sc, uint32_t i)
{
    while (i > 0) {
        uint32_t p = (i - 1) / 2;
        if (sc->heap[p].due_us <= sc->heap[i].due_us) break;
        heap_swap(&sc->heap[p], &sc->heap[i]);
        i = p;
    }
}

static void heap_down(USched *sc, uint32_t i)
{
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < sc->heap_len && sc->heap[l].due_us < sc->heap[m].due_us) m = l;
        if (r < sc->heap_len && sc->heap[r].due_us < sc->heap[m].due_us) m = r;
        if (m == i) break;
        heap_swap(&sc->heap[m], &sc->heap[i]);
        i = m;
    }
}

int usched_timer_add(UVM *vm, UTimer t)
{
    USched *sc = uvm_sched(vm);
    if (sc->heap_len == sc->heap_cap) {
        uint32_t cap = sc->heap_cap ? sc->heap_cap * 2 : 4;
        UTimer *h = (UTimer *)ugc_raw_realloc(vm, sc->heap,
                                              (size_t)sc->heap_cap * sizeof(UTimer),
                                              (size_t)cap * sizeof(UTimer));
        if (!h) return -1;
        sc->heap = h;
        sc->heap_cap = cap;
    }
    sc->heap[sc->heap_len] = t;
    heap_up(sc, sc->heap_len);
    sc->heap_len++;
    return 0;
}

static void heap_remove_at(USched *sc, uint32_t i)
{
    sc->heap_len--;
    if (i != sc->heap_len) {
        sc->heap[i] = sc->heap[sc->heap_len];
        heap_down(sc, i);
        heap_up(sc, i);
    }
}

/* The heap index of `s`'s sleeper record, or -1 when it has none. */
static int heap_find_sleeper(const USched *sc, const UStrand *s)
{
    for (uint32_t i = 0; i < sc->heap_len; i++)
        if (sc->heap[i].period_us == 0 && sc->heap[i].strand == s) return (int)i;
    return -1;
}

bool usched_has_timer(UVM *vm, const UStrand *s)
{
    return heap_find_sleeper(uvm_sched(vm), s) >= 0;
}

/* Finish a bulk removal: heap[0 .. w) are the keepers, in whatever order
 * the scan left them, so rebuild the heap property bottom-up.
 *
 * A scan that called heap_remove_at per victim would MISS records.  That
 * routine fills the hole with the array's LAST entry and sifts it, and a
 * sift UP carries it above the scan position -- to an index the scan has
 * already read -- so a victim that lands there survives.  It is not a
 * rare interleaving: three periodics under one tag among seven arm
 * orders is enough. */
static void usched_heap_rebuild(USched *sc, uint32_t w)
{
    if (w == sc->heap_len) return;
    sc->heap_len = w;
    for (uint32_t i = w / 2; i > 0; i--) heap_down(sc, i - 1);
}

void usched_timers_drop_tag(UVM *vm, const UTag *tag)
{
    USched *sc = uvm_sched(vm);
    if (tag == NULL) return;
    uint32_t w = 0;
    for (uint32_t i = 0; i < sc->heap_len; i++)
        if (!(sc->heap[i].period_us != 0 && sc->heap[i].tag == tag))
            sc->heap[w++] = sc->heap[i];
    usched_heap_rebuild(sc, w);
}

void usched_timers_drop_realm(UVM *vm, const URealm *realm)
{
    USched *sc = uvm_sched(vm);
    if (realm == NULL) return;
    uint32_t w = 0;
    for (uint32_t i = 0; i < sc->heap_len; i++)
        if (sc->heap[i].realm != realm) sc->heap[w++] = sc->heap[i];
    usched_heap_rebuild(sc, w);
}

/* --- the clock ---------------------------------------------------------- */

uint64_t usched_now(UVM *vm)
{
    if (vm->clock_us) return vm->clock_us(vm->clock_ud);
    return uvm_sched(vm)->fallback_now_us;
}

/* --- park and wake ------------------------------------------------------ */

bool usched_may_deschedule(const UStrand *s)
{
    if (s->is_spare) return false;
    for (uint16_t i = 0; i < s->nframes; i++)
        if (s->frames[i].is_boundary) return false;
    return true;
}

int usched_park(UStrand *s, UStrand **waitlist, uint64_t wake_us)
{
    if (s == NULL || !usched_may_deschedule(s)) return -1;
    s->state = USTRAND_PARKED;
    s->waiting_on = (void *)waitlist;
    s->wake_us = wake_us;
    s->link = NULL;
    if (waitlist) { s->link = *waitlist; *waitlist = s; }
    return 0;
}

/* Detach `s` from the wait list it is on, if any. */
static void usched_unlink_wait(UStrand *s)
{
    UStrand **head = (UStrand **)s->waiting_on;
    if (head == NULL) return;
    for (UStrand **pp = head; *pp; pp = &(*pp)->link) {
        if (*pp == s) { *pp = s->link; break; }
    }
    s->waiting_on = NULL;
    s->link = NULL;
}

void usched_wake(UStrand *s, UValue payload)
{
    if (s == NULL || s->state != USTRAND_PARKED) return;
    usched_unlink_wait(s);
    {
        USched *sc = uvm_sched(s->vm);
        int i = heap_find_sleeper(sc, s);
        if (i >= 0) heap_remove_at(sc, (uint32_t)i);
    }
    s->wake_us = 0;
    /* The payload rides in `transfer`, which an in-flight unwind owns —
     * never overwrite one. */
    if (payload.kind != UV_NIL && s->unwind == UUNWIND_NONE) s->transfer = payload;
    /* A gate still held leaves the strand parked with nothing to wait
     * for; clearing the last gate is what enqueues it. */
    if (s->gates != 0) { s->link = NULL; return; }
    usched_enqueue(s);
}

/* --- spawning ------------------------------------------------------------ */

/* Undo the realm linkage for a child whose stack or frame allocation
 * failed.  Left on the list it would be a permanent zombie with no
 * frames, and stopping its tag later would enqueue something dispatch
 * cannot run. */
static UStrand *usched_spawn_failed(URealm *realm, const UStrand *c)
{
    for (UStrand **pp = &realm->strands; *pp; pp = &(*pp)->next_in_realm) {
        if (*pp == c) { *pp = c->next_in_realm; break; }
    }
    return NULL;
}

UStrand *usched_spawn(UVM *vm, URealm *realm, UClosure *cl, UTag *tag,
                      UValue recv, const UValue *argv, uint8_t argc)
{
    if (cl == NULL || cl->proto == NULL || realm == NULL) return NULL;
    UProto *p = cl->proto;
    if (p->arity_prologue ? (argc > p->nparams) : (argc != p->nparams)) return NULL;

    UStrand *c = ustrand_new(vm, realm);
    if (!c) return NULL;
    /* Reachable before anything else allocates: the realm is a fixed
     * root and threading the strand onto it is what keeps it alive. */
    c->next_in_realm = realm->strands;
    realm->strands = c;
    c->tag = tag;

    if (ustrand_ensure_stack(c, (uint32_t)argc + (uint32_t)p->max_reg + 1u) != 0)
        return usched_spawn_failed(realm, c);
    for (uint8_t k = 0; k < argc; k++) c->stack[k] = argv[k];
    if (ustrand_push_frame_args(c, cl, recv, 0, 0, argc) != 0)
        return usched_spawn_failed(realm, c);
    if (p->arity_prologue && p->nparams > 0) c->stack[p->nparams] = uv_int((int64_t)argc);

    /* Newcomers are gated.  A strand entering the scope of a blocked or
     * frozen tag takes that tag's bits, so it parks on its first
     * safepoint instead of running, and the gate's release frees it with
     * every other member.  usched_enqueue is what applies that, above. */
    c->gates = utag_gate_bits(tag);
    usched_enqueue(c);
    return c;
}

/* --- death and reaping --------------------------------------------------- */

static void usched_on_death(UVM *vm, UStrand *s)
{
    USched *sc = uvm_sched(vm);
    s->state = USTRAND_DEAD;
    /* Spec section 9: what escapes the top frame kills the strand and is
     * reported through the diag callback AND urbi_last_error.  The walker
     * has already rendered it into vm->last_error; `threw` is what keeps
     * the clean-run clear from wiping it before the host can read it (see
     * uexec_finish_run), and the diag hook is what a DETACHED strand --
     * one with no caller to return a code to -- surfaces through.  The
     * awaited strand is excluded from both: its caller is about to be
     * handed the same failure as a return value, and reporting it twice
     * is how the same throw ends up printed twice. */
    if (s->unwind == (uint8_t)UUNWIND_THROW && vm->last_error[0] && s != sc->awaited) {
        sc->threw = 1;
        if (vm->diag)
            vm->diag(vm, vm->diag_ud, 3 /* syslog LOG_ERR */, vm->last_error, strlen(vm->last_error));
    }
    /* Joiners first: usched_wake needs them still threaded on s->joiners. */
    while (s->joiners) {
        UStrand *j = s->joiners;
        s->joiners = j->link;
        j->link = NULL;
        j->waiting_on = NULL;          /* already unlinked by hand */
        usched_wake(j, uv_nil());
    }
    s->link = sc->dead;
    sc->dead = s;
    /* Now that the dead list roots it, and only now: uwatch_body_done
     * re-evaluates a `whenever` condition, which allocates. */
    uwatch_body_done(vm, s);
}

/* Unlink every strand that died since the last step from its realm, which
 * is the last thing holding it: the collector takes it from there. */
static void usched_reap(UVM *vm)
{
    USched *sc = uvm_sched(vm);
    while (sc->dead) {
        UStrand *s = sc->dead;
        sc->dead = s->link;
        s->link = NULL;
        if (s->realm) {
            for (UStrand **pp = &s->realm->strands; *pp; pp = &(*pp)->next_in_realm) {
                if (*pp == s) { *pp = s->next_in_realm; break; }
            }
            s->next_in_realm = NULL;
        }
    }
}

/* --- the ISR ring -------------------------------------------------------- */

/* An injected payload reaches script as one integer: the first eight
 * bytes, zero-extended.  A zero-length injection is nil.  Anything richer
 * (a struct, a float vector) is the embedder's to unpack from that word,
 * because the runtime has no way to know the layout. */
static UValue isr_payload_value(const UIsrRec *r)
{
    if (r->n == 0) return uv_nil();
    uint64_t w = 0;
    uint8_t n = r->n < 8 ? r->n : 8;
    for (uint8_t i = 0; i < n; i++) w |= (uint64_t)r->payload.bytes[i] << (8u * i);
    return uv_int((int64_t)w);
}

static void usched_drain_isr(UVM *vm)
{
    USched *sc = uvm_sched(vm);
    for (;;) {
        uint32_t head = __atomic_load_n(&sc->isr_head, __ATOMIC_ACQUIRE);
        if (sc->isr_tail == head) return;
        UIsrRec r = sc->isr[sc->isr_tail % USCHED_ISR_SLOTS];
        __atomic_store_n(&sc->isr_tail, sc->isr_tail + 1u, __ATOMIC_RELEASE);
        if (r.id < sc->event_count && sc->events[r.id])
            uevent_emit(vm, sc->events[r.id], isr_payload_value(&r));
    }
}

/* --- firing timers -------------------------------------------------------- */

static void usched_fire_due(UVM *vm, uint64_t now)
{
    USched *sc = uvm_sched(vm);
    while (sc->heap_len > 0 && sc->heap[0].due_us <= now) {
        if (sc->heap[0].period_us == 0) {
            UStrand *s = sc->heap[0].strand;
            heap_remove_at(sc, 0);
            usched_wake(s, uv_nil());     /* allocates nothing */
            continue;
        }
        /* Periodic: RE-ARM FIRST, then spawn.  usched_spawn allocates and
         * may therefore collect, and the heap record is the only thing
         * rooting the body closure, the tag and the realm -- lifting the
         * record out before spawning would leave all three reachable from
         * nothing but C locals.  Re-arming past `now` is also what makes a
         * clock jump produce one body rather than one per missed tick. */
        UTimer *t = &sc->heap[0];
        UClosure *body = t->body;
        UTag     *tag  = t->tag;
        URealm   *realm = t->realm;
        uint64_t due = t->due_us + t->period_us;
        if (due <= now) due = now + t->period_us;
        t->due_us = due;
        heap_down(sc, 0);                 /* `t` is stale from here on */
        UValue recv = realm ? uv_obj(realm->globals) : uv_nil();
        (void)usched_spawn(vm, realm, body, tag, recv, NULL, 0);
    }
}

/* --- the step ------------------------------------------------------------- */

bool usched_has_live_work(UVM *vm)
{
    if (!vm) return false;
    const USched *sc = uvm_sched(vm);
    /* An armed watcher is pending work even with nothing runnable: a host
     * write between steps is exactly what it is there to notice.  A wait
     * with nobody waiting on it is not -- see uwatch_has_live_work. */
    return sc->run_head != NULL || sc->heap_len > 0 || uwatch_has_live_work(vm);
}

USchedStep usched_step(UVM *vm, uint32_t budget, uint64_t *next_wake_us)
{
    USched *sc = uvm_sched(vm);
    if (vm->clock_us == NULL) sc->fallback_now_us++;
    /* A strand that died throwing during the PREVIOUS step left its
     * message in the error channel for the host to read.  This is where
     * it goes, so the channel describes this step and not an older one. */
    if (sc->threw) {
        vm->last_error[0] = '\0';
        vm->last_error_code = URBI_OK;
        sc->threw = 0;
    }

    usched_reap(vm);
    usched_drain_isr(vm);
    /* One clock reading for the whole step.  Re-sampling inside the loop
     * would let a short-period `every` fire again the moment its own body
     * finished, and an unbounded step would never return. */
    usched_fire_due(vm, usched_now(vm));
    /* Before the queue, so a slot a HOST wrote between two steps reaches
     * the conditions that read it even when no strand is runnable. */
    uwatch_drain(vm);

    uint32_t remaining = budget;
    while (sc->run_head) {
        uint32_t slice = 0;
        if (budget != 0) slice = remaining < USCHED_SLICE ? remaining : USCHED_SLICE;
        UStrand *s = usched_dequeue(sc);
        int st = uexec_run(vm, s, slice);
        if (st == USTRAND_DEAD) usched_on_death(vm, s);
        else if (st != USTRAND_PARKED) usched_enqueue(s);
        /* The strand is back on the run queue or the dead list, so it is
         * rooted again and this is a safe place to collect. */
        ugc_maybe_collect(vm);
        /* Every armed condition is re-evaluated here, once, if anything
         * wrote to a watched object during that slice.  A write made by a
         * watcher body or condition re-arms the NEXT drain, which is what
         * bounds one pass. */
        uwatch_drain(vm);
        /* Charging.  uexec_run does not report how many instructions it
         * actually ran, so the budget is spent by outcome: a strand that
         * comes back READY used its whole slice or yielded, and is charged
         * for it; one that parked or died is charged one, because it
         * FINISHED and a queue of short-lived strands should not report
         * "still runnable" just because there were many of them.  The
         * budget still bounds the step either way. */
        if (budget != 0) {
            uint32_t spent = (st == USTRAND_READY) ? slice : 1u;
            remaining = remaining > spent ? remaining - spent : 0u;
            if (remaining == 0) break;
        }
    }
    usched_reap(vm);

    if (sc->run_head) return USTEP_RAN;
    if (sc->heap_len > 0) {
        if (next_wake_us) *next_wake_us = sc->heap[0].due_us;
        return USTEP_IDLE_UNTIL;
    }
    return USTEP_QUIESCENT;
}

void usched_run_inline(UVM *vm, UStrand *s)
{
    if (s == NULL) return;
    usched_unqueue(s);
    while (s->state != USTRAND_DEAD) {
        int st = uexec_run(vm, s, 0);
        if (st == USTRAND_DEAD) { usched_on_death(vm, s); return; }
        if (st == USTRAND_PARKED) return;
    }
}

/* --- GC ------------------------------------------------------------------- */

void usched_mark(UVM *vm)
{
    USched *sc = uvm_sched(vm);
    for (UStrand *s = sc->run_head; s; s = s->link) ugc_mark(vm, &s->cell);
    for (UStrand *s = sc->dead; s; s = s->link) ugc_mark(vm, &s->cell);
    if (sc->current) ugc_mark(vm, &sc->current->cell);
    for (uint32_t i = 0; i < sc->heap_len; i++) {
        const UTimer *t = &sc->heap[i];
        if (t->strand) ugc_mark(vm, &t->strand->cell);
        if (t->body)   ugc_mark(vm, &t->body->cell);
        if (t->tag)    ugc_mark(vm, &t->tag->cell);
        if (t->realm)  ugc_mark(vm, (UCell *)t->realm);
    }
    for (uint16_t i = 0; i < sc->event_count; i++)
        if (sc->events[i]) ugc_mark(vm, &sc->events[i]->cell);
}
