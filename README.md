# urbi-embedded

![ci](https://github.com/aklofas/urbi-embedded/actions/workflows/ci.yml/badge.svg)

An embeddable orchestration scripting language for robotics and physical systems, in pure C99.

Implements **urbiscript** — a prototype-based, parallel-by-default, event-driven language designed for coordinating sensors, actuators, and reactive control loops on fast underlying code. Sits above C/C++ control loops the way Lua sits above game engines: handles concurrency, time, events, and cancellation as first-class primitives instead of patterns the developer has to construct by hand.

**Status:** walking skeleton. Lexer, parser, arena allocator, bytecode emitter, module loader + verifier, register-based VM, interactive REPL, and `.chk` conformance-fixture runner are complete. 314 unit tests + 30 REPL integration cases + the first `.chk` fixture (`arithmetic.chk`) all passing at release / debug / ASan / UBSan / switch-dispatch / valgrind / cross-ARM / cross-RISC-V. Tagged `v0.1.0-skeleton`.

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

Produces `build/host/liburbi.a`. All build variants (release, debug, sanitizers, cross-compiles) land in `build/<TARGET>/` subtrees — see `CONTRIBUTING.md` for the full list. Public API currently exposes `urbi_version()`; compiler-internal module surfaces (lexer, parser, arena, module, emitter) are stable within the library but not yet re-exported through `urbi.h`. The host embedding API arrives at the C API milestone.

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

## Source layout

```text
src/urbi.h        public top-level C API
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
