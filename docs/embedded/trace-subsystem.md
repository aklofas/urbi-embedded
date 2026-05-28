# Trace subsystem

The trace subsystem (`<urbi/trace.h>`, v0.11.0) is a kernel-style runtime
tracer for debugging embedded bring-up and runtime behaviour. It compiles out
to **zero bytes** by default, costs **one predicted-not-taken branch** per
tracepoint when compiled in but the channel is disabled, and writes fixed-size
binary records into a per-VM ring when a channel is enabled.

It is **EXPERIMENTAL** — the API may change before v1.0.

## Configuration flags

| Flag | Default | Effect |
| --- | --- | --- |
| `URBI_TRACE` | undefined (off) | Master gate. Off ⇒ every `URBI_TP` macro expands to `(void)0`, no trace fields on `struct UVM`, no ring/emit symbols in the archive. |
| `URBI_TRACE_CHANNELS` | `0xFFFFFFFF` | Per-channel compile mask (only meaningful when `URBI_TRACE=1`). Channels masked out are stripped regardless of runtime level. |
| `URBI_TRACE_RING_DEPTH` | `256` | Trace-ring capacity in records (~24 B each; 256 ⇒ ~6 KB). |

Trace-on builds are allowed without `URBI_ENABLE_REPL` (the REPL is a consumer,
not the substrate) and with `URBI_BYTECODE_ONLY=1` (embedded deployments are
exactly where trace is useful). The default `URBI_TRACE`-off archive is
byte-identical to a non-trace build — enforced by `make test-trace-compiled-out`.

## Level model

Tracepoints reuse the existing public `ULogLevel` (severity-ascending):

- `URBI_LOG_DEBUG (0)` — per-opcode / per-iteration detail
- `URBI_LOG_INFO (1)` — milestone / phase boundary
- `URBI_LOG_WARN (2)` — recoverable / degraded
- `URBI_LOG_ERROR (3)` — fault

Each channel carries a runtime threshold. A tracepoint emits iff
`level != URBI_TRACE_OFF && severity >= level`. The sentinel `URBI_TRACE_OFF`
(`-1`, distinct from any `ULogLevel`) disables a channel; all channels default
to `URBI_TRACE_OFF` at `urbi_trace_init`.

> Note: this supersedes the 2026-05-17 trace-subsystem draft, which proposed a
> 6-level verbosity enum. The shipped runtime reuses the real 4-level
> `ULogLevel` (no ABI change) plus the `URBI_TRACE_OFF` sentinel.

## Record format

Records are a fixed 24-byte binary `UTraceRecord` (no host pointers in exported
traces; decode names/opcodes on the host via the firmware ELF). String markers
copy a bounded 8-byte prefix inline.

```c
typedef struct {
    uint64_t ts_us;      /* host_time_us(); 0 if no clock installed */
    uint32_t seq;        /* monotonic per-VM; gaps ⇒ dropped records  */
    uint16_t strand_id;  /* VM-local (low 16 bits of strand pointer)  */
    uint8_t  channel;    /* UTraceChannel */
    uint8_t  level;      /* ULogLevel severity */
    uint16_t schema_id;  /* UTraceSchema */
    uint16_t _pad;
    union { struct { uint32_t a, b; } words; char str[8]; } payload;
} UTraceRecord;
```

## Channels

| Channel | Covers |
| --- | --- |
| `URBI_TRACE_VM` | reserved (dispatch slice / throw / unwind) |
| `URBI_TRACE_SCHED` | strand start / block / yield / resume / exit |
| `URBI_TRACE_GC` | phase transitions, heap-locked alloc-denied |
| `URBI_TRACE_WATCHER` | watcher install / fire / body-complete |
| `URBI_TRACE_EVENT` | event emit (ring-drain reserved) |
| `URBI_TRACE_TAG` | tag stop / block / unblock / freeze / unfreeze |
| `URBI_TRACE_REPL` | REPL eval, session open / close |
| `URBI_TRACE_USER` | `Debug.trace("…")` script markers |

The **ISR event-injection** tracepoint (`urbi_inject_event`) is intentionally
**not** instrumented in v0.11.0: it runs in ISR context and needs the ISR-safe
SPSC ring path rather than the main-strand `urbi_trace_emit`. Tracked as a
v0.11.x TODO.

## Capture workflow

```c
/* Enable the channels you care about, run, then drain. */
urbi_trace_set_level(vm, URBI_TRACE_SCHED, URBI_LOG_DEBUG);
urbi_trace_set_level(vm, URBI_TRACE_GC,    URBI_LOG_INFO);

/* ... run the workload ... */

/* Option A — pull binary records and decode them yourself: */
UTraceRecord recs[64];
uint32_t dropped;
size_t n = urbi_trace_snapshot(vm, recs, 64, &dropped);

/* Option B — flush as text over the embedder's writer_fn "trace" channel: */
urbi_trace_flush_to_writer(vm);
```

`urbi_trace_stats(vm, &stats)` reports lifetime emitted, dropped (ring
overflow), high-water fill, and ring depth.

## Bring-up cookbook

The macro family (all no-ops when `URBI_TRACE` is off):

- `URBI_TP_MILESTONE(vm, ch, "msg")` — phase boundary marker.
- `URBI_TP_ONCE(...)` / `URBI_TP_FIRST_N(...)` — fire once / the first N times
  (spot-check inputs without flooding).
- `URBI_TP_PERIODIC(..., every_n, ...)` — every Nth hit (forward-progress in a
  tight loop).
- `URBI_TP_THRESHOLD(..., var, threshold, ...)` — **stay silent until `var`
  crosses `threshold`**. The single most useful pattern for localizing a hang:
  silent for the normal cases, fires on the runaway one.

Symptom → channels to enable:

- "realm/global comes back NULL, no error" → `URBI_TRACE_GC` + `URBI_TRACE_SCHED`
- "watcher never fires" → `URBI_TRACE_WATCHER` + `URBI_TRACE_EVENT`
- "strand wedged / never completes" → `URBI_TRACE_SCHED` (watch for a missing
  `sched_exit`)
- "tag.stop()/freeze did nothing" → `URBI_TRACE_TAG`

## Determinism

Trace state (ring, sequence, levels) is excluded from
`urbi_get_determinism_checksum`. A `URBI_TRACE=1` build passes the determinism
presets unchanged — verified by `make test-determinism-trace`. Because channels
default off, compiling the subsystem in has zero behavioural effect until an
embedder enables a channel.

## Host tooling

A host decoder (binary ring → Chrome Trace / Perfetto JSON) and GDB
pretty-printers / heap walkers land in a later tag of the v0.11.x arc. For now,
`utrace_format` produces a stable, parseable one-line text form per record.
