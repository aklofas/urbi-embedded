/* SPDX-License-Identifier: BSD-3-Clause */
/* src/runtime/utrace.c — trace ring, per-channel levels, emit, drain (v0.11.0).
 *
 * Under URBI_TRACE=1: full ring + emit + control implementation.
 * Under URBI_TRACE=0: no-op stubs for the control API so embedder code that
 * calls urbi_trace_set_level / _snapshot / _stats links in both build modes.
 * The ring/emit internals (urbi_trace_emit*, urbi_trace_channel_level) exist
 * ONLY under URBI_TRACE=1 — the tracepoint macros that reference them are
 * preprocessor-stripped in the OFF build, so there is no link dependency. */
#include "urbi/trace.h"
#include "vm/uvm.h"

#if URBI_TRACE

static const char *const k_channel_names[URBI_TRACE_CHANNEL_MAX] = {
    "vm", "sched", "gc", "watcher", "event", "tag", "repl", "user"
};

const char *urbi_trace_channel_name(uint8_t channel)
{
    return (channel < URBI_TRACE_CHANNEL_MAX) ? k_channel_names[channel] : "?";
}

void urbi_trace_init(struct UVM *vm)
{
    UTraceState *t;
    int i;
    if (!vm) return;
    t = &vm->trace;
    for (i = 0; i < URBI_TRACE_CHANNEL_MAX; i++) t->level[i] = URBI_TRACE_OFF;
    t->head = t->count = t->seq = t->dropped = t->emitted = t->high_water = 0;
}

int8_t urbi_trace_channel_level(const struct UVM *vm, uint8_t channel)
{
    if (!vm || channel >= URBI_TRACE_CHANNEL_MAX) return URBI_TRACE_OFF;
    return vm->trace.level[channel];
}

void urbi_trace_set_level(struct UVM *vm, uint8_t channel, int8_t level)
{
    if (!vm || channel >= URBI_TRACE_CHANNEL_MAX) return;
    vm->trace.level[channel] = level;
}

int8_t urbi_trace_get_level(const struct UVM *vm, uint8_t channel)
{
    return urbi_trace_channel_level(vm, channel);
}

void urbi_trace_set_level_all(struct UVM *vm, int8_t level)
{
    int i;
    if (!vm) return;
    for (i = 0; i < URBI_TRACE_CHANNEL_MAX; i++) vm->trace.level[i] = level;
}

static uint64_t trace_now_us(const struct UVM *vm)
{
    return (vm->host_time_us) ? vm->host_time_us(vm->host_time_ud) : 0ULL;
}

/* Stable-enough VM-local strand id: low 16 bits of the strand pointer.
 * Mirrors Job.uid (which exposes the pointer). cur_strand may be NULL
 * outside dispatch. */
static uint16_t trace_cur_strand_id(const struct UVM *vm)
{
    return vm->cur_strand ? (uint16_t)(uintptr_t)vm->cur_strand : (uint16_t)0;
}

static UTraceRecord *trace_reserve(struct UVM *vm)
{
    UTraceState *t = &vm->trace;
    UTraceRecord *r = &t->ring[t->head];
    t->head = (t->head + 1u) % URBI_TRACE_RING_DEPTH;
    if (t->count < URBI_TRACE_RING_DEPTH) {
        t->count++;
        if (t->count > t->high_water) t->high_water = t->count;
    } else {
        t->dropped++;   /* overwrote an undrained record */
    }
    t->emitted++;
    r->seq = t->seq++;
    r->ts_us = trace_now_us(vm);
    r->strand_id = trace_cur_strand_id(vm);
    return r;
}

void urbi_trace_emit(struct UVM *vm, uint8_t channel, uint8_t level,
                     uint16_t schema_id, uint32_t a, uint32_t b)
{
    UTraceRecord *r;
    if (!vm) return;
    r = trace_reserve(vm);
    r->channel = channel; r->level = level; r->schema_id = schema_id; r->_pad = 0;
    r->payload.words.a = a; r->payload.words.b = b;
}

void urbi_trace_emit_str(struct UVM *vm, uint8_t channel, uint8_t level,
                         uint16_t schema_id, const char *s, size_t n)
{
    UTraceRecord *r;
    size_t i = 0;
    if (!vm) return;
    r = trace_reserve(vm);
    r->channel = channel; r->level = level; r->schema_id = schema_id; r->_pad = 0;
    /* Bounded copy into the 8-byte inline buffer. n==0 means "use strlen". */
    if (s) {
        if (n == 0) { for (; i < 8 && s[i]; i++) r->payload.str[i] = s[i]; }
        else        { for (; i < 8 && i < n && s[i]; i++) r->payload.str[i] = s[i]; }
    }
    for (; i < 8; i++) r->payload.str[i] = '\0';
}

size_t urbi_trace_snapshot(struct UVM *vm, UTraceRecord *out, size_t cap,
                           uint32_t *out_dropped)
{
    UTraceState *t;
    size_t n, i;
    uint32_t tail;
    if (!vm || !out) { if (out_dropped) *out_dropped = 0; return 0; }
    t = &vm->trace;
    n = (t->count < cap) ? t->count : cap;
    /* oldest-first: tail = head - count (mod depth) */
    tail = (t->head + URBI_TRACE_RING_DEPTH - t->count) % URBI_TRACE_RING_DEPTH;
    for (i = 0; i < n; i++)
        out[i] = t->ring[(tail + i) % URBI_TRACE_RING_DEPTH];
    if (out_dropped) *out_dropped = t->dropped;
    t->count = 0; t->head = 0; t->dropped = 0;  /* drain clears */
    return n;
}

void urbi_trace_stats(const struct UVM *vm, UTraceStats *out)
{
    if (!out) return;
    if (!vm) {
        out->emitted = out->dropped = out->high_water = 0;
        out->ring_depth = URBI_TRACE_RING_DEPTH;
        return;
    }
    out->emitted    = vm->trace.emitted;
    out->dropped    = vm->trace.dropped;
    out->high_water = vm->trace.high_water;
    out->ring_depth = URBI_TRACE_RING_DEPTH;
}

#else  /* !URBI_TRACE : control API becomes no-op stubs so callers still link */

void urbi_trace_set_level(struct UVM *vm, uint8_t c, int8_t l)
{ (void)vm; (void)c; (void)l; }

int8_t urbi_trace_get_level(const struct UVM *vm, uint8_t c)
{ (void)vm; (void)c; return URBI_TRACE_OFF; }

void urbi_trace_set_level_all(struct UVM *vm, int8_t l)
{ (void)vm; (void)l; }

size_t urbi_trace_snapshot(struct UVM *vm, UTraceRecord *o, size_t c, uint32_t *d)
{ (void)vm; (void)o; (void)c; if (d) *d = 0; return 0; }

void urbi_trace_stats(const struct UVM *vm, UTraceStats *o)
{ (void)vm; if (o) { o->emitted = o->dropped = o->high_water = 0; o->ring_depth = 0; } }

const char *urbi_trace_channel_name(uint8_t c)
{ (void)c; return "?"; }

#endif /* URBI_TRACE */
