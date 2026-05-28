/* SPDX-License-Identifier: BSD-3-Clause */
/* include/urbi/trace.h — runtime trace subsystem (v0.11.0, EXPERIMENTAL).
 *
 * Master gate URBI_TRACE: undefined/0 ⇒ every tracepoint macro is (void)0,
 * no UVM fields, no ring/emit symbols in the archive.  Per-channel compile
 * mask URBI_TRACE_CHANNELS strips individual channels.  Runtime per-channel
 * level (a ULogLevel severity threshold) gates emission; URBI_TRACE_OFF
 * disables a channel entirely.
 *
 * Levels reuse the public ULogLevel (DEBUG=0..ERROR=3, severity-ascending,
 * <urbi/urbi.h>).  A tracepoint emits iff
 *     level != URBI_TRACE_OFF && severity >= level.
 * Tap severity mapping: per-opcode/per-iteration → DEBUG; milestone/phase →
 * INFO; degraded → WARN; fault → ERROR.
 *
 * The control/drain/stats API below is linkable in BOTH build modes (no-op
 * stubs when URBI_TRACE=0) so embedder code compiles either way; the ring
 * and emit internals exist only under URBI_TRACE=1. */
#ifndef URBI_TRACE_H
#define URBI_TRACE_H

#include <stdint.h>
#include <stddef.h>
#include "urbi/urbi.h"   /* ULogLevel, struct UVM (fwd) */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef URBI_TRACE
#  define URBI_TRACE 0
#endif

/* Channel taxonomy. New channels insert before URBI_TRACE_CHANNEL_MAX. */
typedef enum {
    URBI_TRACE_VM      = 0,   /* dispatch slice, throw/unwind (NOT per-opcode) */
    URBI_TRACE_SCHED   = 1,   /* strand create/start/block/yield/resume/exit */
    URBI_TRACE_GC      = 2,   /* phase, slice, alloc-denied */
    URBI_TRACE_WATCHER = 3,   /* install/fire/body-complete */
    URBI_TRACE_EVENT   = 4,   /* emit, ring-drain */
    URBI_TRACE_TAG     = 5,   /* stop/block/unblock/freeze/unfreeze */
    URBI_TRACE_REPL    = 6,   /* session open/close, eval start/end */
    URBI_TRACE_USER    = 7,   /* Debug.trace() markers */
    URBI_TRACE_CHANNEL_MAX = 8
} UTraceChannel;

/* Per-channel "disabled" sentinel (distinct from any ULogLevel value). */
#define URBI_TRACE_OFF ((int8_t)-1)

/* Schema IDs — per-channel payload selectors (host decoder switches on these). */
typedef enum {
    URBI_TP_MILESTONE         = 0,  /* payload.str: bounded marker text */
    URBI_TP_SCHED_CREATE      = 1,
    URBI_TP_SCHED_START       = 2,
    URBI_TP_SCHED_BLOCK       = 3,  /* a = reason, b = strand id */
    URBI_TP_SCHED_YIELD       = 4,
    URBI_TP_SCHED_RESUME      = 5,
    URBI_TP_SCHED_EXIT        = 6,  /* a = strand id, b = status */
    URBI_TP_GC_PHASE          = 7,  /* a = new phase */
    URBI_TP_GC_SLICE          = 8,  /* a = bytes reclaimed */
    URBI_TP_GC_ALLOC_DENIED   = 9,  /* a = requested size (heap locked) */
    URBI_TP_WATCHER_INSTALL   = 10,
    URBI_TP_WATCHER_FIRE      = 11,
    URBI_TP_WATCHER_COMPLETE  = 12,
    URBI_TP_EVENT_EMIT        = 13, /* a = n waiters woken */
    URBI_TP_EVENT_DRAIN       = 14, /* a = n drained */
    URBI_TP_TAG_OP            = 15, /* a = op (0 stop,1 block,2 unblock,3 freeze,4 unfreeze) */
    URBI_TP_REPL_SESSION      = 16, /* a = 1 open / 0 close */
    URBI_TP_REPL_EVAL         = 17, /* a = 1 start / 0 end */
    URBI_TP_USER_MARKER       = 18  /* payload.str: Debug.trace() text */
} UTraceSchema;

/* Fixed 24-byte binary record. No raw host pointers in exported traces. */
typedef struct {
    uint64_t ts_us;        /* host_time_us(); 0 if clock unavailable */
    uint32_t seq;          /* monotonic per-VM; gaps ⇒ dropped records */
    uint16_t strand_id;    /* VM-local; 0 if none */
    uint8_t  channel;      /* UTraceChannel */
    uint8_t  level;        /* ULogLevel severity of this tap */
    uint16_t schema_id;    /* UTraceSchema */
    uint16_t _pad;
    union {
        struct { uint32_t a, b; } words;
        char str[8];       /* NUL-padded bounded marker text */
    } payload;
} UTraceRecord;

#if URBI_TRACE

#ifndef URBI_TRACE_CHANNELS
#  define URBI_TRACE_CHANNELS  0xFFFFFFFFUL   /* all channels compiled in */
#endif
#ifndef URBI_TRACE_RING_DEPTH
#  define URBI_TRACE_RING_DEPTH 256           /* records; ~6 KB at 24 B */
#endif

#define URBI_TRACE_CHANNEL_COMPILED(ch) (((URBI_TRACE_CHANNELS) >> (ch)) & 1UL)

/* Per-VM trace state. Present on struct UVM only under URBI_TRACE. */
typedef struct {
    int8_t       level[URBI_TRACE_CHANNEL_MAX]; /* per-channel threshold */
    UTraceRecord ring[URBI_TRACE_RING_DEPTH];
    uint32_t     head;        /* next write index */
    uint32_t     count;       /* live records (<= depth) */
    uint32_t     seq;         /* monotonic record sequence */
    uint32_t     dropped;     /* overflow drops since last drain */
    uint32_t     emitted;     /* lifetime emitted */
    uint32_t     high_water;  /* max count seen */
} UTraceState;

/* Internal (defined in utrace.c). */
void   urbi_trace_init(struct UVM *vm);   /* all channels OFF, ring empty */
void   urbi_trace_emit(struct UVM *vm, uint8_t channel, uint8_t level,
                       uint16_t schema_id, uint32_t a, uint32_t b);
void   urbi_trace_emit_str(struct UVM *vm, uint8_t channel, uint8_t level,
                           uint16_t schema_id, const char *s, size_t n);
int8_t urbi_trace_channel_level(const struct UVM *vm, uint8_t channel);

/* Core tracepoint: one array-load + compare when disabled, no call.
 * `vm` may be NULL (no-op). Arguments are evaluated ONLY when the channel
 * is compiled in AND enabled at/below `level`. */
#define URBI_TP(vm, ch, level, schema, a, b)                              \
    do {                                                                  \
        if (URBI_TRACE_CHANNEL_COMPILED(ch)) {                            \
            int8_t _lvl = urbi_trace_channel_level((vm), (ch));           \
            if (_lvl != URBI_TRACE_OFF && (int)(level) >= (int)_lvl) {    \
                urbi_trace_emit((vm), (uint8_t)(ch), (uint8_t)(level),    \
                                (uint16_t)(schema), (uint32_t)(a),        \
                                (uint32_t)(b));                           \
            }                                                             \
        }                                                                 \
    } while (0)

#define URBI_TP_STR(vm, ch, level, schema, s, n)                          \
    do {                                                                  \
        if (URBI_TRACE_CHANNEL_COMPILED(ch)) {                            \
            int8_t _lvl = urbi_trace_channel_level((vm), (ch));           \
            if (_lvl != URBI_TRACE_OFF && (int)(level) >= (int)_lvl) {    \
                urbi_trace_emit_str((vm), (uint8_t)(ch), (uint8_t)(level),\
                                    (uint16_t)(schema), (s), (n));        \
            }                                                             \
        }                                                                 \
    } while (0)

#else  /* URBI_TRACE == 0 : strip every tracepoint */

#define URBI_TP(vm, ch, level, schema, a, b)        ((void)0)
#define URBI_TP_STR(vm, ch, level, schema, s, n)    ((void)0)

#endif /* URBI_TRACE */

/* Bring-up primitives (prior spec §9). All no-ops when URBI_TRACE=0 because
 * they expand through URBI_TP. Static counters are file-private (single VM
 * per thread, v1.0 contract; multi-VM sharing is benign — see trace doc). */
#define URBI_TP_MILESTONE(vm, ch, msg) \
    URBI_TP_STR((vm), (ch), URBI_LOG_INFO, URBI_TP_MILESTONE, (msg), 0)

#define URBI_TP_ONCE(vm, ch, level, schema, a, b)                         \
    do { static uint8_t _once = 0;                                        \
         if (!_once) { _once = 1; URBI_TP((vm),(ch),(level),(schema),(a),(b)); } \
    } while (0)

#define URBI_TP_FIRST_N(vm, ch, level, n, schema, a, b)                   \
    do { static uint32_t _c = 0;                                          \
         if (_c < (uint32_t)(n)) { _c++;                                  \
             URBI_TP((vm),(ch),(level),(schema),(a),(b)); } } while (0)

#define URBI_TP_PERIODIC(vm, ch, level, every_n, schema, a, b)            \
    do { static uint32_t _c = 0;                                          \
         if (++_c % (uint32_t)(every_n) == 0)                             \
             URBI_TP((vm),(ch),(level),(schema),(a),(b)); } while (0)

/* The highest-value pattern: silent until `var` crosses `threshold`. */
#define URBI_TP_THRESHOLD(vm, ch, level, var, threshold, schema, a, b)    \
    do { if ((var) >= (threshold))                                        \
             URBI_TP((vm),(ch),(level),(schema),(a),(b)); } while (0)

/* === Public control / drain / stats API (EXPERIMENTAL; no-ops if !URBI_TRACE) === */

/* Set/get a channel's runtime severity threshold. URBI_TRACE_OFF disables.
 * NULL vm is a no-op / returns URBI_TRACE_OFF. */
void   urbi_trace_set_level(struct UVM *vm, uint8_t channel, int8_t level);
int8_t urbi_trace_get_level(const struct UVM *vm, uint8_t channel);
void   urbi_trace_set_level_all(struct UVM *vm, int8_t level);

/* Drain up to `cap` records into `out` (oldest-first), clearing them from the
 * ring. Returns count written; *out_dropped (if non-NULL) gets the number of
 * records dropped due to ring overflow since the last drain. */
size_t urbi_trace_snapshot(struct UVM *vm, UTraceRecord *out, size_t cap,
                           uint32_t *out_dropped);

/* Stats: high-water fill, total dropped, total emitted. NULL-tolerant. */
typedef struct {
    uint32_t emitted;
    uint32_t dropped;
    uint32_t high_water;
    uint32_t ring_depth;
} UTraceStats;
void urbi_trace_stats(const struct UVM *vm, UTraceStats *out);

/* Human-readable channel name (e.g. "sched"); "?" for out-of-range. */
const char *urbi_trace_channel_name(uint8_t channel);

#ifdef __cplusplus
}
#endif
#endif /* URBI_TRACE_H */
