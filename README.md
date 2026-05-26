# urbi-embedded

![ci](https://github.com/aklofas/urbi-embedded/actions/workflows/ci.yml/badge.svg)

An embeddable orchestration scripting language for robotics and physical systems, in pure C99.

Implements **urbiscript** — a prototype-based, parallel-by-default, event-driven language designed for coordinating sensors, actuators, and reactive control loops on fast underlying code. Sits above C/C++ control loops the way Lua sits above game engines: handles concurrency, time, events, and cancellation as first-class primitives instead of patterns the developer has to construct by hand.

**Status:** v0.10.6-stabilization — Wave 7 (arc-closing wave) of the v0.10.x architectural refactor arc. ABI **frozen at 0/18/0** with a `_Static_assert` pin in `include/urbi/version.h` (further pre-v1.0 changes follow the post-freeze policy at `docs/api-stability.md` §3). Wire format **frozen at v1.9 / 0x19** with a `_Static_assert` pin in `src/chunk/uchunk_io.c`. 6 worktrees:

- **W1 — listener-teardown race fix.** Single-owner session teardown via `urepl_request_teardown` (reader thread) + `urepl_session_reap_pending` (VM thread under `sessions_mutex`). Closes the audit-1 F6 / roadmap F9 race that gated `test_repl_multi_client` behind `URBI_TEST_MULTI_CLIENT=1`. Test promoted to default; new `repl-multi-client-stress` CI job runs 100 ASan trials.
- **W2 — ABI freeze pin.** `_Static_assert` in `version.h` + new `docs/api-stability.md` post-freeze policy + `test-abi-freeze` CI gate. Bumped from 0/17/0 to 0/18/0 at wave wrap-up for the W4 `UReplConfig.rate_limit_per_second` field — first deliberate use of the post-freeze policy.
- **W3 — wire-format freeze pin.** `_Static_assert` in `uchunk_io.c` + reconciled `docs/internals/bytecode-format.md` v1.8 → v1.9 drift + `test-wire-freeze` CI gate.
- **W4 — REPL security gates.** 6 named tests (non-loopback-bind-token, token accept/reject + teardown, rate-limit, compile-budget denial, malformed-NDJSON tolerance, per-session output isolation) + 5 OOM-injection tests. `UReplConfig.rate_limit_per_second` int field (POSIX-only enforcement). `URBI_ERR_INVALID_CONFIG` `#define` alias for the pre-existing `URBI_ERR_INSECURE_CONFIG` (no new error slot). Closes release F13 + audit-1 F16.
- **W5 — release-readiness completion.** 32/32 release-readiness rows resolved (22 passing-evidence, 4 manual procedure, 6 removed from v1.0 claims). Coverage policy: Path A (enforce `--fail-under-line 85`; line 87% at baseline; 90% remains aspirational v1.x target). Test-tier definitions (devtest / releasetest / shiptest) + release-notes template + manual-procedures doc. New gates: `test-stdlib-bytecode-fresh` + `test-dependency-pins`.
- **W6 — design-risks register triage.** 8 workspace-root entries tagged `v1.0-rc` / `v0.9.x` / "Handle before v1.0" disposed: 2 closed, 1 downgraded, 1 mapped to release-readiness, 3 confirmed already-v1.x, 1 already-RESOLVED. Arc exit criterion §11 satisfied: 0 entries remain open for v1.0-rc.

ABI 0/17/0 → 0/18/0 (16th and final use of pre-v1.0 escape clause; the pin is the freeze symbol). Wire v1.9 / 0x19 unchanged (W3 pins the existing format). Closes the v0.10.x architectural refactor arc. Next milestone: **v0.11.x ROS2 (M9)**. Tagged `v0.10.6-stabilization`.

## Design goals

- Pure C99, single library, zero external dependencies
- Builds with `make` — no CMake, no autotools, no bootstrap
- Target footprint: < 400 KB flash on Cortex-M class MCUs
- Host-pluggable allocator, I/O sink, time source, panic handler
- No global state — multiple VM instances coexist, fully isolated
- Bytecode / source split: embedded targets can omit the compiler
- BSD-3-Clause throughout

## Supported targets

| Target | Status | CI gate | Runtime smoke | Hardware evidence |
|---|---|---|---|---|
| Linux x86_64 (host) | shipped | host-test matrix | n/a | n/a |
| Raspberry Pi Pico (RP2040 / Cortex-M0+) | shipped | cross-pico + cross-pico-repl | none | yes — see `docs/release/hardware-validation.md` |
| ESP32-S3 (Xtensa LX7, ESP-IDF v6.0.1) | shipped | cross-esp32s3 | none | yes — eye_demo bring-up |
| STM32F4 (Cortex-M4F) | shipped | cross-stm32f4 | none | yes — Mandelbrot demo |
| ARM Cortex-M7 (generic) | shipped | cross-arm | none | n/a — archive build only |
| RISC-V rv32imc (generic) | shipped | cross-riscv | none | n/a — archive build only |
| STM32H7 | planned | n/a | n/a | n/a — see ROADMAP |
| ESP32-C3 | planned | n/a | n/a | n/a — see ROADMAP |

## Build

```sh
make
```

Produces `build/host/liburbi.a`. All build variants (release, debug, sanitizers, cross-compiles) land in `build/<TARGET>/` subtrees — see `CONTRIBUTING.md` for the full list. The public API is spread across `<urbi/types.h>`, `<urbi/urbi.h>`, `<urbi/gc.h>`, `<urbi/sched.h>`, and `<urbi/object.h>` — VM lifecycle, chunk loading, strand spawn / step driver, ISR-safe event injection, realm globals, GC primitives, and the object surface. The headers are self-contained: external consumers using only `-Iinclude` resolve cleanly without internal includes. See `docs/embedding-guide.md` for the full embedding contract (host integration patterns, FreeRTOS pattern, REPL service).

## Using the REPL

Build the `urbi` binary:

```sh
make urbi-bin   # produces build/host/urbi
```

Interactive session:

```sh
./build/host/urbi -i
1 + 2
[00000001] 3
5 / 2
[00000012] 2.5
```

Evaluate a single expression:

```sh
./build/host/urbi -e "1 + 2"
3
```

Run a script:

```sh
./build/host/urbi script.urb
```

Disassemble:

```sh
./build/host/urbi --dump-bytecode -e "1 + 2 * 3"
```

See `./build/host/urbi --help` for the full flag list.

## REPL service

Opt-in subsystem (build with `URBI_ENABLE_REPL=1`): NDJSON line-protocol REPL over TCP / Unix socket / UART, with bearer-token auth, per-session output isolation, and 9 introspection ops. Builds two extra host binaries: `urbi-server` (headless) and `urbi-send` (client).

Build:

```sh
make URBI_ENABLE_REPL=1            # liburbi.a with REPL support
make urbi-server-bin URBI_ENABLE_REPL=1  # build/host/urbi-server
make urbi-send-bin   URBI_ENABLE_REPL=1  # build/host/urbi-send
```

Start a server on loopback (no token needed):

```sh
./build/host/urbi-server --port 54000
```

From a second shell, send one-shot ops:

```sh
./build/host/urbi-send eval "1 + 2"             # → 3
./build/host/urbi-send introspect coros         # → JSON list of strands
./build/host/urbi-send --tail eval "every(1s) { echo 'tick' }"
```

Exposing the server on a LAN interface requires `--token`:

```sh
./build/host/urbi-server --bind 0.0.0.0 --port 54000 --token "$(openssl rand -hex 16)"
./build/host/urbi-send --host robot.local:54000 --token "$TOK" eval "Robot.battery"
```

Embedders can also combine local linenoise REPL + network service in one process via the `urbi --listen` flag, or start the service programmatically with `urbi_repl_serve` from `<urbi/repl.h>`. See `docs/embedding-guide.md` §12 (REPL Service) and `docs/internals/repl-service.md` for the full API + wire-protocol reference.

## Source layout

```text
include/urbi/   public C API headers (urbi.h, gc.h, sched.h, object.h, ...)
src/
├── chunk/      bytecode + UProto + UChunkIO
├── emit/       compiler emit
├── event/      UEvent + native event registration
├── gc/         incremental GC + barriers
├── lex/        lexer
├── object/     UObject + UShape + UIC + UChunkInstance
├── parse/      parser
├── realm/      URealm + lobby + per-realm globals
├── repl/       REPL service + transports + listener
├── runtime/    UCallFrame + UUpvalCell + unwind + cleanup
├── sched/      cooperative scheduler + UStrand
├── stdlib/     baked stdlib + Object/List/Dict/etc.
├── tag/        UTag
├── value/      UValue + intern + arena
├── vm/         dispatch loop + OP_* handlers
└── watcher/    UWatcher + install/eval/drain/spawn
tools/          host binaries (urbi, urbi-server, urbi-send) + vendored linenoise
```

Subsystem-directory layout under `src/`; each subsystem is a
self-contained set of translation units. The `tools/` directory
contains host binaries and vendored linenoise — neither is part of
`liburbi.a`.

## Documentation

- `CONTRIBUTING.md` — build, test, cross-compile, and contribution how-tos
- `docs/STYLE.md` — code-level style decisions (naming, const-correctness, error model, initialization, headers, tests)

## License

BSD-3-Clause. See `LICENSE`.
