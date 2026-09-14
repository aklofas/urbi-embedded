/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uexec.c — VM lifecycle, GC hooks, realms, closures, throwing.
 * The dispatch loop itself lives in uexec_ops.c.  See rt/uexec.h. */

#include "rt/uexec.h"
#include "chunk/uchunk.h"   /* uchunk_destroy — the UCELL_PROTO finaliser */

/* --- accessors (the four the lower layers reach the VM through) ------ */

UGc       *uvm_gc(UVM *vm)       { return &vm->gc; }
UStrTab   *uvm_strings(UVM *vm)  { return &vm->strings; }
UObjStats *uvm_objstats(UVM *vm) { return &vm->objstats; }
USched    *uvm_sched(UVM *vm)    { return &vm->sched; }

/* --- small freestanding string helpers ------------------------------- */

/* Appends as much of `s` as fits, always NUL-terminating.  Returns the
 * new length.  src/rt has no <stdio.h> by policy, so error strings are
 * assembled by concatenation rather than snprintf. */
static size_t uexec_str_append(char *buf, size_t cap, size_t at, const char *s)
{
    if (cap == 0) return 0;
    while (s && *s && at + 1 < cap) buf[at++] = *s++;
    buf[at] = '\0';
    return at;
}

/* Names for the built-in prototypes, used to label a throw before the
 * boot table exists (and to fill the exception's `name` slot, which is
 * what uexec_unwind reads back).  Index-parallel with the UP_* enum. */
static const char *const uexec_proto_names[UP_COUNT] = {
    "Object", "Integer", "Float", "String", "Boolean", "nil", "void", "List", "Dictionary", "Symbol",
    "Closure", "Tag", "Event", "Job", "Exception", "TypeError", "ArityError", "LookupError",
    "OutOfMemoryError", "IndexError", "RangeError", "DivByZero", "Lobby", "Channel", "Math", "System", "Date",
    "Duration", "RegExp", "Mutex", "Pair", "Triplet", "Tuple", "Debug", "Global", "CallMessage"
};

/* --- GC hooks -------------------------------------------------------- */

void uvm_gc_mark_fixed(UVM *vm)
{
    for (int i = 0; i < UP_COUNT; i++) if (vm->protos[i]) ugc_mark(vm, &vm->protos[i]->cell);
    if (vm->root_globals) ugc_mark(vm, &vm->root_globals->cell);
    for (URealm *r = vm->realms; r; r = r->next) ugc_mark(vm, &r->cell);
    for (UStrand *s = vm->spare; s; s = s->link) ugc_mark(vm, &s->cell);
    for (UStrand *s = vm->spare_active; s; s = s->link) ugc_mark(vm, &s->cell);
    usched_mark(vm);
    uwatch_mark(vm);
    if (vm->test_mark_extra) vm->test_mark_extra(vm, vm->test_mark_ud);
}

/* A closure keeps its chunk alive through the root proto's back-pointer
 * (see rt/uexec.h).  owning_module_instance is declared as a
 * UChunkInstance* by the kept loader header; the new core reuses the
 * field verbatim as "the UProtoCell this tree was bound into". */
static void uvm_mark_chunk_of(UVM *vm, UProto *p)
{
    if (!p) return;
    UProto *root = uproto_root_of(p);
    if (root && root->owning_module_instance) ugc_mark(vm, (UCell *)root->owning_module_instance);
}

void uvm_gc_trace(UVM *vm, UCell *c)
{
    switch (c->type) {
    case UCELL_OBJ: uobj_trace(vm, (UObject *)c); break;
    case UCELL_LIST: ulist_trace(vm, (UList *)c); break;
    case UCELL_DICT: udict_trace(vm, (UDict *)c); break;
    case UCELL_STRAND: ustrand_trace(vm, (UStrand *)c); break;
    case UCELL_PROPS: {
        const UProps *p = (const UProps *)c;
        ugc_mark_value(vm, p->getter);
        ugc_mark_value(vm, p->setter);
        ugc_mark_value(vm, p->value);
        break;
    }
    case UCELL_CLOSURE: {
        UClosure *cl = (UClosure *)c;
        uvm_mark_chunk_of(vm, cl->proto);
        if (cl->proto_obj) ugc_mark(vm, &cl->proto_obj->cell);
        for (uint8_t i = 0; i < cl->nupvals; i++) if (cl->upvals[i]) ugc_mark(vm, &cl->upvals[i]->cell);
        break;
    }
    case UCELL_UPVAL: {
        UUpval *u = (UUpval *)c;
        /* Open upvalues point into a live register window, which the
         * owning strand's trace already covers. */
        if (u->ptr == &u->closed) ugc_mark_value(vm, u->closed);
        break;
    }
    case UCELL_REALM:
        urealm_trace(vm, (URealm *)c);
        break;
    case UCELL_TAG:   utag_trace(vm, (UTag *)c); break;
    case UCELL_EVENT: uevent_trace(vm, (UEvent *)c); break;
    case UCELL_WATCHER: uwatch_trace(vm, (UWatcher *)c); break;
    default:
        /* Nothing else owns GC cells.  UCELL_PROTO in particular: binding
         * rewrote its constants to UV_SYM and its IC names to USym, both
         * immortal, so a bound chunk has no children to mark. */
        break;
    }
}

void uvm_gc_finalize(UVM *vm, UCell *c)
{
    switch (c->type) {
    case UCELL_OBJ: uobj_finalize(vm, (UObject *)c); break;
    case UCELL_LIST: ulist_finalize(vm, (UList *)c); break;
    case UCELL_DICT: udict_finalize(vm, (UDict *)c); break;
    case UCELL_STRAND: ustrand_finalize(vm, (UStrand *)c); break;
    case UCELL_PROTO: {
        UProtoCell *pc = (UProtoCell *)c;
        /* Unlink from vm->bound_protos before the chunk goes away.  Every
         * other entry on the list is still allocated (sweep frees one cell
         * at a time and each unlinks itself here). */
        for (UProtoCell **pp = &vm->bound_protos; *pp; pp = &(*pp)->next_bound) {
            if (*pp == pc) { *pp = pc->next_bound; break; }
        }
        if (pc->root) {
            pc->root->owning_module_instance = NULL;
            uchunk_destroy(pc->root, NULL);   /* refcount is always 0 here: the new core never bumps it */
            pc->root = NULL;
        }
        break;
    }
    default: break;
    }
}

/* --- lifecycle -------------------------------------------------------- */

UVM *uvm_open(UAllocFn alloc, void *ud)
{
    if (!alloc) return NULL;
    UVM *vm = (UVM *)alloc(NULL, sizeof(UVM), ud);
    if (!vm) return NULL;
    memset(vm, 0, sizeof *vm);
    ugc_init(&vm->gc, alloc, ud);
    vm->gc.hooks.mark_fixed = uvm_gc_mark_fixed;
    vm->gc.hooks.trace      = uvm_gc_trace;
    vm->gc.hooks.finalize   = uvm_gc_finalize;
    if (ustrtab_init(vm, &vm->strings) != 0) { alloc(vm, 0, ud); return NULL; }
    return vm;
}

void uvm_close(UVM *vm)
{
    if (!vm) return;
    UAllocFn alloc = vm->gc.alloc;
    void *ud = vm->gc.alloc_ud;
    /* Drop the fixed roots first so the final sweep really does reach
     * every cell (including chunks, whose finaliser frees the loader
     * buffers). */
    for (int i = 0; i < UP_COUNT; i++) vm->protos[i] = NULL;
    vm->root_globals = NULL;
    vm->realms = NULL;
    vm->main_realm = NULL;
    vm->spare = NULL;
    vm->spare_active = NULL;
    vm->sched.run_head = vm->sched.run_tail = vm->sched.current = NULL;
    vm->sched.dead = NULL;
    vm->watch.all = NULL;
    /* The timer heap is raw (non-cell) memory the scheduler owns; its
     * records only POINT at cells, so it is released here rather than by
     * the sweep. */
    ugc_raw_free(vm, vm->sched.heap, (size_t)vm->sched.heap_cap * sizeof(UTimer));
    vm->sched.heap = NULL;
    vm->sched.heap_len = vm->sched.heap_cap = 0;
    for (uint16_t i = 0; i < vm->sched.event_count; i++) vm->sched.events[i] = NULL;
    vm->sched.event_count = 0;
    vm->test_mark_extra = NULL;
    ugc_destroy(vm);
    ustrtab_destroy(vm, &vm->strings);
    alloc(vm, 0, ud);
}

/* --- closures --------------------------------------------------------- */

UClosure *uclosure_native(UVM *vm, int (*fn)(UVM *, UValue, UValue *, uint8_t, UValue *),
                          uint8_t min_args, uint8_t max_args)
{
    UClosure *cl = uclosure_new(vm, NULL, 0);
    if (!cl) return NULL;
    cl->native = fn;
    cl->min_args = min_args;
    cl->max_args = max_args;
    if (vm->protos[UP_CLOSURE]) cl->proto_obj = vm->protos[UP_CLOSURE];
    return cl;
}

/* --- dispatch prototype for a receiver value -------------------------- */

/* Every value kind resolves slots somewhere.  An object resolves on
 * itself; every atom resolves on the shared prototype the boot table
 * installed, so `1.clone()` and `"ab".size` reach Integer/String without
 * boxing.  A cell that carries its own proto pointer (List, Dict) uses
 * it when set — a list built before the boot table existed falls back to
 * the VM's List proto.  NULL only before uboot_init has run, which is
 * the one state in which a slot access legitimately has nowhere to go. */
UObject *uv_dispatch_proto(UVM *vm, UValue recv)
{
    switch (recv.kind) {
    case UV_OBJ:   return (UObject *)recv.v.p;
    case UV_INT:   return vm->protos[UP_INTEGER];
    case UV_FLOAT: return vm->protos[UP_FLOAT];
    case UV_STR: case UV_SYM: return vm->protos[UP_STRING];
    case UV_BOOL:  return vm->protos[UP_BOOLEAN];
    case UV_NIL:   return vm->protos[UP_NIL];
    case UV_VOID:  return vm->protos[UP_VOID];
    case UV_CELL:
        switch (((UCell *)recv.v.p)->type) {
        case UCELL_LIST: {
            UList *l = (UList *)recv.v.p;
            return l->proto ? l->proto : vm->protos[UP_LIST];
        }
        case UCELL_DICT: {
            UDict *d = (UDict *)recv.v.p;
            return d->proto ? d->proto : vm->protos[UP_DICT];
        }
        case UCELL_CLOSURE: {
            UClosure *cl = (UClosure *)recv.v.p;
            return cl->proto_obj ? cl->proto_obj : vm->protos[UP_CLOSURE];
        }
        case UCELL_TAG:    return vm->protos[UP_TAG];
        case UCELL_EVENT:  return vm->protos[UP_EVENT];
        case UCELL_STRAND: return vm->protos[UP_STRAND];
        default:           return NULL;
        }
    default: return NULL;
    }
}

/* --- throwing --------------------------------------------------------- */

/* The one raise site.  `with_position` prepends "line N: " -- see
 * uexec_throw_here's declaration for which failures earn one.
 *
 * The message slot carries the type name ("TypeError: too few arguments")
 * rather than leaving it implicit, because that string is what an
 * uncaught throw prints and what `e.message` hands back to script; the
 * separate `name` slot keeps the type available on its own. */
static int uexec_throw_impl(UVM *vm, UStrand *s, int which_proto, const char *msg,
                            int with_position)
{
    const char *pname = (which_proto >= 0 && which_proto < UP_COUNT)
                      ? uexec_proto_names[which_proto] : "Exception";
    char text[256];
    text[0] = '\0';
    /* One derivation feeds both the message prefix and the `line` slot;
     * the slot is filled for every throw, the prefix only for the raises
     * that earn a position (see uexec_throw_here). */
    uint32_t line = uexec_current_line(s);
    size_t at = with_position ? uexec_position_prefix(s, line, text, sizeof text, 0) : 0;
    at = uexec_str_append(text, sizeof text, at, pname);
    at = uexec_str_append(text, sizeof text, at, ": ");
    (void)uexec_str_append(text, sizeof text, at, msg ? msg : "");

    /* Record the pending failure eagerly: the walker re-derives it from
     * the exception object, but a throw raised while allocation is failing
     * would otherwise have nothing to report. */
    (void)uexec_str_append(vm->last_error, sizeof vm->last_error, 0, text);
    vm->last_error_code = URBI_ERR_UNCAUGHT_THROW;

    UObject *e = uobj_new(vm, (which_proto >= 0 && which_proto < UP_COUNT) ? vm->protos[which_proto] : NULL);
    if (e) {
        UValue ev = uv_obj(e);
        USTRAND_ROOT(s, ev);
        USym *kname = usym_cstr(vm, "name");
        USym *kmsg  = usym_cstr(vm, "message");
        USym *kline = usym_cstr(vm, "line");
        USym *pn    = usym_cstr(vm, pname);
        UStr *m     = ustr_new(vm, text, strlen(text));
        if (kname && pn) (void)uobj_set_local(vm, e, kname, uv_sym(pn), 0);
        if (kmsg && m)   (void)uobj_set_local(vm, e, kmsg, uv_str(m), 0);
        if (kline)       (void)uobj_set_local(vm, e, kline, uv_int((int64_t)line), 0);
        USTRAND_UNROOT(s, ev);
        s->transfer = ev;
    } else {
        s->transfer = uv_nil();
    }
    s->unwind = UUNWIND_THROW;
    return UEXEC_THROW;
}

int uexec_throw(UVM *vm, UStrand *s, int which_proto, const char *msg)
{ return uexec_throw_impl(vm, s, which_proto, msg, 0); }

int uexec_throw_here(UVM *vm, UStrand *s, int which_proto, const char *msg)
{ return uexec_throw_impl(vm, s, which_proto, msg, 1); }

int uexec_throw_value(UVM *vm, UStrand *s, UValue v)
{
    (void)vm;
    s->transfer = v;
    s->unwind = UUNWIND_THROW;
    return UEXEC_THROW;
}

/* --- spare strands ----------------------------------------------------- */

UStrand *uvm_spare_acquire(UVM *vm, URealm *realm)
{
    UStrand *s = vm->spare;
    if (s) {
        vm->spare = s->link;
        s->link = NULL;
        s->realm = realm;
    } else {
        s = ustrand_new(vm, realm);
        if (!s) return NULL;
        s->is_spare = 1;
    }
    s->state = USTRAND_READY;
    s->unwind = UUNWIND_NONE;
    s->nframes = 0;
    s->ncleanup = 0;
    s->transfer = uv_nil();
    s->result = uv_nil();
    s->resume_slot = 0;
    s->croots = NULL;
    /* An acquired spare is off the free list and not yet on any realm or
     * run queue, so nothing else keeps it alive: park it on the in-use
     * list, which mark_fixed walks. */
    s->link = vm->spare_active;
    vm->spare_active = s;
    return s;
}

void uvm_spare_release(UVM *vm, UStrand *s)
{
    if (!s) return;
    for (UStrand **pp = &vm->spare_active; *pp; pp = &(*pp)->link) {
        if (*pp == s) { *pp = s->link; break; }
    }
    ustrand_close_upvals(s, 0);
    s->nframes = 0;
    s->ncleanup = 0;
    s->state = USTRAND_PARKED;
    s->unwind = UUNWIND_NONE;
    s->transfer = uv_nil();
    s->result = uv_nil();
    s->resume_slot = 0;
    s->croots = NULL;
    s->tag = NULL;
    s->realm = NULL;
    s->link = vm->spare;
    vm->spare = s;
}

/* --- chunk binding ------------------------------------------------------ */

/* Intern one proto's IC names in place: proto->ic_names is a pointer array
 * sized ic_count and owned by the proto's allocator (freed by
 * uproto_destroy_buffers), so it is rewritten to hold USym* rather than a
 * second array being allocated.  Deserialised chunks arrive with
 * ic_names == NULL and only ic_name_strs populated, so the array is
 * allocated here in that case. */
static int uproto_bind_one(UVM *vm, UProto *p)
{
    /* Both branches below need the proto's allocator: one to allocate an
     * ic_names array a deserialised chunk arrives without, the other to
     * release the loader-owned constant buffers it has just interned.
     * decode_proto tolerates a NULL alloc_fn through a hosted fallback,
     * so neither may assume it is set. */
    if (p->alloc_fn == NULL && (p->ic_count > 0 || p->const_count > 0)) return -1;
    if (p->ic_count > 0) {
        if (p->ic_names == NULL) {
            p->ic_names = (USymbol **)p->alloc_fn(NULL, (size_t)p->ic_count * sizeof(USymbol *), p->alloc_ud);
            if (!p->ic_names) return -1;
        }
        for (uint16_t i = 0; i < p->ic_count; i++) {
            const char *nm = (p->ic_name_strs && p->ic_name_strs[i]) ? p->ic_name_strs[i] : "";
            USym *sym = usym_intern(vm, nm, strlen(nm));
            if (!sym) return -1;
            p->ic_names[i] = (USymbol *)sym;
        }
    }
    for (size_t i = 0; i < p->const_count; i++) {
        if (p->constants[i].kind != (uint8_t)UVAL_STR) continue;
        const char *b = (const char *)p->constants[i].v.p;
        USym *sym = usym_intern(vm, b ? b : "", b ? strlen(b) : 0);
        if (!sym) return -1;
        if (p->constants_owned && b) p->alloc_fn((void *)b, 0, p->alloc_ud);
        p->constants[i] = uv_sym(sym);
    }
    /* Every UVAL_STR constant is now a USym, so the loader must not try to
     * free the (already released) buffers a second time. */
    p->constants_owned = false;
    for (size_t i = 0; i < p->nested_count; i++) {
        if (p->nested[i] && uproto_bind_one(vm, p->nested[i]) != 0) return -1;
    }
    return 0;
}

UProtoCell *uproto_bind(UVM *vm, UProto *root)
{
    if (!root) return NULL;
    UProtoCell *pc = (UProtoCell *)ugc_alloc(vm, UCELL_PROTO, sizeof(UProtoCell));
    if (!pc) { uchunk_destroy(root, NULL); return NULL; }
    /* Publish the back-pointer and the list link before interning: an
     * intern can grow the symbol table but never collects, and a later
     * failure still leaves a consistent (destroyable) cell. */
    pc->root = root;
    pc->next_bound = vm->bound_protos;
    vm->bound_protos = pc;
    root->owning_module_instance = (struct UChunkInstance *)pc;
    if (uproto_bind_one(vm, root) != 0) return NULL;
    return pc;
}
