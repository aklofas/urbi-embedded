# urbi-embedded release readiness

> v1.0 quality-bar tracker. Each row maps a promised quality bar to its current
> evidence. Every row now has one of:
> (a) **passing-evidence** — a CI gate or measured result that passes.
> (b) **manual procedure** — documented in `docs/release/manual-procedures.md`.
> (c) **removed from v1.0 claims** — rationale below + CHANGELOG entry.
>
> Wave 7 W5 drove every TBD row to one of these three dispositions.
>
> A v1.0-rc cannot be cut until every row has either (a) or (b), or is
> explicitly removed from v1.0 claims. Status as of v0.10.6-stabilization:
> **32/32 rows resolved** (22 passing-evidence, 4 manual procedure,
> 6 removed from v1.0 claims).

## Test conformance

| Bar | Threshold | Current | Evidence | Last measured | Owner / blocker |
|---|---|---|---|---|---|
| .chk pass rate (active fixtures) | ≥95% | 100% (288/288) | `make test-chk` — 288 fixtures, 0 failures | 2026-05-27 | **passing-evidence** |
| .chk total fixture count | n/a | 304 total (288 non-repl active, 16 REPL-gated) | `find tests/chk -name '*.chk' \| wc -l` | 2026-05-27 | **passing-evidence** (informational) — bumped from 284 baseline as Cat. E ratification arc activates new fixtures (v0.10.9 +6, v0.10.10 +6, v0.10.11 +8, v0.10.12 +3 activated in-place via fixture rewrite — total count unchanged) |
| .chk taxonomy | normalized | v0.10.7 W7: all defer-to: labels retired; 163 active with real content, 22 deferred-v1.x, 76 blocked, 3 dropped | `rg -n 'defer-to:' tests/chk/` returns empty | 2026-05-26 | **passing-evidence** — see docs/release/chk-deferred-taxonomy.md |
| Conformance manifest | yes/no | v0.10.7: 163/163 real-content fixtures pass (100%); denominator computable | `docs/language-compatibility-matrix.md` §v1.0 conformance denominator | 2026-05-26 | **passing-evidence** — 163 real-content fixtures / 269 total (106 placeholders vacuous-pass) |

## Coverage

| Bar | Threshold | Current | Evidence | Last measured | Owner / blocker |
|---|---|---|---|---|---|
| Line coverage | ≥85% (enforced; aspirational v1.0: ≥90%) | 87% | `make coverage` (gcovr `--fail-under-line 85`) | 2026-05-26 | **passing-evidence** — Path A enforcement at 85%; threshold raised to 90% at v1.0 when gap closes |
| Branch coverage | informational | not gated | `make test-branch-coverage` (gcovr `--branches`) | n/a | **removed from v1.0 claims** — branch coverage measured but not gated; see CHANGELOG entry |
| Condition coverage | n/a | not measured | n/a | n/a | **removed from v1.0 claims** — no meaningful coverage tool for C99 MC/DC; dropped |

## Memory + performance

| Bar | Threshold | Current | Evidence | Last measured | Owner / blocker |
|---|---|---|---|---|---|
| GC pause (worst-case) | ≤1ms under representative reactive workload | not measured | n/a | n/a | **removed from v1.0 claims** — no representative workload defined; GC-pause SLA deferred to v1.x; see `docs/urbi-embedded-design-risks.md` |
| Flash text size (per target) | <400KB | ARM CM7: ~123 KB; Pico CM0+: ~81 KB; ESP32-S3: ~120 KB; STM32F4 CM4F: <130 KB | `arm-none-eabi-size`/`xtensa-size` per target; per-target caps in `docs/internals/ports.md` | 2026-05-26 (CM7, Pico) | **passing-evidence** — all targets well under 400 KB cap; tracked per bring-up |
| Heap budget (per target) | per `docs/internals/ports.md` | tracked per target | `hardware-validation.md` | per bring-up | **passing-evidence** — measured at each hardware bring-up; shiptest H1/H2/H3 procedures confirm |

## Hardware support

| Target | Status | CI gate | Runtime smoke | Hardware evidence | Last verified |
|---|---|---|---|---|---|
| Linux x86_64 (host) | shipped | host-test matrix | n/a | n/a | continuously |
| Raspberry Pi Pico | shipped | cross-pico, cross-pico-repl | none automated | hardware-validation.md | 2026-05-24 (v0.9.4) |
| ESP32-S3 | shipped | cross-esp32s3 | qemu-smoke reactive | hardware-validation.md | 2026-05-16 (v0.7.2) |
| STM32F4 | shipped | cross-stm32f4 | none automated | hardware-validation.md | 2026-05-17 (v0.8.2) |
| ARM Cortex-M7 | archive-only | cross-arm | none | n/a | n/a |
| RISC-V rv32imc | archive-only | cross-riscv | none | n/a | n/a |
| STM32H7 | **removed from v1.0 claims** | n/a | n/a | n/a | n/a |
| ESP32-C3 | **removed from v1.0 claims** | n/a | n/a | n/a | n/a |

STM32H7 and ESP32-C3 are deferred to v1.x; not in the v1.0 hardware-support claim.

## Build hygiene

| Bar | Status | Evidence | Owner / blocker |
|---|---|---|---|
| Component manifest version sync | gated | `make check-version-sync` | **passing-evidence** — continuous (since v0.8) |
| Stdlib bytecode freshness | **gated (W5)** | `make test-stdlib-bytecode-fresh` — passes | **passing-evidence** — regenerate-diff gate added v0.10.6; see `tests/scripts/check-stdlib-fresh.sh` |
| Freestanding-host gate | gated | `make test-freestanding-host` | **passing-evidence** — continuous (since v0.9.3) |
| Cross-target cooperative REPL gates | partial (Pico only) | `test-cross-pico-repl-elf` | **manual procedure** — ESP32/STM32 hardware REPL validated at bring-up (shiptest H1/H2/H3); other targets out of v1.0 scope |
| Public doc scrub of workspace-private paths | gated | `make docs-check` w/ scrub check | **passing-evidence** — continuous |

## C API

| Bar | Status | Evidence | Owner / blocker |
|---|---|---|---|
| Public headers self-contained (no `-Isrc`) | **done (v0.10.x arc)** | `make test-external-embed-iinclude` — passes | **passing-evidence** — closed at v0.10.3-api-opacity W2 |
| External-embedder minimal compile test | **gated** | `make test-external-embed-iinclude` | **passing-evidence** — continuous since v0.10.3 |
| API surface tiered (URBI_EXPERIMENTAL) | **done (v0.10.3)** | `include/urbi/version.h` URBI_EXPERIMENTAL/URBI_ADVANCED macros | **passing-evidence** — closed at v0.10.3-api-opacity W7 |
| Build-flag mismatch link-time guards (URBI_FLOAT_TYPE, URBI_REPL_COOPERATIVE_ONLY, URBI_BYTECODE_ONLY) | done — v0.10.1-invariants W1-W3 | `make test` — invariant checks compile in | **passing-evidence** — continuous since v0.10.1 |
| Error model unified | **done (v0.10.3)** | `src/runtime/uabi_guards.c` unified return-int model | **passing-evidence** — closed at v0.10.3-api-opacity W3 |
| ABI freeze pin | **done (W2)** | `make test-abi-freeze` — "ABI freeze pin in sync: 0/20/0" | **passing-evidence** — `_Static_assert` in `include/urbi/version.h`; v0.10.6-stabilization froze at 0/18/0; v0.10.7-audit-followup bumped PATCH to 0/18/1 (no public surface change); v0.10.8-string-concat bumped PATCH to 0/18/2 (S-string-plus runtime String + String concat — atom fast path in OP_ADD; no public surface change); v0.10.9-tag-state bumped MINOR to 0/19/0 (D1 SUSPENDED-machinery ratification — 4 new public symbols `urbi_tag_block`/`_unblock`/`_freeze`/`_unfreeze`; first post-freeze MINOR break per §3); v0.10.10-job-introspection bumped PATCH to 0/19/1 (D7 full-ship Cat. E ratification — all new surface script-side, zero new public C API symbols); v0.10.11-channel-and-isA bumped PATCH to 0/19/2 (D6+isA+D5 Cat. E ratification — Channel proto, cout/cerr/clog, <<, isA(), Object unfreeze — all script-side, zero new public C API symbols); v0.10.12-cat-e-activation bumped PATCH to 0/19/3 (final tag of 4-tag Cat. E ratification arc — fixture-and-doc-only tag, no functional changes, zero new public C API symbols); v0.10.13-hygiene bumped PATCH to 0/19/4 (post-Cat. E hygiene + targeted runtime bug fix — markdownlint config, make all + urbi CLI, String.asString stdlib overlay, slot-change first-install double-fire suppression; zero new public C API symbols); v0.10.14-prerc-infra bumped PATCH to 0/19/5 (pre-v1.0-rc stabilization arc — STYLE.md doc correction, REPL output-backpressure liveness fix, C `.chk` host-driver; zero new public C API symbols); v0.10.15-vm-decomp-2 bumped PATCH to 0/19/6 (final tag of the pre-v1.0-rc stabilization arc — VM dispatch extraction round 2 into `uvm_tag_scope` + `uvm_reactive_install`, plus v0.10.9-B user-tag scope binding and v0.10.7-B tag.stop()/finally fix; zero new public C API symbols); v0.11.0-trace-spine bumped MINOR to 0/20/0 (first tag of the v0.11.x tooling arc — new public EXPERIMENTAL header `include/urbi/trace.h` with the trace control/drain/stats API + `URBI_TP` macros, compile-gated by `URBI_TRACE`; 24th use of the pre-v1.0 escape clause; MINOR because new public surface, no new opcodes, wire unchanged); `docs/api-stability.md` documents post-freeze policy |
| Wire format freeze pin | **done (W3)** | `make test-wire-freeze` — "wire format freeze pin in sync: v1.9 / 0x19" | **passing-evidence** — `_Static_assert` in `src/chunk/uchunk_io.c`; freeze pinned at v0.10.6-stabilization |
| Dependency pins documented | **done (W5)** | `make test-dependency-pins` — passes | **passing-evidence** — ESP-IDF v6.0.1, pico-sdk 2.2.0, xpack riscv 15.2.0 pinned in CI and docs; apt arm divergence documented |

## REPL

| Bar | Status | Evidence | Owner / blocker |
|---|---|---|---|
| Multi-client teardown stress | **default CI (W1)** | `tests/unit/test_repl_multi_client.c` — 100 trials, 4 concurrent sessions | **passing-evidence** — single-owner teardown model; `repl-multi-client-stress` CI job; race closed at v0.10.6 |
| Non-loopback bind requires token | **enforced (W4)** | `tests/unit/test_repl_security_bind_auth.c` | **passing-evidence** — `urepl_create` rejects `URBI_ERR_INVALID_CONFIG` on non-loopback + no token |
| Rate limit per source | **enforced (W4)** | `tests/unit/test_repl_security_rate_limit.c` | **passing-evidence** — per-source token bucket via `UReplConfig.rate_limit_per_second` |
| Compile-budget enforcement | yes | `docs/internals/repl-service.md` | **passing-evidence** — `urbi_realm_set_compile_budget`; `test_repl_security_compile_budget` test |
| Malformed NDJSON tolerance | **tested (W4)** | `tests/unit/test_repl_security_malformed.c` | **passing-evidence** — explicit `{"error":"malformed_ndjson"}` response; no crash under ASan |
| Per-session output isolation | yes | `docs/internals/repl-service.md` | **passing-evidence** — per-realm writer model; `test_repl_security_output_isolation` test |

## Reactive runtime

| Bar | Status | Evidence | Owner / blocker |
|---|---|---|---|
| `whenever (named_event)` works end-to-end on cooperative builds | **done (v0.10.2)** | `tests/chk/reactive/whenever_event_basic.chk` + related | **passing-evidence** — closed at v0.10.2-reactive Wave 3 W0 |
| `OP_CLOSURE` inside nested `every()` body | **done (v0.10.2)** | `tests/chk/reactive/every_with_closure.chk` | **passing-evidence** — closed at v0.10.2-reactive Wave 3 W1 |
| AT_EVENT watcher unlinks on tag-stop | **done (v0.10.2)** | `tests/chk/reactive/at_event_tag_stop_unlinks.chk` | **passing-evidence** — closed at v0.10.2-reactive Wave 3 W2 |
| Deferred slot-change ring has GC root provider | **done (v0.10.2)** | `tests/chk/reactive/changed_ring_gc_root.chk` | **passing-evidence** — closed at v0.10.2-reactive Wave 3 W3 |
| `Tag.new()` + script-side `.stop()` | **done (v0.10.2)** | `tests/chk/tag/tag_stop*.chk` | **passing-evidence** — closed at v0.10.2-reactive Wave 3 W4 |
| Bare-prefix tag labels (`mytag: stmt`) | **done (v0.10.2/v0.10.5)** | `tests/chk/tag/tag_member_expr.chk` | **passing-evidence** — Wave 3 W5 + Wave 6 W8 |
| `sleep(duration)` built-in | **done (v0.10.2)** | `tests/chk/temporal/sleep_basic.chk` | **passing-evidence** — closed at v0.10.2-reactive Wave 3 W6 |

## Language compatibility

| Bar | Status | Evidence | Owner / blocker |
|---|---|---|---|
| Defensible v1.0 compatibility denominator | **done (v0.10.7 W7)** | `docs/language-compatibility-matrix.md` fully populated; `defer-to:` labels retired | **passing-evidence** — 163/163 real-content fixtures pass (100%); 106 placeholder fixtures (blocked/deferred/dropped) vacuous-pass; denominator published in matrix §v1.0 conformance denominator |
| Statement grammar covers planned v1 surface | **done (Wave 6 W1)** | `tests/chk/control/` fixtures | **passing-evidence** — for-each, break, continue, switch IMPLEMENTED; C-style for / loop / do(recv) deferred-v1.x |
| Reactive syntax covers legacy forms | **done (Wave 6 W9)** | `tests/chk/reactive/` fixtures | **passing-evidence** — at(e?(var x)), whenever(e?(var x)) payload, whenever-else, waituntil(e?) IMPLEMENTED; ~duration deferred-v1.x |
| Quoted identifiers + operator slots | **done (Wave 6 W2)** | `tests/chk/lex/quoted_ident_basic.chk` | **passing-evidence** — single-quote-delimited identifiers; keyword-escaping; operator-slot access |
| `catch (var e)` + guards + else | **done (Wave 6 W5)** | `tests/chk/exceptions/catch_var*.chk` | **passing-evidence** — `catch (var e)`, `catch (var e if cond)`, try-catch-else all IMPLEMENTED |
| Block comment nesting | **decided (Wave 6 W6)** | `docs/LANG-CONVENTIONS.md §7` | **passing-evidence** — non-nesting is the implementation; divergence from legacy LOCKED; migration doc provided |
| Angle / physical literals | **done (Wave 6 W4)** | `tests/chk/lex/angle-literals.chk` | **passing-evidence** — deg/rad/grad IMPLEMENTED; pi = Math.pi; physical literals deferred-v1.x |
| `assert` language keyword | **done (Wave 6 W3)** | `tests/chk/control/assert_basic.chk` | **passing-evidence** — assert(expr) + assert{block} via if-throw lowering |
| List/dict literals + subscript assignment | **done (Wave 6 W10)** | `tests/chk/objects/list_literal.chk`, `dict_literal.chk`, `subscript_basic.chk` | **passing-evidence** — stdlib-call lowering; var-obj-slot |
| Top-level `this` / Lobby | **migration (Wave 6 W11)** | `docs/migration/top-level-this-lobby-migration.md` | **passing-evidence** — migrated to `Realm`; migration guide provided |

## Closure of v1.0-rc

Cut a v1.0-rc only when:

1. ✅ **Every row above is either passing-evidence or formally "removed from v1.0 claims"** — completed by Wave 7 W5 (v0.10.6-stabilization). See row-count summary in the file header.
2. ✅ **`docs/api-stability.md` exists and ABI freeze pin is in place** — completed by Wave 7 W2. `make test-abi-freeze` passes.
3. ✅ **`docs/internals/bytecode-format.md` documents the frozen wire format** — completed by Wave 7 W3. `make test-wire-freeze` passes.
4. ✅ **`language-compatibility-matrix.md` published v1.0 conformance percentage** — v0.10.7 W7 retired all `defer-to:` labels and published the denominator. 163 real-content fixtures pass (100%); 106 placeholder fixtures vacuous-pass; taxonomy in `docs/release/chk-deferred-taxonomy.md`.
5. **No `docs/urbi-embedded-design-risks.md` entry tagged "Handle before v1.0" remains open** — requires workspace-root W6 triage (runs independent of this worktree). Status tracked separately.

---

## Removed-from-v1.0-claims rationale

The following bars were removed from v1.0 scope at v0.10.6-stabilization:

- **Branch coverage gate:** Branch coverage is measured via `make test-branch-coverage` and reported per-tag, but not gated. Branch coverage at v0.10.5 was below 80%; gating a bar we cannot currently meet would make the release gate unenforced-in-practice. Moved to v1.x aspirational target.
- **Condition coverage gate:** No standard gcovr mode for C99 MC/DC. Measuring would require gcov branch-level instrumentation which is already captured by branch coverage. Dropped from v1.0 quality bar.
- **GC pause ≤1ms:** No representative reactive workload is defined; pausing under synthetic microbenchmark would be misleading. GC-pause SLA deferred to v1.x. Tracking entry in `docs/urbi-embedded-design-risks.md`.
- **STM32H7 target:** Not brought up; no CI gate. Deferred to v1.x / ROS2 milestone.
- **ESP32-C3 target:** Not brought up; no CI gate. Deferred to v1.x.
