# urbi-embedded

![ci](https://github.com/aklofas/urbi-embedded/actions/workflows/ci.yml/badge.svg)

An embeddable orchestration scripting language for robotics and physical systems, in pure C99.

Implements **urbiscript** — a prototype-based, parallel-by-default, event-driven language designed for coordinating sensors, actuators, and reactive control loops on fast underlying code. Sits above C/C++ control loops the way Lua sits above game engines: handles concurrency, time, events, and cancellation as first-class primitives instead of patterns the developer has to construct by hand.

**Status:** tagged `v0.16.0-shell`, which builds the shell around the re-founded core: four generic cross-compile presets exercised in CI, a host-declared heap budget the collector paces against, a scheduler fix for a strand that survives an inner `stop` under an outer `block` or `freeze`, and a single-threaded `urbi-server` tool that takes over the eval service's TCP and Unix transports from the library. The language is intact: separators-encode-concurrency (`;` `|` `,` `&`), the reactive trio (`at` / `whenever` / `waituntil`), first-class tags with `stop` / `block` / `freeze`, prototype OOP, and `try` / `catch` / `finally`. The parser does every desugar (32 AST kinds, down from 49) and the emitter is a single-cursor design with pinned temporaries, writing a bytecode format (41 opcodes, an `EXTARG` prefix for slot sites above 255) that the unwinder walks directly to run `finally` bodies on both the normal path and a jump. 369 conformance fixtures pass against it with none failing. Measured on this build: a booted VM costs 67,702 bytes on a 64-bit host, an idle strand 616 bytes, and a ten-thousand-iteration loop gives every byte back. The ROS2 bridge and every hardware board are parked and return at their own tag; the networked eval service returns in this one, as the separate tool below. ABI 0/27/0; wire v2.0 / 0x20.

## 30-second quickstart

```sh
make                                     # build liburbi.a + the urbi binary
echo "1 + 2" | ./build/host/urbi -i      # -> [..........] 3
./build/host/urbi -i                     # interactive REPL
```

Embedding a VM in your own C program is one header and a handful of calls — the [embedding guide](docs/embedding-guide.md) walks a complete program, and the build compiles every sample on that page and runs the complete one. The MCU ports (Pico, ESP32-S3, STM32F4) are parked while the core settles; [the ports guide](docs/internals/ports.md) records what each one needed.

## Design goals

- Pure C99, single library, zero external dependencies
- Builds with `make` — no CMake, no autotools, no bootstrap
- Target footprint: under 400 KB of flash on Cortex-M class MCUs
- Host-pluggable allocator, I/O sink, time source, panic handler
- No global state — multiple VM instances coexist, fully isolated
- Bytecode / source split: embedded targets can omit the compiler
- BSD-3-Clause throughout

## Supported targets

| Target | Status | Note |
|---|---|---|
| Linux x86_64 (host) | shipped | the canonical development target; the whole CI matrix runs here |
| Raspberry Pi Pico (RP2040 / Cortex-M0+) | parked | brought up and hardware-validated against the previous core; re-attached at its own tag |
| ESP32-S3 (Xtensa LX7) | parked | brought up and hardware-validated (eye_demo); re-attached at its own tag |
| STM32F4 (Cortex-M4F) | parked | brought up and hardware-validated (Mandelbrot demo); re-attached at its own tag |
| ARM Cortex-M7 (generic) | archive build (CI) | cross-compiled, archive-gated, and footprint-measured on every push; no board attached |
| RISC-V rv32imc (generic) | archive build (CI) | cross-compiled, archive-gated, and footprint-measured on every push; no board attached |

Cortex-M7 and RISC-V are generic silicon with no board behind them: four
presets (`arm-cortex-m0plus`, `arm-cortex-m4f`, `arm-cortex-m7`,
`riscv32`), each built full and bytecode-only, compile clean and pass
the freestanding-archive gate on every push — see
[the build system doc](docs/internals/build-system.md) for the preset
names and shapes. The `arm-cortex-m4f` preset also boots and runs under qemu's
Cortex-M4 model, where a booted VM now measures 48,980 bytes and an idle
strand 466 bytes: the 32-bit figure this page used to promise for later
is measured for real. The three real boards above were brought up and
hardware-validated against a runtime this core has since replaced; none
has been rebuilt on the new one yet, and each returns at its own tag.

## Build

```sh
make
```

Produces `build/host/liburbi.a`. Every build variant (debug, sanitizers, coverage) lands in its own `build/<TARGET>/` subtree — see `CONTRIBUTING.md` for the list.

The public API is 44 functions in `<urbi/urbi.h>`, with values in `<urbi/types.h>`, version macros in `<urbi/version.h>` and the optional eval service in `<urbi/repl.h>`. The headers are self-contained: `-Iinclude` is the whole include path an embedder needs. `docs/embedding-guide.md` is the contract.

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

## Eval service

An NDJSON line protocol over a byte stream the host supplies: one JSON
request per line in, result and output envelopes out. Each connected
stream is one session with its own realm, its own globals and its own
output, so two clients cannot see each other's variables and neither
sees the other's `echo`.

It is cooperative. Nothing starts a thread and nothing opens a socket:
the host calls `urbi_repl_serve_step` from the same loop it calls
`urbi_step` from, which is what makes it usable on a microcontroller and
what keeps every VM touch on one thread. It is in every build the
compiler frontend is in.

See `docs/embedding-guide.md` for the calls and
`docs/internals/repl-service.md` for the protocol.

```json
> {"id":1,"op":"eval","code":"echo(1+2)"}
< {"id":1,"kind":"output","channel":"clog","msg":"[00000000] *** 3\n"}
< {"id":1,"kind":"result","value":"nil"}
< {"id":1,"kind":"done"}
```

Two ops: `eval` and `introspect`. The compile budget in the config is
applied to every session's realm and caps source bytes, parser depth and
AST nodes, because the text arrives from outside.

The networked half lives outside the library now, as the `urbi-server`
tool: one `poll()` loop, no threads, `--tcp HOST:PORT` and/or `--unix
PATH`, an optional `--token` bearer check it speaks itself, and a
`--max-clients` cap, with each accepted socket registered as the same
`UTransport` the cooperative core above already uses. `make urbi-server`
builds it; `urbi-send` is its command-line client.

## Source layout

```text
include/urbi/   the public API: urbi.h, types.h, version.h, repl.h, require.h
src/
├── lex/        lexer
├── parse/      parser and AST
├── emit/       emitter, disassembler, serializer, and the one compile entry point
├── chunk/      the bytecode container: writer, loader, verifier, UProto
├── util/       shared by the frontend: AST arena, varint codec, freestanding helpers
├── rt/         the runtime — see docs/internals/runtime.md
├── stdlib/     built-in methods in C, plus stdlib.u baked to bytecode
├── host/       public API whose implementation needs libc (the value formatter)
├── repl/       the cooperative NDJSON eval service; the networked server is tools/urbi-server.c
├── ros/        parked; returns at its own tag
└── urobotics/  parked; returns at its own tag
tools/          host binaries (urbi, urbi-server, urbi-send, the stdlib bake tool) + vendored linenoise
```

`src/rt/` holds to a strict include order and a freestanding rule —
no libc beyond five headers — which is what lets the same source build
for a microcontroller. `tests/scripts/check_rt_layering.sh` enforces
both. The `tools/` directory is not part of `liburbi.a`.

## Documentation

- [`docs/embedding-guide.md`](docs/embedding-guide.md) — the C API, with a complete worked program
- [`docs/internals/runtime.md`](docs/internals/runtime.md) — how the runtime works
- [`docs/internals/architecture.md`](docs/internals/architecture.md) — how the compiler works
- `CONTRIBUTING.md` — build, test, and contribution how-tos
- `docs/STYLE.md` — naming, const-correctness, error model, initialization, headers, tests

## License

BSD-3-Clause. See `LICENSE`.
