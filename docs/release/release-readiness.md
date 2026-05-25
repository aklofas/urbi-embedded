# urbi-embedded release readiness

> v1.0 quality-bar tracker. Each row maps a promised quality bar to its current
> evidence. Rows marked TBD will be driven to closure across the v0.10.x
> architectural refactor arc; Wave 7 W5 is the convergence task.
>
> A v1.0-rc cannot be cut until every row has either (a) a passing-evidence
> entry or (b) an explicit "removed from v1.0 claims" rationale linked to a
> ROADMAP update.

## Test conformance

| Bar | Threshold | Current | Evidence | Last measured | Owner / blocker |
|---|---|---|---|---|---|
| .chk pass rate (v1.0 denominator) | ≥95% | TBD | `tests/chk/` corpus via `make test` | TBD | language-compatibility-matrix.md (W14) + Wave 6 |
| .chk total fixture count | n/a | TBD | `find tests/chk -name '*.chk' \| wc -l` | TBD | n/a (informational) |
| Conformance manifest exists | yes/no | NO | machine-readable manifest per audit-1 F14 | n/a | Wave 6 (audit-1 F14 closure) |

## Coverage

| Bar | Threshold | Current | Evidence | Last measured | Owner / blocker |
|---|---|---|---|---|---|
| Line coverage | ≥90% | TBD | `make coverage` (gcovr) | TBD | Wave 7 W5 — policy reconcile (release F9) |
| Branch coverage | ≥80% | not measured | n/a | n/a | Wave 7 W5 |
| Condition coverage | n/a | not measured | n/a | n/a | TBD — drop or measure |

## Memory + performance

| Bar | Threshold | Current | Evidence | Last measured | Owner / blocker |
|---|---|---|---|---|---|
| GC pause (worst-case) | ≤1ms under representative reactive workload | TBD | per-target measurement | TBD | Wave 7 — define workload + measure or downgrade claim |
| Flash text size (per target) | <400KB | partial | per-target `size` output | per-tag | Wave 7 W5 — continuous tracking gate (release F9) |
| Heap budget (per target) | per docs/internals/ports.md | mostly tracked | hardware-validation.md | per bring-up | W13 — drive to completion |

## Hardware support

| Target | Status | CI gate | Runtime smoke | Hardware evidence | Last verified |
|---|---|---|---|---|---|
| Linux x86_64 (host) | shipped | host-test matrix | n/a | n/a | continuously |
| Raspberry Pi Pico | shipped | cross-pico, cross-pico-repl | none | hardware-validation.md | 2026-05-24 (v0.9.4) |
| ESP32-S3 | shipped | cross-esp32s3 | qemu-smoke reactive | hardware-validation.md | 2026-05-16 (v0.7.2) |
| STM32F4 | shipped | cross-stm32f4 | none | hardware-validation.md | 2026-05-17 (v0.8.2) |
| ARM Cortex-M7 | archive-only | cross-arm | none | n/a | n/a |
| RISC-V rv32imc | archive-only | cross-riscv | none | n/a | n/a |
| STM32H7 | planned | n/a | n/a | n/a | n/a |
| ESP32-C3 | planned | n/a | n/a | n/a | n/a |

## Build hygiene

| Bar | Status | Evidence | Owner / blocker |
|---|---|---|---|
| Component manifest version sync | gated | `make check-version-sync` | W8 — gate landed |
| Stdlib bytecode freshness | not gated | n/a | Wave 7 W5 — regen-diff gate |
| Freestanding-host gate | gated | `make test-freestanding-host` | continuous (since v0.9.3) |
| Cross-target cooperative REPL gates | partial (Pico only) | `test-cross-pico-repl-elf` | Wave 1 follow-up + Wave 7 |
| Public doc scrub of workspace-private paths | gated | `make docs-check` w/ scrub check | W10 (this wave) |

## C API

| Bar | Status | Evidence | Owner / blocker |
|---|---|---|---|
| Public headers self-contained (no `-Isrc`) | NO | audit-1 F1 | Wave 4 W1 + W2 |
| External-embedder minimal compile test | not gated | n/a | Wave 4 W6 |
| API surface tiered (URBI_EXPERIMENTAL) | not done | audit-1 F13 | Wave 4 W7 |
| Build-flag mismatch link-time guards (URBI_FLOAT_TYPE, URBI_REPL_COOPERATIVE_ONLY, URBI_BYTECODE_ONLY) | not done | audit-1 F2, roadmap F7 | Wave 2 W1-W3 |
| Error model unified | NO (4 styles) | API F2 | Wave 4 W3 |
| ABI freeze pin | not done | n/a | Wave 7 W2 |
| Wire format freeze pin | not done | n/a | Wave 7 W3 |

## REPL

| Bar | Status | Evidence | Owner / blocker |
|---|---|---|---|
| Multi-client teardown stress | opt-in (URBI_TEST_MULTI_CLIENT=1) | tests/unit/test_repl_multi_client.c | Wave 7 W1 — promote to default |
| Non-loopback bind requires token | partial | release F13 | Wave 7 W4 |
| Rate limit per source | TBD | TBD | Wave 7 W4 |
| Compile-budget enforcement | yes | docs/internals/repl-service.md | continuous |
| Malformed NDJSON tolerance | TBD | TBD | Wave 7 W4 |
| Per-session output isolation | yes | docs/internals/repl-service.md | continuous |

## Reactive runtime

| Bar | Status | Evidence | Owner / blocker |
|---|---|---|---|
| `whenever (named_event)` works end-to-end on cooperative builds | NO (broken by construction) | reactive F1 | Wave 3 W0 |
| `OP_CLOSURE` inside nested `every()` body | NO (proto index OOB) | reactive F4 | Wave 3 W1 |
| AT_EVENT watcher unlinks on tag-stop | NO (leaks on event chain) | reactive F2 | Wave 3 W2 |
| Deferred slot-change ring has GC root provider | NO (weak ref) | audit-1 F9 | Wave 3 W3 |
| `Tag.new()` + script-side `.stop()` | NO (C-only today) | reactive F3, audit-1 F5 | Wave 3 W4 |
| Bare-prefix tag labels (`mytag: stmt`) | NO (brace block required) | legacy F3 | Wave 3 W5 + Wave 6 W8 |
| `sleep(duration)` built-in | NO | legacy F15 | Wave 3 W6 |

## Language compatibility

| Bar | Status | Evidence | Owner / blocker |
|---|---|---|---|
| Defensible v1.0 compatibility denominator | NO | legacy F1 | W14 (this wave scaffold) + Wave 6 (fill) |
| Statement grammar covers planned v1 surface | partial | legacy F2 | Wave 6 W1 |
| Reactive syntax covers legacy forms | partial | legacy F4 | Wave 6 W9 |
| Quoted identifiers + operator slots | NO | legacy F5 | Wave 6 W2 |
| `catch (var e)` + guards + else | NO | legacy F6 | Wave 6 W5 |
| Block comment nesting | non-nesting (diverges) | legacy F7 | Wave 6 W6 (decision) |
| Angle / physical literals | docs claim, lexer misses | legacy F8 | Wave 6 W4 |
| `assert` language keyword | NO | legacy F9 | Wave 6 W3 |
| List/dict literals + subscript assignment | partial | legacy F14 | Wave 6 W10 |
| Top-level `this` / Lobby | TBD | legacy F13 | Wave 6 W11 |

## Closure of v1.0-rc

Cut a v1.0-rc only when:

1. Every row above is either passing-evidence or formally "removed from v1.0 claims" (rationale linked to ROADMAP update).
2. `docs/api-stability.md` exists and ABI freeze pin is in place (Wave 7 W2).
3. `docs/internals/bytecode-format.md` documents the frozen wire format (Wave 7 W3).
4. `language-compatibility-matrix.md` published v1.0 conformance percentage.
5. No `docs/urbi-embedded-design-risks.md` entry tagged "Handle before v1.0" remains open.
