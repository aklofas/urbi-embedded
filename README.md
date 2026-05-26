# urbi-embedded

![ci](https://github.com/aklofas/urbi-embedded/actions/workflows/ci.yml/badge.svg)

An embeddable orchestration scripting language for robotics and physical systems, in pure C99.

Implements **urbiscript** — a prototype-based, parallel-by-default, event-driven language designed for coordinating sensors, actuators, and reactive control loops on fast underlying code. Sits above C/C++ control loops the way Lua sits above game engines: handles concurrency, time, events, and cancellation as first-class primitives instead of patterns the developer has to construct by hand.

**Status:** v0.10.4-vm-decomp — Wave 5 of the v0.10.x architectural refactor arc: behaviour-preserving decomposition of the VM monolith. ABI 0/16/0 (was 0/15/0 — 14th use of pre-v1.0 escape clause; struct UVM size shift visible to embedders calling `urbi_vm_sizeof()` per W1 of v0.10.3). Wire format unchanged at wire v1.9 / 0x19 (internal-architecture-only wave). W1 extracts slot-access helpers from `src/vm/uvm.c` (2040 → 1728 lines) into new internal TUs `src/vm/uvm_slot.{h,c}` + `src/vm/uvm_self.c` (`vm_resolve_ic`, `vm_trace_slot_read_if_needed`, `vm_getslot_value`, `vm_dispatch_getter`, `vm_setslot_value`, `vm_dispatch_setter`, `vm_self_lookup`); OP_GETSLOT / OP_SETSLOT / OP_SELF dispatch arms shrink from 162/172/132 to 43/60/48 lines (decode → call helper → branch on UVmSlotResult). LOCAL-slot discipline (recv-specific `slots[]` cache vs `slot_idx[]` re-resolution; OBJ-IC-POLY pin) lives in `vm_resolve_ic` exclusively. 6 new OBJ-IC-POLY regression tests in `tests/unit/test_vm_slot_helpers.c`. W2 extracts UWatcherState off UVM root: 10 watcher-related fields (`watcher_active_count`, `watcher_dirty_count`, `watcher_pool_*` 5 fields, `in_watcher_eval`/`_scratch`/`_install`) move from `struct UVM` into new `struct UWatcherState` in `src/watcher/uwatcher_state.{h,c}`. UVM gains `struct UWatcherState *watchers` allocated at `urbi_vm_init`; ~79 source-tree + ~131 test-tree callsites swept. `active_watchers_head` deliberately retained on UVM (GC walker + drain loop hot path). W3 extracts UReplState + UTestHooks off UVM root: `vm->repl_server` becomes `vm->repl` of type `struct UReplState *` (currently 1 field; structured for future expansion); 4 test-hook function pointers (`test_watcher_condition_hook`/`_fire_hook`/`_onleave_hook`/`test_install_cond_hook`) become `vm->test_hooks` of type `struct UTestHooks *`. Internal-only refactor — no public-API changes, no behaviour changes, no signature changes. Releasetest 26/26 gates green; 1970 unit cases (+6 from W1 OBJ-IC-POLY pins); ASan + UBSan + valgrind clean. Tagged `v0.10.4-vm-decomp`.

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
