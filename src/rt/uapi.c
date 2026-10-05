/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uapi.c — the public C API, implemented on the runtime core.
 * See include/urbi/urbi.h for the contract of every function here. */

#include "rt/uboot.h"
#include "rt/ustdlib_glue.h"
#include "rt/uwatch.h"
#include "urbi/urbi.h"
#include "chunk/uchunk.h"
#include "emit/ufront.h"

/* src/rt/ is held to the freestanding rule (no libc beyond <stdint.h>,
 * <stddef.h>, <stdbool.h>, <string.h>, <math.h>), which
 * tests/scripts/check_rt_layering.sh enforces.  urbi_value_to_string
 * needs snprintf's "%.14g" and therefore lives in src/host/uformat.c. */

#ifndef URBI_BYTECODE_ONLY
/* Copies a literal into a caller buffer, always NUL-terminating. */
static void uapi_set_err(char *err, size_t errcap, const char *msg)
{
    if (!err || errcap == 0) return;
    size_t i = 0;
    while (msg[i] && i + 1 < errcap) { err[i] = msg[i]; i++; }
    err[i] = '\0';
}
#endif /* !URBI_BYTECODE_ONLY */

/* ===================================================================
 * Lifecycle
 * =================================================================== */

UVM *urbi_open(UVMAllocFn alloc, void *ud, const UVMConfig *config)
{
    UVM *vm = uvm_open((UAllocFn)alloc, ud);
    if (!vm) return NULL;
    /* config->step_budget is what urbi_step spends when its caller passes
     * 0; leaving it unset keeps 0 meaning "until nothing is runnable".
     * The heap budget is set before boot so the stdlib install already
     * paces against it. */
    if (config) {
        uvm_sched(vm)->default_budget = config->step_budget;
        vm->gc.heap_budget = config->heap_budget;
    }
#if __STDC_HOSTED__ && !defined(URBI_BYTECODE_ONLY)
    /* src/host is in this archive on a hosted build, so the unwinder and
     * the stdlib can borrow its formatter for the values the core cannot
     * spell on its own.  A freestanding or bytecode-only build has no
     * formatter and leaves the hook NULL; nothing here reaches into
     * src/host directly, the symbol is the public one from <urbi/urbi.h>. */
    vm->render_value = urbi_value_to_string;
#endif
    /* config->boot_stdlib defaults to 1; a host that passes a config
     * asking for 0 gets a VM with no built-ins at all, which is only
     * useful for measuring the bare core. */
    if (!config || config->boot_stdlib) {
        if (uboot_init(vm) != URBI_OK) { uvm_close(vm); return NULL; }
    }
    return vm;
}

void urbi_close(UVM *vm) { uvm_close(vm); }

int urbi_step(UVM *vm, uint32_t budget, uint64_t *next_wake_us)
{
    if (!vm) return URBI_ERR_INVALID_ARG;
    /* A caller that names no budget gets the one the VM was configured
     * with, which is itself 0 -- "until nothing is runnable" -- unless the
     * host asked for a bounded slice at urbi_open. */
    if (budget == 0) budget = uvm_sched(vm)->default_budget;
    /* USTEP_* and URBI_STEP_* are the same three values, declared apart
     * so the core's header does not have to be the public one. */
    switch (usched_step(vm, budget, next_wake_us)) {
    case USTEP_IDLE_UNTIL: return URBI_STEP_IDLE_UNTIL;
    case USTEP_QUIESCENT:  return URBI_STEP_QUIESCENT;
    default:               return URBI_STEP_RAN;
    }
}

bool urbi_has_live_work(UVM *vm) { return usched_has_live_work(vm); }

void urbi_set_clock(UVM *vm, uint64_t (*fn)(void *ud), void *ud)
{ if (vm) { vm->clock_us = fn; vm->clock_ud = ud; } }

void urbi_set_diag(UVM *vm, void (*fn)(UVM *, void *, int, const char *, size_t), void *ud)
{ if (vm) { vm->diag = fn; vm->diag_ud = ud; } }

void urbi_set_writer(UVM *vm, void (*fn)(void *, const char *, size_t, const char *, size_t), void *ud)
{ if (vm) { vm->writer = fn; vm->writer_ud = ud; } }

void urbi_set_wake(UVM *vm, void (*fn)(void *ud), void *ud)
{ if (vm) { vm->wake = fn; vm->wake_ud = ud; } }

/* ===================================================================
 * Realms
 * =================================================================== */

URealm *urbi_realm_new(UVM *vm) { return vm ? urealm_new(vm) : NULL; }

/* Lazily created: a host that only ever calls urbi_run(vm, NULL, ...)
 * never names a realm, so the main one is built on first demand rather
 * than at open. */
URealm *urbi_realm_main(UVM *vm)
{
    if (!vm) return NULL;
    if (!vm->main_realm) (void)urealm_new(vm);
    return vm->main_realm;
}

void urbi_realm_free(UVM *vm, URealm *realm) { urealm_free(vm, realm); }

void urbi_realm_set_writer(UVM *vm, URealm *realm,
                           void (*fn)(void *, const char *, size_t, const char *, size_t),
                           void *ud)
{ if (vm) urealm_set_writer(vm, realm ? realm : urbi_realm_main(vm), fn, ud); }

UValue urbi_realm_tag(UVM *vm, URealm *realm)
{
    if (!vm) return urbi_make_nil();
    if (!realm) realm = urbi_realm_main(vm);
    return (realm && realm->root_tag) ? uv_ptr(UV_CELL, realm->root_tag) : urbi_make_nil();
}

/* ===================================================================
 * Code
 * =================================================================== */

/* The source-taking entry points (urbi_compile, urbi_run, urbi_watch)
 * need the compiler frontend, which a bytecode-only build does not have.
 * They are absent from that archive rather than stubbed, so a call to one
 * fails at link time instead of at run time. */
#ifndef URBI_BYTECODE_ONLY
int urbi_compile(UVM *vm, const char *src, size_t n, const char *name,
                 uint8_t **out_bytes, size_t *out_len, char *err, size_t errcap)
{
    if (!vm || !src || !out_bytes || !out_len) return URBI_ERR_INVALID_ARG;
    *out_bytes = NULL;
    *out_len = 0;

    UProto *root = NULL;
    /* No realm, so no budget: urbi_compile is the host compiling its own
     * source ahead of time, not a session compiling text off a wire. */
    int rc = ufront_compile(vm, src, n, name, NULL, &root, err, errcap);
    if (rc != URBI_OK) return rc;

    ptrdiff_t need = ufront_serialize(root, NULL, 0);
    if (need < 0) {
        uapi_set_err(err, errcap, "serialize size-query failed");
        uchunk_destroy(root, NULL);
        return URBI_ERR_COMPILE;
    }
    uint8_t *buf = (uint8_t *)vm->gc.alloc(NULL, (size_t)need, vm->gc.alloc_ud);
    if (!buf) { uchunk_destroy(root, NULL); return URBI_ERR_OOM; }
    ptrdiff_t wrote = ufront_serialize(root, buf, (size_t)need);
    uchunk_destroy(root, NULL);
    if (wrote != need) {
        vm->gc.alloc(buf, 0, vm->gc.alloc_ud);
        uapi_set_err(err, errcap, "serialize produced an unexpected length");
        return URBI_ERR_COMPILE;
    }
    *out_bytes = buf;
    *out_len = (size_t)need;
    return URBI_OK;
}
#endif /* !URBI_BYTECODE_ONLY */

void urbi_chunk_free(UVM *vm, uint8_t *bytes, size_t n)
{
    (void)n;
    if (vm && bytes) vm->gc.alloc(bytes, 0, vm->gc.alloc_ud);
}

/* The chunk allocator the loader uses, routed at the VM's allocator so a
 * loaded chunk's buffers come from the same place as a compiled one's. */
static void *uapi_chunk_alloc(void *ptr, size_t nbytes, void *ud)
{
    UVM *vm = (UVM *)ud;
    return vm->gc.alloc(ptr, nbytes, vm->gc.alloc_ud);
}

int urbi_load(UVM *vm, URealm *realm, const uint8_t *bytes, size_t n, UValue *out)
{
    if (out) *out = urbi_make_nil();
    if (!vm || !bytes) return URBI_ERR_INVALID_ARG;
    if (!realm) realm = urbi_realm_main(vm);
    if (!realm) return URBI_ERR_INVALID_ARG;

    UProto *root = NULL;
    UChunkLoadError lrc = uchunk_deserialize(&root, bytes, n, uapi_chunk_alloc, vm,
                                             vm->last_error, sizeof vm->last_error);
    if (lrc != UCHUNK_LOAD_OK) {
        vm->last_error_code = (lrc == UCHUNK_LOAD_UNSUPPORTED_VERSION
                            || lrc == UCHUNK_LOAD_FLAVOR_MISMATCH
                            || lrc == UCHUNK_LOAD_BAD_MAGIC)
                            ? URBI_ERR_BYTECODE_VERSION_MISMATCH
                            : (lrc == UCHUNK_LOAD_OOM ? URBI_ERR_OOM : URBI_ERR_COMPILE);
        return vm->last_error_code;
    }

    UProtoCell *pc = uproto_bind(vm, root);
    if (!pc) return URBI_ERR_OOM;
    pc->cell.flags |= UCELL_F_RTPIN;
    UClosure *cl = uclosure_new(vm, root, 0);
    pc->cell.flags &= (uint16_t)~UCELL_F_RTPIN;
    if (!cl) return URBI_ERR_OOM;
    if (vm->protos[UP_CLOSURE]) cl->proto_obj = vm->protos[UP_CLOSURE];

    /* Same path as urbi_run: a loaded chunk is a chunk, and it gets the
     * same scheduled strand and the same pump. */
    return uexec_run_chunk(vm, realm, cl, out);
}

#ifndef URBI_BYTECODE_ONLY
int urbi_run(UVM *vm, URealm *realm, const char *src, size_t n, const char *name,
             UValue *out, char *err, size_t errcap)
{
    if (!vm) return URBI_ERR_INVALID_ARG;
    if (!realm) realm = urbi_realm_main(vm);
    return uexec_run_source(vm, realm, src, n, name, out, err, errcap);
}
#endif /* !URBI_BYTECODE_ONLY */

int urbi_call(UVM *vm, URealm *realm, UValue callee, UValue recv,
              const UValue *argv, uint8_t argc, UValue *out)
{
    if (out) *out = urbi_make_nil();
    if (!vm) return URBI_ERR_INVALID_ARG;
    if (!realm) realm = urbi_realm_main(vm);
    if (callee.kind != UV_CELL || ((UCell *)callee.v.p)->type != UCELL_CLOSURE)
        return URBI_ERR_INVALID_ARG;
    UStrand *s = uvm_spare_acquire(vm, realm);
    if (!s) return URBI_ERR_OOM;
    UValue res = urbi_make_nil();
    int crc = uexec_call(vm, s, (UClosure *)callee.v.p, recv, argv, argc, &res);
    uvm_spare_release(vm, s);
    int rc = uexec_finish_run(vm, crc);
    if (rc == URBI_OK && out) *out = res;
    return rc;
}

/* ===================================================================
 * Values
 * =================================================================== */

UValue urbi_make_string(UVM *vm, const char *bytes, size_t n)
{
    if (!vm || !bytes) return urbi_make_nil();
    UStr *s = ustr_new(vm, bytes, n);
    return s ? uv_str(s) : urbi_make_nil();
}

/* The public constructors accept a NULL pointer, and a kind query is what
 * an embedder runs before validating anything, so a NULL cell answers
 * "no" / "nil" rather than being dereferenced. */
static bool uapi_cell_is(UValue v, UCellType t)
{ return v.kind == UV_CELL && v.v.p != NULL && ((const UCell *)v.v.p)->type == (uint8_t)t; }

bool urbi_value_is_closure(UValue v) { return uapi_cell_is(v, UCELL_CLOSURE); }
bool urbi_value_is_event(UValue v)   { return uapi_cell_is(v, UCELL_EVENT); }
bool urbi_value_is_tag(UValue v)     { return uapi_cell_is(v, UCELL_TAG); }
bool urbi_value_is_strand(UValue v)  { return uapi_cell_is(v, UCELL_STRAND); }

urbi_value_kind_t urbi_value_kind(UValue v)
{
    if (v.kind == UV_SYM) return URBI_VALUE_STR;
    if (v.kind != UV_CELL) return (urbi_value_kind_t)v.kind;
    if (v.v.p == NULL) return URBI_VALUE_NIL;
    switch (((const UCell *)v.v.p)->type) {
    case UCELL_CLOSURE: return URBI_VALUE_CLOSURE;
    case UCELL_EVENT:   return URBI_VALUE_EVENT;
    case UCELL_TAG:     return URBI_VALUE_TAG;
    case UCELL_STRAND:  return URBI_VALUE_STRAND;
    default:            return URBI_VALUE_CELL;
    }
}

const char *urbi_value_as_str(UValue v, size_t *out_len)
{
    if ((v.kind != UV_STR && v.kind != UV_SYM) || v.v.p == NULL) { if (out_len) *out_len = 0; return NULL; }
    uint32_t n = 0;
    const char *p = uv_str_bytes(v, &n);
    if (out_len) *out_len = n;
    return p;
}

/* One pin bit per cell, not a counter — see the header.  The runtime
 * never touches THIS bit: what the core needs to hold across an
 * allocation goes on a strand's C-root stack, or on the separate
 * UCELL_F_RTPIN bit (src/rt/ugc.h) when there is no strand to hang it
 * on.  So a host's pin is only ever cleared by that host's own
 * urbi_unref, whatever script runs in between. */
void urbi_ref(UVM *vm, UValue v)
{
    (void)vm;
    if (v.kind == UV_STR || v.kind == UV_OBJ || v.kind == UV_CELL)
        ((UCell *)v.v.p)->flags |= UCELL_F_PINNED;
}

void urbi_unref(UVM *vm, UValue v)
{
    (void)vm;
    if (v.kind == UV_STR || v.kind == UV_OBJ || v.kind == UV_CELL)
        ((UCell *)v.v.p)->flags &= (uint16_t)~UCELL_F_PINNED;
}

/* ===================================================================
 * Globals and slots
 * =================================================================== */

int urbi_global_get(UVM *vm, URealm *realm, const char *name, UValue *out)
{
    if (out) *out = urbi_make_nil();
    if (!vm || !name) return URBI_ERR_INVALID_ARG;
    if (!realm) realm = urbi_realm_main(vm);
    if (!realm || !realm->globals) return URBI_ERR_INVALID_ARG;
    const USym *sym = usym_cstr(vm, name);
    if (!sym) return URBI_ERR_OOM;
    UObjSlotRef ref;
    if (!uobj_resolve(vm, realm->globals, sym, &ref)) return URBI_ERR_INVALID_ARG;
    if (out) *out = uobj_slot_value(&ref);
    return URBI_OK;
}

int urbi_global_set(UVM *vm, URealm *realm, const char *name, UValue v)
{
    if (!vm || !name) return URBI_ERR_INVALID_ARG;
    if (!realm) realm = urbi_realm_main(vm);
    if (!realm || !realm->globals) return URBI_ERR_INVALID_ARG;
    USym *sym = usym_cstr(vm, name);
    if (!sym) return URBI_ERR_OOM;
    bool existed = uobj_find_local(realm->globals, sym) >= 0;
    if (uobj_set_local(vm, realm->globals, sym, v, 0) < 0) return URBI_ERR_OOM;
    /* A host write is a write: it re-arms the dirty set and fires the
     * slot's change event exactly as OP_SETSLOT would, which is what lets
     * a slot set between two steps wake a `waituntil` on it. */
    uexec_note_write(vm, realm->globals, sym, v, existed);
    return URBI_OK;
}

int urbi_slot_get(UVM *vm, UValue obj, const char *name, UValue *out)
{
    if (out) *out = urbi_make_nil();
    if (!vm || !name) return URBI_ERR_INVALID_ARG;
    UObject *o = uv_dispatch_proto(vm, obj);
    if (!o) return URBI_ERR_INVALID_ARG;
    const USym *sym = usym_cstr(vm, name);
    if (!sym) return URBI_ERR_OOM;
    UObjSlotRef ref;
    if (!uobj_resolve(vm, o, sym, &ref)) return URBI_ERR_INVALID_ARG;
    if (out) *out = uobj_slot_value(&ref);
    return URBI_OK;
}

int urbi_slot_set(UVM *vm, UValue obj, const char *name, UValue v)
{
    if (!vm || !name) return URBI_ERR_INVALID_ARG;
    if (obj.kind != UV_OBJ) return URBI_ERR_INVALID_ARG;
    UObject *o = (UObject *)obj.v.p;
    if (o->cell.flags & UOBJ_F_READONLY) return URBI_ERR_INVALID_ARG;
    USym *sym = usym_cstr(vm, name);
    if (!sym) return URBI_ERR_OOM;
    bool existed = uobj_find_local(o, sym) >= 0;
    if (uobj_set_local(vm, o, sym, v, 0) < 0) return URBI_ERR_OOM;
    uexec_note_write(vm, o, sym, v, existed);
    return URBI_OK;
}

/* ===================================================================
 * Host functions
 * =================================================================== */

int urbi_register(UVM *vm, const char *path, urbi_native_fn fn,
                  uint8_t min_args, uint8_t max_args)
{
    if (!vm || !path || !fn) return URBI_ERR_INVALID_ARG;
    URealm *realm = urbi_realm_main(vm);   /* creates it if the host never named one */
    if (!realm || !realm->globals) return URBI_ERR_INVALID_STATE;

    UObject *owner = realm->globals;
    const char *seg = path;
    for (;;) {
        const char *dot = seg;
        while (*dot && *dot != '.') dot++;
        if (*dot == '\0') break;                    /* `seg` is the final slot name */
        const USym *sym = usym_intern(vm, seg, (size_t)(dot - seg));
        if (!sym) return URBI_ERR_OOM;
        UObjSlotRef ref;
        if (!uobj_resolve(vm, owner, sym, &ref)) return URBI_ERR_INVALID_STATE;
        UValue v = uobj_slot_value(&ref);
        if (v.kind != UV_OBJ) return URBI_ERR_INVALID_STATE;
        owner = (UObject *)v.v.p;
        seg = dot + 1;
    }
    if (*seg == '\0') return URBI_ERR_INVALID_ARG;

    USym *name = usym_cstr(vm, seg);
    if (!name) return URBI_ERR_OOM;
    /* uclosure_native may collect, and `owner` is only reachable through
     * a C local here.  Root it on a spare strand rather than through
     * urbi_ref: the pin bit belongs to the host, and clearing it here
     * would unpin an object the embedder had pinned itself. */
    UStrand *s = uvm_spare_acquire(vm, realm);
    if (!s) return URBI_ERR_OOM;
    UValue ownerv = uv_obj(owner);
    USTRAND_ROOT(s, ownerv);
    UClosure *cl = uclosure_native(vm, fn, min_args, max_args);
    USTRAND_UNROOT(s, ownerv);
    uvm_spare_release(vm, s);
    if (!cl) return URBI_ERR_OOM;
    /* The interned bytes, not the caller's `path`: a diagnostic must not
     * point into a buffer the host is free to reuse. */
    cl->name = name->bytes;
    return uobj_set_local(vm, owner, name, uv_ptr(UV_CELL, cl), 0) < 0 ? URBI_ERR_OOM : URBI_OK;
}

int urbi_throw(UVM *vm, const char *proto, const char *msg)
{
    if (!vm) return URBI_ERR_INVALID_ARG;
    UStrand *s = vm->sched.current;
    if (!s) return URBI_ERR_INVALID_STATE;
    int which = UP_EXCEPTION;
    if (proto) {
        static const struct { const char *name; int idx; } known[] = {
            { "TypeError", UP_TYPEERROR }, { "ArityError", UP_ARITYERROR },
            { "LookupError", UP_LOOKUPERROR }, { "OutOfMemoryError", UP_OOMERROR },
            { "IndexError", UP_INDEXERROR }, { "RangeError", UP_RANGEERROR },
            { "DivByZero", UP_DIVBYZERO }, { "Exception", UP_EXCEPTION }
        };
        for (size_t k = 0; k < sizeof known / sizeof known[0]; k++) {
            if (strcmp(known[k].name, proto) == 0) { which = known[k].idx; break; }
        }
    }
    return uexec_throw(vm, s, which, msg);
}

/* ===================================================================
 * Events, watchers, tags
 * ===================================================================
 *
 * urbi_watch is the one entry still waiting on the reactive task; the
 * rest run on the scheduler. */

/* A name for a tag or an event arrives as an interned symbol so the cell
 * holds no pointer into a host buffer it does not own. */
static UValue uapi_name_value(UVM *vm, const char *name, int *oom)
{
    *oom = 0;
    if (name == NULL || name[0] == '\0') return urbi_make_nil();
    UValue v = urbi_make_str_interned(vm, name, strlen(name));
    if (v.kind == UV_NIL) *oom = 1;
    return v;
}

int urbi_event_new(UVM *vm, URealm *realm, const char *name, UValue *out)
{
    (void)realm;
    if (out) *out = urbi_make_nil();
    if (!vm) return URBI_ERR_INVALID_ARG;
    int oom = 0;
    UValue nv = uapi_name_value(vm, name, &oom);
    if (oom) return URBI_ERR_OOM;
    UEvent *e = uevent_new(vm, nv);
    if (!e) return URBI_ERR_OOM;
    if (out) *out = uv_ptr(UV_CELL, e);
    return URBI_OK;
}

int urbi_event_emit(UVM *vm, UValue event, UValue payload)
{
    if (!vm || event.kind != UV_CELL || ((UCell *)event.v.p)->type != UCELL_EVENT)
        return URBI_ERR_INVALID_ARG;
    uevent_emit(vm, (UEvent *)event.v.p, payload);
    return URBI_OK;
}

int urbi_event_register(UVM *vm, URealm *realm, const char *name, urbi_event_id_t *out_id)
{
    if (out_id) *out_id = URBI_EVENT_ID_INVALID;
    if (!vm || !name) return URBI_ERR_INVALID_ARG;
    USched *sc = uvm_sched(vm);
    /* An id-registered event is held for the life of the VM (usched_mark
     * roots the table), so an interrupt handler can never name one the
     * collector has taken. */
    for (uint16_t i = 0; i < sc->event_count; i++) {
        const UEvent *e = sc->events[i];
        if (e && urbi_is_str(e->name) && strcmp(urbi_str_cstr(e->name), name) == 0) {
            if (out_id) *out_id = (urbi_event_id_t)i;
            return URBI_OK;
        }
    }
    /* OOM, not INVALID_STATE: the id table is a fixed resource that ran
     * out, and INVALID_STATE is this API's "not available in this build"
     * marker. */
    if (sc->event_count >= USCHED_MAX_EVENTS) return URBI_ERR_OOM;
    UValue ev = urbi_make_nil();
    int rc = urbi_event_new(vm, realm, name, &ev);
    if (rc != URBI_OK) return rc;
    sc->events[sc->event_count] = (UEvent *)ev.v.p;
    if (out_id) *out_id = (urbi_event_id_t)sc->event_count;
    sc->event_count++;
    return URBI_OK;
}

/* The one entry point an interrupt handler may call.  No allocation, no
 * lock, no VM state touched beyond one ring slot: the release store on
 * `isr_head` is what publishes the record to the next urbi_step. */
int urbi_inject_event(UVM *vm, urbi_event_id_t id, const urbi_event_payload_t *payload, size_t n)
{
    if (!vm || n > URBI_EVENT_PAYLOAD_MAX) return URBI_ERR_INVALID_ARG;
    USched *sc = &vm->sched;
    /* Reject an id nothing was registered under, rather than accepting it
     * and dropping it silently at the drain.  Reading event_count from an
     * interrupt races with a concurrent urbi_event_register, whose only
     * outcome is rejecting an id registered microseconds ago. */
    if (id >= sc->event_count) return URBI_ERR_INVALID_ARG;
    uint32_t head = sc->isr_head;
    uint32_t tail = __atomic_load_n(&sc->isr_tail, __ATOMIC_ACQUIRE);
    if (head - tail >= USCHED_ISR_SLOTS) return URBI_ERR_OOM;   /* ring full: drop */
    UIsrRec *r = &sc->isr[head % USCHED_ISR_SLOTS];
    r->id = id;
    r->n = (uint8_t)n;
    if (payload && n) memcpy(&r->payload, payload, n);
    __atomic_store_n(&sc->isr_head, head + 1u, __ATOMIC_RELEASE);
    if (vm->wake) vm->wake(vm->wake_ud);
    return URBI_OK;
}

int urbi_event_value(UVM *vm, urbi_event_id_t id, UValue *out)
{
    if (out) *out = urbi_make_nil();
    if (!vm || !out) return URBI_ERR_INVALID_ARG;
    USched *sc = uvm_sched(vm);
    if (id >= sc->event_count || sc->events[id] == NULL) return URBI_ERR_INVALID_ARG;
    *out = uv_ptr(UV_CELL, sc->events[id]);
    return URBI_OK;
}

/* The expression is compiled as an ordinary chunk and its root closure
 * becomes the watcher's condition: a chunk's value is its last statement's
 * value, which for a one-expression source is the expression.  The
 * callback takes the place of a body closure -- see rt/uwatch.h. */
#ifndef URBI_BYTECODE_ONLY
int urbi_watch(UVM *vm, URealm *realm, const char *expr,
               int (*cb)(UVM *, void *, UValue), void *ud)
{
    if (!vm || !expr || !cb) return URBI_ERR_INVALID_ARG;
    if (!realm) realm = urbi_realm_main(vm);
    if (!realm) return URBI_ERR_INVALID_ARG;

    UProto *root = NULL;
    int rc = ufront_compile(vm, expr, strlen(expr), "<watch>", &realm->budget, &root,
                            vm->last_error, sizeof vm->last_error);
    if (rc != URBI_OK) { vm->last_error_code = rc; return rc; }

    UProtoCell *pc = uproto_bind(vm, root);   /* takes ownership either way */
    if (!pc) return URBI_ERR_OOM;
    pc->cell.flags |= UCELL_F_RTPIN;
    UClosure *cl = uclosure_new(vm, root, 0);
    pc->cell.flags &= (uint16_t)~UCELL_F_RTPIN;
    if (!cl) return URBI_ERR_OOM;
    if (vm->protos[UP_CLOSURE]) cl->proto_obj = vm->protos[UP_CLOSURE];

    cl->cell.flags |= UCELL_F_RTPIN;         /* reachable from nothing yet */
    /* Tagged with the realm's connection tag, which urbi_realm_tag hands
     * back, so a host can stop its own watches. */
    const UWatcher *w = uwatch_install_host(vm, realm, cl, cb, ud);
    cl->cell.flags &= (uint16_t)~UCELL_F_RTPIN;
    return w ? URBI_OK : URBI_ERR_OOM;
}
#endif /* !URBI_BYTECODE_ONLY */

int urbi_tag_new(UVM *vm, URealm *realm, const char *name, UValue *out)
{
    (void)realm;
    if (out) *out = urbi_make_nil();
    if (!vm) return URBI_ERR_INVALID_ARG;
    int oom = 0;
    UValue nv = uapi_name_value(vm, name, &oom);
    if (oom) return URBI_ERR_OOM;
    UTag *t = utag_new(vm, nv);
    if (!t) return URBI_ERR_OOM;
    if (out) *out = uv_ptr(UV_CELL, t);
    return URBI_OK;
}

/* The public signature takes a plain UVM handle like every other entry
 * point; this helper only validates, but its callers mutate. */
/* cppcheck-suppress constParameterPointer */
static UTag *uapi_tag(UVM *vm, UValue tag)
{
    if (!vm || tag.kind != UV_CELL || ((UCell *)tag.v.p)->type != UCELL_TAG) return NULL;
    return (UTag *)tag.v.p;
}

int urbi_tag_stop(UVM *vm, UValue tag)
{
    UTag *t = uapi_tag(vm, tag);
    if (!t) return URBI_ERR_INVALID_ARG;
    (void)utag_stop(vm, t);
    return URBI_OK;
}

static int uapi_tag_gate(UVM *vm, UValue tag, uint8_t bit, bool on)
{
    UTag *t = uapi_tag(vm, tag);
    if (!t) return URBI_ERR_INVALID_ARG;
    utag_gate(vm, t, bit, on);
    return URBI_OK;
}

int urbi_tag_block(UVM *vm, UValue tag)    { return uapi_tag_gate(vm, tag, USTRAND_GATE_BLOCKED, true); }
int urbi_tag_unblock(UVM *vm, UValue tag)  { return uapi_tag_gate(vm, tag, USTRAND_GATE_BLOCKED, false); }
int urbi_tag_freeze(UVM *vm, UValue tag)   { return uapi_tag_gate(vm, tag, USTRAND_GATE_FROZEN, true); }
int urbi_tag_unfreeze(UVM *vm, UValue tag) { return uapi_tag_gate(vm, tag, USTRAND_GATE_FROZEN, false); }

/* ===================================================================
 * Errors, GC, version
 * =================================================================== */

/* The public signature takes a plain UVM handle, like every other entry
 * point; it does not become const just because this one only reads. */
/* cppcheck-suppress constParameterPointer */
int urbi_last_error(UVM *vm, UErrorInfo *info)
{
    if (!vm) return URBI_ERR_INVALID_ARG;
    if (info) { info->code = vm->last_error_code; info->message = vm->last_error; }
    return vm->last_error_code;
}

void urbi_clear_error(UVM *vm)
{
    if (!vm) return;
    vm->last_error[0] = '\0';
    vm->last_error_code = URBI_OK;
}

void urbi_gc_collect(UVM *vm) { if (vm) ugc_collect(vm); }

int urbi_gc_stats(UVM *vm, UGcStats *out)
{
    if (!vm || !out) return URBI_ERR_INVALID_ARG;
    out->bytes_live  = vm->gc.bytes_live;
    out->bytes_since = vm->gc.bytes_since;
    out->cells_live  = vm->gc.cells_live;
    out->cycles      = vm->gc.cycles;
    out->heap_budget = vm->gc.heap_budget;
    return URBI_OK;
}

const char *urbi_version(void) { return URBI_RELEASE_STRING; }

void urbi_api_version(int *out_major, int *out_minor, int *out_patch)
{
    if (out_major) *out_major = URBI_API_VERSION_MAJOR;
    if (out_minor) *out_minor = URBI_API_VERSION_MINOR;
    if (out_patch) *out_patch = URBI_API_VERSION_PATCH;
}
