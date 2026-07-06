/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * utrace_format.c — manual (no vsnprintf) trace record formatter and the
 * default writer_fn text backend.  Freestanding-safe.
 *
 * Converts URBI_TRACE event records to human-readable text without calling
 * vsnprintf or any libc function, so trace output is available on bare-metal
 * targets that lack printf.
 */
#include "urbi/trace.h"
#include "vm/uvm.h"

#if URBI_TRACE

static const char *const k_level_name[] = { "DEBUG", "INFO", "WARN", "ERROR" };
static const char *const k_schema_name[] = {
    "milestone", "sched_create", "sched_start", "sched_block", "sched_yield",
    "sched_resume", "sched_exit", "gc_phase", "gc_slice", "gc_alloc_denied",
    "watcher_install", "watcher_fire", "watcher_complete", "event_emit",
    "event_drain", "tag_op", "repl_session", "repl_eval", "user_marker"
};
#define K_SCHEMA_COUNT ((uint16_t)(sizeof k_schema_name / sizeof k_schema_name[0]))

static size_t put(char *b, size_t cap, size_t at, const char *s)
{
    while (*s && at + 1 < cap) b[at++] = *s++;
    return at;
}

static size_t put_u64(char *b, size_t cap, size_t at, uint64_t v)
{
    char tmp[20];
    int i = 0;
    if (v == 0) { if (at + 1 < cap) b[at++] = '0'; return at; }
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i-- > 0 && at + 1 < cap) b[at++] = tmp[i];
    return at;
}

size_t utrace_format(char *buf, size_t cap, const UTraceRecord *rec)
{
    size_t at = 0;
    if (!buf || cap == 0 || !rec) return 0;
    at = put(buf, cap, at, "seq=");      at = put_u64(buf, cap, at, rec->seq);
    at = put(buf, cap, at, " t=");       at = put_u64(buf, cap, at, rec->ts_us);
    at = put(buf, cap, at, " strand=");  at = put_u64(buf, cap, at, rec->strand_id);
    at = put(buf, cap, at, " ");
    at = put(buf, cap, at, urbi_trace_channel_name(rec->channel));
    at = put(buf, cap, at, "/");
    at = put(buf, cap, at, (rec->level < 4) ? k_level_name[rec->level] : "?");
    at = put(buf, cap, at, " ");
    at = put(buf, cap, at, (rec->schema_id < K_SCHEMA_COUNT)
                            ? k_schema_name[rec->schema_id] : "?");
    if (rec->schema_id == URBI_TP_MILESTONE || rec->schema_id == URBI_TP_USER_MARKER) {
        char s[9];
        int i;
        for (i = 0; i < 8; i++) s[i] = rec->payload.str[i];
        s[8] = '\0';
        at = put(buf, cap, at, " \"");
        at = put(buf, cap, at, s);
        at = put(buf, cap, at, "\"");
    } else {
        at = put(buf, cap, at, " a="); at = put_u64(buf, cap, at, rec->payload.words.a);
        at = put(buf, cap, at, " b="); at = put_u64(buf, cap, at, rec->payload.words.b);
    }
    buf[at] = '\0';
    return at;
}

void urbi_trace_flush_to_writer(struct UVM *vm)
{
    UTraceRecord rec[32];
    uint32_t dropped;
    size_t n;
    if (!vm || !vm->writer_fn) return;
    while ((n = urbi_trace_snapshot(vm, rec, 32, &dropped)) > 0) {
        size_t i;
        for (i = 0; i < n; i++) {
            char buf[128];
            size_t len = utrace_format(buf, sizeof buf, &rec[i]);
            vm->writer_fn(vm->writer_ud, "trace", 5, buf, len, rec[i].ts_us);
        }
        if (n < 32) break;
    }
}

#endif /* URBI_TRACE */
