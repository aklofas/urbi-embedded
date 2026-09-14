/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/usched_natives.c — the script surface of the scheduler: Tag,
 * Event, Job, and the `sleep` / `every` / `scopeTag` / detach globals.
 *
 * These live beside the scheduler rather than in src/stdlib/ because
 * every one of them is scheduler state, not library data: they park a
 * strand, arm a timer, or walk the strand graph.  A src/stdlib file
 * reaches the runtime through rt/ustdlib_glue.h alone and cannot do any
 * of that — which is the point of that boundary, not an oversight.
 *
 * Exec-rank (see rt/usched.h). */

#include "rt/uboot.h"
#include "rt/ustdlib_glue.h"

/* --- shared argument handling ------------------------------------------ */

/* A duration as the language spells it: an Integer is microseconds (the
 * lexer already scales `100ms` to 100000), a Float is seconds.  Returns
 * 0 on success, -1 when the value is not a usable duration. */
static int dur_us(UValue v, uint64_t *out)
{
    if (v.kind == UV_INT) {
        if (v.v.i < 0) return -1;
        *out = (uint64_t)v.v.i;
        return 0;
    }
    if (v.kind == UV_FLOAT) {
        double f = v.v.f;
        /* !(f >= 0) rejects NaN along with the negatives; the upper bound
         * rejects +inf and anything whose microsecond conversion would
         * not fit an int64, for which the cast would be undefined. */
        if (!(f >= 0.0) || f > 9.2e12) return -1;
        *out = (uint64_t)(int64_t)(f * 1e6);
        return 0;
    }
    return -1;
}

static UTag *as_tag(UValue v)
{
    if (v.kind != UV_CELL || ((UCell *)v.v.p)->type != UCELL_TAG) return NULL;
    return (UTag *)v.v.p;
}

static UEvent *as_event(UValue v)
{
    if (v.kind != UV_CELL || ((UCell *)v.v.p)->type != UCELL_EVENT) return NULL;
    return (UEvent *)v.v.p;
}

static UStrand *as_strand(UValue v)
{
    if (v.kind != UV_CELL || ((UCell *)v.v.p)->type != UCELL_STRAND) return NULL;
    return (UStrand *)v.v.p;
}

/* --- Tag ---------------------------------------------------------------- */

static int tag_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self;
    UValue name = uv_nil();
    if (nargs > 0) {
        if (!urbi_is_str(args[0])) return urbi_raise_type(vm, "Tag.new: name must be a String", out);
        name = args[0];
    }
    UTag *t = utag_new(vm, name);
    if (!t) return urbi_raise_oom(vm, out);
    *out = uv_ptr(UV_CELL, t);
    return UEXEC_OK;
}

/* tag.stop() — cancel every strand in the tag's scope.
 *
 * Ordinary from anywhere, inside the scope or outside it (spec section 9
 * retires the old "fatal outside the scope" rule).  When the CALLING
 * strand is one of the members, the unwind starts right here: utag_stop
 * has already set its pending STOP, so returning UEXEC_THROW hands
 * control to the walker instead of finishing the call. */
static int tag_stop(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    UTag *t = as_tag(self);
    if (!t) return urbi_raise_type(vm, "tag.stop: receiver is not a Tag", out);
    *out = uv_nil();
    return utag_stop(vm, t) ? UEXEC_THROW : UEXEC_OK;
}

/* The four gate operations differ only in bit and direction. */
static int tag_gate_op(UVM *vm, UValue self, UValue *out, uint8_t bit, bool on, const char *what)
{
    UTag *t = as_tag(self);
    if (!t) return urbi_raise_type(vm, what, out);
    *out = uv_nil();
    utag_gate(vm, t, bit, on);
    return UEXEC_OK;
}

static int tag_block(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{ (void)a; (void)n; return tag_gate_op(vm, self, out, USTRAND_GATE_BLOCKED, true, "tag.block: receiver is not a Tag"); }
static int tag_unblock(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{ (void)a; (void)n; return tag_gate_op(vm, self, out, USTRAND_GATE_BLOCKED, false, "tag.unblock: receiver is not a Tag"); }
static int tag_freeze(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{ (void)a; (void)n; return tag_gate_op(vm, self, out, USTRAND_GATE_FROZEN, true, "tag.freeze: receiver is not a Tag"); }
static int tag_unfreeze(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{ (void)a; (void)n; return tag_gate_op(vm, self, out, USTRAND_GATE_FROZEN, false, "tag.unfreeze: receiver is not a Tag"); }

static int tag_blocked(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{
    (void)a; (void)n;
    const UTag *t = as_tag(self);
    if (!t) return urbi_raise_type(vm, "tag.blocked: receiver is not a Tag", out);
    *out = uv_bool((t->flags & UTAG_F_BLOCKED) != 0);
    return UEXEC_OK;
}

static int tag_frozen(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{
    (void)a; (void)n;
    const UTag *t = as_tag(self);
    if (!t) return urbi_raise_type(vm, "tag.frozen: receiver is not a Tag", out);
    *out = uv_bool((t->flags & UTAG_F_FROZEN) != 0);
    return UEXEC_OK;
}

static int tag_enter_ev(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{
    (void)a; (void)n;
    UTag *t = as_tag(self);
    if (!t) return urbi_raise_type(vm, "tag.enter: receiver is not a Tag", out);
    UEvent *e = utag_enter_event(vm, t);
    if (!e) return urbi_raise_oom(vm, out);
    *out = uv_ptr(UV_CELL, e);
    return UEXEC_OK;
}

static int tag_leave_ev(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{
    (void)a; (void)n;
    UTag *t = as_tag(self);
    if (!t) return urbi_raise_type(vm, "tag.leave: receiver is not a Tag", out);
    UEvent *e = utag_leave_event(vm, t);
    if (!e) return urbi_raise_oom(vm, out);
    *out = uv_ptr(UV_CELL, e);
    return UEXEC_OK;
}

static const UMethodDef ustdlib_tag_methods[] = {
    { "new",      tag_new,      0, 1 },
    { "stop",     tag_stop,     0, 0 },
    { "block",    tag_block,    0, 0 },
    { "unblock",  tag_unblock,  0, 0 },
    { "freeze",   tag_freeze,   0, 0 },
    { "unfreeze", tag_unfreeze, 0, 0 },
    { "blocked",  tag_blocked,  0, 0 },
    { "frozen",   tag_frozen,   0, 0 },
    { "enter",    tag_enter_ev, 0, 0 },
    { "leave",    tag_leave_ev, 0, 0 }
};

/* --- Event --------------------------------------------------------------- */

static int event_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self;
    UValue name = uv_nil();
    if (nargs > 0) {
        if (!urbi_is_str(args[0])) return urbi_raise_type(vm, "Event.new: name must be a String", out);
        name = args[0];
    }
    UEvent *e = uevent_new(vm, name);
    if (!e) return urbi_raise_oom(vm, out);
    *out = uv_ptr(UV_CELL, e);
    return UEXEC_OK;
}

static int event_emit(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    UEvent *e = as_event(self);
    if (!e) return urbi_raise_type(vm, "event.emit: receiver is not an Event", out);
    uevent_emit(vm, e, nargs > 0 ? args[0] : uv_nil());
    *out = uv_nil();
    return UEXEC_OK;
}

static const UMethodDef ustdlib_event_methods[] = {
    { "new",  event_new,  0, 1 },
    { "emit", event_emit, 0, 1 }
};

/* --- Job ----------------------------------------------------------------- */

/* A Job IS the strand cell: `Job.current()` hands back the running
 * strand, and uv_dispatch_proto routes UCELL_STRAND at the Job
 * prototype, so there is no wrapper object to keep in step. */
static int job_current(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{
    (void)self; (void)a; (void)n;
    UStrand *s = uvm_current_strand(vm);
    *out = s ? uv_ptr(UV_CELL, s) : uv_nil();
    return UEXEC_OK;
}

static int job_uid(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{
    (void)a; (void)n;
    const UStrand *s = as_strand(self);
    if (!s) return urbi_raise_type(vm, "Job.uid: receiver is not a Job", out);
    *out = uv_int((int64_t)s->id);
    return UEXEC_OK;
}

static int job_status(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{
    (void)a; (void)n;
    const UStrand *s = as_strand(self);
    if (!s) return urbi_raise_type(vm, "Job.status: receiver is not a Job", out);
    const char *name;
    switch (s->state) {
    case USTRAND_RUNNING: name = "running"; break;
    case USTRAND_READY:   name = "ready";   break;
    case USTRAND_PARKED:  name = "waiting"; break;
    default:              name = "dead";    break;
    }
    *out = urbi_make_str_interned(vm, name, strlen(name));
    return out->kind == UV_NIL ? urbi_raise_oom(vm, out) : UEXEC_OK;
}

/* The tags covering the strand, outermost first: the tag each TAG_SCOPE
 * entry displaced on its way in, then the innermost ambient one. */
static int job_tags(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{
    (void)a; (void)n;
    UStrand *s = as_strand(self);
    if (!s) return urbi_raise_type(vm, "Job.tags: receiver is not a Job", out);
    *out = urbi_list_new(vm);
    if (out->kind == UV_NIL) return urbi_raise_oom(vm, out);
    for (uint16_t i = 0; i < s->ncleanup; i++) {
        const UCleanup *c = &s->cleanup[i];
        if (c->kind != (uint8_t)UCLEAN_TAG_SCOPE || c->saved.kind != UV_CELL) continue;
        if (urbi_list_append(vm, *out, c->saved) != 0) return urbi_raise_oom(vm, out);
    }
    if (s->tag && urbi_list_append(vm, *out, uv_ptr(UV_CELL, s->tag)) != 0)
        return urbi_raise_oom(vm, out);
    return UEXEC_OK;
}

static int job_jobs(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{
    (void)self; (void)a; (void)n;
    *out = urbi_list_new(vm);
    if (out->kind == UV_NIL) return urbi_raise_oom(vm, out);
    for (URealm *r = vm->realms; r; r = r->next) {
        for (UStrand *s = r->strands; s; s = s->next_in_realm) {
            if (s->state == USTRAND_DEAD) continue;
            if (urbi_list_append(vm, *out, uv_ptr(UV_CELL, s)) != 0)
                return urbi_raise_oom(vm, out);
        }
    }
    return UEXEC_OK;
}

static const UMethodDef ustdlib_job_methods[] = {
    { "current", job_current, 0, 0 },
    { "jobs",    job_jobs,    0, 0 },
    { "uid",     job_uid,     0, 0 },
    { "status",  job_status,  0, 0 },
    { "tags",    job_tags,    0, 0 }
};

/* --- globals -------------------------------------------------------------- */

/* sleep(d) — park until now + d, then continue with nil.
 *
 * The nil is already in the caller's destination register by the time the
 * strand parks (do_call writes the native's result before the dispatch
 * loop notices the state change), so resuming lands on the instruction
 * after the call with the right value in place.
 *
 * A strand that may not park — a spare, or one inside a synchronous
 * uexec_call — returns immediately instead.  Its caller is a C frame
 * waiting for a value, and there is no scheduler to hand control to. */
static int sleep_native(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)nargs;
    uint64_t us;
    if (dur_us(args[0], &us) != 0)
        return urbi_raise_type(vm, "sleep: duration must be an Integer (microseconds) or a Float (seconds)", out);
    *out = uv_nil();
    UStrand *s = uvm_current_strand(vm);
    if (!s || !usched_may_deschedule(s)) return UEXEC_OK;
    /* Arm first, park second.  The other order has to undo the park when
     * the heap cannot grow, and re-enqueueing a strand that is about to
     * throw splices the run queue into the dead list. */
    uint64_t due = usched_now(vm) + us;
    UTimer t;
    memset(&t, 0, sizeof t);
    t.due_us = due;
    t.strand = s;
    t.realm = s->realm;   /* so realm teardown can drop it */
    if (usched_timer_add(vm, t) != 0) return urbi_raise_oom(vm, out);
    (void)usched_park(s, NULL, due);   /* checked above; cannot be refused */
    return UEXEC_OK;
}

/* every(period, body) — arm a periodic under the caller's ambient tag.
 *
 * The tag is what makes `mytag: every(P) ...; mytag.stop()` work: the
 * timer belongs to the tag, so stopping the tag drops it even though the
 * scope that installed it closed long ago. */
static int every_native(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)nargs;
    uint64_t period;
    if (dur_us(args[0], &period) != 0 || period == 0)
        return urbi_raise_type(vm, "every: period must be a positive Duration", out);
    if (!urbi_is_closure(args[1]))
        return urbi_raise_type(vm, "every: body must be a Function", out);
    UStrand *s = uvm_current_strand(vm);
    UTimer t;
    memset(&t, 0, sizeof t);
    t.due_us = usched_now(vm) + period;
    t.period_us = period;
    t.body = (UClosure *)args[1].v.p;
    t.tag = s ? s->tag : NULL;
    t.realm = uvm_current_realm(vm);
    if (t.realm == NULL) return urbi_raise_type(vm, "every: no realm to run the body in", out);
    if (usched_timer_add(vm, t) != 0) return urbi_raise_oom(vm, out);
    *out = uv_nil();
    return UEXEC_OK;
}

static int scope_tag_native(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{
    (void)self; (void)a; (void)n;
    const UStrand *s = uvm_current_strand(vm);
    *out = (s && s->tag) ? uv_ptr(UV_CELL, s->tag) : uv_nil();
    return UEXEC_OK;
}

/* detach/disown differ in one thing: the tag the child inherits.  detach
 * keeps the caller's ambient tag chain; disown strips every user tag and
 * keeps only the realm's connection tag, so the child outlives whatever
 * scope spawned it but still dies with its realm. */
static int spawn_native(UVM *vm, UValue *args, UValue *out, bool keep_tags, const char *what)
{
    if (!urbi_is_closure(args[0])) return urbi_raise_type(vm, what, out);
    UStrand *s = uvm_current_strand(vm);
    URealm *r = uvm_current_realm(vm);
    if (r == NULL) return urbi_raise_type(vm, "detach: no realm to run the strand in", out);
    UTag *tag = keep_tags ? (s ? s->tag : r->root_tag) : r->root_tag;
    UValue recv = (s && s->nframes) ? s->frames[s->nframes - 1].recv : uv_obj(r->globals);
    if (usched_spawn(vm, r, (UClosure *)args[0].v.p, tag, recv, NULL, 0) == NULL)
        return urbi_raise_oom(vm, out);
    *out = uv_nil();
    return UEXEC_OK;
}

static int detach_native(UVM *vm, UValue self, UValue *args, uint8_t n, UValue *out)
{ (void)self; (void)n; return spawn_native(vm, args, out, true, "detach: argument is not a Function"); }
static int disown_native(UVM *vm, UValue self, UValue *args, uint8_t n, UValue *out)
{ (void)self; (void)n; return spawn_native(vm, args, out, false, "disown: argument is not a Function"); }

/* Lobby.connectionTag — the READING realm's connection tag, so one
 * shared Lobby prototype answers correctly in every realm.  A plain slot
 * could only ever hold one realm's tag. */
static int lobby_connection_tag(UVM *vm, UValue self, UValue *a, uint8_t n, UValue *out)
{
    (void)self; (void)a; (void)n;
    URealm *r = uvm_current_realm(vm);
    *out = (r && r->root_tag) ? uv_ptr(UV_CELL, r->root_tag) : uv_nil();
    return UEXEC_OK;
}

static const UMethodDef ustdlib_sched_globals[] = {
    { "sleep",            sleep_native,     1, 1 },
    { "every",            every_native,     2, 2 },
    { "scopeTag",         scope_tag_native, 0, 0 },
    { "__detach_strand",  detach_native,    1, 1 },
    { "__disown_strand",  disown_native,    1, 1 }
};

/* --- installation ---------------------------------------------------------- */

int usched_natives_init(UVM *vm)
{
    int rc = uboot_install_methods(vm, vm->protos[UP_TAG], ustdlib_tag_methods,
                                   (uint16_t)(sizeof ustdlib_tag_methods / sizeof ustdlib_tag_methods[0]));
    if (rc == URBI_OK)
        rc = uboot_install_methods(vm, vm->protos[UP_EVENT], ustdlib_event_methods,
                                   (uint16_t)(sizeof ustdlib_event_methods / sizeof ustdlib_event_methods[0]));
    if (rc == URBI_OK)
        rc = uboot_install_methods(vm, vm->protos[UP_STRAND], ustdlib_job_methods,
                                   (uint16_t)(sizeof ustdlib_job_methods / sizeof ustdlib_job_methods[0]));
    /* The five scheduler globals go on the shared root object, so a bare
     * `sleep(1s)` resolves from any realm the same way `echo` does. */
    if (rc == URBI_OK)
        rc = uboot_install_methods(vm, vm->root_globals, ustdlib_sched_globals,
                                   (uint16_t)(sizeof ustdlib_sched_globals / sizeof ustdlib_sched_globals[0]));
    if (rc != URBI_OK) return rc;

    {
        USym *name = usym_cstr(vm, "connectionTag");
        UClosure *g = uclosure_native(vm, lobby_connection_tag, 0, 0);
        if (!name || !g) return URBI_ERR_OOM;
        g->name = "connectionTag";
        if (urbi_object_install_property(vm, vm->protos[UP_LOBBY], name,
                                         uv_ptr(UV_CELL, g), uv_nil(), uv_nil()) != 0)
            return URBI_ERR_OOM;
    }
    return URBI_OK;
}
