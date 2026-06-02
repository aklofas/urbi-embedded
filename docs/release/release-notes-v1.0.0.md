# Release notes — v1.0.0

## v1.0.0 — 2026-06-01

The first stable release of urbi-embedded: a from-scratch, pure-C99,
embeddable implementation of **urbiscript** — the prototype-based,
parallel-by-default, event-driven robotics scripting language. No prior
reimplementation of urbiscript existed in any language; this is the first.

### What ships in this release

**Language (complete):**

- Separators encode concurrency: `;` (sequential, yields), `|` (sequential,
  atomic), `,` (parallel fire-and-forget), `&` (parallel join).
- Reactive runtime: persistent `at (cond) body` watchers, `whenever` (level),
  `waituntil` (one-shot), first-class events with sync/async emit, slot-change
  subscriptions, tag enter/leave.
- First-class tags with three cancellation modes (`stop` / `block` / `freeze`),
  scope binding, and tag-stop absorption (a local `t.stop()` terminates the
  tagged block and resumes after it).
- Prototype OOP: multi-proto MRO, copy-on-write inherited slots, getters/setters,
  `isA`, hidden classes + inline caches.
- Exceptions: `try` / `catch` (with `var`, guards, `else`) / `finally` —
  `finally` runs on every exit kind; native + scripted exception subclasses.
- Time-aware literals (`100ms`, `1s`, `180deg`, `pi`), list/dict literals +
  subscripting, operators incl. `&&` / `||` / `%`, Kotlin-style bitwise methods,
  a stdlib data layer (String / List / Dict / Object-reflection / Integer /
  Float / RegExp) at ~80% usage-weighted coverage.

**Runtime:** tracing-free register bytecode VM, incremental tri-color GC with
safe-point discipline, cooperative scheduler with a 4-state `urbi_step` driver
(`RUNNING` / `WAKE_AT` / `QUIESCENT` / `FATAL`), per-VM isolation (no global
state), host-pluggable allocator / I/O / clock / panic.

**Embedding + tooling:** a frozen public C API (core `urbi.h` + aux split),
a bytecode/source deployment split (`URBI_BYTECODE_ONLY` for compiler-less
firmware), an NDJSON REPL service (TCP / Unix / UART) with source- and
bytecode-mode hot-patch, a remote trace decoder, perf counters, and a
memory-debug harness.

**ROS2:** host rcl/rclc/Fast-DDS transport (`URBI_ENABLE_ROS2` +
`URBI_ROS_BACKEND=rcl`) with pub/sub, service client/server, and a
pure-urbiscript Standard Robotics API facet overlay (`Robotics.*`,
`URBI_ENABLE_UROBOTICS`) plus a facet↔ROS2 binding contract. A documented
micro-ROS-on-MCU path covers on-silicon ROS2.

**Supported targets:** Linux x86_64 (host), Raspberry Pi Pico (RP2040),
ESP32-S3, STM32F4 — all validated on real hardware at the v1.0 codebase. See
[hardware-validation.md](hardware-validation.md). Generic Cortex-M7 and RISC-V
rv32imc build as archives.

**ABI version:** 1/0/0 — **frozen** (see [docs/api-stability.md](../api-stability.md);
the pre-v1.0 escape clause is now retired, post-1.0 breaks follow semver §3).

**Wire format version:** v1.9 / 0x19 (see
[docs/internals/bytecode-format.md](../internals/bytecode-format.md)).

### Bug fixes

Three runtime correctness fixes landed before the freeze:

- **`finally` on the normal completion path** (`v0.11.4-D`) — `finally` now runs
  on normal fall-through, not only during throw/tag-stop unwind (fixes the legacy
  `mutex` `onLeave` pattern).
- **tag-stop absorption / resume-after-scope** (`v0.10.15-B`) — a local
  `t.stop()` inside `t: { ... }` terminates the tagged block and resumes after
  it, instead of killing the strand.
- **lone-sleeper wake** (`v0.11.4-A`) — `urbi_step` now wakes a sole expired
  `sleep()`-ing strand via a pre-dispatch pump.

### Breaking changes (vs the last pre-1.0 tag)

Two ABI changes landed under the final pre-1.0 escape (see api-stability §6 #33):

- `urbi_set_time_us` → **`urbi_set_clock_fn`** (a plain rename; same signature).
- The library is built `-fvisibility=hidden`; only the documented public `urbi_*`
  surface is exported when linked into a shared object (internal symbols hidden).

### Intentional divergences from legacy urbiscript 2.x

By design (see the [conformance report](conformance-report.md)): the `closure`
keyword and bare-`function` are retired (eager-by-default + per-parameter
`lazy`); `callMessage` is gone; symbolic bitwise operators are methods; block
comments are non-nesting; `Integer` is a distinct type; `import` is a
compile-time host directive, not a runtime keyword.

### Known deferrals (not in this release)

- On-silicon micro-ROS / ESP32-C3 / STM32H7 — v1.0 ROS2 is host-rcl + a
  documented MCU path.
- Host networking (`Socket` / `Server`) — served by the C API + ROS2.
- Vision/audio/media facets, urbiscript-level finalizers — v2.0 / v1.x.
- Accepted known-issues (targeted v1.0.1+): `v0.10.9-C`, `v0.10.10-A`,
  `v0.10.10-D`, `v0.11.3-C`, `v1.0-loader-count-quiescence` — see the
  conformance report's known-issues list.
