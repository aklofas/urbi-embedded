/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uwatch.c — the reactive runtime.  See rt/uwatch.h for the model. */

#include "rt/uwatch.h"

UWatchState *uvm_watch(UVM *vm) { return &vm->watch; }

/* --- GC ------------------------------------------------------------------ */

void uwatch_mark(UVM *vm)
{
    for (UWatcher *w = vm->watch.all; w; w = w->next) ugc_mark(vm, &w->cell);
}

void uwatch_trace(UVM *vm, UWatcher *w)
{
    if (w->cond)    ugc_mark(vm, &w->cond->cell);
    if (w->event)   ugc_mark(vm, &w->event->cell);
    if (w->body)    ugc_mark(vm, &w->body->cell);
    if (w->onleave) ugc_mark(vm, &w->onleave->cell);
    if (w->tag)     ugc_mark(vm, &w->tag->cell);
    if (w->realm)   ugc_mark(vm, (UCell *)w->realm);
    if (w->body_strand) ugc_mark(vm, &w->body_strand->cell);
    /* The wait list is threaded through each waiting strand's own `link`;
     * walking it is traversal of that list, exactly as ustrand_trace does
     * for `joiners`. */
    for (UStrand *s = w->waiters; s; s = s->link) ugc_mark(vm, &s->cell);
    ugc_mark_value(vm, w->payload);
}

/* --- the dirty count ------------------------------------------------------ */

void uwatch_observe(const UVM *vm, UObject *o)
{
    if (o && vm->watch.observing) o->cell.flags |= UOBJ_F_WATCHED;
}

void uwatch_mark_dirty(UVM *vm, UObject *o)
{
    /* `o` names the object that was written, which the seam keeps for the
     * day a drain filters by realm; today the drain's only question is
     * whether ANY watched object moved (see rt/uexec.h). */
    (void)o;
    if (vm->watch.ndirty != 0xFFFFFFFFu) vm->watch.ndirty++;
}

/* --- list maintenance ------------------------------------------------------
 *
 * A watcher is never unlinked while a walk is in progress: cancelling sets
 * `armed = 0` and the sweep, which only ever runs outside a walk, drops
 * it.  That is what replaces the old core's PENDING_UNREGISTER flag, its
 * cascade rescan and the eval-pass stamp the rescan needed. */

static void uwatch_unlink_from_event(UWatcher *w)
{
    if (w->event == NULL) return;
    for (UWatcher **pp = &w->event->watchers; *pp; pp = &(*pp)->next_on_event) {
        if (*pp == w) { *pp = w->next_on_event; break; }
    }
    w->next_on_event = NULL;
    w->event = NULL;
}

static void uwatch_wake_waiters(UWatcher *w, UValue payload);

static void uwatch_sweep(UVM *vm)
{
    UWatchState *ws = &vm->watch;
    if (ws->draining) return;
    for (UWatcher **pp = &ws->all; *pp; ) {
        UWatcher *w = *pp;
        if (w->armed) { pp = &w->next; continue; }
        *pp = w->next;
        w->next = NULL;
        uwatch_unlink_from_event(w);
        /* A watcher cancelled while somebody was still waiting on it --
         * `t.stop()` on a tag holding a `waituntil` -- must not leave that
         * strand's `waiting_on` pointing into a cell nothing roots any
         * more.  In practice the stop has already woken it; this is what
         * makes that ordering a convenience rather than a requirement. */
        uwatch_wake_waiters(w, uv_nil());
    }
}

/* --- reporting ------------------------------------------------------------ */

static void uwatch_report(UVM *vm, const char *what, const char *msg)
{
    if (vm->diag == NULL) return;
    char text[288];
    size_t at = 0;
    for (const char *p = what; *p && at + 1 < sizeof text; p++) text[at++] = *p;
    for (const char *p = msg; p && *p && at + 1 < sizeof text; p++) text[at++] = *p;
    text[at] = '\0';
    vm->diag(vm, vm->diag_ud, 3 /* syslog LOG_ERR */, text, at);
}

/* Absorb a throw that reached a spare strand's call boundary: the watcher
 * runtime never propagates one to whoever happened to be running.  The
 * error channel is restored to what it held before, because a condition
 * raising is not the host's last error. */
static void uwatch_absorb(UVM *vm, UStrand *sp, const char *what,
                          const char *saved_err, int saved_code)
{
    uwatch_report(vm, what, vm->last_error);
    sp->unwind = (uint8_t)UUNWIND_NONE;
    sp->transfer = uv_nil();
    sp->resume_slot = 0;
    memcpy(vm->last_error, saved_err, sizeof vm->last_error);
    vm->last_error_code = saved_code;
}

/* --- running a watcher's code ---------------------------------------------
 *
 * Every one of these runs on a SPARE strand.  A spare may not be
 * descheduled, so a `;` inside the code it runs is a plain sequence point
 * and the call returns having finished -- which is what makes `at sync`
 * mean what it says. */

static UValue uwatch_recv(const UWatcher *w)
{
    return (w->realm && w->realm->globals) ? uv_obj(w->realm->globals) : uv_nil();
}

/* How many arguments a watcher body takes: the payload when it has a
 * parameter slot for one, nothing otherwise.  `at (e?(var x))` compiles
 * its body with one formal; `at (cond)` compiles it with none. */
static uint8_t uwatch_body_argc(const UClosure *cl)
{
    if (cl == NULL) return 0;
    if (cl->native) return (cl->min_args <= 1 && cl->max_args >= 1) ? 1u : 0u;
    return (cl->proto && cl->proto->nparams >= 1) ? 1u : 0u;
}

/* Evaluate `w->cond` and report its truth, leaving the VALUE it produced
 * in `w->payload` -- a host watch is handed that value, and the watcher is
 * the only root the value has once the spare strand is released.  0 ok,
 * -1 when the condition raised (the watcher is disarmed) or a spare could
 * not be acquired. */
static int uwatch_eval_cond(UVM *vm, UWatcher *w, bool *out)
{
    if (w->cond == NULL || w->realm == NULL || w->realm->globals == NULL) return -1;
    UStrand *sp = uvm_spare_acquire(vm, w->realm);
    if (sp == NULL) return -1;
    sp->tag = w->tag;

    char saved_err[sizeof vm->last_error];
    memcpy(saved_err, vm->last_error, sizeof saved_err);
    int saved_code = vm->last_error_code;

    uint8_t prev = vm->watch.observing;
    vm->watch.observing = 1;
    UValue v = uv_nil();
    int rc = uexec_call(vm, sp, w->cond, uwatch_recv(w), NULL, 0, &v);
    vm->watch.observing = prev;

    if (rc != UEXEC_OK) {
        uwatch_absorb(vm, sp, "at condition raised: ", saved_err, saved_code);
        w->armed = 0;
        uvm_spare_release(vm, sp);
        return -1;
    }
    w->payload = v;              /* rooted here, before the spare goes back */
    *out = uv_truthy(v);
    uvm_spare_release(vm, sp);
    return 0;
}

/* Run one of the watcher's closures to completion on a spare strand. */
static void uwatch_run_inline(UVM *vm, UWatcher *w, UClosure *cl, UValue payload,
                              const char *what)
{
    if (cl == NULL || w->realm == NULL) return;
    UStrand *sp = uvm_spare_acquire(vm, w->realm);
    if (sp == NULL) return;
    sp->tag = w->tag;

    char saved_err[sizeof vm->last_error];
    memcpy(saved_err, vm->last_error, sizeof saved_err);
    int saved_code = vm->last_error_code;

    uint8_t argc = uwatch_body_argc(cl);
    w->payload = payload;
    UValue out = uv_nil();
    int rc = uexec_call(vm, sp, cl, uwatch_recv(w), argc ? &w->payload : NULL, argc, &out);
    w->payload = uv_nil();
    if (rc != UEXEC_OK) uwatch_absorb(vm, sp, what, saved_err, saved_code);
    uvm_spare_release(vm, sp);
}

/* Spawn one of the watcher's closures as a scheduled strand under the
 * watcher's tag.  NULL when there is nothing to spawn. */
static UStrand *uwatch_spawn(UVM *vm, UWatcher *w, UClosure *cl, UValue payload)
{
    if (cl == NULL || cl->proto == NULL || w->realm == NULL) return NULL;
    uint8_t argc = uwatch_body_argc(cl);
    /* usched_spawn allocates twice before it copies the argument window,
     * so the payload rides on the watcher (a GC root) rather than in a C
     * local. */
    w->payload = payload;
    UStrand *c = usched_spawn(vm, w->realm, cl, w->tag, uwatch_recv(w),
                              argc ? &w->payload : NULL, argc);
    w->payload = uv_nil();
    return c;
}

/* The rising-edge action, shared by the drain and the event fan-out. */
static void uwatch_fire(UVM *vm, UWatcher *w, UValue payload, bool force_inline)
{
    w->fired = 1;
    if (w->host_cb) { w->host_cb(vm, w->host_ud, payload); return; }
    if (w->mode == (uint8_t)UWATCH_AT_SYNC || force_inline) {
        uwatch_run_inline(vm, w, w->body, payload, "at sync body raised: ");
        return;
    }
    UStrand *c = uwatch_spawn(vm, w, w->body, payload);
    if (w->mode == (uint8_t)UWATCH_WHENEVER && w->cond) w->body_strand = c;
}

static void uwatch_leave(UVM *vm, UWatcher *w)
{
    if (w->onleave == NULL || !w->fired) return;
    w->fired = 0;
    /* An at-sync watcher's falling edge is as synchronous as its rising
     * one; everything else spawns. */
    if (w->mode == (uint8_t)UWATCH_AT_SYNC)
        uwatch_run_inline(vm, w, w->onleave, uv_nil(), "at sync onleave raised: ");
    else
        (void)uwatch_spawn(vm, w, w->onleave, uv_nil());
}

static void uwatch_wake_waiters(UWatcher *w, UValue payload)
{
    while (w->waiters) {
        UStrand *s = w->waiters;
        w->waiters = s->link;
        s->link = NULL;
        s->waiting_on = NULL;
        usched_wake(s, payload);
    }
}

/* --- the drain ------------------------------------------------------------ */

void uwatch_drain(UVM *vm)
{
    UWatchState *ws = &vm->watch;
    if (ws->draining || ws->ndirty == 0 || ws->all == NULL) return;
    /* Cleared BEFORE the walk: a write made by a body or a condition
     * during this drain re-arms the next one rather than extending this
     * one, which is what bounds a pass. */
    ws->ndirty = 0;
    uwatch_sweep(vm);
    ws->draining = 1;

    /* The tail as it stands now: a watcher installed by one of the bodies
     * or conditions below is appended past it and joins the NEXT drain,
     * never the one that installed it. */
    UWatcher *last = ws->all;
    while (last && last->next) last = last->next;
    for (UWatcher *w = ws->all, *next; w; w = next) {
        next = (w == last) ? NULL : w->next;
        if (!w->armed || w->cond == NULL) continue;
        bool now = false;
        if (uwatch_eval_cond(vm, w, &now) != 0) continue;
        if (!w->armed) continue;              /* cancelled while the condition ran */
        bool was = w->last != 0;
        w->last = now ? 1u : 0u;
        if (now && !was) {
            if (w->mode == (uint8_t)UWATCH_WAITUNTIL) {
                w->armed = 0;
                uwatch_wake_waiters(w, uv_nil());
            } else if (w->mode == (uint8_t)UWATCH_WHENEVER && w->body_strand != NULL) {
                /* Its body is still in flight; the death path is what asks
                 * the condition again and re-fires. */
            } else {
                uwatch_fire(vm, w, w->payload, false);
            }
        } else if (!now && was) {
            uwatch_leave(vm, w);
        }
        w->payload = uv_nil();
    }

    ws->draining = 0;
    uwatch_sweep(vm);
}

void uwatch_body_done(UVM *vm, const UStrand *dead)
{
    UWatchState *ws = &vm->watch;
    UWatcher *w = ws->all;
    while (w && w->body_strand != dead) w = w->next;
    if (w == NULL) return;
    w->body_strand = NULL;
    if (!w->armed || w->mode != (uint8_t)UWATCH_WHENEVER || w->cond == NULL) return;

    /* THE SAME BRACKET uwatch_drain AND uwatch_event_fired USE, and for the
     * same reason: the condition below is arbitrary script, and a cancel
     * reached from inside it (`t.stop()`, or an emit whose fan-out sweeps
     * on the way out) would otherwise unlink `w` from vm->watch.all -- its
     * only GC root -- while this function still holds it.  The next
     * allocation inside that same condition then frees it, and everything
     * after the call here is a use-after-free.  Holding `draining` makes
     * uwatch_sweep a no-op for the duration; the sweep at the bottom is
     * what actually reclaims. */
    uint8_t prev = ws->draining;
    ws->draining = 1;

    /* `whenever` is a reactive loop: the body having finished is the
     * question "does the condition still hold?", asked again. */
    bool now = false;
    if (uwatch_eval_cond(vm, w, &now) == 0 && w->armed) {
        w->last = now ? 1u : 0u;
        if (now) uwatch_fire(vm, w, w->payload, false);
        else uwatch_leave(vm, w);
        w->payload = uv_nil();
    }

    ws->draining = prev;
    uwatch_sweep(vm);
}

/* --- events ---------------------------------------------------------------- */

void uwatch_event_fired(UVM *vm, UEvent *e, UValue payload, bool sync)
{
    if (e == NULL || e->watchers == NULL) return;
    UWatchState *ws = &vm->watch;
    uint8_t prev = ws->draining;
    ws->draining = 1;
    /* The tail as it stands NOW.  A subscription made by one of these
     * bodies lands past it and does not receive the emission it was
     * installed by -- the pre-registered-subscribers rule. */
    UWatcher *last = e->watchers;
    while (last->next_on_event) last = last->next_on_event;
    for (UWatcher *w = e->watchers, *next; w; w = next) {
        next = (w == last) ? NULL : w->next_on_event;
        if (!w->armed) continue;
        if (w->mode == (uint8_t)UWATCH_ONCE) {
            w->armed = 0;
            uwatch_wake_waiters(w, payload);
            continue;
        }
        uwatch_fire(vm, w, payload, sync && w->mode == (uint8_t)UWATCH_AT_SYNC);
    }
    ws->draining = prev;
    uwatch_sweep(vm);
}

/* --- installation ----------------------------------------------------------- */

static UWatcher *uwatch_new(UVM *vm, uint8_t mode, URealm *realm, UTag *tag)
{
    UWatcher *w = (UWatcher *)ugc_alloc(vm, UCELL_WATCHER, sizeof(UWatcher));
    if (w == NULL) return NULL;
    w->mode = mode;
    w->armed = 1;
    w->realm = realm;
    w->tag = tag;
    w->payload = uv_nil();
    /* APPENDED, like the per-event list: a drain walks in registration
     * order, which is the order the corpus pins when several watchers see
     * the same edge.  Reachable before anything else can allocate. */
    UWatcher **pp = &vm->watch.all;
    while (*pp) pp = &(*pp)->next;
    *pp = w;
    return w;
}

UWatcher *uwatch_install(UVM *vm, UStrand *s, uint8_t mode, UClosure *cond,
                         UEvent *event, UClosure *body, UClosure *onleave)
{
    URealm *realm = s ? s->realm : vm->main_realm;
    if (realm == NULL) realm = vm->main_realm;
    UWatcher *w = uwatch_new(vm, mode, realm, s ? s->tag : NULL);
    if (w == NULL) return NULL;
    w->cond = cond;
    w->body = body;
    w->onleave = onleave;
    if (event) {
        /* APPENDED, not prepended: the corpus pins registration order on
         * the fan-out, so the list has to read the way it was built. */
        w->event = event;
        UWatcher **pp = &event->watchers;
        while (*pp) pp = &(*pp)->next_on_event;
        *pp = w;
    }
    /* A freshly installed condition has never been evaluated, so the next
     * drain is what decides whether it already holds -- which is how
     * `at (c)` fires when `c` is true at install time. */
    if (cond) uwatch_mark_dirty(vm, NULL);
    return w;
}

UWatcher *uwatch_install_host(UVM *vm, URealm *realm, UClosure *cond,
                              int (*cb)(UVM *, void *, UValue), void *ud)
{
    if (realm == NULL) realm = vm->main_realm;
    UWatcher *w = uwatch_new(vm, (uint8_t)UWATCH_AT, realm, realm ? realm->root_tag : NULL);
    if (w == NULL) return NULL;
    w->cond = cond;
    w->host_cb = cb;
    w->host_ud = ud;
    uwatch_mark_dirty(vm, NULL);
    return w;
}

int uwatch_waituntil(UVM *vm, UStrand *s, UClosure *cond)
{
    /* Evaluated on the WAITING strand, not on a spare: the condition is
     * the caller's own code, so a throw in it belongs to the caller and
     * propagates rather than being absorbed. */
    UValue recv = (s->realm && s->realm->globals) ? uv_obj(s->realm->globals) : uv_nil();
    uint8_t prev = vm->watch.observing;
    vm->watch.observing = 1;
    UValue v = uv_nil();
    int rc = uexec_call(vm, s, cond, recv, NULL, 0, &v);
    vm->watch.observing = prev;
    if (rc != UEXEC_OK) return -1;
    if (uv_truthy(v)) return 0;
    /* A spare strand, or one inside a synchronous call, has a C frame
     * waiting for its value and no scheduler to hand control back to, so
     * the wait degrades to "already satisfied" rather than wedging. */
    if (!usched_may_deschedule(s)) return 0;

    UWatcher *w = uwatch_new(vm, (uint8_t)UWATCH_WAITUNTIL, s->realm, s->tag);
    if (w == NULL) return -1;
    w->cond = cond;
    /* The wait list lives inside a watcher, which vm->watch.all roots, so
     * `waiting_on` is not the dangling pointer the park contract warns
     * about. */
    (void)usched_park(s, &w->waiters, 0);
    return 1;
}

/* --- cancellation ------------------------------------------------------------ */

void uwatch_tag_stopped(UVM *vm, const UTag *t)
{
    if (t == NULL) return;
    for (UWatcher *w = vm->watch.all; w; w = w->next)
        if (w->tag == t) w->armed = 0;
    uwatch_sweep(vm);
}

void uwatch_realm_dropped(UVM *vm, const URealm *r)
{
    if (r == NULL) return;
    for (UWatcher *w = vm->watch.all; w; w = w->next)
        if (w->realm == r) w->armed = 0;
    uwatch_sweep(vm);
}

/* --- slot-change events -------------------------------------------------------
 *
 * The event for (object, slot) lives in a HIDDEN SLOT on the object
 * itself, named "\x01" + the slot's name.  A side table keyed by object
 * would have to either keep every subscribed object alive for the life of
 * the VM or find out when one died; an ordinary slot dies with its owner
 * and needs no bookkeeping at all.  The leading byte is one no identifier
 * can contain, and obj_slotNames filters it out. */

#define UWATCH_HIDDEN_MAX 192

static USym *uwatch_hidden_sym(UVM *vm, const USym *name)
{
    if (name->len + 1u > UWATCH_HIDDEN_MAX) return NULL;
    char buf[UWATCH_HIDDEN_MAX];
    buf[0] = '\x01';
    memcpy(buf + 1, name->bytes, name->len);
    return usym_intern(vm, buf, (size_t)name->len + 1u);
}

UEvent *uwatch_slot_change_event(UVM *vm, UObject *o, const USym *name)
{
    USym *hidden = uwatch_hidden_sym(vm, name);
    if (hidden == NULL) return NULL;
    int i = uobj_find_local(o, hidden);
    if (i >= 0 && o->values[i].kind == UV_CELL
        && ((UCell *)o->values[i].v.p)->type == UCELL_EVENT)
        return (UEvent *)o->values[i].v.p;

    /* Pin the owner across the allocations below: it is reachable only
     * through the caller's register, which OP_GETSLOT_CHANGE_EVENT
     * re-derives afterwards, and ugc_alloc may collect. */
    o->cell.flags |= UCELL_F_PINNED;
    UEvent *e = uevent_new(vm, uv_nil());
    int rc = -1;
    if (e) rc = uobj_set_local(vm, o, hidden, uv_ptr(UV_CELL, e), 0);
    o->cell.flags &= (uint16_t)~UCELL_F_PINNED;
    if (rc < 0) return NULL;

    /* The watched slot is NOT created here.  Subscribing to a slot that
     * does not exist yet must leave the first write to it an INSTALL, not
     * a change -- so the object is flagged instead and the marker is
     * attached by uwatch_slot_installed when that write lands. */
    o->cell.flags |= UOBJ_F_CHANGE_EVENTS;
    int si = uobj_find_local(o, name);
    if (si >= 0) o->attrs[si] |= USLOT_CHANGED_EVENT;
    return e;
}

void uwatch_slot_installed(UVM *vm, UObject *o, const USym *name)
{
    const USym *hidden = uwatch_hidden_sym(vm, name);
    if (hidden == NULL || uobj_find_local(o, hidden) < 0) return;
    int si = uobj_find_local(o, name);
    if (si >= 0) o->attrs[si] |= USLOT_CHANGED_EVENT;
}

void uwatch_slot_changed(UVM *vm, UObject *o, const USym *name, UValue v)
{
    const USym *hidden = uwatch_hidden_sym(vm, name);
    if (hidden == NULL) return;
    int i = uobj_find_local(o, hidden);
    if (i < 0 || o->values[i].kind != UV_CELL
        || ((UCell *)o->values[i].v.p)->type != UCELL_EVENT) return;
    uevent_emit_to(vm, (UEvent *)o->values[i].v.p, v, false);
}

/* --- liveness ------------------------------------------------------------------ */

bool uwatch_has_live_work(const UVM *vm)
{
    for (const UWatcher *w = vm->watch.all; w; w = w->next) {
        if (!w->armed) continue;
        /* A wait that nobody is waiting on, and a subscription with
         * nothing to run, are both as quiescent as an event nobody can
         * emit any more. */
        if (w->mode == (uint8_t)UWATCH_WAITUNTIL || w->mode == (uint8_t)UWATCH_ONCE) {
            if (w->waiters) return true;
            continue;
        }
        if (w->body || w->host_cb) return true;
    }
    return false;
}
