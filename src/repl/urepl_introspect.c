/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_introspect.c — 9 introspection C primitives (v0.9.1 Task 19).
 *
 * Each primitive walks a chunk of VM state on the MAIN thread and emits a
 * single JSON object.  See urepl_introspect.h banner for the contract;
 * inline notes here cover per-primitive choices and runtime caveats.
 *
 * Profile note (carry-forward): no per-function / per-opcode / per-watcher
 * profiling infrastructure exists in v0.9.1.  urbi_introspect_profile emits
 * empty arrays + a `note` field documenting the deferral; the wire shape is
 * locked so future profile data lands at the same JSON path without a
 * second tooling round-trip.
 *
 * Buffer protocol: every snprintf call is followed by an overflow check.
 * On overflow we return -1 with *out_n holding a best-effort required-size
 * estimate (which may be conservative — we don't try to compute exact
 * truncation cost).  Callers should retry with at least 2x the estimate. */

#include "repl/urepl_introspect.h"

#include "event/uevent.h"
#include "event/uevent_registry.h"
#include "gc/ugc.h"
#include "object/uobject.h"
#include "object/ushape.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "tag/utag.h"
#include "runtime/ucleanup.h"
#include "urbi/urbi.h"
#include "value/uintern.h"
#include "vm/uvm.h"
#include "watcher/uwatcher.h"

#include <stdio.h>
#include <string.h>

/* Inline helper macros.  EMIT_FMT appends a printf-style formatted segment
 * and gives up with -1 + estimate on overflow.  The first arg is the
 * "leftover-cap" expression; subsequent args are the format and operands. */
#define EMIT_INIT()  size_t n = 0
#define EMIT_FMT(...) do {                                              \
    int _w = snprintf(buf + n, (cap > n) ? (cap - n) : 0, __VA_ARGS__); \
    if (_w < 0) { *out_n = cap + 64; return -1; }                       \
    if ((size_t)_w >= ((cap > n) ? (cap - n) : 0)) {                    \
        *out_n = n + (size_t)_w + 64;                                   \
        return -1;                                                      \
    }                                                                   \
    n += (size_t)_w;                                                    \
} while (0)
#define EMIT_DONE()  do { *out_n = n; return URBI_OK; } while (0)

/* ---- shared helpers -------------------------------------------------- */

/* Map a USTRAND_* state to a short stable name.  Reads only the upper-nibble
 * via USTRAND_GET_STATE; reason sub-codes are surfaced separately in the
 * "wait" field below. */
static const char *
strand_state_name(uint8_t state)
{
    switch (state & USTRAND_STATE_MASK) {
    case USTRAND_DORMANT:   return "dormant";
    case USTRAND_READY:     return "ready";
    case USTRAND_RUNNING:   return "running";
    case USTRAND_WAITING:   return "waiting";
    case USTRAND_DEAD:      return "dead";
    case USTRAND_SUSPENDED: return "suspended";
    default:                return "unknown";
    }
}

static const char *
strand_wait_reason_name(uint8_t state)
{
    if ((state & USTRAND_STATE_MASK) != USTRAND_WAITING) return NULL;
    switch (state & USTRAND_REASON_MASK) {
    case USTRAND_REASON_SLEEP:   return "sleep";
    case USTRAND_REASON_WATCHER: return "watcher";
    case USTRAND_REASON_EVENT:   return "event";
    case USTRAND_REASON_JOIN:    return "join";
    case USTRAND_REASON_HOST:    return "host";
    default:                     return NULL;
    }
}

/* JSON-escape and write a string slice into buf (no surrounding quotes).
 * Returns 0 on success, -1 on overflow; *n_inout updated either way. */
static int
emit_json_escape(char *buf, size_t cap, size_t *n_inout,
                 const char *src, size_t src_len)
{
    size_t n = *n_inout;
    size_t i;
    for (i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        const char *esc = NULL;
        char hex[8];
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b";  break;
        case '\f': esc = "\\f";  break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (c < 0x20) {
                snprintf(hex, sizeof hex, "\\u%04x", (unsigned)c);
                esc = hex;
            }
            break;
        }
        if (esc != NULL) {
            size_t need = strlen(esc);
            if (n + need >= cap) { *n_inout = n + need + 64; return -1; }
            /* Appends raw bytes — caller manages NUL-termination at end of buffer.
             * NOLINTNEXTLINE(bugprone-not-null-terminated-result) */
            memcpy(buf + n, esc, need);
            n += need;
        } else {
            if (n + 1 >= cap) { *n_inout = n + src_len - i + 64; return -1; }
            buf[n++] = (char)c;
        }
    }
    *n_inout = n;
    return 0;
}

/* ---- coros ----------------------------------------------------------- */

/* Synthesize a coro_id from the strand pointer (low 32 bits).  UStrand has
 * no explicit id field — the pointer's address suffices as a unique handle
 * per VM run, and that's the same key debug_stack_native expects from
 * Debug.stack(id).  Display as a 32-bit unsigned integer in JSON so it
 * round-trips through json_parse to UVAL_INT cleanly. */
static uint32_t
strand_coro_id(const UStrand *s)
{
    return (uint32_t)((uintptr_t)s & 0xFFFFFFFFu);
}

int
urbi_introspect_coros(UVM *vm, char *buf, size_t cap, size_t *out_n)
{
    if (vm == NULL || buf == NULL || out_n == NULL) return URBI_ERR_INVALID_ARG;
    EMIT_INIT();
    EMIT_FMT("{\"coros\":[");
    int first = 1;
    for (URealm *r = vm->realms_head; r != NULL; r = r->next_in_vm) {
        for (UStrand *s = r->strands_head; s != NULL; s = s->next_in_realm) {
            const char *state = strand_state_name(s->state);
            const char *reason = strand_wait_reason_name(s->state);
            EMIT_FMT("%s{\"id\":%u,\"state\":\"%s\"",
                     first ? "" : ",",
                     strand_coro_id(s), state);
            if (reason != NULL) {
                EMIT_FMT(",\"wait\":\"%s\"", reason);
            }
            EMIT_FMT(",\"realm\":%u}", (unsigned)r->id);
            first = 0;
        }
    }
    EMIT_FMT("]}");
    EMIT_DONE();
}

/* ---- tags ------------------------------------------------------------ */

/* Tags are owned by realms (one root tag per realm via urbi_realm_create).
 * Host-created child tags via urbi_tag_create thread on parent->... — but
 * UTag itself has no central registry.  We enumerate realm root tags here;
 * the v1.x design-risks register tracks "central tag list" as a deferred
 * improvement. */
int
urbi_introspect_tags(UVM *vm, char *buf, size_t cap, size_t *out_n)
{
    if (vm == NULL || buf == NULL || out_n == NULL) return URBI_ERR_INVALID_ARG;
    EMIT_INIT();
    EMIT_FMT("{\"tags\":[");
    int first = 1;
    for (URealm *r = vm->realms_head; r != NULL; r = r->next_in_vm) {
        UTag *t = r->tag;
        if (t == NULL) continue;
        const char *name_str = NULL;
        size_t name_len = 0;
        if (t->name.kind == (uint8_t)UVAL_STR && t->name.v.p != NULL) {
            name_str = (const char *)t->name.v.p;
            name_len = strlen(name_str);
        }
        uint32_t member_count = 0;
        for (UCleanupEntry *e = t->member_strands_head;
             e != NULL && member_count < 65535U;
             e = e->next_member) {
            member_count++;
        }
        EMIT_FMT("%s{\"realm\":%u,\"flags\":%u",
                 first ? "" : ",",
                 (unsigned)r->id, (unsigned)t->flags);
        if (name_str != NULL) {
            EMIT_FMT(",\"name\":\"");
            if (emit_json_escape(buf, cap, &n, name_str, name_len) != 0) {
                *out_n = n;
                return -1;
            }
            EMIT_FMT("\"");
        }
        EMIT_FMT(",\"member_count\":%u}", (unsigned)member_count);
        first = 0;
    }
    EMIT_FMT("]}");
    EMIT_DONE();
}

/* ---- watchers -------------------------------------------------------- */

int
urbi_introspect_watchers(UVM *vm, char *buf, size_t cap, size_t *out_n)
{
    if (vm == NULL || buf == NULL || out_n == NULL) return URBI_ERR_INVALID_ARG;
    EMIT_INIT();
    EMIT_FMT("{\"watchers\":[");
    int first = 1;
    for (UWatcher *w = vm->active_watchers_head; w != NULL; w = w->next_active) {
        EMIT_FMT("%s{\"id\":%u,\"mode\":%u,\"flags\":%u,\"refire\":%u}",
                 first ? "" : ",",
                 (unsigned)((uintptr_t)w & 0xFFFFFFFFu),
                 (unsigned)w->mode,
                 (unsigned)w->flags,
                 (unsigned)w->pending_refire_count);
        first = 0;
    }
    EMIT_FMT("]}");
    EMIT_DONE();
}

/* ---- events ---------------------------------------------------------- */

int
urbi_introspect_events(UVM *vm, char *buf, size_t cap, size_t *out_n)
{
    if (vm == NULL || buf == NULL || out_n == NULL) return URBI_ERR_INVALID_ARG;
    EMIT_INIT();
    EMIT_FMT("{\"events\":[");
    int first = 1;
    UEventRegistry *reg = &vm->event_registry;
    for (size_t i = 0; i < reg->count; i++) {
        const UEventRegistryEntry *e = &reg->entries[i];
        if (e->tombstoned) continue;
        /* Count subscribers: at-watchers + one-shot waiters. */
        uint32_t sub_count = 0;
        if (e->event != NULL) {
            for (UWatcher *w = e->event->at_watchers_head; w != NULL;
                 w = w->next_in_event) {
                sub_count++;
            }
            for (UStrand *s = e->event->waiters_head; s != NULL;
                 s = s->next_event_waiter) {
                sub_count++;
            }
        }
        EMIT_FMT("%s{\"id\":%u,\"subscribers\":%u",
                 first ? "" : ",",
                 (unsigned)e->id, (unsigned)sub_count);
        if (e->name != NULL && e->name_len > 0) {
            EMIT_FMT(",\"name\":\"");
            if (emit_json_escape(buf, cap, &n, e->name, e->name_len) != 0) {
                *out_n = n;
                return -1;
            }
            EMIT_FMT("\"");
        }
        EMIT_FMT("}");
        first = 0;
    }
    EMIT_FMT("]}");
    EMIT_DONE();
}

/* ---- profile (carry-forward stub) ----------------------------------- */

int
urbi_introspect_profile(const UVM *vm, char *buf, size_t cap, size_t *out_n)
{
    if (vm == NULL || buf == NULL || out_n == NULL) return URBI_ERR_INVALID_ARG;
    EMIT_INIT();
    /* Additive: the three locked arrays stay present (per_function/per_opcode/
     * per_watcher are reserved for v1.x); add counters + epoch. */
#if URBI_PERF_COUNTERS
    EMIT_FMT("{\"per_function\":[],\"per_opcode\":[],\"per_watcher\":[],"
             "\"epoch\":%u,\"counters\":{"
             "\"opcodes\":%zu,\"calls\":%zu,\"returns\":%zu,"
             "\"slot_get\":%zu,\"slot_set\":%zu,\"ic_hit\":%zu,\"ic_miss\":%zu,"
             "\"native_calls\":%zu,\"ctx_switches\":%zu,\"yields\":%zu,"
             "\"blocks\":%zu,\"watcher_installs\":%zu,\"watcher_fires\":%zu,"
             "\"event_emits\":%zu}}",
             (unsigned)vm->perf.epoch,
             vm->perf.opcodes, vm->perf.calls, vm->perf.returns,
             vm->perf.slot_get, vm->perf.slot_set, vm->perf.ic_hit, vm->perf.ic_miss,
             vm->perf.native_calls, vm->perf.ctx_switches, vm->perf.yields,
             vm->perf.blocks, vm->perf.watcher_installs, vm->perf.watcher_fires,
             vm->perf.event_emits);
#else
    EMIT_FMT("{\"per_function\":[],\"per_opcode\":[],\"per_watcher\":[],"
             "\"epoch\":0,\"counters\":null,"
             "\"note\":\"built without URBI_PERF_COUNTERS\"}");
#endif
    EMIT_DONE();
}

/* ---- gc -------------------------------------------------------------- */

int
urbi_introspect_gc(const UVM *vm, char *buf, size_t cap, size_t *out_n)
{
    if (vm == NULL || buf == NULL || out_n == NULL) return URBI_ERR_INVALID_ARG;
    EMIT_INIT();
    EMIT_FMT("{\"alive_bytes\":%zu,\"threshold\":%zu,\"total_allocated\":%zu,"
             "\"phase\":%u,\"cycles\":%zu,\"slices\":%zu,"
             "\"last_gc_us\":%llu,\"total_gc_us\":%llu}",
             (size_t)urbi_gc_live_bytes(vm),
             (size_t)urbi_gc_threshold(vm),
             (size_t)vm->gc_total_allocated,
             (unsigned)urbi_gc_phase(vm),
             vm->gc_cycles, vm->gc_slices,
             (unsigned long long)vm->last_gc_us,
             (unsigned long long)vm->total_gc_us);
    EMIT_DONE();
}

/* ---- lobbies --------------------------------------------------------- */

int
urbi_introspect_lobbies(UVM *vm, char *buf, size_t cap, size_t *out_n)
{
    if (vm == NULL || buf == NULL || out_n == NULL) return URBI_ERR_INVALID_ARG;
    EMIT_INIT();
    EMIT_FMT("{\"lobbies\":[");
    int first = 1;
    for (URealm *r = vm->realms_head; r != NULL; r = r->next_in_vm) {
        if ((r->flags & REALM_REPL) == 0U) continue;
        EMIT_FMT("%s{\"lobby\":\"lobby-%p\",\"realm\":%u}",
                 first ? "" : ",",
                 (void *)r,
                 (unsigned)r->id);
        first = 0;
    }
    EMIT_FMT("]}");
    EMIT_DONE();
}

/* ---- stack ----------------------------------------------------------- */

int
urbi_introspect_stack(const UVM *vm, uint32_t coro_id,
                      char *buf, size_t cap, size_t *out_n)
{
    if (vm == NULL || buf == NULL || out_n == NULL) return URBI_ERR_INVALID_ARG;
    EMIT_INIT();
    /* Find the strand. */
    const UStrand *target = NULL;
    for (const URealm *r = vm->realms_head; r != NULL && target == NULL;
         r = r->next_in_vm) {
        for (const UStrand *s = r->strands_head; s != NULL; s = s->next_in_realm) {
            if (strand_coro_id(s) == coro_id) { target = s; break; }
        }
    }
    if (target == NULL) {
        EMIT_FMT("{\"stack\":[],\"error\":\"unknown_coro\"}");
        EMIT_DONE();
    }
    EMIT_FMT("{\"stack\":[");
    int first = 1;
    int fc = target->frame_count;
    if (fc < 0) fc = 0;
    if (fc > UVM_MAX_FRAMES) fc = UVM_MAX_FRAMES;
    /* Walk from top frame downward so frame[0] is the most-recent call. */
    for (int i = fc - 1; i >= 0; i--) {
        EMIT_FMT("%s{\"frame\":%d}",
                 first ? "" : ",", i);
        first = 0;
    }
    EMIT_FMT("]}");
    EMIT_DONE();
}

/* ---- slots ----------------------------------------------------------- */

/* Walk a UObject's shape lineage and emit one entry per slot.  The shape
 * tree's leaf carries `count` slots; walking parent-ward yields one slot
 * per step (the leaf's `name` is the freshly-added slot, parent->name was
 * its parent's freshly-added slot, etc.).  This gives newest-first order;
 * we accept that — clients can re-sort if they need stable order. */
static int
emit_object_slots(UObject *obj, char *buf, size_t cap, size_t *n_inout)
{
    size_t n = *n_inout;
    int first = 1;
    if (obj == NULL || obj->shape == NULL) {
        *n_inout = n;
        return 0;
    }
    UShape *sh = obj->shape;
    while (sh != NULL && sh->count > 0) {
        if (sh->name != NULL) {
            const char *name = (const char *)sh->name;
            size_t name_len = strlen(name);
            int w = snprintf(buf + n, (cap > n) ? (cap - n) : 0,
                             "%s{\"name\":\"",
                             first ? "" : ",");
            if (w < 0 || (size_t)w >= ((cap > n) ? (cap - n) : 0)) {
                *n_inout = n + 64; return -1;
            }
            n += (size_t)w;
            if (emit_json_escape(buf, cap, &n, name, name_len) != 0) {
                *n_inout = n; return -1;
            }
            uint32_t idx = sh->index;
            const char *kind_name = "nil";
            if (idx < obj->shape->count && obj->slots != NULL) {
                UValue v = obj->slots[idx];
                switch (v.kind) {
                case UVAL_NIL:     kind_name = "nil"; break;
                case UVAL_INT:     kind_name = "int"; break;
                case UVAL_FLOAT:   kind_name = "float"; break;
                case UVAL_BOOL:    kind_name = "bool"; break;
                case UVAL_STR:     kind_name = "string"; break;
                case UVAL_CLOSURE: kind_name = "closure"; break;
                case UVAL_OBJECT:  kind_name = "object"; break;
                case UVAL_EVENT:   kind_name = "event"; break;
                case UVAL_VOID:    kind_name = "void"; break;
                default:           kind_name = "other"; break;
                }
            }
            w = snprintf(buf + n, (cap > n) ? (cap - n) : 0,
                         "\",\"kind\":\"%s\"}", kind_name);
            if (w < 0 || (size_t)w >= ((cap > n) ? (cap - n) : 0)) {
                *n_inout = n + 64; return -1;
            }
            n += (size_t)w;
            first = 0;
        }
        sh = sh->parent;
    }
    *n_inout = n;
    return 0;
}

int
urbi_introspect_slots(UVM *vm, URealm *realm,
                      const char *obj_path, size_t obj_path_len,
                      char *buf, size_t cap, size_t *out_n)
{
    if (vm == NULL || buf == NULL || out_n == NULL) return URBI_ERR_INVALID_ARG;
    EMIT_INIT();

    URealm *r = (realm != NULL) ? realm : urbi_realm_global(vm);
    UObject *target = NULL;
    if (r == NULL) {
        EMIT_FMT("{\"slots\":[],\"error\":\"no_realm\"}");
        EMIT_DONE();
    }

    /* Empty path = realm's global_object. */
    if (obj_path == NULL || obj_path_len == 0) {
        target = r->global_object;
    } else {
        /* Resolve first segment as a realm-global, then walk dotted parts.
         * v0.9.1 keeps this simple — dotted resolution only follows
         * UVAL_OBJECT slots.  Hitting a non-object segment emits error. */
        const char *p = obj_path;
        size_t remaining = obj_path_len;
        const char *dot = memchr(p, '.', remaining);
        size_t seg_len = (dot != NULL) ? (size_t)(dot - p) : remaining;
        char seg[256];
        if (seg_len >= sizeof(seg)) seg_len = sizeof(seg) - 1;
        memcpy(seg, p, seg_len);
        seg[seg_len] = '\0';
        UValue val;
        int grc = urbi_realm_get_global(vm, r, seg, seg_len, &val);
        if (grc != URBI_OK) {
            EMIT_FMT("{\"slots\":[],\"error\":\"unknown_global\"}");
            EMIT_DONE();
        }
        if (val.kind != (uint8_t)UVAL_OBJECT || val.v.p == NULL) {
            EMIT_FMT("{\"slots\":[],\"error\":\"not_an_object\"}");
            EMIT_DONE();
        }
        target = (UObject *)val.v.p;
        if (dot != NULL) {
            remaining -= (seg_len + 1);
            p = dot + 1;
            while (remaining > 0 && target != NULL) {
                dot = memchr(p, '.', remaining);
                seg_len = (dot != NULL) ? (size_t)(dot - p) : remaining;
                if (seg_len >= sizeof(seg)) seg_len = sizeof(seg) - 1;
                memcpy(seg, p, seg_len);
                seg[seg_len] = '\0';
                const USymbol *sym = (const USymbol *)ustr_intern(vm, seg, seg_len);
                if (sym == NULL) {
                    EMIT_FMT("{\"slots\":[],\"error\":\"oom\"}");
                    EMIT_DONE();
                }
                int32_t idx = urbi_shape_find_slot(target->shape, sym);
                if (idx < 0 || target->slots == NULL
                    || target->slots[idx].kind != (uint8_t)UVAL_OBJECT
                    || target->slots[idx].v.p == NULL) {
                    EMIT_FMT("{\"slots\":[],\"error\":\"path_miss\"}");
                    EMIT_DONE();
                }
                target = (UObject *)target->slots[idx].v.p;
                if (dot == NULL) break;
                remaining -= (seg_len + 1);
                p = dot + 1;
            }
        }
    }

    EMIT_FMT("{\"slots\":[");
    if (target != NULL) {
        if (emit_object_slots(target, buf, cap, &n) != 0) {
            *out_n = n;
            return -1;
        }
    }
    EMIT_FMT("]}");
    EMIT_DONE();
}
