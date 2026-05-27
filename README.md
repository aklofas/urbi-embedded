# urbi-embedded

![ci](https://github.com/aklofas/urbi-embedded/actions/workflows/ci.yml/badge.svg)

An embeddable orchestration scripting language for robotics and physical systems, in pure C99.

Implements **urbiscript** — a prototype-based, parallel-by-default, event-driven language designed for coordinating sensors, actuators, and reactive control loops on fast underlying code. Sits above C/C++ control loops the way Lua sits above game engines: handles concurrency, time, events, and cancellation as first-class primitives instead of patterns the developer has to construct by hand.

**Status:** v0.10.7-audit-followup — post-arc fix wave addressing 14 findings from two external audits of the v0.10.x arc. ABI 0/18/1 (PATCH bump only — no public surface change). Wire v1.9 / 0x19 unchanged. 7 worktrees:

- **W1 — switch/continue semantic fix.** Separate parser break-target from continue-target so `continue` inside a switch with no enclosing loop is a parse error and `continue` in switch-in-loop targets the outer loop (matches C/JavaScript semantics). Closes audit-2 #1.
- **W2 — compound subscript single-evaluation.** `l[i] += v` now evaluates `l` and `i` exactly once via temp registers + new `AST_REG_REF` emit-only synthetic node. Closes audit-1 F4 / audit-2 #2.
- **W3 — REPL single-owner teardown.** Fixed `__ATOMIC_RELAXED` store → `RELEASE`; `r->session = NULL` writes serialized under `sessions_mutex`; stop-path direct destroy documented as single-threaded by construction. Closes audit-1 F1 / audit-2 #6.
- **W4 — doc-drift sweep.** 6 truthfulness defects: ABI freeze docs `0/17/0` → `0/18/0`, wire v1.7 → v1.9 in `module-system.md` + `uemit_serialize.c` comment, reactive-runtime internals refreshed to shipped state, GC cell-inventory `UStrand`/`UTag` sizes corrected against static asserts, CallMessage impact taxonomy unified (23 files via single ripgrep), coverage gate hard-local-vs-advisory-CI clarified.
- **W5 — public-doc scrub gate.** New `tests/scripts/check-public-doc-scrub.sh` wired into `make docs-check` greps tracked files for workspace-private paths + tool-context filenames; honors `scrub-allow:` opt-out. Cleaned 9 pre-existing violations. Closes audit-2 #3.
- **W6 — freeze-gate breadth.** `check-abi-freeze.sh` + `check-wire-freeze.sh` now lint hardcoded test literals (`tests/unit/test_api_version.c`, `test_version.c`) AND release-evidence docs (`api-stability.md`, `release-readiness.md`) against `version.h` macros. Catches the W7-followup-#1 drift class. Closes audit-1 F3 / audit-2 #5.
- **W7 — `.chk` defer-to taxonomy.** Retired `defer-to: M*/T*` labels on 106 deferred fixtures. New 4-bucket taxonomy: `active` / `deferred: v1.x` / `dropped` / `blocked: <work-item>`. 5 fixtures flipped to active (features shipped during arc, marker stale). New `docs/release/chk-deferred-taxonomy.md`. v1.0 conformance denominator computable. Closes audit-2 #8.

ABI 0/18/0 → 0/18/1 (PATCH-only; pre-v1.0 escape clause not invoked). Wire v1.9 / 0x19 unchanged. Next milestone: **v0.11.x ROS2 (M9)**. Tagged `v0.10.7-audit-followup`.

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
