# Roadmap to v1.0

The release sequence to v1.0, the exit criteria each release must clear, and the non-goals for v1.0.

---

## Vision

An embeddable runtime for urbiscript written in pure C99, single library, no external dependencies.

| Property        | Goal                                                                                   |
| --------------- | -------------------------------------------------------------------------------------- |
| Flash footprint | < 400 KB on Cortex-M class MCUs                                                        |
| GC pause        | ≤ 1 ms under typical reactive workload on 32-bit embedded                              |
| Architectures   | x86_64 Linux, ARM Cortex-M7 (STM32H7), RISC-V 32-bit (ESP32-C3); Xtensa LX7 (ESP32-S3) as a bonus port |
| Host hooks      | Pluggable allocator, time source, panic handler                                        |

Language features kept native:

| Construct                                 | Provides                        |
| ----------------------------------------- | ------------------------------- |
| Statement separators `,` `&` `\|` `;`     | Concurrency                     |
| `at` / `whenever` / `every` / `waituntil` | Reactive control                |
| Tags                                      | Structured cancellation         |
| Time and angle literals                   | Units in the lexical grammar    |
| `lazy` keyword                            | Opt-in lazy argument evaluation |

Intended deployment surface: drone autopilots, motor controllers, agricultural robots, and research platforms.

Development practice: TDD throughout.

---

## Releases to v1.0

| Tag                  | Scope                                                                                                                                                                                                                                                                                          | Done  |
| -------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----- |
| `v0.1.0-skeleton`    | End-to-end pipeline: parser, AST, single-pass emitter, tiny VM. `urbi -i` REPL evaluates arithmetic. First `.chk` fixture passes. Stop-the-world mark-sweep GC with barrier-ready interfaces.                                                                                                  | `[ ]` |
| `v0.2.0-expressions` | Strings, lists, dicts (dynamic and bounded variants), control flow, functions, prototype objects with shape-based dispatch and inline caches, time/angle literals. Most of `tests/2.x/{object,control,string,list,float}/` passing. Sub-MB Linux binary proven.                                | `[ ]` |
| `v0.3.0-concurrency` | Stackful coroutines, priority-aware scheduler, statement separators (`,` `&` `\|` `;`), tags with stop / freeze / block. Incremental mark-sweep with bounded step size; ≤ 1 ms GC pause on 32-bit embedded under typical reactive workload, measured and CI-gated.                             | `[ ]` |
| `v0.4.0-reactive`    | First-class events, `at` / `whenever` / `every` / `waituntil`, tag enter/leave events, debouncing (`~ duration`). `tests/2.x/reactive/` corpus passing.                                                                                                                                        | `[ ]` |
| `v0.5.0-stdlib`      | Standard library: `List`, `Dict`, `Float`, `String`, `Date`, `Duration`, `Mutex`, `Tag`, `Event`, plus bounded-container variants (`FixedList`, `FixedDict`, `RingBuffer`). `urbi_lock_heap()` for static-allocation-after-init. Total Linux binary < 400 KB.                                  | `[ ]` |
| `v0.6.0-embedded`    | Full public C API (≤ 80 functions); ESP-IDF component manifest; STM32H7 HAL integration; sandbox API (instruction and allocation budgets, host-call allow-list); LED-from-urbiscript demos on both targets. < 400 KB flash on each.                                                            | `[ ]` |
| `v0.7.0-repl`        | NDJSON REPL protocol over TCP and UART; per-session lobbies; introspection commands (`:coros`, `:tags`, `:watchers`, ...); end-to-end hot-reload demonstrated on ESP32-C3 over UART; `urbi-send` CLI.                                                                                          | `[ ]` |
| `v0.8.0-ros2`        | micro-ROS bridge (conditionally compiled): `ros.subscribe()`, `ros.publisher()`, `ros.client()`, `ros.service()`, plus `at (ros.topic?(var msg))` reactive integration. Primary demo: ESP32-C3 pub/sub against a Linux ROS2 graph with reactive command handling. Bridge build < 500 KB flash. | `[ ]` |
| `v1.0.0`             | Release prep: README polish, `docs/RELEASE_CHECKLIST.md`, per-category conformance report, examples directory covering all three architectures, hardware-in-loop validation on real STM32H7 and ESP32-C3.                                                                                      | `[ ]` |

The expressions / concurrency / reactive chain (`v0.2.0` → `v0.3.0` → `v0.4.0`) is strictly sequential — each layer builds on the previous semantics. The embedded port (`v0.6.0`), REPL (`v0.7.0`), and ROS2 bridge (`v0.8.0`) can reorder if motivation dictates; ROS2 depends on having a stable C API and a usable REPL for debugging.

---

## Quality bars at v1.0

The numbers are pass/fail gates, not aspirations:

- ≥ 90% line coverage, ≥ 80% branch coverage
- ≥ 95% of ported `.chk` conformance fixtures passing
- Worst-case GC pause ≤ 1 ms on 32-bit embedded under typical reactive workload (10–50 active coroutines, 5–20 watchers, 50–200 KB heap, 1–10 KB/s allocation rate)
- < 400 KB binary footprint demonstrated on all three target architectures
- A third-party developer, given only the public repo, can clone-to-working-REPL on Linux in ≤ 5 minutes and flash both embedded targets via documented vendor workflows

CI runs the full battery on every commit: host (release / debug / ASan / UBSan), ARM and RISC-V cross-compile, static analysis. Per-merge to main: Valgrind sweep and a 1-hour libFuzzer run per fuzz target. Per release tag: hardware-in-loop validation on actual STM32H7 and ESP32-C3 hardware.

README badges show conformance %, coverage %, p99 GC pause, fuzzer uptime, and per-architecture build status — live, updated by CI.

---

## Non-goals for v1.0

- **Not safety-critical.** No IEC 61508, DO-178, or MISRA certification. Out of scope unless customer-funded.
- **Not bug-compatible with urbi 2.x.** Language is preserved; minor semantic divergences are documented with rationale rather than emulated. Conformance is measured against the 2.x corpus, but the gate is "≥ 95% pass," not "100% bug-for-bug."
- **Not a JIT.** Bytecode interpreter only. A method JIT for Linux is a post-v1.0 consideration; tracing JIT is the wrong shape for event-driven code and stays out.
- **Not multi-threaded per VM.** One cooperative scheduler per VM. Parallelism comes from multiple VM instances on multiple host threads.
- **Not a general-purpose shell.** The REPL targets behavior introspection and hot-patch, not interactive scripting ergonomics.

---

## Beyond v1.0

Direction-only sketch; not committed.

- **v1.x — measurement-certified real-time.** Exhaustive worst-case characterization across all three target architectures, published pause-time distributions, regression guards. The deliverable is a Real-Time Characterization Report with concrete numbers.
- **v2.0 — opt-in hard-real-time subset.** `URBI_MODE_REALTIME` build flag enabling a restricted language subset: no dynamic allocation after init, bounded containers only, no lazy evaluation. Static analyzer (`urbi-rt-check`) flags violations. Reference applications: drone reactive flight-mode controller, servo control loop, CAN bus handler.
- **Other post-v1.0 work.** Xtensa LX7 port (ESP32-S3 hardware already available), Cortex-M4 bare-metal, WASM target, stackless CPS coroutines, optional method JIT for Linux, and IDE tooling (tree-sitter grammar, language server, VS Code / JetBrains plugins) maintained as separate repositories.

The v1.0 design bakes in cheap-now / expensive-later affordances for v2.0: pluggable allocator with `urbi_lock_heap()`, bounded-container stdlib types from day one, configurable prototype-chain depth, deterministic opcode dispatch, scheduler priority hooks, and instruction / allocation budgets in the sandbox API.
