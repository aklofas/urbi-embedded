# urbi-embedded roadmap

## Vision

An embeddable runtime for urbiscript written in pure C99, single library, no external dependencies.

| Property        | Goal                                                                                   |
| --------------- | -------------------------------------------------------------------------------------- |
| Flash footprint | < 400 KB on Cortex-M class MCUs                                                        |
| GC pause        | ≤ 1 ms under typical reactive workload on 32-bit embedded                              |
| Architectures   | x86_64 Linux, ARM Cortex-M (STM32, RP2040), RISC-V 32-bit (ESP32-C3), Xtensa LX7 (ESP32-S3) |
| Host hooks      | Pluggable allocator, time source, panic handler                                        |

Language features kept native:

| Construct                                 | Provides                        |
| ----------------------------------------- | ------------------------------- |
| Statement separators `,` `&` `\|` `;`     | Concurrency                     |
| `at` / `whenever` / `every` / `waituntil` | Reactive control                |
| Tags                                      | Structured cancellation         |
| Time and angle literals                   | Units in the lexical grammar    |

Intended deployment surface: drone autopilots, motor controllers, agricultural robots, and research platforms.

---

## Shipped

(newest first; one line per tag; see CHANGELOG.md for details)

- 2026-05-24 — `v0.9.4-pico-example` — Raspberry Pi Pico hardware bring-up + REPL cooperative-portability sub-project (ABI 0/13/0 → 0/14/0).
- 2026-05-19 — `v0.9.3-ci-hardening` — Host-side freestanding gate + cross-toolchain auto-detect.
- 2026-05-19 — `v0.9.2-uproto-only` — UModule struct deleted; UProto absorbs root metadata (wire v1.7→v1.8 byte-identical, ABI 0/12/0→0/13/0).
- 2026-05-19 — `v0.9.1-repl-service` — TCP/Unix/UART NDJSON REPL + listener pthread + 9 introspection primitives (ABI 0/11/0→0/12/0).
- 2026-05-19 — `v0.9.0-repl` — REPL foundation: realm-per-session lobby model + urbi_unload + closure shrink (ABI 0/11/0).
- 2026-05-18 — `v0.8.5-recursive-emit` — Truly-recursive emit + per-UProto ic_index DFS pre-order (ABI 0/9/0→0/10/0).
- 2026-05-18 — `v0.8.4-closure-lifetime` — UClosure + UUpvalCell GC promotion (ABI 0/8/0→0/9/0).
- 2026-05-18 — `v0.8.3-valgrind-and-cross-verify` — Interstitial hygiene: valgrind wedge fix + cross-esp32s3 golden refresh.
- 2026-05-18 — `v0.8.2-stm32f4-mandelbrot` — First non-RTOS port (STM32F429I bare metal) + Mandelbrot demo + 7 latent runtime bugs fixed.
- 2026-05-17 — `v0.8.1-uproto-root` — Variant B refcount fusion, wire format v1.7, ABI 0/8/0.
- 2026-05-16 — `v0.8.0-loader-strand` — Persistent loader strand restores chunk-top parallel-by-syntax.
- 2026-05-16 — `v0.7.3-bugfixes` — Cascade-wake structural fix via UProto refcount (ABI 0/7/3→0/7/4).
- 2026-05-16 — `v0.7.2-esp32` — ESP-IDF v6.0.1 + ESP32-S3-EYE port + 10-bug runtime hardening hunt (ABI 0/7/1→0/7/3).
- 2026-05-14 — `v0.7.1-embedding-api` — Library-complete C embedding API, 18 spec gaps closed (ABI 0/7/0→0/7/1).
- 2026-05-10 — `v0.7.0-c-api` — Public C API formalization, ABI 0/7/0.
- 2026-05-10 — `v0.6.2-language-completion` — 5 v1.0 emit/VM gaps closed; wire format v1.5→v1.6.
- 2026-05-10 — `v0.6.1-stdlib` — M6 Wave 2: full Tier 1 stdlib + bake tool.
- 2026-05-09 — `v0.6.0-stdlib-scaffold` — M6 Wave 1: string literals + atom-method dispatch + 9 Object root methods.
- 2026-05-09 — `v0.5.8-cleanup` — Pre-M6 cleanup ramp final wave (Wave 6 of 6): strict-tooling + docstring gate.
- 2026-05-08 — `v0.5.7.1` — Wire-format hash gate determinism hotfix.
- 2026-05-08 — `v0.5.7-fixes` — Fix wave.
- 2026-05-07 — `v0.5.6-bytecode` — Wire format v1.4→v1.5.
- 2026-05-07 — `v0.5.5-naming` — Naming hygiene.
- 2026-05-06 — `v0.5.4-decompose` — File decomposition.
- 2026-05-06 — `v0.5.3-layout` — Source-layout reorganization.
- 2026-05-05 — `v0.5.2-scratch-frame-followup` — Scratch-frame follow-up.
- 2026-05-05 — `v0.5.1-cond-unstub` — Condition unstub.
- 2026-05-04 — `v0.5.0-reactive` — M5 reactive runtime: `at` / `whenever` / `every` / `waituntil`, first-class events, tags.
- 2026-05-02 — `v0.4.0-objects` — M4 prototype object model: slot lookup, inline cache, multi-proto MRO.
- 2026-04-28 — `v0.3.0-concurrency` — M3 cooperative scheduler: stackful coroutines, statement separators, tags, incremental GC.
- 2026-04-25 — `v0.2.0-expressions` — M2 expressions, closures, control flow, statement-separator parse.
- 2026-04-24 — `v0.1.0-skeleton` — M1 walking skeleton: lexer, parser, 8-opcode VM, first `.chk` fixture.

---

## Active milestone arc: v0.10.x (architectural refactor before v1.0)

The v0.10.x arc is a series of seven architectural refactor passes addressing structural debt surfaced by hardware testing and ten parallel static audits. Each pass ships as its own milestone tag.

- **v0.10.0-truthfulness** — Documentation + manifests + CI gates aligned with code state. ABI/wire unchanged. *(Active.)*
- **v0.10.1-invariants** — Convert comment-only contracts to enforced ones (asserts that survive release + freestanding).
- **v0.10.2-reactive** — Close language-USP gaps (`whenever` named-event, OP_CLOSURE in every body, tag cancellation, deferred slot-change ring rooting, `sleep`, `Tag.new`).
- **v0.10.3-api-opacity** — Public API freeze preconditions (opaque UVM, error model unification, vm-first-arg sweep, embedding guide rewrite).
- **v0.10.4-vm-decomp** — Behaviour-preserving VM dispatch + UVM struct decomposition.
- **v0.10.5-legacy-decisions** — Explicit decision per legacy-language gap (control flow, quoted identifiers, exception syntax, etc.).
- **v0.10.6-stabilization** — Listener teardown fix + ABI/wire freeze pin + REPL security gates + release-readiness completion.

---

## Remaining (post-v0.10.x)

- **v0.11.x** — ROS2 integration + Standard Robotics API (was M9).
- **v1.0** — Release. Ships after v0.10.x arc + v0.11.x land.

---

## Post-v1.0 (deferred)

Direction-only; not committed:

- Multi-VM concurrency.
- Generational GC.
- Per-arena / arena-per-tag GC.
- Preemptive scheduler mode.
- Weak references.
- Live-system bytecode upgrade tooling.
- True f32 mode for FPU-less targets.
- Method JIT for Linux.

---

## Quality bars at v1.0

The numbers are pass/fail gates, not aspirations:

- ≥ 90% line coverage, ≥ 80% branch coverage
- ≥ 95% of ported `.chk` conformance fixtures passing
- Worst-case GC pause ≤ 1 ms on 32-bit embedded under typical reactive workload (10–50 active coroutines, 5–20 watchers, 50–200 KB heap, 1–10 KB/s allocation rate)
- < 400 KB binary footprint demonstrated on all three target architectures
- A third-party developer, given only the public repo, can clone-to-working-REPL on Linux in ≤ 5 minutes and flash embedded targets via documented vendor workflows

CI runs the full battery on every commit: host (release / debug / ASan / UBSan), ARM and RISC-V cross-compile, static analysis. Per-merge to main: Valgrind sweep. Per release tag: hardware-in-loop validation on actual hardware.

---

## Non-goals for v1.0

- **Not safety-critical.** No formal safety-certification in v1.0 scope.
- **Not bug-compatible with urbi 2.x.** Language is preserved; minor semantic divergences are documented with rationale rather than emulated. Conformance is measured against the 2.x corpus, but the gate is "≥ 95% pass," not "100% bug-for-bug."
- **Not a JIT.** Bytecode interpreter only. A method JIT for Linux is a post-v1.0 consideration.
- **Not multi-threaded per VM.** One cooperative scheduler per VM. Parallelism comes from multiple VM instances on multiple host threads.
- **Not a general-purpose shell.** The REPL targets behavior introspection and hot-patch, not interactive scripting ergonomics.
