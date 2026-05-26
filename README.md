# urbi-embedded

![ci](https://github.com/aklofas/urbi-embedded/actions/workflows/ci.yml/badge.svg)

An embeddable orchestration scripting language for robotics and physical systems, in pure C99.

Implements **urbiscript** — a prototype-based, parallel-by-default, event-driven language designed for coordinating sensors, actuators, and reactive control loops on fast underlying code. Sits above C/C++ control loops the way Lua sits above game engines: handles concurrency, time, events, and cancellation as first-class primitives instead of patterns the developer has to construct by hand.

**Status:** v0.10.5-legacy-decisions — Wave 6 of the v0.10.x architectural refactor arc: per-construct decisions on every legacy-language compatibility gap. ABI 0/17/0 (was 0/16/0 — 15th use of pre-v1.0 escape clause; MINOR bump for new public surface: 5 new tokens, 6+ new AST node kinds, 4+ new parser entry points). Wire format unchanged at wire v1.9 / 0x19 (no opcode additions — all new constructs lowered to existing opcodes via stdlib calls or jmp patterns). 11 worktrees:

- **W1 — control flow.** `for (var x : iter)` / `for (var x in iter)`, `break`, `continue`, `switch` IMPLEMENTED (lowered to OP_JMP with loop-context patch-lists; no new opcodes). C-style `for (init; cond; step)`, `loop`, `do (recv)` DEFERRED-v1.x with migration recipe.
- **W2 — quoted identifiers.** `'+'` / `'()'` / quoted-keyword slot access IMPLEMENTED; lexer adds `scan_quoted_ident` emitting `TOK_IDENT` with body pointer.
- **W3 — assert keyword.** `assert(expr)` + `assert { block }` IMPLEMENTED, lowered to `if-throw` (no new opcode).
- **W4 — angle literals.** `180deg` / `1rad` / `200grad` numeric suffixes producing `TOK_FLOAT` (radians) IMPLEMENTED via `apply_angle_suffix` (divide-then-multiply for exact IEEE-754 multiples of π). `Math.pi` already shipped as named constant. Physical literals DEFERRED-v1.x.
- **W5 — exception syntax.** `catch (var e)` + `catch (var e if guard)` + `try-catch-else` all IMPLEMENTED (parse_try extension + emit guard / else paths).
- **W6 — block comments.** Non-nesting DROPPED-LOCKED (intentional divergence from legacy; manual-unwind migration recipe at `docs/migration/block-comments-migration.md`).
- **W7 — CallMessage migration.** Permanently DROPPED (introspection removed at object-model level; incompatible with cooperative single-VM-per-thread scheduler + embedded heap budgets). 278-line migration design at `docs/migration/callmessage-migration.md`. `PORT_NOTES.md` walked end-to-end; stale "no float literals" / "no closure upvalue capture" / "no multi-slot class bodies" claims removed.
- **W8 — tag-expr widening.** `Tag.scope: body` (member-expr tag) IMPLEMENTED. `tag: body onleave handler` DEFERRED-v1.x. (Wave 3 W5 already closed bare-statement form.)
- **W9 — reactive surface.** `at (e?(var x)) body`, `whenever (e?(var x)) body` (payload binding), `whenever-else` (falling-edge), `waituntil (e?)` IMPLEMENTED. `at (cond ~ duration)`, `watch(expr)`, `$wheneverOn/$wheneverOff` DEFERRED-v1.x.
- **W10 — list/dict literals + subscript + slot-install.** `[1,2,3]`, `["a" => 1]`, `l[i]`, `l[i] = v`, `l[i] += v`, `var obj.slot = v` all IMPLEMENTED via stdlib-call lowering (`List.new` + `.append`, `Dict.new` + `.set`, `.get` / `.set` for subscript). 4 new tokens (`[`, `]`, `=>`, `+=`); 4 new AST kinds.
- **W11 — top-level `this` / Lobby.** MIGRATED to `Realm` (already-shipped per-realm global singleton since v0.9.0). Migration recipe at `docs/migration/top-level-this-lobby-migration.md`.

`docs/language-compatibility-matrix.md` fully populated; v1.0 conformance denominator now computable. Releasetest 26/26 gates green; 269 fixtures × 2 sanitizers = 538 runs clean; 1970 unit cases. Tagged `v0.10.5-legacy-decisions`.

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
