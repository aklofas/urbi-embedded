# urbi-embedded

![ci](https://github.com/aklofas/urbi-embedded/actions/workflows/ci.yml/badge.svg)

An embeddable orchestration scripting language for robotics and physical systems, in pure C99.

Implements **urbiscript** — a prototype-based, parallel-by-default, event-driven language designed for coordinating sensors, actuators, and reactive control loops on fast underlying code. Sits above C/C++ control loops the way Lua sits above game engines: handles concurrency, time, events, and cancellation as first-class primitives instead of patterns the developer has to construct by hand.

**Status:** tagged `v0.15.1-core-hardening`, a hardening pass over the previous tag (eleven defects found by a post-ship review of the runtime core, each fixed with a regression test) — the compiler frontend has been rebuilt on top of the re-founded runtime core, so there is no pre-1.0 compatibility promise and the numbers below are current-build facts, not commitments. The language is intact: separators-encode-concurrency (`;` `|` `,` `&`), the reactive trio (`at` / `whenever` / `waituntil`), first-class tags with `stop` / `block` / `freeze`, prototype OOP, and `try` / `catch` / `finally`. The parser now does every desugar (32 AST kinds, down from 49) and the emitter is a single-cursor design with pinned temporaries, writing a new bytecode format (41 opcodes, an `EXTARG` prefix for slot sites above 255) that the unwinder walks directly to run `finally` bodies on both the normal path and a jump. 351 conformance fixtures pass against it with none failing. Measured on this build: a booted VM costs 67,642 bytes on a 64-bit host, an idle strand 616 bytes, and a ten-thousand-iteration loop gives every byte back. The ROS2 bridge, the networked REPL server and every hardware port are parked and return in a later phase. ABI 0/26/0; wire v2.0 / 0x20.

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
| Raspberry Pi Pico (RP2040 / Cortex-M0+) | parked | brought up and hardware-validated against the previous core |
| ESP32-S3 (Xtensa LX7) | parked | brought up and hardware-validated (eye_demo) |
| STM32F4 (Cortex-M4F) | parked | brought up and hardware-validated (Mandelbrot demo) |
| ARM Cortex-M7 (generic) | parked | archive build only |
| RISC-V rv32imc (generic) | parked | archive build only |

Every cross target is parked: the runtime they were brought up against
has been replaced, and none has been rebuilt on the new one. Phase 5
re-attaches them, and that is when the 32-bit footprint figure gets
measured for real.

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

The networked server — a listener, per-connection threads, bearer-token
auth, rate limiting and the TCP / Unix / UART transports — is not in this
build. It returns in a later phase, on the same `UTransport` vtable.

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
├── repl/       the cooperative NDJSON eval service; the networked server is parked
├── ros/        parked until Phase 5
└── urobotics/  parked until Phase 5
tools/          host binaries (urbi, the stdlib bake tool) + vendored linenoise
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
