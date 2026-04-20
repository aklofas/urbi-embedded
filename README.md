# urbi-embedded

![ci](https://github.com/aklofas/urbi-embedded/actions/workflows/ci.yml/badge.svg)

An embeddable orchestration scripting language for robotics and physical systems, in pure C99.

Implements **urbiscript** — a prototype-based, parallel-by-default, event-driven language designed for coordinating sensors, actuators, and reactive control loops on fast underlying code. Sits above C/C++ control loops the way Lua sits above game engines: handles concurrency, time, events, and cancellation as first-class primitives instead of patterns the developer has to construct by hand.

**Status:** pre-release skeleton. Nothing implemented yet.

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

Produces `build/host/liburbi.a`. All build variants (release, debug, sanitizers, cross-compiles) land in `build/<TARGET>/` subtrees — see `CONTRIBUTING.md` for the full list. Skeleton currently exports only `urbi_version()`; the rest fills in across successive release milestones.

## Source layout

```
src/urbi.h        public C API
src/urbi.c        top-level glue, version
src/ulex.c        lexer
src/uparse.c      parser
src/udesugar.c    desugaring pass
src/uemit.c       bytecode emitter
src/uvm.c         bytecode interpreter
```

Lua-style flat layout. Copy `src/*` into a host project and build.

## License

BSD-3-Clause. See `LICENSE`.
