# urbi-embedded

![ci](https://github.com/aklofas/urbi-embedded/actions/workflows/ci.yml/badge.svg)

An embeddable orchestration scripting language for robotics and physical systems, in pure C99.

Implements **urbiscript** — a prototype-based, parallel-by-default, event-driven language designed for coordinating sensors, actuators, and reactive control loops on fast underlying code. Sits above C/C++ control loops the way Lua sits above game engines: handles concurrency, time, events, and cancellation as first-class primitives instead of patterns the developer has to construct by hand.

**Status:** M8 part 2 — networked REPL service. NDJSON line-protocol REPL over pluggable transports (TCP / Unix socket / UART / pty / in-process), 9 introspection ops + `Debug` urbiscript namespace, bearer-token auth with per-source rate-limiting, per-realm output writer + compile-budget, `Global` mutable shared atom (15 builtin atom protos marked readonly), `share/urbi/lobby.u` overlay (`echo` / `wall` / `handleDisconnect` / `Lobby.lobbies`), and three host binaries (`urbi`, `urbi-server`, `urbi-send`). Opt-in via `URBI_ENABLE_REPL=1`. Builds on the v0.9.0 realm-per-session lobby model, M6 stdlib (atoms, containers, runtime primitives), M5 reactive runtime (`at` / `whenever` / `waituntil` / `every` / tag-scope `enter` / `leave`), M4 prototype object model + GC, M3 cooperative scheduler + incremental tri-color GC (sub-3 µs pause), parallel separators (`,` / `&`), exception unwind, realm/namespace chunk lifecycle, ISR-safe event ring, and determinism infrastructure. Bytecode wire format v1.7, ABI 0/12/0. **1981 unit cases / 14011 checks** (URBI_ENABLE_REPL=1) passing at release / debug / ASan / UBSan / valgrind / cross-ARM / cross-RISC-V / stress / GC-none / 3-preset × 100-run determinism. Strict-tooling all-categories hard-gated. Tagged `v0.9.1-repl-service`.

## Design goals

- Pure C99, single library, zero external dependencies
- Builds with `cc src/*.c && ar` — no CMake, no autotools, no bootstrap
- Target footprint: < 400 KB flash on Cortex-M class MCUs
- Host-pluggable allocator, I/O sink, time source, panic handler
- No global state — multiple VM instances coexist, fully isolated
- Bytecode / source split: embedded targets can omit the compiler
- BSD-3-Clause throughout

## Target platforms

| Platform | Architecture | RTOS / environment |
|---|---|---|
| Linux dev workstation | x86_64 | host toolchain |
| STM32H7 (Discovery, Nucleo) | ARM Cortex-M7 | FreeRTOS or bare-metal |
| ESP32-C3 | RISC-V 32-bit (IMC) | ESP-IDF + FreeRTOS |
| ESP32-S3 (bonus) | Xtensa LX7 | ESP-IDF + FreeRTOS |

## Build

```sh
make
```

Produces `build/host/liburbi.a`. All build variants (release, debug, sanitizers, cross-compiles) land in `build/<TARGET>/` subtrees — see `CONTRIBUTING.md` for the full list. The public API exposes 346+ symbols spread across `<urbi/types.h>`, `<urbi/urbi.h>`, `<urbi/gc.h>`, `<urbi/sched.h>`, and `<urbi/object.h>` — VM lifecycle, module / chunk loading, strand spawn / step driver, ISR-safe event injection, realm globals, GC primitives, and the M4 object surface. The headers are self-contained: external consumers using only `-Iinclude` resolve cleanly without internal includes. The full embedding contract (host integration patterns, FreeRTOS pattern, Standard Robotics API) is formalised at the C API milestone.

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
include/urbi/urbi.h  public top-level C API (umbrella)
include/urbi/gc.h    public GC C API
include/urbi/sched.h public scheduler C API
src/urbi.c        top-level glue, version
src/ulex.h        lexer API
src/ulex.c        lexer
src/uast.h        AST node types
src/uarena.h      arena allocator API
src/uarena.c      chunk-list bump allocator (hosted / pluggable / static)
src/uparse.h      parser API
src/uparse.c      streaming Pratt-style parser
src/umodule.h     UModule struct, UValue, opcodes, instruction helpers
src/umodule.c     module deserializer + verifier
src/uemit.h       emitter API
src/uemit.c       single-pass emitter + disassembler + serializer
src/uvm.h         VM API
src/uvm.c         register-based interpreter (computed-goto + switch)
src/uvalue.h      UValue-to-string formatter API
src/uvalue.c      formatter implementation (hosted only)
tools/urbi.c      REPL binary — wires the pipeline end-to-end
tools/linenoise.h vendored line editor header
tools/linenoise.c vendored line editor implementation
```

Lua-style flat layout. Copy `src/*` into a host project and build.
The `tools/` directory contains the REPL binary and vendored linenoise;
neither is part of `liburbi.a`.

## Documentation

- `CONTRIBUTING.md` — build, test, cross-compile, and contribution how-tos
- `docs/STYLE.md` — code-level style decisions (naming, const-correctness, error model, initialization, headers, tests)

## License

BSD-3-Clause. See `LICENSE`.
