# Changelog

## v0.9.4-pico-example — 2026-05-24

### Added

- **Raspberry Pi Pico bring-up** on RP2040 (Cortex-M0+ / ARMv6-M /
  264 KB SRAM / no FPU / no integer divide). Cross-build target
  `cross-pico` plus bytecode-only variant `cross-pico-bytecode-only`,
  freestanding signature golden gate
  `test-cross-pico-freestanding-golden`, and GHA job `cross-pico`.
  `releasetest` auto-detects cross-pico when `arm-none-eabi-gcc`
  probe-compile passes. Hardware validation completed 2026-05-24.
- **USB CDC REPL transport** for Pi Pico
  (`src/repl/urepl_transport_usb_cdc_pico.c`,
  `UREPL_USB_CDC_PICO_TRANSPORT`). TinyUSB CDC, single-host,
  non-pollable; drives the cooperative `urbi_repl_serve_step` data
  plane from the embedder's main loop.
- **Cooperative `urbi_repl_serve_step` data plane.** Stub at v0.9.1 →
  fully wired four-phase non-blocking sweep (accept / read / write /
  close) for transports whose `pollable_fd_fn` returns `-1`. Mixed
  mode (TCP via pthread + USB CDC via cooperative) supported in one
  process. `spawn_reader` skips `pthread_create` for non-pollable
  transports (Pico has no pthread).
- **`every (period) body` language feature.** Lexer keyword
  `TOK_KW_EVERY`, parser sugar (body wrapped to zero-argument lambda),
  and stdlib C-native `every()` driven by the scheduler sleep-queue
  directly. No new opcodes; no wire-format change. Two new `.chk`
  fixtures (`tests/chk/reactive/every/basic.chk`,
  `tests/chk/reactive/every/nested.chk` — the nested case is a pinned
  regression target for the closure-in-body restriction filed
  against v1.x).
- **`examples/pico/repl_demo/`** ships a CMake-native interactive demo:
  onboard LED + on-die ADC4 temperature + BOOTSEL button +
  TIMER_IRQ_0 100 ms tick, exercised from urbiscript with the
  cooperative REPL driving USB CDC.
- **`docs/internals/ports.md`** + **`docs/reference/embedded-port-sources.md`**.
  Per-silicon footprint + idiosyncrasy reference; pinned upstream SDK
  tags (esp-idf v6.0.1 / stm32cube-f4 v1.28.2 / pico-sdk 2.2.0).

### Changed

- **`urbi_repl_serve_step` semantics.** No-op stub → real cooperative
  driver. Signature unchanged; embedders that previously called it
  expecting a no-op now get the data-plane sweep. The pthread listener
  short-circuits when no registered transport has a kernel-pollable
  listener fd (`src/repl/urepl_listener.c:634-648`).
- **`.chk` corpus** grows from 239 → 241 (basic + nested every fixtures);
  unit cases unchanged (BSP code is host-empty).
- **`docs/embedding-guide.md`** §4 documents `every (period) body`; §12
  documents `UREPL_USB_CDC_PICO_TRANSPORT`.
- **`docs/internals/repl-service.md`** documents the cooperative
  `urbi_repl_serve_step` data plane next to the pthread-listener model.

### Cooperative-only REPL build path

New build flag `URBI_REPL_COOPERATIVE_ONLY=1` (paired with
`URBI_ENABLE_REPL=1`) compiles the REPL service without
`<pthread.h>` / `<sys/eventfd.h>` / `<sys/socket.h>`. Used by
`make cross-pico` (auto-paired) and the new `make cross-pico-repl`
target. New typedef shim `src/repl/urepl_threading.h` introduces
`urbi_mutex_t` / `urbi_cond_t` / `urbi_thread_t` + `UREPL_*` macros
that no-op on cooperative builds and resolve to pthread on POSIX.
Embedders MUST link with matching `URBI_REPL_COOPERATIVE_ONLY`
setting (struct layouts of UReplServer / UReplReader / UReplQueue /
UReplRingbuf differ; same trap class as `URBI_FLOAT_TYPE`).

Filtered TUs on cooperative builds (not compiled):
`urepl_transport_tcp.c`, `urepl_transport_unix.c`,
`urepl_transport_pty.c`, `urepl_auth.c`.

`urepl_listener.c` is kept in the build (it hosts the cooperative
`urepl_*_sweep` functions used by `urbi_repl_serve_step`) but its
POSIX-only sections are guarded with `#ifndef URBI_REPL_COOPERATIVE_ONLY`:
eventfd helpers, listener_main thread, reader_main thread,
full spawn_reader, drain_accepts / wake_all_readers implementations.
Cooperative stubs preserve the dispatcher's unchanged call surface.

`urbi_repl_serve` (threaded entry point) is not exposed on cooperative
builds — its body is filtered out via the dropped auth / transport TUs
and listener threads. Use `urbi_repl_serve_init` +
`urbi_repl_serve_step` instead, driven from the embedder's main loop.

`dispatch_auth` auto-approves on cooperative builds. Freestanding
embedded targets (USB CDC, UART) have no network threat model;
token-based auth is meaningful only on socket transports which are not
compiled in on cooperative builds. Embedders bringing up custom
cooperative transports with network exposure should re-evaluate (v1.x:
add an opt-in `cooperative_auth_token` knob — filed in design-risks).

CI: new `cross-pico-repl` GHA job builds `liburbi.a` +
`examples/pico/repl_demo/build/repl_demo.elf` end-to-end. Phase 0 of
`make releasetest` includes `cross-pico-repl` whenever
`arm-none-eabi-gcc` is present.

### Changed (ABI)

- **ABI** bumped `0/13/0` → `0/14/0` (10th pre-v1.0 escape clause;
  internal struct layouts of UReplServer / UReplReader / UReplQueue /
  UReplRingbuf differ between cooperative and POSIX modes).
  **Wire format** stays at `v1.8 / 0x18`.

### Footprint

`arm-none-eabi-size build/.../liburbi.a` at v0.9.4 (xpack
`arm-none-eabi-gcc` 14.2.1 @ `-Os`, calibrated 2026-05-24 from
`7fbb17d`):

- cross-pico (Cortex-M0+) full: 114,713 B text (88.2% of 130 KB cap)
- cross-pico bytecode-only: 82,599 B text (84.9% of 95 KB cap)

Caps locked at full **130 KB** / bytecode-only **95 KB** — 10 KB
above M4F/M7 to absorb the M0+ libgcc-helper floor (no FPU, no
integer divide, no LDREX/STREX, so every float op, every `/`/`%`,
and every atomic resolves through `__aeabi_*` / `__atomic_*`
helpers). Per S35 cap-revision policy (S43).

### Why

The Pi Pico is the first non-FPU, no-divide silicon the runtime has
been brought up on, and the first port whose primary REPL transport
is USB CDC rather than UART or TCP — both factors push the cooperative
`urbi_repl_serve_step` path from "stub for completeness" to "the
only path that exists on this hardware." The `every()` language
feature was a long-standing v1.0 emit gap (legacy stdlib defined it;
v0.6.2 left it stubbed); bringing up the Pico's BSP demo workload
made closing the gap a hard prerequisite. v0.9.3's freestanding-host
gate caught one cross-pico variant of the snprintf-leak class before
the port shipped.

See `docs/milestones/v0.9.4-pico-example.md` for the full
retrospective.

### Hardware bring-up — what shipped vs deferred

Hardware bring-up on a real Raspberry Pi Pico completed 2026-05-24.
Two findings were filed as v1.x deferrals (see design-risks register) and the demo was adapted to
ship within the constraints:

**Working end-to-end on real silicon:**

- urbi VM boot on Cortex-M0+ (`urbi_vm_init` → `urbi_realm_global` →
  `urbi_stdlib_boot`).
- BSP integration: 5 host functions registered
  (`led_on`/`led_off`/`led_toggle`/`led_pwm`/`temp_celsius`/`button_pressed`),
  2 named events (`tick`, `pressed`).
- TIMER_IRQ_0 ISR fires at 10 Hz, polls BOOTSEL via the QSPI_SS
  bit-bang trick, debounces, and injects `pressed` events on rising
  edge via the ISR-safe `urbi_inject_event` single-producer ring.
- USB CDC enumerates (TinyUSB device-side); console output via
  `dbg_print` (dual-channel UART + CDC) surfaces every init step.
- C-side host watcher (`urbi_register_watcher` for the `pressed`
  event) fires on each BOOTSEL press → `gpio_xor` toggles the LED.

**Deferred to v1.x (filed in design-risks):**

- **REPL session model is too heavy for ~256 KB SRAM.** Per-session
  realm + lobby.u compile needs >50 KB on top of stdlib_boot's
  ~165 KB baseline; Pico's ~241 KB usable heap can't accommodate
  the first session. Cooperative `urbi_repl_serve_init` succeeds
  but `urbi_repl_serve_step` panics on first CDC connect. The
  `repl_demo` example ships with REPL service disabled and a
  C-side watcher providing the interactive surface.
- **`whenever (named_event) { body }` install doesn't dispatch on
  cooperative-only builds.** The watcher installs without error
  but its body never fires even on synthetic event injection.
  Workaround: install the equivalent watcher via the C-side
  `urbi_register_watcher` API, which works reliably.

The demo's `repl_demo.u` still installs `whenever (pressed)
{ led_toggle() }` as a documentation pattern, but the interactive
behavior is provided by `main.c`'s C-side watcher until the v1.x
fix lands. Press BOOTSEL on the Pico → LED toggles.

## v0.9.3-ci-hardening — 2026-05-19

### Added

- **`test-freestanding-host` Makefile target.** Host-`cc`-based per-TU
  compile under `-ffreestanding -DURBI_BYTECODE_ONLY=1` + nm-grep
  against the forbidden-libc symbol regex
  (`printf` / `snprintf` / `malloc` / `free` / `strtod` / `strtol` / `abort` / `exit` / …).
  Catches the freestanding-leak class without any cross-toolchain
  dependency. Added to `RELEASETEST_PHASE1` (20 → 21 gates).
- **Cross-toolchain auto-detect in `releasetest`.** Probes for
  `arm-none-eabi-gcc`, `riscv-none-elf-gcc`, and `xtensa-esp-elf-gcc`
  at startup via probe-compile (try-compile `#include <string.h>`,
  not just `command -v`). When `present`, runs the corresponding
  cross-build + `test-freestanding.sh` archive check in a sequential
  Phase 0 before Phase 1. When `broken` (binary on PATH but sysroot
  missing) or `absent` (binary not on PATH), skips the cross gates
  with a contextual banner pointing at `docs/cross-toolchain-setup.md`.
- **`docs/cross-toolchain-setup.md`.** Documents the xpack toolchain
  install recipe and the `riscv-none-elf-*` → `riscv64-unknown-elf-*`
  symlink rule. Referenced from the releasetest banner.
- **Sourceable shell helpers** (`tests/scripts/_bytecode-only-tus.sh`,
  `tests/scripts/_freestanding-forbidden.sh`). Single source of truth
  for the URBI_BYTECODE_ONLY TU keep-list and the forbidden-libc
  regex, shared between the host-side gate and the existing cross
  archive check.

### Changed

- **`tests/scripts/build-bytecode-only.sh`** now sources the keep-list
  from `_bytecode-only-tus.sh` instead of inlining it.
- **`tests/scripts/test-freestanding.sh`** now sources the forbidden
  regex from `_freestanding-forbidden.sh` instead of inlining it.
- **Bytecode-only keep-list** no longer includes `src/urbi_aux.c`.
  The optional `liburbi_aux.a` convenience layer uses `<stdio.h>` by
  design and is already skipped by cross-build freestanding targets
  (commit `3a9e939`); the host-side keep-list now matches.

### Why

Two consecutive ships (`v0.9.1-repl-service`, `v0.9.2-uproto-only`)
merged with `make releasetest` 22/22 green but turned GHA CI red on
`cross-stm32f4-bytecode-only` + `cross-esp32s3-bytecode-only`
(`builtin_lobby_send` used `snprintf` unconditionally). Releasetest
did not exercise the freestanding-clean property at all on the dev
box — cross-arm/cross-riscv toolchains shipped broken sysroots and
cross-esp32s3 wasn't in the target list. This tag closes the gate
before the next port (Pico) lands and surfaces another instance of
the same leak class.

No ABI / wire / corpus changes. ABI stays `0/13/0`, wire stays `v1.8`.

## v0.9.2-uproto-only — 2026-05-19 (Approach C: UModule removed)

### Changed

- **Type model.** `UModule` struct removed entirely. `UProto` absorbs the root-only metadata fields (`source_name`, `origin_vm`, `next_proto_serial`, `total_proto_count`, `next_in_realm`, `owning_realm`, `heap_allocated`). Every chunk submitted to the runtime is now a `UProto*` (root proto). Closes canonical trajectory step N+3.
- **Public API.** 7 functions renamed: `urbi_load_module` → `urbi_load_chunk`, `urbi_module_from_bytes` → `urbi_chunk_from_bytes`, `urbi_module_free` → `urbi_chunk_free`, `urbi_module_instance_{create,destroy}` → `urbi_chunk_instance_{create,destroy}`, `urbi_get_or_create_module_instance` → `urbi_get_or_create_chunk_instance`, `urbi_load_translate_load_err` → `urbi_chunk_translate_load_err`. Argument types: any `UModule*` parameter is now `UProto*`. Six functions keep their names (`urbi_run_chunk`, `urbi_run_script`, `urbi_unload`) but change argument type.
- **Wire format v1.7 → v1.8.** Semantic bump only — byte layout unchanged (Task 4.1's cliff preserved v1.7 byte ordering; the spec §4.1 description of a separate UModule-header section was imprecise). v1.7 bytecode rejected with `UCHUNK_LOAD_UNSUPPORTED_VERSION`. Re-emit from source to migrate.
- **ABI 0/12/0 → 0/13/0** (9th use of pre-v1.0 escape clause).
- **Vocabulary.** Runtime nomenclature uses chunk + proto exclusively. "Module" retired as a runtime noun. See `docs/embedding-guide.md` "Vocabulary" section.

### Renamed (internal)

- `UModuleInstance` → `UChunkInstance`.
- `UModuleAllocFn` → `UChunkAllocFn` (note: spec originally proposed `UAllocFn` but that name was already taken by `src/value/uarena.h`).
- `UModuleLoadError` → `UChunkLoadError`; enum values `ULOAD_*` → `UCHUNK_LOAD_*`.
- `umodule_*` functions → `uproto_*` / `uchunk_*` (per uproto-only design §5.3).
- `src/module/` → `src/chunk/`. `umodule.h` split into `uproto.h` (type + per-proto ops) + `uchunk.h` (loader/IO API). Strand-driver files renamed `uchunk_strand.{c,h}` to disambiguate.
- `urbi-embedded/docs/internals/realm-and-modules.md` → `realm-and-chunks.md`.

### Removed

- `struct UModule` definition. The forward declaration `struct UModule;` in `include/urbi/types.h` and `include/urbi/urbi.h` is gone.
- `UStrand.module` field deleted; `s->root_proto` carries chunk identity.
- Embedder code that allocates `UModule` on the stack must switch to `urbi_chunk_from_bytes` (heap path) returning `UProto*`.

### Deferred

- `URootMeta` side-struct optimization (consolidate root-only fields, save ~1.5 KB per 50-nested module). v1.x; documented in `docs/urbi-embedded-design-risks.md`.
- Defensive assert in `uchunk_destroy_internal` for the unreachable `alloc==NULL && heap_allocated==true` state. v1.x latent footgun.

## v0.9.1-repl-service — 2026-05-19 (M8 part 2 of 2)

**Theme:** Network/protocol layer on top of the v0.9.0 realm-per-session
lobby foundation. NDJSON line-protocol REPL service over pluggable
transports (TCP / Unix socket / UART / pty / in-process buffer), 9
introspection C primitives + `Debug` urbiscript namespace, bearer-token
auth with per-source rate-limiting, per-realm output writer +
compile-budget guard, `Global` mutable shared atom (with 15 builtin
atom protos marked readonly), `share/urbi/lobby.u` overlay (`echo` /
`wall` / `handleDisconnect` / `Lobby.lobbies`), and three host binaries
(`urbi --listen`, `urbi-server`, `urbi-send`). Opt-in via
`URBI_ENABLE_REPL=1`.

**ABI:** `0/11/0` → `0/12/0` (8th pre-v1.0 escape-clause use — URealm
grows by ~24 B for the compile_budget + writer fields; new public
symbols; new error codes; UObject.flags gains `URBI_OBJ_FLAG_READONLY`).

**Wire format:** v1.7 / 0x17 unchanged.

**NDJSON protocol:** v0.9.1 schema (see `docs/internals/repl-service.md`);
locked forward to v1.0 (additive only).

**Corpus:** 1821 / 13342 → 1981 / 14011 (default URBI_ENABLE_REPL=0
build) / 1981 / 14011 (URBI_ENABLE_REPL=1 build); +15 `.chk` fixtures
under `tests/chk/repl/`.

### Added — Public C API

- **`urbi_repl_serve(vm, &cfg, *out_err)`** + **`urbi_repl_stop(server)`**
  — threaded mode lifecycle. Spawns the listener pthread + per-client
  reader subthreads on Linux; defaults to TCP loopback. Returns NULL
  with `URBI_ERR_INSECURE_CONFIG` for non-loopback binds without
  `auth_token`.
- **`urbi_repl_serve_init` / `_step(timeout_us)` / `_shutdown`** —
  step-driven mode for bare-metal / no-pthread hosts. One
  `serve_step` performs at most one accept + read + dispatch + write
  cycle across all registered transports.
- **`urbi_repl_register_transport(server, vtable, listener_state)`** —
  attach a `UTransport` (TCP / Unix / UART / pty / in-process) to the
  server. Multiple transports per server supported.
- **`UReplConfig` struct** + **`UReplServer` opaque handle** +
  **`UTransport` vtable** in `<urbi/repl.h>` (new public header).
- **`urbi_realm_set_writer(vm, realm, fn, ud)`** — per-realm output
  writer. Strands under `realm` route channel writes here first; if
  unset, falls back to the VM-wide writer installed via
  `urbi_set_writer`. Used by the REPL service to route per-session
  output to the originating client.
- **`urbi_realm_set_compile_budget(realm, &budget)` +
  `urbi_realm_get_compile_budget(realm)`** — install / inspect
  per-realm parser limits. `budget == NULL` clears (unlimited). Three
  limits per `UCompileBudget`:
  - `max_parser_depth` — recursive-descent stack ceiling (raises
    `URBI_ERR_COMPILE_BUDGET_DEPTH`).
  - `max_ast_nodes` — total `make_node` allocations per compile
    (`URBI_ERR_COMPILE_BUDGET_NODES`).
  - `max_source_bytes` — checked once at `urbi_repl_eval` entry
    (`URBI_ERR_COMPILE_BUDGET_SOURCE`).
- **`URBI_DEFAULT_REPL_BUDGET`** — exported constant
  (256 / 100000 / 1 MiB) auto-applied by `urbi_realm_create_repl`. The
  global realm has no budget by default (trusted host code).
- **`urbi_vm_write_in_realm(vm, realm, channel, ...)`** — new dispatch
  entry; consults the realm writer first, then the VM writer, then the
  built-in default. `urbi_vm_write` becomes a thin wrapper (realm=NULL).
- **`UCompileBudget` struct** in `<urbi/types.h>` (3× `uint32_t`).
- **`URBI_OBJ_FLAG_READONLY`** — new bit (bit 7) on `UObject.flags`.
  Public spelling `UPROTO_FLAG_READONLY`. `OP_SETSLOT` raises
  `TypeError: cannot mutate frozen prototype` when the receiver
  carries this bit. Host-side C API mutators are not gated — they
  populate protos before the bit is set.

### Added — Introspection primitives (`<urbi/repl.h>`)

- **9 introspection C primitives** in `src/repl/urepl_introspect.{c,h}`:
  `urbi_introspect_coros`, `_tags`, `_watchers`, `_events`, `_profile`,
  `_gc`, `_lobbies`, `_stack(coro_id)`, `_slots(realm, obj_path)`. Each
  walks VM linked lists on the MAIN thread and emits a single JSON
  object into a caller-provided buffer. Used by the NDJSON `introspect`
  op (dispatcher) and the urbiscript `Debug` namespace. Wire JSON
  shape locked forward to v1.0 (additive only).
- **`Debug` urbiscript namespace** — 9 C-native methods
  (`Debug.coros()`, `Debug.tags()`, ..., `Debug.stack(id)`,
  `Debug.slots(path)`) bound on each realm's `Global.Debug` slot. Each
  method calls the corresponding introspect primitive, validates the
  JSON via `ujson_parse`, and returns the JSON as a urbi `String`.
- **`vm->debug_proto`** (void\* in UVM, REPL-condition-free header).
  Single per-VM singleton; GC reachability via `object_roots_walker`.

### Added — Error codes (`<urbi/types.h>`)

- `URBI_ERR_FROZEN_PROTO` (-21) — `OP_SETSLOT` on a readonly atom proto.
- `URBI_ERR_COMPILE_BUDGET_DEPTH` (-22) — parser depth exceeded.
- `URBI_ERR_COMPILE_BUDGET_NODES` (-23) — AST-node allocation cap
  exceeded.
- `URBI_ERR_COMPILE_BUDGET_SOURCE` (-24) — source-byte cap exceeded.
- `URBI_ERR_INSECURE_CONFIG` (-25) — `urbi_repl_serve` refusing
  non-loopback bind without an `auth_token`.

### Added — NDJSON protocol (`src/repl/urepl_ndjson.{c,h}`)

- **Request parser** for client ops: `auth`, `eval`, `cancel`,
  `introspect`, `lobby_new`, `lobby_close`. Field extraction via the
  in-tree tiny JSON parser; bounded depth + node count to resist DoS.
- **Response emitter** for server kinds: `hello`, `auth_ok`, `result`,
  `output`, `done`, `error`, `event`, `goodbye`. Field-by-field escape
  coverage for all control characters + `\uXXXX` for non-ASCII bytes.
- **Framing:** one JSON document per line, `\n` terminator (`\r\n`
  accepted from client). 1 MiB max line length matches default
  `max_source_bytes`; longer lines trigger
  `{kind:"error", code:"frame_too_large"}` and connection close.
- **Synclines:** server wraps multi-line eval text with v0.9.0
  `//#push N "FILE"` / `//#pop` synclines so runtime errors report
  the client-supplied file:line rather than `<stdin>:N`.
- **Id correlation rules:** correlated responses echo the originating
  op's `id`. `output` from strands outliving an `eval` carries
  `lobby` + `channel` but no `id`.

### Added — Dispatcher + queues (`src/repl/urepl_dispatch.{c,h}`)

- **MPSC eval queue** in `src/repl/urepl_queue.{c,h}` — intrusive
  singly-linked list; mutex + condvar; pushed by reader subthreads,
  drained on the VM thread at every `urbi_step` boundary.
- **SPSC output ringbuf** — one per session, 64 KiB default (tunable
  via `UReplConfig.output_ringbuf_cap`). Lock-free; writer = VM thread,
  reader = reader subthread. Overflow drops oldest bytes and emits
  `{kind:"error", code:"output_overflow"}`.
- **Job dispatcher** routes each popped `UReplJob` to its handler
  (`dispatch_eval` / `dispatch_introspect` / `dispatch_cancel` /
  `dispatch_auth` / `dispatch_lobby_new` / `dispatch_lobby_close`).
  Output produced by the dispatched op flows through the session's
  per-realm writer back to the session's ringbuf.

### Added — Auth + transports

- **TCP transport** (`src/repl/urepl_transport_tcp.c`) — POSIX
  sockets, IPv4, non-blocking accept. Default port 54000.
- **In-process buffer transport** (`src/repl/urepl_buffer_transport.c`)
  — pure-memory loopback for unit tests.
- **Linux pty pair transport** (`src/repl/urepl_transport_pty.c`) —
  `openpty()`-backed; eager-accept pass on the listener; used by the
  CI UART harness.
- **Embedded UART transport stubs** — `urepl_transport_uart_freertos.c`
  / `_esp_idf.c` / `_pico.c` / `_linux.c` — each `#ifdef`-gated, thin
  pass-through to platform UART primitives. Single-client by design
  (no auth required — physical access is auth).
- **Listener pthread** (`src/repl/urepl_listener.{c,h}`) — accept loop
  on each registered transport; spawns per-client reader subthreads.
- **Bearer-token auth** (`src/repl/urepl_auth.{c,h}`) — constant-time
  token compare (`urepl_auth_constant_time_compare`); per-source IP /
  pid rate-limit (5 failed attempts within 30 s → 60 s lockout, LRU
  table of 8 entries).
- **Atomic shutdown flag** + session-create-on-VM-thread refactor
  (closes the embedded ABA between listener pthread create + VM-thread
  realm destroy).

### Added — Lobby stdlib (`src/stdlib/lobby.u`)

- **`Lobby` atom proto** with C-native `__builtin_lobby_send(msg, tag,
  prefix)` registered on `Object`. Formats
  `[%08u:%s] %s %s\n` (no `:tag` segment when tag is empty) and writes
  through `strand->realm->writer` → `vm->writer` → default-writer
  fallback. The existing `echo` global re-routes through this primitive
  so per-realm routing applies; backward-compat preserved for hosts
  using `urbi_set_writer`.
- **`Lobby.echo(msg, tag, prefix)`** — formatted output via the lobby's
  writer.
- **`Lobby.wall(msg, tag)`** — broadcast to every other lobby.
- **`Lobby.handleDisconnect()`** — hook called by the C dispatcher on
  session disconnect. Default fires the per-lobby `onDisconnect` event;
  user code may override.
- **`Lobby.onDisconnect`** — per-lobby `Event.new()`; fires once at
  disconnect.
- **`Lobby.lobbies`** — list of active lobbies maintained by the C
  dispatcher (mutated at session-create / destroy via
  `urbi_lobby_lobbies_push` / `_remove`). urbiscript can read but not
  reassign (slot protected by the `Lobby` proto's readonly bit).

### Added — Tiny JSON parser (`src/repl/ujson.{c,h}`)

- Recursive-descent JSON parser. Supports object, array, string (with
  `\uXXXX` and surrogate pairs), int / double, bool, null. DoS-bounded:
  `UJSON_MAX_DEPTH=32`, `UJSON_MAX_NODES=10000`, `UJSON_MAX_LEN=1 MiB`.
  Returns a `UJsonNode` tree freed via `ujson_free_node`. No external
  dependencies; not part of the public API.

### Added — Host binaries (`tools/`)

- **`urbi-server`** (`tools/urbi-server.c`) — headless network REPL
  server. Flags: `--bind ADDR` (default 127.0.0.1), `--port N` (default
  54000; 0 = kernel-assigned), `--token TOK` (or `URBI_REPL_TOKEN`
  env), `--max-clients N` (default 16), `--script FILE.u` (optional
  boot script run under the global realm with compile-budget off),
  `--quiet`. Refuses to start with `URBI_ERR_INSECURE_CONFIG` for
  non-loopback `--bind` without `--token`.
- **`urbi-send`** (`tools/urbi-send.c`) — one-shot NDJSON client. Ops:
  `eval CODE`, `introspect WHAT`, `cancel TAG`, `lobby-new`,
  `lobby-close ID`, `tail`. Flags: `--host HOST[:PORT]` (default
  127.0.0.1:54000), `--token TOK` (or `URBI_REPL_TOKEN`), `--lobby ID`,
  `--tail`. Exit codes: 0 = ok, 1 = eval/server error,
  2 = usage/network/auth error.
- **`urbi --listen [ADDR:]PORT`** — existing `urbi` REPL gains a
  network listener flag. Local linenoise REPL on stdin/stdout + network
  service in one process. `--token TOK` accepted alongside `--listen`.

### Added — Tests + corpus

- **`tests/chk/repl/` corpus** (15 fixtures):
  `eval_arithmetic.chk`, `eval_throws.chk`, `output_streaming.chk`,
  `hot_reload.chk`, `lobby_isolation.chk`, `wall_broadcasts.chk`,
  `echo_routes_to_session.chk`, `handleDisconnect_override.chk`,
  `global_cross_session.chk`, `object_readonly.chk`,
  `introspect_coros.chk`, `debug_coros.chk`, `budget_depth.chk`,
  `budget_nodes.chk`, `budget_source.chk`. Driven by an in-process
  runner that pipes NDJSON in via the buffer transport and diffs
  envelopes against a golden stream with normalization for ts /
  addresses / volatile ids.
- **Unit tests** (`tests/unit/test_repl_*.c`):
  `test_repl_ndjson_parse.c`, `test_repl_ndjson_emit.c`,
  `test_repl_dispatcher.c`, `test_repl_auth_flow.c`,
  `test_repl_per_realm_writer.c`, `test_repl_buffer_transport.c`,
  `test_repl_uproto_readonly.c`, `test_repl_global_atom.c`,
  `test_repl_tcp_loopback.c`, `test_repl_uart_pty.c`,
  `test_repl_chk_corpus.c` (the in-process .chk runner harness),
  `test_repl_multi_client.c` (4-client integration, opt-in via
  `URBI_TEST_MULTI_CLIENT=1` — see Known issues below).
- **Tests migrated to the Global idiom** in support of the breaking
  readonly change:
  `tests/integration/repl_smoke.sh` (cross-line shared-proto smoke now
  uses `Global.foo`; adds `Object.x = ...` → TypeError regression),
  `tests/unit/test_multi_realm.c` (three cross-realm sharing scenarios
  ported from `Object.*` to `Global.*`),
  `tests/chk/objects/fallback.chk` (COW slot-write fixture migrated to
  `Global.blurg`), `tests/chk/objects/atom_method_dispatch.chk`
  (rewritten to verify atom-method dispatch via the existing C-native
  methods rather than script-installed `Integer.marker = 42`).

### Changed

- **ABI 0/11/0 → 0/12/0** (8th pre-v1.0 escape-clause use). URealm
  grows by ~24 B (`has_compile_budget` bool + `compile_budget` struct +
  `writer_fn` + `writer_ud`); no reorder of existing fields. Public
  API additions enumerated above. `<urbi/repl.h>` is a new public
  header.
- **`urbi_vm_write` is now a thin wrapper** around
  `urbi_vm_write_in_realm` with `realm == NULL`. Source compatibility
  preserved; the underlying dispatch now consults the per-realm writer
  chain when called from a strand with a non-NULL realm.
- **`echo` re-routed** through `__builtin_lobby_send` so per-realm
  writer routing applies. Hosts using `urbi_set_writer` continue to
  see all output via the VM-writer fallback.
- **Compile-budget enforcement** threaded through `UParser`: new
  fields `budget`, `cur_depth`, `node_count`, `budget_exceeded`
  (sticky), `budget_err`. Depth checked at `parse_expression` entry;
  node count checked in `make_node`; source bytes checked at
  `urbi_repl_eval` entry before any alloc.

### Breaking

- **`Object.x = 5`** and other mutation of the 15 builtin atom protos
  (Object, Integer, Float, String, Boolean, Nil, Void, List, Dict,
  Symbol, Tag, Event, Mutex, Date, Duration) now raises `TypeError:
  cannot mutate frozen prototype` at runtime. Migration: use
  `Global.x = 5` for cross-session mutable shared state. The v0.9.0
  `Object.x = 5` idiom was explicitly noted as temporary; this is the
  clean cutover. Host-side C API (`urbi_object_set_local_slot`,
  `urbi_realm_set_global`, stdlib registration helpers) is unaffected.

### Runtime

- **URealm fields:** `has_compile_budget` bool + `compile_budget`
  (`UCompileBudget`) + `writer_fn` + `writer_ud` (~24 B with
  alignment). No reorder.
- **`vm->debug_proto`** — new per-VM singleton (void\*) for the
  `Debug` namespace.
- **15 builtin atom protos** marked readonly at `urbi_stdlib_boot`:
  Object, Integer, Float, String, Boolean, Nil, Void, List, Dict,
  Symbol, Tag, Event, Mutex, Date, Duration. Symbol/Void/Duration are
  codebase extras not in the spec's 15-list but covered for parity.
  Function / Closure / Lobby don't exist as standalone atom protos at
  v0.9.1 baseline (Lobby itself is added in this release as a single
  proto; the others remain v1.x).
- **`Global` stays mutable** by design — the designated cross-session
  shared atom (spec §4.1). The proto already existed since M6 Phase 8
  as `vm->global_namespace_proto`; v0.9.1 adopts it as the spec's
  "Global" without a separate allocation.

### Build

- **`URBI_ENABLE_REPL=1`** opts in to the REPL service. Default 0.
  Compiles `src/repl/`, `src/repl/ujson.{c,h}`, baked
  `src/stdlib/lobby.u`, the `Debug` namespace registration, and the
  `__builtin_lobby_send` primitive. Implies the compiler frontend
  (incompatible with `URBI_BYTECODE_ONLY=1` — `<urbi/repl.h>` carries
  a `#error` guard).
- `tools/urbi-compile-stdlib` (bake tool) now picks up `$(REPL_SRCS)`
  in its link list when `URBI_ENABLE_REPL=1`. Required because
  `stdlib_boot.o` references `urbi_debug_namespace_register` which
  itself references the introspect / JSON primitives in `src/repl/`.
- New build targets: `make urbi-server-bin URBI_ENABLE_REPL=1` and
  `make urbi-send-bin URBI_ENABLE_REPL=1`.

### Tests

Corpus growth under default build (`URBI_ENABLE_REPL=0`):

- 1821 cases / 13342 checks → 1847 cases / 13513 checks
  (+26 cases / +171 checks). The default-build delta comes from the
  Phase 1 foundations: per-realm writer + compile-budget unit tests,
  Global-idiom migrations, readonly-bit fixtures, multi-realm
  scenarios.

Corpus growth under `URBI_ENABLE_REPL=1` build:

- 1821 / 13342 → 1981 / 14011 (+160 cases / +669 checks). The
  REPL=1 delta absorbs every Phase 2–8 contribution: NDJSON parse /
  emit, dispatcher / auth / per-realm writer, buffer / TCP / pty /
  multi-client transport, 15 `.chk/repl/` fixtures via the in-process
  NDJSON runner, introspect-each suite, JSON-validity suite.

### Known issues / Carry-forward to v1.x

- **`Debug.*()` returns a `String`** rather than a structured Dict /
  List. v1.0 has no `UVAL_LIST` / `UVAL_DICT` — lists and dicts are
  UObjects in the `URBI_ATOM_LIST` / `URBI_ATOM_DICT` families.
  Building one from C requires walking `containers.c` internals; out
  of v0.9.1 scope. Wire JSON shape is locked, so any v1.x upgrade to
  first-class structured returns is transparent to clients that
  already parse the string.
- **Profile primitive is a stub** — `urbi_introspect_profile` emits
  empty `per_function` / `per_opcode` / `per_watcher` arrays plus
  `note:"profiling deferred to v1.x"`. No profiling infrastructure
  exists in v0.9.1; the wire shape is locked for forward
  compatibility.
- **Tag enumeration is per-realm-root** — `urbi_introspect_tags`
  walks `vm->realms_head` and emits one entry per realm root tag.
  Host-created child tags (via `urbi_tag_create`) are not centrally
  enumerated at v0.9.1; a central tag registry is in the v1.x
  design-risks register.
- **`coro_id` is the strand pointer's low 32 bits** — `UStrand` has
  no explicit id field at v0.9.1. Identity is stable per VM run (no
  strand recycling within one run). v1.x may upgrade to a monotonic
  counter when multi-host introspection requires cross-process
  stability.
- **Closure-body bare-name resolution doesn't walk `this`** — filed
  in `docs/urbi-embedded-design-risks.md`. Unqualified identifier
  references inside a closure body resolve through the realm-global
  fallback in v0.9.1 — they do NOT walk the closure's `this` proto
  chain. This makes `Lobby.echo` / `Lobby.wall` non-functional from
  urbiscript because the body references `__builtin_lobby_send`
  unqualified. Workaround in the v0.9.1 `.chk` corpus +
  multi-client integration test: call
  `Lobby.__builtin_lobby_send(...)` directly. Disposition: v1.x
  emit follow-up (implicit-`this` fallback in `emit_ident_arm`).
- **Listener-teardown race under multi-client stress** — filed in
  `docs/urbi-embedded-design-risks.md`. The 4-client multi-client
  integration test (`tests/unit/test_repl_multi_client.c`)
  intermittently segfaults (~50% rate) during harness teardown —
  race between `urepl_session_destroy` running on the listener
  pthread (on EOF) and `urbi_repl_stop` running on the test thread.
  Mitigation: the suite is opt-in via `URBI_TEST_MULTI_CLIENT=1`;
  default `make test URBI_ENABLE_REPL=1` stays green and the
  scenarios' semantic coverage overlaps green unit-test coverage.
  Disposition: v0.9.x or v1.0-rc — investigate under
  `--tool=helgrind`.

---

## v0.9.0-repl-foundation — 2026-05-19

### Foundation (M8 part 1 of 2)

- **`urbi_unload(vm, module)`** — new public API; unloads a single module from its realm's registry; defer-destroy via root_proto->refcount rescue.
- **`urbi_realm_create_repl(vm)`** — new convenience; creates a URealm with `REALM_REPL` set.
- **`urbi_realm_destroy` extended** to walk `loaded_protos_head` before strand teardown.
- **`urbi_repl_eval`** now heap-allocates the UModule per line (closes CHSTR-027); previous stack-alloc pattern leaked address-aliasing across REPL lines.

### Runtime

- **UClosure shrunk** 56 → 48 B by retiring `origin_module_instance`.  Replaced by per-UProto `owning_module_instance` back-pointer; OP_CLOSURE simplified from ~50 LOC fallback chain to 5 LOC single read.  Closes v0.8.5 "Partial bundle" carry-forward.
- **URealm gains `loaded_protos_head`** — singly-linked list of UModule shells loaded under the realm; lifecycle registry only (not a GC root chain).
- **UModule gains `next_in_realm` + `owning_realm`** for the registry linkage.

### Lexer

- **Synclines** — `//#line N "FILE"`, `//#push N "FILE"`, `//#pop` recognized in the comment-skip path; rewrite `lex->line` + `lex->source_name` so error messages report correct file:line for framed multi-line REPL submissions.  Stack depth tunable via `URBI_SYNCLINE_STACK_MAX` (default 4).  Malformed / overflow / underflow degrade silently.

### ABI

- **0/10/0 → 0/11/0** (7th pre-v1.0 escape-clause use): UClosure, UProto, UModule, URealm all change layout; ULexer gains transient state.

### Build modes

- All three modes preserved: bytecode-only / default / (REPL service introduced in v0.9.1).  `urbi_unload` + `urbi_realm_create_repl` are always available; synclines ride with the lexer (off in bytecode-only builds).

### Footprint

`size --total liburbi.a` at v0.9.0-repl-foundation (host x86-64 gcc -Os):

- host: 215,581 B text + 5,016 B data = 220,597 B total (~55% of 400 KB cap)
- arm Cortex-M7 bytecode-only (`URBI_BYTECODE_ONLY=1`): 75,895 B text (~95% of 80 KB cap)
- riscv rv32imc bytecode-only: 93,382 B text (~98% of 95 KB cap)

`cross-arm` / `cross-riscv` full-library targets fail on this machine due to the
ARM/RISC-V bare-metal toolchain missing hosted-C sysroot headers (`string.h` etc.
under `-ffreestanding`).  This is a pre-existing toolchain environment gap
(confirmed present on v0.8.5 baseline); the bytecode-only variants compile clean.
CI remains authoritative for full-library cross-target verification.

### Tests

- `tests/unit/test_uproto_owning_mi.c` — 4 cases verifying `UProto.owning_module_instance` is stamped on every proto by `urbi_module_instance_create`.
- `tests/unit/test_loaded_protos_registry.c` — 2 cases verifying `URealm.loaded_protos_head` registration at `urbi_run_chunk` entry.
- `tests/unit/test_multi_realm.c` — 4 cases gating Phase 2 OP_CLOSURE rewire (isolation, shared atom, cross-realm call, cross-realm-survives-destroy).
- `tests/unit/test_umodule_instance_lifetime.c` — 2 cases pinning the instance-unlink-on-module-destroy invariant.
- `tests/unit/test_urbi_unload.c` — 5 cases for the `urbi_unload` public API + the CHSTR-027 heap-alloc regression + `urbi_realm_create_repl` smoke.
- `tests/unit/test_realm_destroy_with_parked_loader.c` — 2 cases verifying `urbi_realm_destroy` walks `loaded_protos_head` and respects stdlib exclusion.
- `tests/unit/test_lexer_syncline.c` — 7 cases for the `//#line`/`//#push`/`//#pop` mini-parser + the `urbi_repl_eval` source-name error-format integration.
- `tests/integration/repl_smoke.sh` — added cross-line `Object.foo` shared-proto smoke case (31 → 32 total).

---

## v0.8.5-recursive-emit — 2026-05-18 (Truly-recursive emitter)

**Tag:** `v0.8.5-recursive-emit`
**Theme:** Closes the post-v0.8.1 backlog item filed in
`docs/urbi-embedded-design-risks.md` ("Truly-recursive emitter —
`OP_CLOSURE Bx` per-enclosing-proto").  Flips `emit_function_literal` to
allocate function-literal `UProto`s under the enclosing parent's
`nested[]` instead of flat under `root_proto->nested[]`; flips the
`OP_CLOSURE` dispatch arm to resolve `Bx` against the executing
`UProto`'s own `nested[]`.  Shrinks `UClosure` by 16 B on 64-bit by
deleting two now-redundant fields.

**ABI:** `0/9/0` → `0/10/0` (6th use of the pre-v1.0 escape clause —
internal struct shrinkage + emitter helper signature change; no
public-symbol additions or removals).
**Wire format:** `v1.7 / 0x17` unchanged.  On-disk bytes change for any
source with nested function literals (deeper tree replaces flat); flat-
only sources are byte-identical.
**Corpus:** 1786/13045/237 → 1795/13146/238 (+9 cases / +101 checks /
+1 fixture — `test_recursive_emit` 7-case suite + `recursive_emit_smoke.chk`).

### Added

- `UProto.ic_index` (runtime-only `uint16_t`): DFS pre-order serial
  assigned at construction.  Root proto gets `ic_index = 0`; each
  subsequent `umodule_alloc_nested_proto` allocation gets
  `++module->next_proto_serial`.
- `UModule.next_proto_serial` + `UModule.total_proto_count` (runtime-only):
  monotonic counter + final total populated identically at emit
  (`uemit_finish`) and deserialize (`umodule_deserialize`).
- `tests/unit/test_recursive_emit.c` — 9-case regression suite:
  `ic_index_root_is_zero`, `ic_index_nested_increments_in_alloc_order`,
  `total_proto_count_set_at_uemit_finish`,
  `proto_instances_n_equals_total_proto_count`,
  `verifier_accepts_emitted_module`, `emitter_produces_recursive_tree`,
  `sibling_density_at_depth`, `mixed_tree_3x3`,
  `ic_index_dense_and_dfs_preorder`.
- `tests/chk/closure/recursive_emit_smoke.chk` — end-to-end 3-deep
  closure with upvalue capture across all layers.
- `tests/golden/v0.8.5-recursive-emit-{bytecode,wire-format}-hashes.txt`
  snapshots.

### Changed

- `umodule_alloc_nested_proto(module)` →
  `umodule_alloc_nested_proto(module, parent_proto)` (internal symbol).
  Sole production caller `emit_function_literal` updated to pass
  `parent_fs->target_proto`.  20 in-tree test call sites updated.
- `emit_function_literal` now allocates `child_proto` under
  `parent_fs->target_proto->nested[]`; `proto_idx = parent.nested_count
  - 1` (per-parent index).  For top-level function literals
  `parent_fs->target_proto` is NULL and the fallback selects
  `root_proto` (byte-identical to pre-v0.8.5 on-disk shape).
- `OP_CLOSURE` `Bx` resolves against `executing_proto->nested[Bx]`
  where `executing_proto = (frame_count > 0) ? cur_cl->proto :
  s->root_proto`.  Replaces the `cur_cl->origin_nested` fallback chain.
- `OP_CLOSURE` IC binding: `cl->proto_inst =
  &mi->proto_instances->entries[child_proto->ic_index]` (per-UProto
  globally-unique index) instead of `entries[bx + 1]`.  Cross-session
  fallback through `par_cl->origin_module_instance` retained for now
  (Partial bundle — see Decision Register row 5 in the spec).
- `decode_verify` walks the proto tree recursively
  (`verify_proto_recursive`) and passes each proto's own `nested_count`
  to `verify_walk_block`.
- `urbi_get_or_create_module_instance` sizes `proto_instances->n` to
  `module->total_proto_count` (recursive total) and populates entries
  in DFS pre-order via the new `init_ic_slices_recursive` helper.
- `uemit_finish` and `umodule_deserialize` back-pointer walks
  (`UProto.root`) made recursive so grandchildren get `root` set
  correctly under recursive emission.

### Removed

- `UClosure.origin_nested` (8 B) — superseded by `cl->proto->nested`.
- `UClosure.origin_nested_count` (2 B + 6 B alignment) — superseded by
  `cl->proto->nested_count`.

### Layout

- `sizeof(UClosure)`: 72 B → 56 B on 64-bit (-16 B).
- `sizeof(UProto)`: +2 B (`ic_index`, alignment-absorbed).
- `sizeof(UModule)`: +4 B (`next_proto_serial` + `total_proto_count`).

### Carry-forward

- `UClosure.origin_module_instance` retained (Partial bundle per spec
  §7.3 Decision Register row 5) — cross-session IC binding still
  routes through it.  Test
  `test_emit_closure_capture::closure_capture_three_deep` is the
  load-bearing regression.  Truly retiring this field requires a
  per-proto back-pointer to its owning `UModuleInstance`; deferred to
  a follow-up tag.

## v0.8.4-closure-lifetime — 2026-05-18 (UClosure + UUpvalCell GC promotion)

**Tag:** `v0.8.4-closure-lifetime`
**Theme:** Closes the closure-lifetime spec Pieces B + C deferred from
v0.7.3.  Promotes `UClosure` and `UUpvalCell` to GC-managed cells via
`urbi_gc_alloc`, removes the pre-GC strand `closure_list` /
`closed_cells` free-list bookkeeping, deletes the watcher OWNS-flag
ownership-transfer model, and surfaces + fixes a latent GC
finalizer-plumbing bug that had been silent since the GC's introduction.

**ABI:** `0/8/0` → `0/9/0` (5th use of the pre-v1.0 escape clause —
internal struct shrinkage; no public-symbol changes).
**Wire format:** `v1.7 / 0x17` unchanged.
**Corpus:** 1783/13023/237 → 1786/13045/237 (+3 cases in test_closure_gc.c).

### Added

- `UTYPE_UPVAL_CELL = 20` GC type tag; UUpvalCell embeds `UCell` at
  offset 0; size pinned at 32 B on 64-bit hosts.
- `walk_uclosure` GC tracer (shades `cl->upvals[i]` + `cl->proto_inst`).
- `walk_upvalcell` GC tracer (yields heapified `u.value` via cb).
- `uclosure_destroy` GC finalizer (decrements
  `uproto_root_of(cl->proto)->refcount` + promotes root_proto to
  `vm->rescued_protos` when refcount hits 0 with self-link sentinel
  set).
- `strand_walk_roots` yields three new root categories:
  `s->entry_closure`, `s->frames[i].closure` per frame, and the
  `s->open_upvals` chain.
- `vm_misc_walk_roots` GC root provider yields `vm->last_return_closure`.
- `tests/unit/test_closure_gc.c` — three new reachability scenarios
  (realm-global anchor survival, heapified-upval reachability across
  GC cycles, multi-cycle watcher closure survival then post-unregister
  collection).
- `URBI_MAX_ROOT_PROVIDERS` bumped 8 → 12 (cap was at exactly 8
  pre-v0.8.4).

### Changed

- `vm_alloc_closure` and `vm_open_upvalue` now allocate via
  `urbi_gc_alloc` (enrolling each cell on `vm->all_cells_head`).
  Closure lifetime is governed by GC reachability instead of
  strand-owned free lists.
- `vm_alloc_closure(vm, proto, **list_head)` →
  `vm_alloc_closure(vm, proto)`.
- `vm_close_upvalues(s, threshold, **closed_list)` →
  `vm_close_upvalues(s, threshold)`.
- Watcher install paths no longer call `strand_closure_unlink` and no
  longer set `URBI_WATCHER_OWNS_*` flags.  Closures are reached via
  `watcher_table_walk_roots`.
- `release_strand_resource_chain` simplified from ~90 lines of
  per-strand-flavor migration to a 3-line head-clear.
- `urbi_native_closure_create` allocates via `urbi_gc_alloc`; anchored
  via the atom-proto slot it's installed into.

### Removed

- `UStrand.closure_list`, `UStrand.closed_cells` fields (−16 B per strand
  on 64-bit).
- `UClosure.next_alloc` field (−8 B per closure).
- `UVM.stdlib_closures`, `UVM.stdlib_upvalues` fields (−16 B per VM).
- `URBI_WATCHER_OWNS_COND / _BODY / _ONLEAVE` macros (3 free bits in
  `UWatcher.flags`).
- `strand_closure_unlink`, `vm_free_open_upvalues` helpers.
- `pool_free` closure / proto free arms.

### Fixed

- **Latent GC finalizer-plumbing bug** (`src/gc/ugc_incremental.c`).
  `urbi_gc_alloc` initialised `cell->type_tag` and `cell->gc_byte =
  current_white` but never mirrored the type's `TYPE_HAS_FINALIZER`
  flag onto the cell's `gc_byte`.  Every type registration in the
  codebase prior to Step B set `flags == 0` and `destroy == NULL`, so
  the gating check `cell->gc_byte & UGC_HAS_FINALIZER` in
  `gc_sweep_step` and `urbi_gc_destroy` was unreachable and untested.
  Step B registered the first finalizer in the codebase
  (`uclosure_destroy` for UTYPE_CLOSURE); Step C-2 routed UClosure
  allocation through `urbi_gc_alloc`, surfacing the bug as a 76985 B /
  1530 alloc nested-proto-buffer leak under the strict-CFLAGS ASan
  releasetest gate.  Fix: at `urbi_gc_alloc` time, consult
  `vm->type_table[type_tag]->flags` and set `UGC_HAS_FINALIZER` on
  `cell->gc_byte` when present.

### Process notes

Plan was re-revised mid-execution to **Option B** (atomic GC promotion
plus legacy purge bundled into one landing).  The original phasing
split GC promotion (Step 1) from legacy purge (Step 5) into separate
landings; Step C-2
execution diagnosed deep structural coupling — once `urbi_gc_alloc`
enrolls a cell on `vm->all_cells_head`, every manual `vm->alloc_fn(cell,
0, ud)` site becomes a UAF on the next GC sweep.  The two phases must
land atomically.  Recorded in the plan file's §5 phasing table.

The audit (Step A) caught two correctness prerequisites missed by the
original plan: `UTYPE_CLOSURE` and `UTYPE_UPVAL_CELL` had no
`type_table` entries, so closure upval-cells would be treated as
leaves by the mark phase.  Step B added both walkers and the closure
finalizer.

---

## v0.8.3-valgrind-and-cross-verify — 2026-05-18 (valgrind wedge fix + cross-esp32s3 golden refresh)

**Tag:** `v0.8.3-valgrind-and-cross-verify`
**Theme:** Interstitial hygiene release.  Closes the two gates skipped
at the v0.8.2 ship (`test-valgrind` wedge + `test-corpus-sanitize`
follow-on) and refreshes the cross-esp32s3 freestanding-signature
golden that drifted from v0.8.2's freestanding Float methods.  No
runtime code change.  Wire format **v1.7 / 0x17** unchanged.  ABI
**0/8/0** unchanged.

### Fixed

- **`test-valgrind` wedge** (v0.8.2 ship "Surprise #7").  The
  `event_ring_multi_thread_fuzz_100k` test is a 100,000-event SPSC
  fuzz that surfaces ring-buffer races by running producer and
  consumer on separate pthreads.  Under valgrind's memcheck thread
  serialization (one CPU for all threads), the producer fills the
  ring and busy-spins on `RING_FULL` while the consumer is
  descheduled — a pathological scheduling that pushed wall-clock
  past 30 min and lost the test's race-detection value entirely.
  Now skipped under valgrind via `-DURBI_SKIP_THREAD_FUZZ_TESTS=1`
  in `test-valgrind` and `test-valgrind-deep` CFLAGS; still runs
  under `make test` and the ASan/UBSan variants.  `test-valgrind`
  now completes in ~5 s green; full `releasetest` returns to ~40 s
  end-to-end (Phase 1 34 s + Phase 2 8 s).
- **`test-cross-esp32s3-freestanding-golden` drift** (CI gate, not
  in releasetest).  v0.8.2's freestanding `Float.abs/floor/ceil/round`
  (commit `3b857ef`) legitimately pulls two new libgcc helpers into
  `liburbi.a`: `__fixdfdi` (`double → int64_t` truncation, from
  `int64_t t = (int64_t)x;` in `flt_floor`/`flt_ceil`) and `__gtsf2`
  (f32 > f32 compare, emitted by `gcc -Os` when it optimises
  `(double)self.v.f > 0.0` into the equivalent f32 compare-with-zero
  in `flt_ceil`).  Golden refreshed; intent verified via objdump
  cross-reference of the calling function.  Gate has been silently
  red since v0.8.2 ship because CI was red across the v0.7.x and
  v0.8.0 ships and this gate is CI-only.

### Changed

- **`test-valgrind` / `test-valgrind-deep` Makefile targets** add
  `-DURBI_SKIP_THREAD_FUZZ_TESTS=1` to CFLAGS.  Documented inline.
- **`tests/golden/v0.7.2-esp32-nm-bytecode-only.txt`** content
  refreshed (+`__fixdfdi`, +`__gtsf2`).  Filename retains v0.7.2
  prefix — the golden-update flow per Makefile §1001 is delete +
  regenerate in place; the prefix is the file's creation ship,
  not its current freshness.  (A future rename pass can align the
  prefix with current ship; not load-bearing.)

### Releasetest

- **All 22 gates green** (Phase 1 34 s + Phase 2 8 s = 42 s total).
  First clean releasetest since v0.8.0-loader-strand.

### Numeric outcomes

- **Test corpus**: 1783/13023/237 (unchanged from v0.8.2).
- **ABI**: **0/8/0** unchanged.
- **Wire format**: **v1.7 / 0x17** unchanged.
- **Commits**: 2 substantive on `topic/v0.8.3-valgrind-and-cross-verify`.
- **Code delta**: 3 files changed, +22 / -6 lines.

## v0.8.2-stm32f4-mandelbrot — 2026-05-17 (STM32F4 bare-metal port + Mandelbrot demo)

**Tag:** `v0.8.2-stm32f4-mandelbrot`
**Theme:** First non-RTOS embedded port.  Bare-metal STM32F429I-DISC1
(Cortex-M4F, 192 KB SRAM + 8 MB SDRAM, 240×320 LCD, L3GD20 gyro, USER
button) with a progressive-refinement Mandelbrot demo as the bring-up
vehicle.  Eight thin port-shim TUs wrap STM32CubeF4 BSP; urbi's
cooperative scheduler runs directly on bare metal — no FreeRTOS.  The
demo exercises arithmetic + host-fn dispatch + ISR-safe event ring +
polled-flag abort/restart cooperative-cancellation pattern + freestanding
allocator survival under tight RAM.  Bring-up surfaced 7 latent runtime
bugs, all fixed inline; none required wire-format or ABI changes.

### Added

- **`components/stm32f4-hal-baremetal/`** managed-component shell wrapping
  STM32CubeF4 HAL.  Eight port shims: `port_allocator` (bump+two-pass-fit
  freelist over a static `.bss` heap, sized for the demo's 8 KB strand
  stacks), `port_time` (DWT cycle counter), `port_writer` (USART1 via
  ST-Link VCP), `port_diag` (URBI_LOG_* routing), `port_isr_check` (CMSIS
  IPSR intrinsic), `port_lcd` (`BSP_LCD_FillRect` with bounds clamp +
  RGB565→ARGB8888 conversion), `port_gyro` (L3GD20 via SPI5, three
  axis-specific host-fns), `port_button` (PA0 EXTI0 → urbi event ring
  from ISR context).
- **`examples/stm32f4/mandelbrot/`** full demo: 247-line `mandelbrot.u`
  plus 317-line `main.c` plus STM32F429ZITx linker script plus minimal
  libc stubs for freestanding link.  Progressive 32→1 pixel-tile refinement with
  polled-flag abort/restart; tilt-pan via gyro integration; button-press
  2× zoom; gyro tilt-intensity bar overlay.  Builds via
  `examples/stm32f4/mandelbrot/Makefile` (out-of-tree, depends on a
  STM32CubeF4 sibling checkout).
- **`make cross-stm32f4`** and **`make cross-stm32f4-bytecode-only`**
  top-level cross-compile targets (Cortex-M4F + f32 UValue layout +
  `URBI_STDLIB_FLAVOR=4` rebake).  CI matrix gains a `cross-stm32f4`
  job alongside the existing `cross-arm` and `cross-esp32s3`.
- **`URBI_STDLIB_FLAVOR`** Makefile knob + `tools/urbi-compile-stdlib-f%`
  pattern rule to rebake the stdlib bytecode for non-default
  `URBI_FLOAT_TYPE` targets.  Tracked `src/stdlib/urbi_stdlib_bytecode.gen.c`
  remains host-baked f64; per-target rebake produces a flavor-matching
  blob and the loader's flavor check (byte 13) no longer rejects.
- **`tests/port_stm32f4/`** host-side unit tests for each port shim, run
  against a mock BSP layer (`mock_bsp.{c,h}`).  Eight tests covering
  allocator behaviour, time/writer/diag plumbing, LCD bounds, gyro
  rate/zero handling, button-event-from-ISR injection.  Wired into
  `make test` via the new `test-port-stm32f4` target.
- **`Float.abs` / `Float.floor` / `Float.ceil` / `Float.round`** real
  implementations in freestanding builds.  Previously throw-stubs;
  silently killed any body strand calling them (the gyro pan body in
  the Mandelbrot demo).  No libm dependency — pure C arithmetic.
- **`tests/chk/reactive/polled_abort_restart.chk`** regression for the
  polled-flag abort/restart pattern used by the Mandelbrot demo (236 →
  237 .chk fixtures).

### Changed

- **`UObject.protos` single-form encoding** switched from shift-encoded
  `(p << 1) | 1` / decode `>> 1` to alignment-tag encoded
  `(uintptr_t)p | 1U` / decode `& ~(uintptr_t)1U`.  The shift encoding
  silently lost bit 31 of the prototype pointer on any heap above
  `0x80000000` (e.g. STM32F4 SDRAM at `0xD0000000`).  Alignment makes
  bit 0 of any aligned UObject pointer free, so OR-tagging is correct
  and costs the same.  Internal change; no public API impact.
- **`urbi_stdlib_boot`** now inherits `vm->alloc_fn` + `vm->alloc_ud` into
  the stdlib UModule it allocates for the baked bytecode.  Pre-fix, the
  module's `alloc_fn` stayed NULL and `module_allocator()` silently fell
  back to `stdlib_alloc` on hosted builds (hiding the bug); on
  freestanding builds the fallback isn't available and
  `umodule_deserialize` failed with `ULOAD_OOM` before any real
  allocation — the symptom was `urbi_realm_global` returning NULL on
  every freestanding embedder.  Two-line fix.
- **`components/urbi` / `components/urbi_aux`** renamed to
  **`components/esp32-idf` / `components/esp32-idf-aux`** to make room
  for the second port-component family.  ESP-IDF embedders updating
  from v0.8.1 must adjust their `idf_component.yml` references.

### Fixed (runtime bugs surfaced during STM32F4 bring-up)

In addition to the architectural changes above, six other latent bugs
were caught by the on-hardware bring-up cycle and fixed inline.  All
six manifest only under the specific allocator / float-layout / display-
orientation conditions of the F4 demo; the host test suite never
exercised them.

- **`tests/unit/test_sched_pool_exhaust.c::count_alloc`** missing realloc
  semantics.  The test's allocator did `malloc(nbytes)` for non-zero
  calls, ignoring the incoming pointer.  Worked by accident before
  `urbi_stdlib_boot` propagated `vm->alloc_fn` into the stdlib UModule;
  after that fix surfaced a segfault inside `umodule_destroy_proto_buffers`
  walking uninitialised nested-slot tails.  Switched to plain
  `realloc(ptr, nbytes)` per the `UVMAllocFn` contract.
- **`tests/unit/test_uobject.c`** atom-singleton decode used the
  superseded `protos >> 1` form; updated to `& ~(uintptr_t)1U` to match
  the new single-form encoding (see Changed above).
- **`UModule.alloc_fn` not propagating** (see Changed above) — the
  bring-up debugging that found this also surfaced silent-failure mode:
  embedders on freestanding targets had no way to know
  `urbi_realm_create` had partially-failed until they tried to use
  realm globals.  No new error code; the existing
  `URBI_ERR_STDLIB_BOOT_FAILED` propagates as expected, just now
  reaches embedders that exercise the path.
- **Stdlib bytecode flavor mismatch on cross-compile** — tracked
  bytecode blob is host-baked f64; cross-stm32f4 runtime is f32.  Per-
  target rebake via the new `URBI_STDLIB_FLAVOR` knob (see Added).
- **Bump-only allocator could not reuse strand-stack blocks** — the
  initial `port_allocator.c` had no freelist; freed 8 KB strand stacks
  leaked.  Rewritten as bump + two-pass-fit freelist (good-fit for
  `size in [need, need*2)` then first-fit fallback) so freed stacks
  stay intact for matching 8 KB requests.
- **`URBI_FLOAT_TYPE` layout mismatch between embedder TU and library**
  silently zeroed every `UVAL_FLOAT`.  Embedder compiled with default
  `URBI_FLOAT_TYPE=8` (`v.f` is double); `liburbi.a` built with `=4`
  (`v.f` is float).  Different store widths at the same struct offset;
  the low 32 bits of any "nice" double are `0x00000000`.  Fixed at the
  call site (embedder must set `-DURBI_FLOAT_TYPE=4` to match the
  library); a follow-up link-time guard is tracked in design-risks
  ("v1.0-rc — URBI_FLOAT_TYPE link-time mismatch is silent").

### Spec / plan

- Spec: v0.8.2 STM32F4 Mandelbrot design
- Plan: v0.8.2 STM32F4 Mandelbrot execution plan
- Retrospective: `docs/milestones/v0.8.2-stm32f4-mandelbrot.md`

### Wire format / ABI

- **Wire format unchanged at v1.7 / 0x17.**  No bytecode-layout work in
  this release; v1.7 modules baked at v0.8.1 continue to load.
- **ABI unchanged at 0/8/0.**  No struct-layout changes in public types;
  no symbol additions; no signature breaks.

### Known gate status at ship

- **Phase 1 (20 gates): all green** — `test`, `test-asan`, `test-ubsan`,
  `test-debug`, `test-switch`, `lint`, `docs-check`, `coverage`,
  `test-stress`, `test-gc-none-build`, `test-scan-build`, `test-cppcheck`,
  `test-tidy-strict`, `test-wire-format-determinism`,
  `test-docstring-coverage`, `test-bake-smoke`, `test-bytecode-only`,
  `test-gc-roots-coverage`, `test-aux-symbols`, `test-embedding-guide`
  all passed clean.
- **Phase 2 `test-valgrind`: NOT RUN at ship.**  Wedged at ~32 min of
  wall-clock vs the historical ~5-min baseline.  Process showed alive
  (RSS growing, not deadlocked) but throughput collapsed to ~4 % CPU
  after an initial high-utilisation phase.  Killed and not retried;
  v0.8.3 will investigate the slowdown.  ASan + UBSan (Phase 1) cover
  the most common leak / UAF / overflow patterns and remained clean.
  Tracked as a v0.8.3 followup item; design-risks entry filed.
- **Phase 2 `test-corpus-sanitize`: NOT RUN** (sequential after
  `test-valgrind`).  Same v0.8.3 followup.

---

## v0.8.1-uproto-root — 2026-05-17 (UModule thin-shell refactor + module-grain lifetime)

**Tag:** `v0.8.1-uproto-root`
**Theme:** Structural consolidation of the module/proto ownership model.
UModule shrinks to a thin 5-field loader shell; all chunk-top data moves
into a new `UProto *root_proto` field.  The two parallel refcount systems
(v0.7.3 per-proto + v0.8.0 per-module) fuse into a single counter on
`root_proto`.  Closes the v0.8.0 forward-path item and unblocks M8 REPL's
realm-owned `loaded_protos[]` registry.

### Changed

- **Wire format v1.6 → v1.7** (`URBI_BYTECODE_VERSION_BYTE = 0x17`).
  Root-chunk fields now serialize inside a recursive `UProto` block rather
  than as flat UModule members.  v1.6 bytecode is rejected with
  `ULOAD_UNSUPPORTED_VERSION` per the existing exact-match policy.
  No in-band migration; embedders re-bake from source.
- **ABI version 0/7/5 → 0/8/0** (`URBI_API_VERSION_NUM = 800`).  Both
  UModule and UProto layout shift; the per-nested rescue path is deleted.
  MINOR-field bump (vs the PATCH-field bumps through 0.7.x) reflects the
  structural significance.  Zero public-API surface changes for embedders
  — all changed types are opaque.
- **Module-grain lifetime (Variant B).** Any closure escaping into realm
  globals now pins the entire owning module structure (shell + all nested
  protos) until VM-destroy.  Previous behavior rescued individual UProtos.
  Trade-off: simpler GC, bounded memory overhead, no per-nested accounting.
  Long-running REPL sessions that load and escape many small chunks will
  accumulate `root_proto` structures; explicit unload via the forthcoming
  M8 `urbi_unload` API is the escape valve.
- **UModule shrunk to a 5-field thin loader shell** (`root_proto`,
  `source_name`, `origin_vm`, `alloc_fn`, `alloc_ud`).  All chunk-top
  fields — instructions, constants, line_deltas, abs_lines, IC metadata,
  nested[], max_reg, nupvals, nparams — migrated to `UProto`.  Host size
  ~200 B → ~40 B; arm ~100 B → ~20 B.
- **`urbi_run_chunk` refcount target** redirected from `UModule.refcount`
  (deleted) to `module->root_proto->refcount`.  Behavior is unchanged for
  embedders; the internal mechanism fuses the two v0.7.3/v0.8.0 counters.
- **`UStrand`** gains a `root_proto` fast-path field alongside `module`.
  Hot-path dispatch (`s->root_proto->instructions`, etc.) uses the new
  field; cold-path error reporting continues via `s->module->source_name`.

### Removed (internal)

- **`urbi_steal_repl_protos`** (file-private to `src/module/uchunk.c`,
  ~90 LOC).  Incoherent under module-grain Variant B; deleted.
- **`vm->stdlib_protos`** — per-nested rescue list.  Replaced by simpler
  `vm->rescued_protos` (whole-`root_proto` list).
- **`UModule.refcount` + `UModule.destroy_requested`** — fused with
  `root_proto->refcount`.
- **UModule duplicate chunk-top fields** — instructions, constants,
  line_deltas, abs_lines, max_reg, nupvals, nparams, ic_count, ic_names,
  ic_name_strs, nested[], nested_count, nested_cap all deleted from UModule.
- **Slot-implicit refcount** — `umodule_alloc_nested_proto` now inits
  `refcount = 0` (was 1 in v0.7.3).  The slot's reachability is implicit
  in `root_proto`'s lifetime under module-grain Variant B.
- **Per-nested rescue walk** in `umodule_destroy_internal`.  Replaced by
  whole-`root_proto` rescue per the new `umodule_destroy` semantics.

### Added (internal)

- **`UProto.nested[]`** — recursive children array.  Root proto populates;
  non-root has `nested_count = 0` under the current flat-on-root emitter.
  Serialized in the v1.7 wire format.
- **`UProto.root`** — back-pointer to the owning module's root proto.
  `NULL` on the root proto itself; set to `module->root_proto` on every
  nested.  Runtime-only; not serialized.
- **`vm->rescued_protos`** — whole-`root_proto` rescue list replacing
  `vm->stdlib_protos`.  `umodule_destroy` transfers a non-zero-refcount
  `root_proto` here; `urbi_vm_destroy` sweeps this list last.
- **`uproto_root_of(proto)`** — NULL-safe inline that chases the `root`
  back-pointer; returns `proto` if `proto->root == NULL` (i.e. proto is
  the root).  Used by all refcount bump/dec sites.
- **`umodule_strand_refcount_dec(module, root_proto, vm)`** — bridge
  helper for strand-destroy decrement; routes through `root_proto.refcount`
  and triggers deferred destroy when the count reaches zero.
- **`UModule.root_proto`** (`UProto *`) — the only mandatory runtime field
  added to UModule; everything else was removed.
- **`urbi_aux_load_and_run` / `urbi_compile_source`** paths updated to
  allocate `root_proto` directly (no alias-copy of fields).

### Fixed

- **Test-helper `utest_e2e_compile_and_run` UAF** on still-live nested
  protos (v0.7.2-shipped OPEN, design-risks entry "v0.7.x — test-helper
  UAFs").  Phase 0 confirmed a clean ASan pass: the v0.7.3 UProto refcount
  combined with the v0.8.0 UModule refcount mechanisms together closed
  the UAF before this refactor began.  Permanent regression net at
  `tests/unit/test_test_helper_uaf_repro.c`.  Design-risks entry deleted.
- **IC-names leak** after non-strand `module->X` hot-path migration
  (Task 5 follow-up; was 928 bytes/stdlib_boot per valgrind).
- **Test-helper refcount target drift** — 5 test files that manually
  incremented `module->refcount` in strand-bind helpers migrated to
  `root_proto->refcount` (Task 7 follow-up).
- **`umodule_deserialize` failure-path leak** of `root_proto` (168 bytes;
  fixed during Task 11 thin-shell cleanup).
- **`urbi_module_from_bytes` parse-failure leak** of `root_proto`
  (fixed during Task 11).
- **Cppcheck strict-gate findings** — multiple `duplicateConditionalAssign`,
  `variableScope`, and `constParameterPointer` diagnostics fixed during
  Task 11; strict gate hard-passes.

### What was deferred (not in this release)

- **Truly-recursive emitter** (Bx scopes per-enclosing-proto) — Bx still
  indexes the flat root-level array; non-root UProto's `nested_count` is
  always 0.  Deferred to the Approach C long-term endgame (§11 of the spec).
- **Approach C (no UModule at all)** — every chunk becomes a bare UProto
  with header fields.  Tracked in design-risks; post-M8 work.
- **Realm-owned `loaded_protos[]` registry** — multi-chunk REPL eval and
  disconnect-cleanup at realm level.  M8 REPL milestone.
- **`urbi_unload(realm, module)`** — explicit module eviction for
  long-running REPL sessions accumulating escaped-closure modules.  M8.
- **`vm->stdlib_closures` sweep ordering invariant** — documented in
  `docs/internals/module-system.md` §6; the ordering is already correct
  as shipped; no code deferral, only future documentation of why.
- **UClosure / UUpvalCell GC promotion** (Phase 2-3 of the v0.7.3
  closure-lifetime spec) — deferred from that spec; still pending.

### Spec / plan

- Spec: v0.8.1 uproto-root design
- Plan: v0.8.1 uproto-root execution plan

---

## v0.8.0-loader-strand — UNRELEASED (persistent loader strand, restore legacy parallel-by-syntax)

**Tag:** `v0.8.0-loader-strand`
**Theme:** Architectural change to `urbi_run_chunk`'s execution model.
Restores legacy urbiscript spec semantics: chunk-top `&` and `,` (and
call-chains-from-chunk-top fork) work end-to-end.  The legacy
aldebaran-urbi conformance corpus at `tests/2.x/separator/` is finally
runnable.

### Changed

- **`urbi_run_chunk`** allocates a real scheduler-managed strand via
  `urbi_strand_create_for_module` instead of a stack-local transient.
  Drives internally via `urbi_step` until the loader strand parks
  (sleep / join-wait / event-wait) or dies (OP_RET / fatal).  Strand
  persists in `realm->strands_head` past return; host's main `urbi_step`
  loop advances it.
- **`umodule_destroy(m, vm)`** becomes refcount-aware: deferred destroy
  when `m->refcount > 0` (typical for chunks loaded but not yet drained);
  immediate destroy when `refcount == 0` (typical for the
  host-after-vm_destroy pattern, which still works unchanged).
- **`urbi_run_chunk` parameter** `const UModule *module` reverts to
  `UModule *module` (refcount mutation).  Reverts one element of the
  v0.5.8-cleanup const sweep.
- **ABI version 0/7/4 → 0/7/5** (URBI_API_VERSION_NUM 704 → 705).
  UModule struct grows two runtime-only fields (uint16_t refcount +
  bool destroy_requested); sizeof shifts.  Third use of the pre-v1.0
  escape clause.
- **`strand_unlink_from_tags`** scans `[0..cleanup_cap)` instead of
  `[0..cleanup_depth)`.  Fixes CHSTR-051: popped TAG_SCOPE entries
  remained linked in `tag->member_strands_head` because
  `strand_cleanup_pop` decrements `cleanup_depth` without unlinking.
  Latent bug exposed by persistent-strand path; fix is contained.
- **`release_strand_resource_chain`** migrates persistent-strand
  closures/upvals to `vm->stdlib_closures`/`vm->stdlib_upvalues` instead
  of freeing eagerly (UAF prevention for realm-global closures).

### Added

- **`UModule.refcount`** + **`UModule.destroy_requested`** fields
  (runtime-only, not serialized).  Bumped at strand bind, decremented
  at strand death.
- **`umodule_refcount_inc/dec`** helpers in `<src/module/umodule.h>`.
  Saturation at UINT16_MAX (log + freeze) + underflow assert; same
  policy as v0.7.3 UProto.refcount.
- **`urbi_strand_create_for_module`** loader-strand helper in
  `<src/sched/ustrand.h>`.
- **`uchunk_loader_drive`** internal driver loop with park-or-die
  state machine in `<src/module/uchunk.h>`.  Uses realm-walk to
  detect loader death (UAF-safe; the strand list is on the realm,
  not on the strand).
- **`URBI_ERR_LOADER_BUDGET`** (-20).  Returned by `urbi_run_chunk`
  when the internal driver's outer iteration cap is exhausted
  (chunk-top runnable but unable to reach a parked/dead state).
- **`tests/unit/test_loader_strand_persistence.c`** — 7 cases covering
  helper allocation, driver park/die, chunk-top `&`/`,`, chain-call
  fork, parks-on-waituntil.
- **`tests/unit/test_module_refcount.c`** — 6 cases covering refcount
  semantics, deferred destroy, host-destroy paths, saturation.
- **Activated legacy chk fixtures** at `tests/chk/separator/`:
  `and-environment.chk`, `comma.chk`, `comma-environment.chk`.  Direct
  ports of the aldebaran-urbi 2.x assertions (adapted for current
  stdlib limitations).

### Fixed

- **Chunk-top `&` and `,` fatal** (v0.7.x design-risks entry "`&`
  fork-join requires urbi_step driver").  Entry deleted; constraint
  structurally resolved.  Forward-pointer added to M8 section for the
  canonical UModule-layout refactor that completes the trajectory.
- **CHSTR-051** — `strand_unlink_from_tags` left popped TAG_SCOPE
  entries linked in `tag->member_strands_head` during fatal unwind.
  Caused UAF in `throw_in_finally.chk` under the persistent-strand
  path.

### What was deferred (not in this release)

- **UModule layout refactor** — root proto becomes a `UProto *root_proto`
  field; instructions/constants/IC metadata move into root_proto;
  UModule shrinks to thin loader shell.  Pre-M8 prerequisite work;
  tracked in the v0.8.0-loader-strand spec §11 "Forward path."
- **Realm-owned `loaded_protos[]` registry.**  Multi-chunk, REPL eval,
  disconnect-cleanup at the realm level.  M8 REPL milestone.
- **`vm->last_loader_strand` introspection seam.**  Forward-compat for
  multi-chunk callers to interrogate loader state.  Reserved for the
  next step; no caller demand today.

---

## v0.7.3-bugfixes — 2026-05-16 (cascade-wake structural fix via UProto refcount)

**Tag:** `v0.7.3-bugfixes`
**Theme:** Closes the v0.7.2-shipped OPEN cascade-wake bug structurally via Piece A of the closure-lifetime spec — UProto refcount. The frame_count tactical fix (committed as the baseline of this release) gets replaced by a refcount-aware ownership model: each UProto carries a `uint16_t refcount`; `vm_alloc_closure` bumps, `pool_free` decrements, `umodule_destroy` discharges the nested-slot ref and rescues non-zero-refcount protos to `vm->stdlib_protos`. Closes WATCH-015 from the v1.x backlog. Full Phase 2-3 of the original closure-lifetime spec (UClosure GC promotion + dead-infrastructure cleanup) deferred to a future release with its own sizing — the cascade fix doesn't need them, and M6 Phase 3 closure migration already mitigates the function-body dangling-cl case the GC promotion would have addressed.

### Fixed

- **Cascade-wake crash** (v0.7.2-shipped OPEN, design-risks entry "waituntil cascade-wake crashes when 3+ strands wake simultaneously"). Structural fix via UProto refcount makes the proto outlive `pool_free`'s decrement when the nested[] slot still references it. Multi-install of the same compile-time cond proto (function called multiple times via at-event refire-queue) is now safe end-to-end.
- **WATCH-015** (v1.x backlog item — "UProto refcount for closure-aliasing safety"). The predicted "future caller invariant change" that would surface the proto-aliasing bug was exactly the cascade scenario.

### Changed

- **`umodule_destroy(UModule *)` → `umodule_destroy(UModule *, UVM *vm)`.** vm = NULL is allowed for the bound-to-no-vm case (e.g. failed-compile cleanup). When non-NULL, the rescue path transfers protos with `refcount > 0` to `vm->stdlib_protos` instead of freeing — surviving closures (held by active watchers, `vm->stdlib_closures`, etc.) keep a valid backing proto for the rest of vm lifetime.
- **`UProto` struct** gains a `uint16_t refcount` field (runtime-only; not serialized — same pattern as `next_alloc`). Saturation at UINT16_MAX: log + leak rather than wrap. Underflow guard at 0 catches missing-bump bugs.
- **ABI version 0/7/3 → 0/7/4** (URBI_API_VERSION_NUM 703 → 704). Reason: umodule_destroy signature break. Per the pre-v1.0 escape clause in version.h, MINOR/PATCH bumps may break ABI with CHANGELOG enumeration; this is the second use (first was v0.7.2-esp32's S41 0/7/1 → 0/7/3 for the urbi_set_diag_fn addition).
- **`strand_closure_unlink`** now decrements the proto refcount before nulling the `nested[k]` slot (Piece A nested-slot accounting). Net effect on existing chunk-top install behavior: identical; `nested[k]` still goes to NULL, but the refcount drop is tracked.
- **`vm_alloc_closure`** bumps `cl->proto->refcount` via `umodule_proto_refcount_inc` after binding the proto pointer.
- **`pool_free` OWNS_* arms** decrement `cl->proto->refcount` before considering freeing the proto; free-the-proto path now gated by `refcount == 0`.

### Added

- **`tests/unit/test_waituntil_cascade.c`** re-enabled (3-strand wake on shared cond). Previously DISABLED at v0.7.2 ship; goes RED at the tactical-fix baseline (T2 revert) and GREEN once Piece A lands (T7).
- **`tests/unit/test_dangling_cl_function_body_install.c`** — regression net for the function-body install path (at-handler installed from inside a callee, calling strand dies before fire). Passes via the existing M6 Phase 3 closure migration + OWNS_BODY transfer; guards against future drift that removes either mechanism.
- **`tests/unit/test_proto_refcount.c`** — direct unit test for the rescue → pool_free → stdlib_protos sweep flow. Pins the no-double-free invariant.

### What was deferred (not in this release)

- **UClosure / UUpvalCell GC promotion** (originally Pieces B + C of the closure-lifetime spec). The cascade-wake fix doesn't need them; the function-body dangling-cl case is already mitigated by M6 Phase 3 migration. Tracked for a future release.
- **Deletion of `closure_list`, `OWNS_*` flags, `strand_closure_unlink`** (originally Phase 3 cleanup). All still in use; refcount lives alongside them rather than replacing them.
- **`urbi_steal_repl_protos`** retained — serves an orthogonal `nested[]` array-rescue role (preserves `cl->origin_nested` validity for cross-session inner-closure calls) that the per-proto refcount rescue does not subsume.

### Spec / plan

- Spec: v0.7.3 closure-lifetime fix design
- Plan: v0.7.3 closure-lifetime fix execution plan (executed through T7 + T9 + T28; T8 deferred per investigation; T10-T27 + T29-T33 deferred to a future release)

---

## v0.7.2-esp32 — 2026-05-16 (M7 Wave 2 part 2 — ESP-IDF port + runtime hardening)

**Tag:** `v0.7.2-esp32`
**Theme:** First MCU port + unexpectedly substantial runtime hardening. ESP-IDF v6.0.1 against ESP32-S3 / S3-EYE; `components/urbi/` managed-component shell consumes the locked v0.7.1 ABI; ESP32-S3-EYE color blob tracking eye_demo on actual hardware. **Bonus bug hunt:** stress-testing the demo with progressively wider urbiscript surface (try/catch/finally, `at sync`, multi-statement at-bodies, `&` fork-join) uncovered 10 real bugs across the parser, emit, and runtime layers — all fixed and validated on hardware. ABI 0/7/1 → 0/7/3 (S41 `urbi_set_diag_fn` landed pre-session, taking the ABI to 0/7/3 directly; tag name retains `v0.7.2-esp32` per the original Wave 2 plan). Wire format v1.5 → **v1.6 / 0x16** (S42 OP_SELF added pre-this-session for method-call dispatch — replaces `vm->last_recv`; no further wire changes during the ship session itself).

### Added

**ESP-IDF port (the original Wave 2 part 2 scope).**

- `components/urbi/` — ESP-IDF managed-component shell. `port_freertos_task.c` (urbi task body + step observer hook), `port_allocator.c` (heap_caps_* PSRAM tier), `port_time.c` (esp_timer_get_time), `port_writer.c` (ESP_LOG channel), `port_isr_check.c` (xPortInIsrContext), `port_wake_from_inject` (xTaskNotifyGiveFromISR). Targets ESP32-S3 initially (`CONFIG_IDF_TARGET="esp32s3"`).
- `components/urbi_aux/` — sibling opt-in aux component (Lua `lua.h` / `lauxlib.h` split pattern). Mirrors host Makefile's separate `liburbi_aux.a` archive. Embedders opt in via `REQUIRES = urbi_aux`; default ESP-IDF builds get core only (smallest footprint).
- `examples/esp32/eye_demo/` — full ESP32-S3-EYE color-blob-tracking demo. Three FreeRTOS tasks (urbi / camera / display), OV2640 240×240 RGB565 capture, chrominance-based blob detector with channel-dominance + min-brightness thresholds, ST7789 240×240 LCD output with crosshair overlay, BOOT button cycling through RED→GREEN→BLUE targets. Demonstrates: classes, methods, `at (event?)`, `at (cond)`, `at (slot.changed?)`, `at sync`, brace-block at-bodies, try/catch/finally, `&` fork-join (via function wrapper), runtime stats snapshot every 2s with ~25 diagnostic counters.
- `tests/qemu/reactive_smoke/` — QEMU smoke test scaffolding (espressif/qemu via `idf.py qemu`). Currently advisory; promotion to required follows v0.5.7-fixes 2-week-stable precedent.

**Runtime hardening — 10 bug fixes** (all surfaced by eye_demo stress-test session):

1. **task #23** — at-body method RETURN: `urbi_unwind` now direct-pops the call frame when no `UCLEANUP_CALL_FRAME` is present (body strands have flat cleanup), preserving outer TAG_SCOPE / TRY_FRAME entries. Crash: at-body invoking class instance method fataled body strand.
2. **task #22** — chunk-top closure dst: `emit_function_literal` respects parent `next_reg` (not just `freereg`), preventing `Realm.fn = function() {...}` from clobbering live temps.
3. **task #13 / S43** — chunk-top var × at-handler closure NULL module deref: closed by task #23's entry_closure fallback chain in `ustrand_consts_for_closure`. Body strands have `s->module == NULL`; pre-fix `OP_LOADK` deref'd a NULL pointer.
4. **task #24 / S44** — catch / finally handler PC uses `s->pc_base`, not `s->module->instructions`. Catch absorption in a method invoked from an at-body crashed with NULL deref (body strands → `s->module == NULL`) AND method-frame try/catch from chunk-top wrote to wrong proto. Two sites fixed in `urbi_unwind`.
5. **S45** — `uemit_declare_local` desyncs `e->next_reg` from `fs->freereg`. The catch handler's variable was clobbered by the body's first temp allocation, leading to `Realm.caught = e` silently storing the realm object instead of the thrown value. Fix in the helper makes the post-condition self-consistent; removes the footgun for all future callers.
6. **emit_nary freereg drift** — between siblings of an NARY expression chain, `e->next_reg = freereg` was insufficient; needed `freereg = fs_temp_floor` first. Bare discarded-result calls left freereg above the floor, causing subsequent var-decls to alias outer-scope locals. Demo manifestation: BlobScan iters stuck at 240 instead of 57 600 with `centroid_x == centroid_y` always.
7. **S47** — `parse_statement_or_expr` now accepts `TOK_LBRACE` → `parse_block`. Original urbi spec allows `at (e?) { stmts }`, `at (cond) { ... } onleave { ... }` — pre-fix the parser routed body to `parse_inner_tier` which doesn't accept LBRACE as an expression prefix.
8. **S48 + 4 followups** — `=` RHS uses `parse_expression` (Pratt-only), not `parse_inner_tier` (which folds `|` / `&` separators). `Realm.a = 1 | Realm.b = 2` mis-parsed as `Realm.a = (1 | (Realm.b = 2))`. Fix applied at 5 sites: `parse_member_access`, `parse_var_decl`, `parse_assign_after_eq_peek`, `parse_return`, `parse_throw`. Per legacy `aldebaran-urbi/tests/2.x/atomic.chk` + `urbistyle.chk` conventions.

### Build

- `components/urbi/CMakeLists.txt` — ESP-IDF managed-component build manifest. `srcs` lists every `src/**/*.c` (~80 files); `include_dirs` `${URBI_ROOT}/include` `${URBI_ROOT}/src`. Embedders consume via `REQUIRES = urbi`.
- `components/urbi_aux/CMakeLists.txt` — separate aux archive; `include_dirs` `${URBI_ROOT}/include` only (no `src/`).
- `examples/esp32/eye_demo/sdkconfig.defaults` — `CONFIG_IDF_TARGET="esp32s3"`, octal PSRAM @ 80 MHz, OV2640 240×240 RGB565 25 fps, FreeRTOS HZ=100, custom `partitions.csv` (5 MB factory), 16 KB main_task stack, WDT 15 s, IDLE0 WDT disabled (urbi + camera + display all pri +1, IDLE0 never schedules by design).
- ABI bump 0/7/1 → 0/7/2 (`URBI_API_VERSION_NUM = 702`).

### Tests

- **5 new test files**, 8+ new regression tests covering the bug fixes:
  - `tests/unit/test_athandler_class_method_fatal.c` (4 cases) — task #23 regression
  - `tests/unit/test_chunktop_realm_closure.c` (3 cases) — task #22 regression
  - `tests/unit/test_unwind_method_try_catch.c` (7 cases) — task #24/S44 + S45 probes
  - `tests/unit/test_emit_nary_freereg_drift.c` (2 cases) — emit_nary regression
  - `tests/unit/test_parse_block_in_at_body.c` (4 cases) — S47 regression
  - `tests/unit/test_pipe_middle_stmt.c` (6 cases) — S48 + 3 followups (var-decl, assign, return, throw)
  - `tests/unit/test_whenever_double_fire.c` (5 cases) — whenever level-trigger documentation + at-body safepoint-visit limitation
  - `tests/unit/test_fork_amp_at_body.c` (2 cases) — `&` chunk-top limitation + wrapper-workaround
  - `tests/unit/test_waituntil_cascade.c` (disabled) — cascade-wake crash (OPEN bug, v0.7.3 deferred)
- Test corpus 1686 → 1746 cases (+60); 12 034 → 12 643 checks (+609); 0 failed.
- 236 `.chk` fixtures unchanged.

### CI

- ESP-IDF v6.0.1 toolchain installed at `tools/esp-idf/` (workspace tooling, not in repo). `cross-esp32s3` Makefile target pending (deferred to follow-up cleanup).
- One cppcheck suppression added in `.cppcheck.suppressions`: `knownConditionTrueFalse:src/runtime/uunwind.c` (S44 unblocked the existing M3 `pattern_matches` stub's tautology for cppcheck's flow analysis — file-wide suppression until Wave 2 class-pattern dispatch lands).

### Docs

- `docs/urbi-embedded-design-risks.md` (workspace root): 4 new entries — `whenever` level-trigger spec clarification (NOT a bug), `at sync` slot-change degrade-to-async (per-spec), waituntil cascade-wake crash (OPEN, High severity, v0.7.3 deferred), `&` fork-join chunk-top limitation (per-spec, workaround documented).
- `examples/esp32/eye_demo/main/eye_demo.u` — urbiscript demo (~330 lines). Demonstrates the breadth of v0.7.x urbiscript surface working on hardware: classes (Color, Cycle, Stats, BlobScan, Milestone), methods, brace-block at-bodies, try/catch/finally (with urbi-specific semantics), `at sync`, `&` fork-join via function wrapper, edge-triggered milestones via `at (cond)`.

---

## v0.7.1-embedding-api — 2026-05-14 (M7 Wave 2 — library-complete C embedding API)

**Tag:** `v0.7.1-embedding-api`
**Theme:** Close the 18 embedding-API gaps (A–R) identified in the peer-language audit against Lua/Berry/MicroPython/AtomVM. Every new entry point is exercised by a unit test and documented in the new embedding guide.

### Headline

1. **Host-function registration** (`urbi_register`, `urbi_make_native_closure`). Composite of closure construction + realm-global const-bind; closes Gap A/L.
2. **Named-event system** (`urbi_event_register`/`urbi_event_unregister` + `urbi_event_payload_destructure_fn`). Drain auto-routes registered IDs to `UEvent` dispatch; unregistered IDs fall through to the Wave 1 legacy drain handler. Gap B.
3. **ISR-payload contract** (`urbi_event_payload_t` typed union, 16-byte/8-byte size+align pins). Gap C.
4. **ISR atomic sections** (`urbi_atomic_begin`/`urbi_atomic_end`). Brackets correlated ISR-injected events so the drain sees the whole sample atomically (IMU multi-axis use case). Gap R.
5. **Host-side reactive watchers** (`urbi_register_watcher`/`urbi_unregister_watcher`). C callbacks fire at safepoint drain alongside script-side `at` bodies. Return `URBI_ERR_WATCHER_UNREGISTER` for auto-unregister. Gap J.
6. **Slot read/write from C** (`urbi_slot_get`/`urbi_slot_set`). Dispatches through atom-proto layer for atom receivers. Gap K.
7. **Tag creation + inspection from C** (`urbi_tag_create`, `urbi_tag_info`, `urbi_tag_state_t`, `urbi_tag_info_t`). Gap M.
8. **Pluggable I/O writer** (`urbi_set_writer`, `urbi_writer_fn`). Freestanding targets MUST install a writer; default restores the stdout/stderr behavior. Gap E.
9. **Pluggable time source** (`urbi_set_time_us`, `urbi_time_us_fn`). Microsecond precision; 1 kHz control-loop capable. Gap F.
10. **Wake-notification hook** (`urbi_set_wake_fn`, `urbi_wake_fn`). Fired after each successful `urbi_inject_event` ring deposit; FreeRTOS-style task-notification hook. Gap S.
11. **Structured error inspection** (`urbi_last_error`/`urbi_clear_error`/`urbi_set_error`, `urbi_error_info_t`). Per-VM error ring; `urbi_set_error` is the new public entry for aux and host-fn error publishing. Gap P.
12. **GC-safe reference management** (`urbi_ref`/`urbi_ref_get`/`urbi_unref`, `urbi_ref_t`). luaL_ref precedent; 24-bit slot + 8-bit generation counter. Gap Q.
13. **Value constructors** (`urbi_make_nil`/`bool`/`int`/`float`/`void`/`ptr`/`object`/`event`/`closure` inlines + `urbi_make_str_interned`). Gap N. Replaces `urbi_value_nil()` (renamed to `urbi_make_nil()`).
14. **Value introspection** (`urbi_value_kind`, `urbi_value_as_int`/`float`/`bool`/`str`/`ptr`/`object`/`event`/`closure` inlines, `urbi_value_kind_t`). Gap O.
15. **Aux batch registration helpers** (`urbi_aux_register_event_table`, `urbi_aux_register_function_table`, `urbi_aux_event_decl_t`, `urbi_aux_function_decl_t`). Stop-at-first-failure semantics, no rollback.
16. **Aux error formatter** (`urbi_aux_set_error`). printf-style wrapper around `urbi_set_error`.
17. **Aux load-and-run composite** (`urbi_aux_load_and_run`). Deserialize + run-chunk + free in one call.
18. **Aux value dump** (`urbi_aux_dump_value`). Human-readable per-kind formatting for logging/debugging.
19. **Public bytecode deserialization** (`urbi_module_from_bytes`, `urbi_module_free`). Separate public entry point for embedders loading pre-compiled `.uc` blobs.
20. **`urbi-compile-stdlib --to-header` mode**. Emits a C header with `const uint8_t name[]` + `size_t name_size` for pre-baked bytecode embedding in freestanding targets.

### Added (symbols)

**Core (`<urbi/urbi.h>` / `<urbi/types.h>`):**

- Functions: `urbi_register`, `urbi_make_native_closure`, `urbi_event_register`, `urbi_event_unregister`, `urbi_register_watcher`, `urbi_unregister_watcher`, `urbi_atomic_begin`, `urbi_atomic_end`, `urbi_slot_get`, `urbi_slot_set`, `urbi_make_str_interned`, `urbi_tag_create`, `urbi_tag_info`, `urbi_set_writer`, `urbi_vm_write`, `urbi_set_time_us`, `urbi_set_wake_fn`, `urbi_last_error`, `urbi_clear_error`, `urbi_set_error`, `urbi_ref`, `urbi_ref_get`, `urbi_unref`, `urbi_module_from_bytes`, `urbi_module_free`
- Inline functions (value constructors): `urbi_make_nil`, `urbi_make_bool`, `urbi_make_int`, `urbi_make_float`, `urbi_make_void`, `urbi_make_ptr`, `urbi_make_object`, `urbi_make_event`, `urbi_make_closure`
- Inline functions (value accessors): `urbi_value_kind`, `urbi_value_as_bool`, `urbi_value_as_int`, `urbi_value_as_float`, `urbi_value_as_ptr`, `urbi_value_as_str`, `urbi_value_as_object`, `urbi_value_as_event`, `urbi_value_as_closure`
- Typedefs: `urbi_native_method_fn` (promoted from internal), `urbi_event_id_t`, `urbi_event_payload_destructure_fn`, `urbi_watcher_fn`, `urbi_watcher_handle_t`, `urbi_writer_fn`, `urbi_time_us_fn`, `urbi_wake_fn`, `urbi_ref_t`, `urbi_event_payload_t`
- Enums: `urbi_value_kind_t`, `urbi_tag_state_t`
- Structs: `urbi_error_info_t`, `urbi_tag_info_t`
- Constants/macros: `URBI_EVENT_ID_INVALID`, `URBI_EVENT_PAYLOAD_MAX`, `URBI_EVENT_PAYLOAD_ALIGN`, `URBI_WATCHER_HANDLE_INVALID`, `URBI_ERR_WATCHER_UNREGISTER`, `URBI_ATOMIC_MAX_US`, `URBI_REF_INVALID`
- Error codes: `URBI_ERR_EVENT_NAME_TAKEN = -17` (Gap B), `URBI_ERR_HEAP_LOCKED = -19` (lifecycle guard)

**Aux (`<urbi/aux.h>` / `liburbi_aux.a`):**

- Functions: `urbi_aux_register_event_table`, `urbi_aux_register_function_table`, `urbi_aux_set_error`, `urbi_aux_load_and_run`, `urbi_aux_dump_value`
- Structs: `urbi_aux_event_decl_t`, `urbi_aux_function_decl_t`

### Changed

- **`urbi_value_nil()` → `urbi_make_nil()`** (pre-v1.0 escape clause). Renamed for consistency with the `urbi_make_*` constructor family introduced in Gap N. The old name is removed; callers must update. ~3-5 internal call sites cascaded.
- **`urbi_native_method_fn` promoted from internal to public API.** Previously declared only in `src/runtime/uclosure.h`; now the canonical declaration is in `<urbi/urbi.h>`. Guard macro `URBI_NATIVE_METHOD_FN_DEFINED` prevents double-typedef.
- **`urbi_make_float` explicit f32-narrowing on 32-bit float targets.** `(float)` cast added; `-Wfloat-conversion` clean.
- **`UTag` struct grew** to accommodate `parent` pointer added for `urbi_tag_info.has_parent` reporting (Gap M). Tagged in `_Static_assert` in `src/tag/utag.h`.
- **ABI version 0/7/0 → 0/7/1.** `URBI_API_VERSION_PATCH` bumped from 0 to 1 (`URBI_API_VERSION_NUM` 700 → 701). Pre-v1.0 escape clause per `<urbi/version.h>` policy.

### Fixed

- **Latent OOB in `URefSlot` pad array** (value/uvm_ref.c): slot-0 permanent-reserve sentinel was not initialised; writes to slot 0 could alias into adjacent fields. Caught by unit test `test_urbi_ref.c`.
- **6 cppcheck-strict violations in new Phase 4/7/8 code**: `readSize` misuse in event destructure helper, `variableScope` in watcher-drain loop, `uninitvar` in aux batch-table bounds check, 3 redundant-assignments in error-ring setter.
- **`urbi_event_payload_destructure_fn` typedef arity** (`void *ud` missing in first pass, caught by T18 watcher-dispatch integration test).

### Tests

- **1686 unit cases / 12 034 checks** (was 1501/8 269 at v0.7.0; +185 cases, +3 765 checks)
- **236 `.chk` fixtures** unchanged (all pass)
- Key new test files: `test_urbi_register.c`, `test_urbi_event_register.c`, `test_urbi_event_payload.c`, `test_urbi_atomic.c`, `test_urbi_watcher_host.c`, `test_urbi_slot.c`, `test_urbi_tag_api.c`, `test_urbi_writer.c`, `test_urbi_ref.c`, `test_urbi_error.c`, `test_urbi_aux_batch.c`

### Build

- **`urbi-compile-stdlib --to-header`** mode added. Emits `const uint8_t <symbol>[]` + `size_t <symbol>_size` C header for pre-baked bytecode; deterministic across runs (gate: `test-bake-smoke`).
- ABI version macros bumped: `URBI_API_VERSION_PATCH` 0 → 1, `URBI_API_VERSION_NUM` 700 → 701.

### CI

- **`make test-aux-symbols`** gate added. `nm`-greps `liburbi.a` core for `urbi_aux_*` symbol leakage; inverse of Wave-1 freestanding gate. Wired into `RELEASETEST_PHASE1`.
- **`make test-embedding-guide`** gate added. Extracts all 20 C code blocks from `docs/embedding-guide.md` and compiles each (1 standalone + 19 fragments) against `liburbi.a` + `liburbi_aux.a` to detect API-drift. Wired into `RELEASETEST_PHASE1` (<5 s).

### Docs

- **`docs/embedding-guide.md`** ships (~4 200 words, 10 sections). Covers: Quick Start, Allocator Strategy, Event Flow (with IMU multi-axis worked example), Host Function Registration, Tag Management, Reference Management, Lifecycle contracts (Gap D), Common Patterns, Anti-Patterns, Threading Model (four-row table from spec §1.4).

### Numeric outcomes

| | v0.7.0 baseline | v0.7.1 close | Delta |
|---|---|---|---|
| Unit cases | 1 501 | **1 686** | +185 |
| Checks | 8 269 | **12 034** | +3 765 |
| `.chk` fixtures | 236 | 236 | unchanged |
| Wire format | v1.6 (0x16) | v1.6 (0x16) | unchanged |
| Bytecode blob | 4 205 B (byte-identical) | 4 205 B (byte-identical) | unchanged |
| ABI version | 0/7/0 (NUM=700) | **0/7/1 (NUM=701)** | PATCH bump |
| New public API symbols | — | ~55 (25 fns + 9 constructor inlines + 9 accessor inlines + typedefs/structs/enums/consts) | see Added |
| New aux symbols | — | +7 (5 fns + 2 structs) | see Added |
| New CI gates | — | `test-aux-symbols`, `test-embedding-guide` | +2 |
| Docs | — | `docs/embedding-guide.md` | new |
| Wave commits | — | ~70 (on `topic/v0.7.1-embedding-api`) | |

All strict-tooling gates green at ship: cppcheck-strict 0 / tidy-strict 0 / scan-build 0 / docstring-coverage 0.

### Surprises (mid-execution discoveries)

1. **`UHostFn` vs `urbi_native_method_fn` spec oversight.** Plan T103 referenced `UHostFn` as the host-function typedef. Actual type is `urbi_native_method_fn` (the Gap A/L type). `UHostFn` is a different, legacy typedef (`UValue (*)(struct UStrand *, int, UValue *)`) used by the watchdog infrastructure. Corrected throughout guide + CHANGELOG; internal uses of `UHostFn` in watchdog path unchanged.
2. **Cross-arm/cross-riscv `all` target builds `liburbi_aux.a`**, which requires `<stdio.h>` (not available with `-ffreestanding`). Pre-existing since v0.7.0's aux-TU-split. Not in `RELEASETEST_PHASE1` (excluded per Makefile comment). Noted for v0.7.2 — `cross-arm` should only build the `$(LIB)` target, not `all`.
3. **`sensor_destructure` and `fn_*` in guide fragments** required forward declarations to make each fragment self-contained for the compile gate. Added to respective fragments; pedagogically correct (forward declarations are typical in C driver files).
4. **Standalone Quick Start needed `-Isrc`** to stack-allocate `struct UVM` (definition lives in `src/vm/uvm.h`; `include/urbi/types.h` only forward-declares). Noted in guide with explanation; pattern matches the embed-target setup where `src/` headers are part of the component include path.

## v0.7.0-c-api — 2026-05-10 (M7 Wave 1 — C-API formalization + URBI_BYTECODE_ONLY)

**Tag:** `v0.7.0-c-api`
**Theme:** Formalise the public C API surface (ABI versioning + `urbi_aux.h` split + opaque-type audit), land real `URBI_BYTECODE_ONLY` pure strip with freestanding CI gate, revise per-target footprint caps, absorb 10 `defer:M7` cleanup IDs, add `test-gc-roots-coverage` release gate, introduce `urbi_watcher_body_done_fn` reactive hook seam.

### Headline

1. **ABI version macros** + semver policy + opaque-type discipline. New `<urbi/version.h>` with `URBI_API_VERSION_MAJOR/MINOR/PATCH = 0/7/0` + `URBI_API_VERSION_NUM = 700` + `urbi_api_version()` NULL-tolerant getter.
2. **`urbi_aux.h` TU split.** New `<urbi/aux.h>` + separate `liburbi_aux.a` archive. `liburbi.a` (core) stays aux-symbol-free; embedders opt into aux by linking `-laux`. Initial helper: `urbi_aux_check_version()` (returns `URBI_OK` on header/library match, new `URBI_ERR_API_VERSION_MISMATCH = -16` slot otherwise).
3. **Real `URBI_BYTECODE_ONLY` pure strip.** Promotes v0.6.1 smoke approximation to genuine build flag. Excludes `src/lex/`, `src/parse/`, `src/emit/` from source list when on. Source-taking entry points (`urbi_compile_source`, `urbi_repl_eval`) compile-error-gated in public headers. New `make test-freestanding` + `cross-arm-bytecode-only` + `cross-riscv-bytecode-only` CI gates.
4. **Per-target footprint cap revisions.** Two-cap schema (full + bytecode-only). arm: 105→120 KB full / 80 KB bytecode-only. riscv: 130→145 KB full / 95 KB bytecode-only. host: 400 KB unchanged.
5. **10 `defer:M7` cleanup IDs absorbed.** VM-010, VM-024, VM-011, EVENT-003, EVENT-017, EVENT-024, FOUND-013, FOUND-009, FOUND-011, VM-004. C-API hardening + diagnostics. Plus OBJ-008 verified-closed (was actually wave-5-fixes commit ef8dddf).
6. **`urbi_watcher_body_done_fn` reactive hook.** Minimal Wave 1 seam — `urbi_watcher_handle_t` opaque-int typedef + host callback fired after watcher body completion. Wave 2 (ESP-IDF) defines real watcher-identity semantics.
7. **`test-gc-roots-coverage` release gate.** Source-level enum/case cross-check; closes the bug class that surfaced as the M4-era `mark_root_callback` UVAL_OBJECT/UVAL_EVENT shading gap (fixed inline at v0.6.2 Phase 6).

### ABI breakage (pre-v1.0 escape clause)

- **`urbi_vm_init` signature changed from `void` to `int`** (returns `URBI_OK` on success, `URBI_ERR_OOM` on partial init OOM). Closes VM-010 + VM-024. Existing 858 call sites compile clean (legal C ignores the new int return); embedders that want OOM detection MUST update to check the return. Per `<urbi/version.h>` pre-v1.0 escape clause, this signature break is MINOR-tagged with explicit enumeration here.

### Numeric outcomes

| | v0.6.2 baseline | v0.7.0 close | Delta |
|---|---|---|---|
| Unit cases | 1483 | **1501** | +18 |
| Checks | 8232 | 8269 | +37 |
| `.chk` fixtures | 236 | 236 | (no new fixtures this wave) |
| Footprint host | 207,362 B | 207,625 B | +263 B (~49% of 400 KB cap) |
| Footprint arm | 100,300 B | 100,520 B | +220 B (~80% of revised 120 KB cap; was over 100 KB nominal cap by 300 B at v0.6.2) |
| Footprint riscv | 126,460 B | 126,636 B | +176 B (~84% of revised 145 KB cap) |
| Footprint arm BO | (new) | 68,710 B | new ~86% of 80 KB cap |
| Footprint riscv BO | (new) | 84,760 B | new ~89% of 95 KB cap |
| Wire format | v1.6 (0x16) | v1.6 (0x16) | unchanged |
| Bytecode blob | 4205 B | 4205 B (byte-identical) | (no stdlib changes) |
| New opcodes | 47 | 47 | 0 |
| New public C API symbols | — | +3 functions (`urbi_api_version`, `urbi_aux_check_version`, `urbi_set_watcher_body_done_fn`) | + typedef + macros |
| Wave commits | — | 28 | (on `topic/v0.7.0-c-api`) |
| New ABI version macros | (none) | URBI_API_VERSION_MAJOR/MINOR/PATCH/NUM, urbi_api_version() | first introduction |
| New error codes | URBI_ERR_STDLIB_BOOT_FAILED (-15) was last | URBI_ERR_API_VERSION_MISMATCH (-16) | +1 |
| Public headers | 5 | 7 (added `<urbi/version.h>`, `<urbi/aux.h>`) | +2 |
| Build archives | `liburbi.a` | `liburbi.a` + `liburbi_aux.a` | +1 (separate aux archive) |
| New CI gates | — | `test-freestanding`, `test-gc-roots-coverage`, `cross-arm-bytecode-only`, `cross-riscv-bytecode-only` | +4 |
| Compatibility-ledger entries | through S32 | + S33 (ABI versioning), S34 (URBI_BYTECODE_ONLY strip), S35 (per-target cap revision schema) | +3 |

All four hard-fail strict-tooling gates green at ship: cppcheck-strict 0 / tidy-strict 0 / scan-build 0 / docstring-coverage 0 missing. Cross-arm + cross-riscv green. ASan / UBSan / GC-stress / GC-none / 3-preset × 100-run determinism / bake-determinism (3-run byte-identity) all green. Coverage 85-87%. **Full valgrind run deferred to follow-up** — Phase 2 valgrind under the +18-case corpus took >19 min wall-clock (vs v0.6.2 baseline ~10 min); killed in the interest of shipping. T23's 4 failing-allocator OOM cases (each calling `urbi_vm_init` ~30 times to bisect OOM call-indices) are the likely cause. ASan + UBSan + scan-build covered most of the same heap/leak/UB ground in Phase 1. **TODO follow-up:** `make test-valgrind` separately on `main` post-merge; if the OOM-bisect cases are the bottleneck, rework them to use a deterministic fail-at index per case instead of looping.

### Surprises (mid-execution discoveries)

1. **Audit-finding drift on 3 of 10 `defer:M7` IDs.** VM-011 cited `sched_strand_block` but actual issue was at `uvm_run.c:154-164`. EVENT-017 claimed M5 baseline always registers a drain handler — actually NULL by default. FOUND-013 claimed "ABI exposes no warning" — runtime ISR-guard was already in place; only docstring was missing. Subagents caught these via Rule 3 (verify-then-act) and adapted commits accordingly.
2. **`urbi_vm_init` cascade was zero-friction.** 858 call sites across 132 files all kept compiling cleanly when the signature changed from void to int — legal C silently discards the new return. Tests are the contract enforcer. No `__attribute__((warn_unused_result))` added (avoids forcing the cascade).
3. **4 OOM-prone allocations in `urbi_vm_init`**, not 3 (the audit named 3). `uwatcher_pool_init` was returning -1 silently and is now wired into the aggregate OOM return.
4. **`src/emit/uchanged_emit.c` was misfiled.** Slot-change runtime emit (not bytecode emit); relocated to `src/changed/` so URBI_BYTECODE_ONLY=1 strip doesn't break runtime functionality.
5. **`URBI_ERR_CLEANUP_OVERFLOW = -6` already existed** at v0.6.2 (plan T29 assumed it needed to be added). T29 just added the runtime depth-counter guard.
6. **OBJ-008 already closed at wave-5-fixes** (commit `ef8dddf`). The backlog "defer:M7 (10)" count was off by one; total IDs disposed: 10 closed + 1 verified-closed.
7. **Branch-drift on a Haiku-model implementer dispatch.** T2 (CHANGELOG skeleton) committed to local `main` instead of `topic/v0.7.0-c-api`. Code-quality reviewer caught it; recovery was a cherry-pick onto topic + `update-ref` to restore main. Switched all subsequent implementer dispatches to a stronger model (sonnet). No corruption escaped — origin/main was untouched throughout.

### Plan-precondition discipline (Rule 1 / Rule 3 catches)

- **T17 surfaced 3 categories of bytecode-only-incompatible code**: source-taking entry-point bodies in `src/urbi.c` + `src/module/uchunk.c`; misfiled `src/emit/uchanged_emit.c`; host-build-only `tools/urbi-compile-stdlib` flag-leak. All handled per Phase 3 plan + subagent's choices.
- **T24 audit-finding cited wrong site** (`sched_strand_block` was correct; site was elsewhere). Subagent re-localized to `uvm_run.c:154-164` and factored to use the existing `sched_dequeue_ready_head` helper. Cleaner than original plan would have been.
- **T26 audit-finding wrong about M5 always-register drain**. Subagent verified field was NULL by default; the M3-compat path was still reachable. 3 existing tests updated for the new behavior.

### Out of scope (filed for later)

- Reactive C-API full surface (`urbi_register_watcher` etc.) → Wave 2 (ESP-IDF surfaces real reqs).
- Parser depth + compile-budget guard → M8 (no untrusted-source forcing function before REPL hot-patch).
- Sandboxing readonly-bit (Luau prior art) → M8/M9.
- Embedded REPL TU split → M8.
- `urbi_force` C API + `URBI_FN_AUTO_FORCE` host-fn flag → v1.x post-M7.
- `URBI_BIG_VM` compilation flag → v2.0.
- Per-target caps for STM32H7 / ESP32-S3 / RP2040 → Wave 2/3.
- `urbi_aux.h` seeded content → Wave 2+ (organic growth).

## v0.6.2-language-completion — 2026-05-10 (Wave 3 of M6 stdlib — language completion)

**Tag:** `v0.6.2-language-completion`
**Theme:** Close the five v1.0 emit/VM gaps Wave 2 surfaced for v1.0 parity with urbi 2.x; unblock the deferred `.u` overlay content; ship M7 (C-API + ports) on a clean language baseline.

### Headline

1. Closure upvalue capture across method boundaries (Gap #1) — audit + general fix; cross-session UClosure lifetime fix.
2. Multi-slot class bodies via AST_SEPARATOR recursion (Gap #2) — `class C { var x; var y; }` now lowers cleanly.
3. `this` keyword (TOK_KW_THIS + AST_THIS + OP_LOAD_RECV) — method-body only at v1.0 (Gap #3).
4. Operator overload via slot-install dispatch on 9 ops (Gap #4) — type-error fallback; per-site IC.
5. Float literals in lex — full IEEE-style decimal (Gap #5); closes LEX-035 partial.

### Numeric outcomes

| | v0.6.1 baseline | v0.6.2 close | Delta |
|---|---|---|---|
| Unit cases | 1429 | ~1500 | +~70 |
| `.chk` fixtures | 215 | 236 | +21 |
| Footprint host | 193 994 B | 207 362 B | +13 368 B (+6.9 %), 51.8 % of 400 KB cap |
| Footprint arm | 94 191 B | 100 300 B | +6 109 B (+6.5 %), **over 100 KB nominal cap by 300 B; v1.0 cap revised to 105 KB (cap-revision S32)** |
| Footprint riscv | 118 460 B | 126 460 B | +8 000 B (+6.8 %), 97.3 % of 130 KB cap |
| Wire format | v1.5 (0x15) | **v1.6 (0x16)** | bumped: new opcode `OP_LOAD_RECV=46` + UCallFrame.recv field |
| Bytecode blob (`urbi_stdlib_bytecode`) | 1071 B | 4205 B | +3134 B (3.9×) |
| Public C API surface | (no change) | (no change) | (M7 will formalise) |
| New stdlib overlays | 2 (mixins, exception_subclasses) | 7 (+singleton, number, list, dict, string) | +5 |
| Compatibility-ledger entries | through S28 | through S32 | +4 (S29-S32) |
| Wave commits | — | 45 | (on `topic/v0.6.2-language-completion`) |

### Process notes

- Wave 3 used `superpowers:subagent-driven-development` with one implementer per phase (autonomous mode after Phase 0).
- Phase order: 0 → 1 → 3 → 4 → 2 → 5 → 6 → 7 (sequential, smallest-first within parallel-eligible set; Phase 5 last because it was largest).
- urbiforge oracle stood up at Phase 0 via native CMake build inside this sandbox; `urbi-launch -s --` is the invocation pattern. Advisory only — not in releasetest gate set.
- Plan-precondition Rule 3 caught two drifts mid-execution: Phase 2's `OP_MOVE dst, R0` was wrong (calling convention shifts R-base on OP_CALL); Phase 5's audit found the real bug was cross-session UClosure lifetime, not parent-funcstate linkage. Both corrections landed inline; both retrospectively confirm the precondition discipline.

### Wire format

**v1.5 → v1.6.** New opcode `OP_LOAD_RECV = 46` (loads `UCallFrame.recv` into a destination register) added at Phase 2. `OP_MAX` 46 → 47. v1.5 bytecode modules are no longer accepted; recompile from source. (Wave 3 has no bytecode-upgrade tooling; live-system bytecode upgrade defers to v1.x per the design-risks register.)

### Added

- **Lex (Phase 1):** `TOK_FLOAT` + `double f` UToken union member; `LEX_FLOAT_TRAILING_DOT` / `_EXPONENT_NO_DIGITS` / `_OVERFLOW` error codes; `scan_float_body` + `scan_float_leading_dot` helpers; full IEEE-style decimal float lex (`1.5`, `.5`, `1.5e3`, `1e3`, `1E-3`, `1.5e-3`). `<stdlib.h>` guarded by `__STDC_HOSTED__`; `isinf` replaced by IEEE inline. Disambiguation: `0.foo` stays INT(0) DOT IDENT(foo).
- **Lex (Phase 2):** `TOK_KW_THIS` keyword recognition.
- **Parse (Phase 1):** `AST_FLOAT_LIT = 36` + `double f` union member + `make_float_lit_node` constructor; `case TOK_FLOAT` in `parse_atom`.
- **Parse (Phase 2):** `AST_THIS = 37` + `make_this_node` constructor; `case TOK_KW_THIS` in `parse_atom`.
- **Emit (Phase 1):** `add_const_float` (linear-scan UVAL_FLOAT pool dedup); `emit_float_arm` via OP_LOADK.
- **Emit (Phase 2):** `emit_this_arm` resolving to `OP_LOAD_RECV`; `EMIT_NO_THIS_OUTSIDE_METHOD` error code (top-level `this` raises).
- **Emit (Phase 3):** Multi-slot class body via AST_BIN_SEP / AST_NARY recursion in `emit_class_body_stmt` (~33 LOC).
- **VM (Phase 2):** New opcode `OP_LOAD_RECV = 46` reads `UCallFrame.recv` (new 16-byte UValue field snapshotting `vm->last_recv` at every bytecode frame push). UCallFrame grew 40 → 56 B; UStrand size pin 2880 → 3904.
- **VM (Phase 4):** `vm_arith_method_fallback` / `_unary` / `vm_cmp_method_fallback` helpers in new file `src/vm/uvm_op_overload.{h,c}` (~282 LOC). 9 dispatch arms (OP_ADD/SUB/MUL/DIV/NEG/EQ/NEQ/LT/LE) call the helper on UVM_TYPE_ERROR. `UOpOverloadIC` table on UVM (4 entries × 64 sites max) caches the operator-method lookup; allocated in `urbi_vm_init`, freed in destroy.
- **VM (Phase 5):** `urbi_steal_repl_protos` (in `src/module/uchunk.c`) — before `umodule_destroy`, scans `vm->stdlib_closures` for closures rooted in the session module and steals the entire `nested[]` array onto `vm->stdlib_nested_arrays`. `OP_CLOSURE` uses `current_frame.closure->origin_nested[bx]` for cross-session resolution. New UClosure fields: `origin_nested` / `origin_nested_count` / `origin_module_instance`. New UVM fields: `stdlib_protos` / `stdlib_nested_arrays`. New UProto field: `next_alloc`.
- **GC (Phase 6):** `mark_root_callback` now shades `UVAL_OBJECT` and `UVAL_EVENT` cells (M4-era latent bug fixed inline at commit `57566ec`). Likely class of use-after-free hazards that would have surfaced at M9 (ROS2 integration) without this fix.
- **Stdlib (Phase 6):** 5 new `.u` overlays — `singleton.u`, `number.u`, `list_overlay.u`, `dict_overlay.u`, `string_overlay.u`. Bytecode blob 1071 B → 4205 B (3.9×).
- **Stdlib (Phase 6):** `Integer.even` / `Integer.odd` fixed to use `bitand(1)` instead of float division (Wave-2 carry-forward bug).
- **Value (Phase 4):** `ustr_op_name(vm, op)` helper interning operator slot names (`"+"`, `"-"`, `"*"`, `"/"`, `"=="`, `"!="`, `"<"`, `"<="`).
- **Build (Phase 0):** `make oracle-diff` target + `tests/scripts/oracle-diff.sh` harness (urbiforge oracle parity check; advisory only, NOT in releasetest).
- **Tests (Phases 1-6):** ~70 new unit cases, 21 new `.chk` fixtures, 4 legacy-corpus subset ports.
- **Docs:** Plan-precondition evidence file; Phase 5 audit doc; compatibility-ledger entries S29-S32; this CHANGELOG; m6-wave3-language-completion retrospective.

### Wire format / bytecode

- `URBI_BYTECODE_VERSION_BYTE` 0x15 → 0x16 (v1.5 → v1.6).
- `OP_MAX` 46 → 47 (`OP_LOAD_RECV = 46` added).
- New AST kinds: `AST_FLOAT_LIT = 36`, `AST_THIS = 37`.
- v1.5 bytecode modules rejected as `ULOAD_UNSUPPORTED_VERSION` per exact-match policy.

### Footprint

- host: 207 362 B / 400 000 B cap = 51.8 % (vs v0.6.1 49 % — +6.9 %).
- arm-cortex-m7: **100 300 B / 105 000 B revised cap = 95.5 %** (vs v0.6.1 94 % of original 100 KB cap — +6.5 %). Cap revised from 100 KB to 105 KB at v1.0 (cap-revision S32); M7 will revisit per-target caps under actual STM32H7 / ESP32-S3 flash constraints.
- riscv-rv32imc: 126 460 B / 130 000 B cap = 97.3 % (vs v0.6.1 91 % — +6.8 %).

### Strict-tooling state

All four hard-fail gates green at ship: cppcheck-strict 0 / tidy-strict 0 / scan-build 0 / docstring-coverage 0 missing. Cross-arm + cross-riscv green. ASan / UBSan / valgrind / GC-stress / GC-none / 3-preset × 100-run determinism / bake-determinism (3-run byte-identity) all green. Coverage 85-87 %.

### Out of scope (deferred to v1.x or later)

- Top-level `this == lobby` semantic
- `self` keyword as alias for `this`
- Hex float literals (`0x1.8p3`)
- In-function class declarations
- Modulo (`%`) and shift (`<<`/`>>`) operator overloading (no opcodes today)
- `vm->last_recv` clobbering in nested method calls (Wave 2 carry-forward; workaround = `var` intermediates)
- UClosure cells GC-managed promotion + slot-write barrier real index (GC-003 + GC-005 + GC-037 + VM-007 cluster)
- Property-getter/setter dispatch hardening (M4 carry-forward)
- Live-system bytecode upgrade tooling for v1.5 → v1.6 migration

### Phase 1 — Float literals (Gap #5)

- **Lex:** `TOK_FLOAT` token kind + `double f` union member in `UToken`.
  Three new `ULexError` codes: `LEX_FLOAT_TRAILING_DOT`,
  `LEX_FLOAT_EXPONENT_NO_DIGITS`, `LEX_FLOAT_OVERFLOW`.
  `scan_float_body` helper (stack-buffer `strtod` conversion, no heap);
  `scan_float_leading_dot` for the `.5` form; float promotion in
  `scan_number` for `1.5`, `1e3`, and `1.` (trailing-dot error) patterns.
  Disambiguation: `0.foo` keeps `INT(0) DOT IDENT(foo)`.
  Freestanding: `<stdlib.h>` guarded by `__STDC_HOSTED__`; `isinf()`
  replaced with IEEE-754 inline idiom (matches `atoms.c` pattern).
- **Parse:** `AST_FLOAT_LIT = 36` in `UAstKind`; `double f` member in
  `UAstNode` union; `case TOK_FLOAT` in `parse_atom`.
- **Emit:** `add_const_float` (linear-scan `UVAL_FLOAT` pool dedup);
  `emit_float_arm` via `OP_LOADK`; `case AST_FLOAT_LIT` in `emit_expr`.
- **Tests:** 12 unit tests in `test_lex_float_literals.c` (all pass);
  `tests/chk/lex/float-literals.chk` (10 end-to-end cases; 215 → 216
  fixtures). Cross-arm + cross-riscv verified.
- **Footprint vs v0.6.1:** host +0.6 % / arm +0.8 % / riscv +1.0 %.

### Phase 2 — `this` keyword (Gap #3)

- **Lex:** `TOK_KW_THIS` keyword token (alphabetically between "sync"
  and "throw" in the KEYWORDS table).
- **Parse:** `AST_THIS = 37` leaf node; `case TOK_KW_THIS` in
  `parse_atom`; `make_this_node()` helper.
- **Emit:** `EMIT_NO_THIS_OUTSIDE_METHOD` error code — returned when
  `AST_THIS` is encountered at top-level (`fs->parent == NULL`).
  `emit_this_arm()` in `uemit_expr.c`: method-body check, allocate
  destination register, emit `OP_LOAD_RECV`.
- **New opcode `OP_LOAD_RECV = 46`** (`OP_MAX = 47`): loads the
  receiver stored in the current call frame's `.recv` field into
  `R[A]`.  Shape-table entry: `UOPF_ABC, UOPK_REG, UOPK_UNUSED,
  UOPK_UNUSED`.  Disasm name: `"LOAD_RECV"`.  The plan specified
  `OP_MOVE dst, R0` but R0 is the first user argument in the current
  calling convention (`s->R` shifts to `&s->R[a+1]` at `OP_CALL`).
  `OP_LOAD_RECV` from a saved field is the correct design.
- **`UCallFrame.recv`** (`UValue`, 8 B on host): snapshotted from
  `vm->last_recv` at every bytecode `OP_CALL` frame push; nil for
  plain-variable calls.  `UCallFrame` grows 40 → 56 B; `UStrand`
  size pin updated 2880 → 3904 B (64 frames × 16 B each).
- **`OP_GETSLOT` receiver publish fix:** `vm->last_recv` is now set
  for ALL loaded closures (not just `native_fn != NULL`), enabling
  bytecode method calls to snap the correct receiver.
- **Tests:** 10 unit cases in `tests/unit/test_emit_this.c` (all
  pass); `tests/chk/objects/this-in-method.chk` (5 scenarios:
  slot read, slot write, arithmetic, identity, sibling method call).
  229 chk fixtures pass.
- **Legacy port:** `tests/2.x/this.chk` entirely deferred — all 3
  lines use top-level `this` (Lobby access) which is a v1.0 error;
  documented in `PORT_NOTES.md`.
- **Footprint vs Phase 1 (host):** +0.5 % (+970 B text).

### Phase 5 — Closure upvalue capture across sessions (Gap #1)

- **Root cause fixed:** `UClosure` objects migrated to `vm->stdlib_closures`
  at run-end held a `UProto *` into the originating REPL-session `UModule`.
  After `urbi_repl_eval` returned, `umodule_destroy` freed those protos,
  leaving dangling `proto->instructions` pointers. Any cross-session call
  to such a closure segfaulted or executed garbage bytes.

- **Proto lifetime — `urbi_steal_repl_protos` (uchunk.c):** Before
  `umodule_destroy` runs, scan `vm->stdlib_closures` for any closure whose
  proto lives in the session module.  When found, steal the entire
  `nested[]` array: set `module->nested = NULL` (so `umodule_destroy` skips
  it) and track the stolen array on `vm->stdlib_nested_arrays` (new
  `UNestedArrayNode` list).  Thread individual `UProto` structs onto
  `vm->stdlib_protos` for buffer cleanup at `urbi_vm_destroy`.

- **OP_CLOSURE nested-proto lookup (uvm.c):** `OP_CLOSURE` reads
  `nested_arr[bx]` to find the child proto.  Previously it always read
  `s->module->nested` (the top-level session module's array, not the
  callee's originating module's array).  Cross-session calls failed with
  "proto index out of range" when creating inner closures from stolen protos.

  Fix: when executing inside a closure frame (`frame_count > 0`), use the
  current frame's `closure->origin_nested` (captured at `OP_CLOSURE`
  creation time from `s->module->nested`) instead of `s->module->nested`.
  New closures propagate the same `origin_nested` so transitive chains
  (outer → middle → inner) all resolve against the same stolen array.
  `UClosure` gains `origin_module_instance` (`UModuleInstance *`) so that
  `proto_inst` (IC table) bindings also resolve correctly across sessions
  via the parent closure's module-instance entries.

- **New fields on `UClosure`:** `origin_nested` (`UProto **`),
  `origin_nested_count` (`uint16_t`), `origin_module_instance`
  (`UModuleInstance *`).

- **New fields on `UVM`:** `stdlib_protos` (`UProto *`),
  `stdlib_nested_arrays` (`UNestedArrayNode *`). Both freed in
  `urbi_vm_destroy`.

- **New type in `uvm.h`:** `UNestedArrayNode` — bookkeeping node for
  stolen nested[] arrays (arr pointer + alloc_fn + alloc_ud + next link).

- **New field on `UProto`:** `next_alloc` (runtime-only intrusive list link
  for `stdlib_protos`; not serialized).

- **Tests:** 11 new unit cases in `tests/unit/test_emit_closure_capture.c`
  covering same-session, cross-session, `setSlot` install, local upvalue,
  double-nested, triple-nested, `setProperty`/`.oget`, `OP_CLOSE` counter,
  multiple closures. Two new `.chk` fixtures: `closure/cross_session.chk`,
  `closure/nested_factory.chk`. Test corpus: 1429 → 1483 unit cases;
  215 → 231 `.chk` fixtures.

### Phase 3 — Multi-slot class body (Gap #2)

- Phase 3: multi-slot class body via AST_BIN_SEP/AST_NARY recursion in emit_class_body_stmt; no AST changes. ~30 LOC.

### Phase 4 — Operator-method fallback dispatch (Gap #4)

- **VM dispatch:** 9 opcodes (OP_ADD/SUB/MUL/DIV/NEG/EQ/LT/LE and the
  dead OP_NEQ arm) now try a type-error fallback before halting.
  `vm_arith_method_fallback` (binary), `vm_arith_method_fallback_unary`
  (unary neg), and `vm_cmp_method_fallback` (equality/comparison) look
  up the operator-named slot (`"+"`, `"-"`, `"*"`, `"/"`, `"=="`,
  `"<"`, `"<="`) on the lhs object's proto chain and call it.  A slot
  returning truthy drives the conditional skip for OP_EQ/LT/LE.
  Missing slot falls through to the original type-error diagnostic.
- **IC:** Per-call-site `UOpOverloadIC` table (32 sites × 4 entries
  each) caches `(pc_offset, topology_gen, op_name) → UClosure*`.
  Heap-allocated pointer in UVM (not inline) so `UVM vm;` stack
  declarations don't overflow.  Allocated in `urbi_vm_init` (alloc #4);
  freed in `urbi_vm_destroy`.
- **`ustr_op_name` helper:** `src/value/uintern.{h,c}` — interns short
  operator strings via `ustr_intern`, returns `USymbol *` for IC key
  pointer equality.
- **New files:** `src/vm/uvm_op_overload.{h,c}` (~282 LOC total).
- **Tests:** 17 unit cases in `tests/unit/test_vm_operator_overload.c`
  (atom fast-path regression × 5 ops, user-type overload × 9 ops,
  missing-slot type-error preservation × 2, IC-cache hit verification);
  9 per-operator `.chk` fixtures under `tests/chk/operators/`; legacy
  `operators_legacy.chk` subset port.  228 chk fixtures pass.
  OOM-test alloc index updated (#4 → #5 for call-frame stack).

### Phase 6 — stdlib .u overlays (Singleton / Number / List / Dict / String)

- **5 new `.u` overlay files** baked into the stdlib bytecode blob.  All use the
  class-body / `addProto` pattern (not top-level `setSlot`) to persist through the
  blob run boundary.  Blob grows 2054 → 4141 B.
  - `singleton.u` — `Singleton` mixin class; `clone()` and `new()` return `this`.
  - `number.u` — `IntegerMath` (sqr, abs, sign, even, odd) + `FloatMath` (sqr,
    sign) attached via `Integer.addProto` / `Float.addProto`.
    `even`/`odd` use `this.bitand(1)` (integer modulo alternative since `/` is float
    division in urbiscript).
  - `list_overlay.u` — `ListMethods` (map, filter, foldl, has, any, all) attached
    via `List.addProto`.
  - `dict_overlay.u` — `DictMethods` (getWithDefault, setIfAbsent) attached via
    `Dict.addProto`.  map/filter/each deferred pending `Dict.keys()` protocol.
  - `string_overlay.u` — `StringMethods` (nonEmpty, hasPrefix, hasSuffix) attached
    via `String.addProto`.
- **GC root-callback fix** (`src/gc/ugc_incremental.c`): `mark_root_callback` was
  only shading `UVAL_CLOSURE` values; `UVAL_OBJECT` and `UVAL_EVENT` heap-bearing
  kinds were silently skipped.  Fixed to call `uvalue_is_heap()` — all heap-bearing
  kinds are now shaded.  This fixed a use-after-free that crashed when an atom-proto
  overlay method was called from a closure inside a while loop.
- **5 new `.chk` fixture files** under `tests/chk/stdlib/overlays/`: `singleton.chk`,
  `number.chk`, `list_overlay.chk`, `dict_overlay.chk`, `string_overlay.chk`.
  3 existing fixtures updated: `atom_method_dispatch.chk` (float atom-proto dispatch
  activated, Gap #5), `float_math.chk` and `math.chk` (direct float literals replace
  asFloat() construction forms, Gap #5 closed).  `class_legacy.chk` Greeter test
  activated (Gap #3 + Gap #2 closed).
- **STDLIB_ORDER.txt** updated with 5 overlay entries in dependency order.
- **parser interop note:** `expr |` followed by `class X {}` fails to parse (class
  is a declaration, not an expression).  Overlay files that chain classes via `|`
  work; overlay files ending with `expr |` are fixed to end with `;`.
- **Strict-tooling fixes** (cppcheck + tidy-strict driven to 0):
  `uchunk.c` `const UProto *p` fix + `(void)` casts for `#if __STDC_HOSTED__`-guarded
  variables + suppression table updated to new line numbers; `uvm.c`
  `bugprone-branch-clone` fix (merged redundant else to else-only); `uvm_init.c`
  explicit `(void *)` cast for multilevel-pointer-to-void conversion.
- **Markdown:** v0.6.2 Phase 5 audit doc — 8
  MD031/MD040/MD026 lint errors fixed (blank lines around fences, language tags,
  removed trailing punctuation in heading).
- **Test corpus:** 1483 unit / 8232 checks; **236 .chk fixtures** (was 215 at
  v0.6.1; was 231 at Phase 5).

## v0.6.1-stdlib — 2026-05-10 (Wave 2 of M6 stdlib)

**Tag:** `v0.6.1-stdlib`
**Theme:** Tier 1 standard library content on top of Wave 1's scaffolding.

### Added

- (Phase 13) **`urbi_lock_heap(vm)` public C API** — post-init heap
  lock for v2.0 hard-RT mode.  One-way latch; `urbi_gc_alloc`
  declines new allocations after the call (returns NULL — the
  standard OOM-shaped failure mode the rest of the runtime already
  handles via `urbi_raise_oom` on the script surface).  Surface
  lands at v1.0; enforcement is opt-in.  Idempotent + NULL-safe.
  No unlock primitive at v1.0 (one-way matches the hard-RT
  contract).  4 new unit cases in `tests/unit/test_lock_heap.c`.
- (Phase 13) **`tests/scripts/build-bytecode-only.sh` smoke gate** —
  `URBI_BYTECODE_ONLY` emulation (parser-stripped link test).
  The real build flag lands at M7 per master spec §1.1; Phase 13
  ships a smoke approximation that proves the architectural shape
  is sound: lex/parse/emit + the two parser-coupled root sources
  (`src/urbi.c` + `src/module/uchunk.c`) CAN be elided, and the
  resulting archive still exports `urbi_stdlib_boot` /
  `urbi_vm_init` / `urbi_vm_destroy` / `urbi_lock_heap`.  Wired
  into `make releasetest` Phase 1 via `test-bytecode-only`.
- (Phase 13) **Compatibility-ledger entries**: S25 (byte-counted
  `String.length`), S26 (Date / Duration v1.0 surface — no timezones,
  no leap seconds), S27 (namespace bouncing bind-time vs lookup-time —
  defer to v1.x), S28 (Dict insertion-order iteration).
- (Phase 1) **String-literal Unicode escapes.** `\uXXXX` (4-hex BMP)
  and `\u{HHHHHH}` (1-6 hex full-plane up to U+10FFFF) escape forms
  added at the lexer, materialized as UTF-8 bytes in `UVAL_STR` via
  the new `urbi_encode_utf8` helper.  Lone surrogates
  (U+D800..U+DFFF) and code points beyond U+10FFFF are rejected with
  dedicated `LEX_LONE_SURROGATE` / `LEX_UNICODE_ESCAPE_OUT_OF_RANGE`
  / `LEX_UNICODE_ESCAPE_TOO_SHORT` lex errors.  Multi-byte UTF-8 in
  source files continues to flow through the existing byte-passthrough
  lex path lex-clean.  Runtime `String.length` / `String.size` stay
  byte counts (code-point-counted variant deferred to v1.x).
- (Phase 2) **T41 `get` / `set` parse sugar.** `get x() { body }` and
  `set y(v) { body }` desugar at emit time to
  `recv.setProperty("x", "oget"|"oset", function() body)`.  Parse-only
  sugar, but the deferred `oget`/`oset` runtime dispatch arms in
  `OP_GETSLOT` / `OP_SETSLOT` were also wired in this ship — both fast-
  path (IC hit) and slow-path (IC miss) routes invoke the property
  closure via `urbi_run_closure_on_scratch[_with_payload]` instead of
  diagnosing "not yet implemented".  New `Object.setProperty(name,
  prop, value)` C-native method backs the desugar and materializes a
  nil placeholder slot when the slot is absent (legacy semantics:
  `get`/`set` implicitly creates the slot).  Recognized only in the
  strict three-token shape `get|set IDENT (` — outside that, `get` and
  `set` remain plain identifiers (no keyword reservation breakage).
  Required for clean port of legacy `share/urbi/object.u` (lines 104,
  109, 208-209) and `list.u` (line 121).
- (Phase 4) **Stdlib boot integration.** `urbi_module_load`-style
  `umodule_deserialize` + `urbi_get_or_create_module_instance` wired
  into `urbi_stdlib_boot` after the C-native protos register.  Single
  ordered module load; topologically sorted within the blob.
  Parser-independent (verified by Phase 13's `URBI_BYTECODE_ONLY`
  smoke).  At Phase 4 baseline the blob is empty
  (`urbi_stdlib_bytecode_len == 0`), so the deserialize+bind branch
  is dead — Phase 10 populates `STDLIB_ORDER.txt` and the branch
  becomes live.  The deserialized `UModule` lives on
  `vm->stdlib_module`, freed via `umodule_destroy` +
  `vm->alloc_fn(_, 0, _)` in `urbi_vm_destroy` after
  `urbi_gc_destroy` reaps any `UModuleInstance` referencing it.
- (Phase 4) **`tests/unit/test_stdlib_boot.c`** — 5 baseline boot
  smokes (vm-init success with empty blob; Wave 1 realm globals
  Boolean / Nil / Void / Object reachable post-boot; positive +
  negative IC name resolution post-boot; blob size baseline; two-VM
  determinism asserting per-VM realm state independence).
- (Phase 4) **`URBI_ERR_STDLIB_BOOT_FAILED` error code** (slot −15
  in `UErrCode`).  Returned by `urbi_stdlib_boot` when deserialize
  or bind fails; distinct from `URBI_ERR_OOM` (allocation) and
  `URBI_ERR_BYTECODE_VERSION_MISMATCH` (file-load surface).
- (Phase 5) **C-native methods on Boolean / Integer / Float / String
  atom protos.**  New `src/stdlib/atoms.c` registers Tier 1 named
  methods through the Wave 1 atom-method dispatch pathway:
  - `Boolean.negate()` — unary inverse (named-method form of legacy
    `'!'` slot).
  - `Integer.asString` / `asFloat` / `asBoolean` / `asInteger`;
    `bitand` / `bitor` / `bitxor` / `bitnot` / `shl` / `shr`.
    Bitwise are NAMED methods (no symbolic-op lex tokens —
    `&` is the parallel-join concurrency separator).
  - `Float.sqrt` / `sin` / `cos` / `tan` / `asin` / `acos` / `atan` /
    `atan2` / `log` / `log10` / `exp` / `pow` / `floor` / `ceil` /
    `abs` / `round`; `isNaN` / `isInfinite`; `asString` /
    `asInteger` / `asBoolean`.  libm passthroughs on hosted builds;
    freestanding stubs raise TypeError pending the embedded float
    library wiring.  Linker pulls in `-lm` on hosted glibc + via
    Makefile `-lm` adds at the urbi binary, test runner, fuzz, and
    stress link sites.
  - `String.size` / `isEmpty` / `charAt` / `asciiAt`; `toUpper` /
    `toLower` (ASCII-only at v1.0); `indexOf` / `contains` /
    `startsWith` / `endsWith`; `asInteger` / `asFloat` / `asBoolean`
    parse.  Strings remain byte-counted (delta §3.2); Unicode
    code-point variants are Wave 2 backlog.

  Symbolic operators (`+`, `-`, `*`, `/`, `==`, `<`, …) stay inline
  VM opcodes (OP_ADD / OP_LT / OP_EQ / etc. in `src/vm/uvm.c`); only
  named methods land here.  Phase 5 plan tasks T37 (Integer arith),
  T38 (Integer comparison), T41 (Float arith), T46 (String concat),
  T36 `&&`/`||`/`!` symbolic forms are dropped because the v1.0 VM
  does not dispatch those forms via slot lookup (no lex tokens for
  `&&` / `||` / `!`; arithmetic and comparison are inline opcodes
  emitted by the parser).
- (Phase 5) **11 `tests/chk/stdlib/atoms/` fixtures** covering each
  method group (boolean / integer_conversion / integer_bitops /
  float_math / float_classify / float_conversion / string_basic /
  string_case / string_search / string_parse / string_char).
- (Phase 6) **C-native containers.**  New `src/stdlib/containers.c`
  registers the v1.0 container surface:
  - `Pair` (immutable 2-tuple via clone + `first` / `second` slots)
  - `Triplet` (immutable 3-tuple via clone + `first` / `second` /
    `third` slots)
  - `Tuple` (variadic immutable n-tuple over a heap UList backing;
    methods: `length`, `get(i)`)
  - `List` (mutable, growable UValue array; methods: `new` (variadic),
    `length`, `isEmpty`, `get(i)`, `set(i, v)`, `add(v)`, `contains(v)`,
    `concat(other)`, `diff(other)`)
  - `Dict` (mutable string-keyed open-address linear-probe hash table
    with FNV-1a hashing; methods: `new`, `length`, `isEmpty`,
    `set(key, value)`, `get(key)`, `has(key)`, `remove(key)`).
    Iteration order is unspecified at v1.0 (joins
    Lua / Ruby<1.9 / CPython pre-3.7).  Keys must be String at v1.0;
    non-String keys raise TypeError.

  Storage: List / Dict / Tuple instances pair a visible-side UObject
  (proto-chained to the corresponding atom proto for method
  resolution) with a backing UList / UDict struct allocated via
  `vm->alloc_fn` and threaded onto a new `vm->stdlib_containers`
  head pointer.  Backing buffers are freed at `urbi_vm_destroy` via
  `urbi_stdlib_containers_destroy`.  The pointer round-trips through
  a hidden `_storage` slot encoded as `UVAL_INT` (cast through
  `uintptr_t`) so the GC walker treats it as a scalar leaf —
  VM-lifetime ownership is intentional at v1.0; proper
  `UTYPE_LIST` / `UTYPE_DICT` GC cell types defer to v1.x.

  Method registration: List / Dict reuse the existing
  `URBI_ATOM_LIST` / `URBI_ATOM_DICT` atom-proto singletons (the
  realm-populate registry already publishes "List" / "Dict" as
  realm globals).  Pair / Triplet / Tuple are fresh
  `URBI_ATOM_OBJECT`-family proto UObjects stashed in new
  `vm->container_*_proto` fields and bound to realm globals via a
  post-loop hook (`urbi_stdlib_register_container_globals`) called
  from `urbi_populate_realm_globals` after the 15-row registry
  loop — this keeps the v1.0 packed-flag CONSTANT enforcement range
  (slots 0..7: Object..List) intact.

  Method-name choice: every operation is a named method.  v1.0 lex
  has no `<<` or `[]` operator tokens; `at` is the reactive `at(...)`
  keyword so list indexing is `.get(i)` (matches `Dict.get`).
  Phase 10's `.u` overlay can synthesize operator wrappers if/when
  the lex/parse extensions land.
- (Phase 6) **6 `tests/chk/stdlib/containers/` fixtures** —
  `pair.chk`, `triplet.chk`, `tuple.chk`, `list_core.chk`,
  `list_concat.chk`, `dict_core.chk`.  Test count delta:
  189 → 195 chk fixtures.
- (Phase 7) **Exception primitive root.**  New
  `src/stdlib/runtime_types.{c,h}` registers `Exception` as a fresh
  `URBI_ATOM_OBJECT`-family proto with two C-native methods:
  `Exception.new(message)` clones the proto and binds a per-instance
  `message` slot; `Exception.raise` calls `urbi_throw` on
  `vm->cur_strand`, depositing `pending_unwind = UEXEC_THROW` and
  `unwind_value = self` so `try/catch` absorbs the Exception
  instance as the catch variable (`catch (e) { e.message }`).
  Realm-global binding lands at slots 15+ via a post-loop hook
  (`urbi_stdlib_register_runtime_globals`), mirroring the container
  globals pattern so the v1.0 packed-flag CONSTANT enforcement range
  (slots 0..7) stays intact.  Exception subclasses (`TypeError`,
  `IndexError`, …) defer to Phase 10's `.u` overlay.
- (Phase 7) **`vm->cur_strand` wired through `urbi_vm_run`.**  Pre-
  Phase-7 only `ustep.c`'s incremental driver set `vm->cur_strand`
  during dispatch; the synchronous `urbi_vm_run` path was a gap.
  Native methods that call `urbi_throw` / `urbi_return_val` /
  `urbi_tag_stop_local` (Exception.raise being the first user) need
  the running strand pointer to deposit unwind state.
- (Phase 7) **OP_CALL native arm: catchable native raise.**  When a
  native_fn returns `UEXEC_OK` with `pending_unwind != UEXEC_OK`
  (i.e. it called `urbi_throw` from inside the C body), the dispatch
  arm now routes through `safepoint:` instead of `NEXT()`, so the
  cleanup-stack walker can absorb the deposited THROW under any
  enclosing `try/catch` frame.  Native functions that return
  `UEXEC_THROW` directly (the legacy `urbi_raise_arity` /
  `urbi_raise_type` / `urbi_raise_oom` path) still fatal-halt with
  the pre-Phase-7 "CALL: native method raised" TypeError — preserves
  `tests/chk/objects/get-set/get_set_arity_reject.chk`-style fixtures
  while enabling the catchable-raise path for stdlib code.
- (Phase 7) **2 `tests/chk/stdlib/runtime/` fixtures** —
  `exception_basic.chk` (constructor, `.message`, `.raise` +
  try/catch absorption, named-instance round-trip),
  `exception_chain.chk` (nested raise/catch, inner-raise-through-
  finally to outer catch).  Test count delta: 195 → 197 chk fixtures.
- (Phase 8) **C-native namespaces.**  New
  `src/stdlib/namespaces.{c,h}` registers five namespace proto
  UObjects bound as realm globals via the same post-loop hook
  pattern as containers + runtime types (slot 15+, past the
  packed-flag CONSTANT enforcement range):
  - `Math`: IEEE-754 constants `pi`, `e`, `nan`, `infinity` (the
    method surface — `sin` / `cos` / `sqrt` / etc. — defers to
    Phase 10's `.u` overlay, which bounces to the Float atom-proto
    methods Phase 5 installed).
  - `System`: host primitives `time` (monotonic-microseconds → Float
    seconds via `vm->host_time_us`), `cycle` (per-VM `lookup_id`
    counter as Integer), `getenv(name)` (libc shim, freestanding-
    nil), `gc` (explicit `urbi_gc_collect`).  Wall-clock-since-epoch
    `System.time` from legacy 2.x narrows to monotonic-since-VM-
    start at v1.0 to avoid a libc `time()` dependency on freestanding
    targets — wall-clock access lands later via the Date primitive
    (Phase 9).
  - `System.Platform.kind`: compile-time string set via `#ifdef`
    cascade — `"linux"` / `"darwin"` / `"windows"` / `"freertos"` /
    `"unknown"`.  Nested as a slot on `System` (not a top-level
    realm global).
  - `Global.length`: reflective slot count of the active realm's
    `global_object`.  Stub for v1.x reflection (`Global.names()`
    etc.).
  - `CallMessage`: stub proto with a `kind` marker slot, reserved for
    v1.x legacy-`fallback()` reflection.
- (Phase 8) **VM fields + GC roots.** `UVM` grows five proto-singleton
  pointers (`math_proto`, `system_proto`, `platform_proto`,
  `global_namespace_proto`, `callmessage_proto`).  Allocated by
  `urbi_stdlib_register_namespaces` (boot-phase); shaded by
  `object_roots_walker` to keep them alive across GC.
- (Phase 8) **5 `tests/chk/stdlib/namespaces/` fixtures** —
  `math.chk` (constants + Float-method dispatch), `system.chk`
  (time / cycle monotonicity / gc / getenv), `system_platform.chk`
  (kind String non-empty), `global.chk` (length > 15 lower bound),
  `callmessage.chk` (proto bound).  Test count delta: 197 → 202 chk
  fixtures.
- (Phase 9) **C-native primitives — `Mutex`, `Date`, `Duration`.**
  New `src/stdlib/primitives.{c,h}` registers three primitive proto
  UObjects bound as realm globals via the same post-loop hook pattern
  as containers + runtime types + namespaces (slot 15+, past the
  packed-flag CONSTANT enforcement range):
  - `Mutex`: cooperative single-VM lock.  `Mutex.new()` clones the
    proto with a hidden `_locked` bool slot; `m.lock()` /
    `m.unlock()` / `m.tryLock()` / `m.locked()` are non-blocking
    flag flips.  v1.0 `URBI_SCHED_COOPERATIVE` contract means the
    "wait" semantics defer to Phase 10's `.u` overlay
    (`Mutex.synchronized` via `waituntil`).
  - `Date`: wall-clock access via libc `time()`.  `Date.now()` /
    `Date.fromSeconds(s)` clone the proto with a hidden `_seconds`
    int slot; `d.seconds()` reads it; `d.asString()` formats UTC
    "YYYY-MM-DD HH:MM:SS" via `gmtime_r` + `strftime` on hosted
    builds (POSIX feature-test macros gate the `time.h` symbols);
    freestanding builds return `0` / `""` since `time()` /
    `strftime` aren't available without libc.  `d.plus(dur)` returns
    a fresh Date advanced by Duration's microseconds-to-seconds
    quotient (Phase 10 overlay can promote to operator form).
  - `Duration`: thin wrapper over integer microseconds.  Time
    literals (`100ms` / `2s` / `1d`) lex to integer microseconds at
    M2; `Duration.fromMicroseconds(us)` wraps such an integer in a
    typed Duration UObject via a hidden `_microseconds` slot.
    `d.asMicroseconds()` / `asMilliseconds()` / `asSeconds()` are
    integer-arithmetic accessors.
- (Phase 9) **VM fields + GC roots.** `UVM` grows three proto-singleton
  pointers (`mutex_proto`, `date_proto`, `duration_proto`).  Allocated
  by `urbi_stdlib_register_primitives` (boot-phase); shaded by
  `object_roots_walker` to keep them alive across GC.
- (Phase 9) **4 `tests/chk/stdlib/primitives/` fixtures** —
  `mutex.chk` (lock / unlock / tryLock / locked flag flips),
  `date.chk` (now / fromSeconds round-trip / asString / monotonic
  granularity), `duration.chk` (fromMicroseconds + as*),
  `date_duration_seam.chk` (`Date.plus(Duration)` arithmetic with
  positive / zero / sub-second-truncate / negative inputs).  Test
  count delta: 202 → 206 chk fixtures.
- (Phase 10) **Public `urbi_compile_source` API.** The build-time
  bake tool needed an entry point for compiling source bytes to
  serialized v1.5 wire-format bytecode.  Added as the first new
  `<urbi/urbi.h>` symbol since v0.5.5 — pipeline mirrors
  `tools/urbi.c`'s in-process compile (lex → arena → parse-loop →
  emit → serialize).  Hosted-only; freestanding builds return
  `URBI_ERR_INVALID_ARG` (pre-compiled bytecode reaches embedded
  targets via `urbi_load_module` instead).
- (Phase 10) **Bake tool actually compiles.** The Phase-3 stub
  walk (`fprintf "would compile"`) is now a real concatenate-then-
  compile-once pass.  Each `.u` file under `src/stdlib/` listed in
  `STDLIB_ORDER.txt` gets read, prefixed with a
  `// === <path> ===` banner, and appended to a single source
  buffer fed through `urbi_compile_source`.  The resulting v1.5
  bytecode goes into `urbi_stdlib_bytecode[]` (1071 bytes at ship).
  Concatenate-then-compile (vs one-module-per-file with framing)
  keeps the boot path single-`UModule` and matches how legacy
  share/urbi composition works anyway.
- (Phase 10) **Boot-runs the stdlib chunk.** The Phase-4 banner
  ("running the root chunk is deferred to a later phase") is
  closed.  After all C-native registration completes inside
  `urbi_populate_realm_globals`, `urbi_run_chunk` runs the
  deserialized `vm->stdlib_module` in the in-flight realm so its
  top-level statements (currently `class X : public Y {}` decls)
  install themselves as realm globals.
- (Phase 10) **Mixin marker classes** (`src/stdlib/mixins.u`) —
  `Comparable`, `Orderable`, `RangeIterable` shipped as
  empty-body shells.  User code can `addProto(Comparable)` for
  vocabulary alignment with the legacy stdlib.  The legacy
  methods (`!=` from `==`, `>`/`<=`/`>=` from `<`,
  map/filter/find from each) require closure upvalue capture
  across `this`/`other` plus multi-slot class bodies, neither of
  which is in v1.0 emit scope — those forms migrate to v1.x.
- (Phase 10) **Exception subclass hierarchy**
  (`src/stdlib/exception_subclasses.u`) — nine subclasses
  (`TypeError`, `ArityError`, `LookupError`, `KeyError`,
  `IndexError`, `RangeError`, `DivByZero`, `IOError`,
  `CapacityError`) declared as empty-body
  `class X : public Exception {}` decls.  KeyError + IndexError
  are two-level (`: public LookupError`).  `.new(message)` and
  `.raise()` resolve through the chain to `Exception`.  Generic
  `catch (e)` binds the raised instance; typed catch syntax is
  v1.x.
- (Phase 10) **2 `tests/chk/stdlib/overlays/` fixtures** —
  `mixins.chk` (realm-global reachability + addProto-on-user-class),
  `exception_subclasses.chk` (.new / .message / try-catch end-to-
  end for all nine subclasses).  Test count delta: 206 → 208 chk
  fixtures.
- (Phase 11) **7 `tests/chk/stdlib/legacy/` ports** of subsets from
  `legacy/repos/aldebaran-urbi/tests/2.x/`: `dict_legacy.chk`,
  `list_legacy.chk`, `mutex_legacy.chk`, `date_legacy.chk`,
  `system_legacy.chk`, `large_string_legacy.chk`,
  `maths_errors_legacy.chk`.  Each is a focused subset of its legacy
  counterpart; the per-fixture rationale + sections deferred to v1.x
  live in `tests/chk/stdlib/legacy/PORT_NOTES.md`.  Plan envisioned
  9-12 ports + 3 Wave-1-deferred-section activations
  (atoms.chk / fallback.chk / inheritance.chk).  Reality: most legacy
  fixtures depend on Phase 10 v1.0 emit gaps (no float literals,
  list / dict literals, closure upvalue capture, multi-slot class
  bodies, `var x.foo = ...` slot install, `assert` / `echo` realm
  globals, string concat, fallback protocol, `do (recv) { ... }`),
  so 7 ports landed and 0 Wave-1 activations.  PORT_NOTES.md catalogs
  the deferred-to-v1.x full-fixture port backlog.  Test count delta:
  208 → 215 chk fixtures.

### Deferred to v1.x

Phase 10's plan envisioned ten `.u` overlay files (~80+ overlay
methods).  The realistic Phase 10 ship is the bake-tool API plus
two minimal-content overlay files (mixins + exception subclasses,
one method between them: none, just the proto markers + subclass
shells).  The remaining eight overlays from the plan all depend on
infrastructure that is not in v1.0 emit scope:

- **Closure upvalue capture across method boundaries** — every
  one of `RangeIterable.map/filter/find`, `List.map/filter/foldl/
  find/reduce/partition`, `Dict.keys/values/merge/invert`,
  `String.split/trim/format/replace`, `Math.sin/cos/...` (lambda
  bouncing to `.asFloat.sin`), `Mutex.synchronized` (try/finally
  closure body), `Number.times/upto/downto` is dead until
  `function(x) { outer_var }` resolves the outer var.
- **Multi-slot class bodies** — `class C { var x = 1; var y = 2 }`
  raises `EMIT_UNSUPPORTED_AST` because the body parses as a single
  `AST_SEPARATOR` and `emit_class_body_stmt` only accepts
  `AST_VAR_DECL`.  Affects every overlay that wants to declare
  more than one slot inside a class body.
- **`this.method()` from within a method body** — also raises
  `EMIT_UNSUPPORTED_AST`.  Receiver-routed dispatch from
  inside a function literal needs upvalue-captured `this` plus a
  callable resolver, neither of which is wired.
- **Operator overrides via `'+'`/`'<'`/`'=='` slot install** —
  symbolic operators are inline VM opcodes (not slot lookups), so
  installing `Duration.'+' = function(other) { ... }` does not
  intercept the operator.  Affects time / number / comparable
  operator-form overlays.
- **Float literals** — `0.5` / `3.14` don't lex as numbers (lex
  consumes `0` then sees `.5` as a slot access).  `Float`
  values come from `.asFloat()` only.  Affects `math_overlay.u`'s
  `Math.pi` constant + every `Float`-arity test that wanted
  literal float arguments.

The closure-upvalue gap is the load-bearing one: it unblocks the
majority of the stdlib content overlays and is the primary
plumbing item that should land before rebooting Phase 10's
deeper deliverables.  Filed under
`docs/urbi-embedded-design-risks.md` v1.x: "closure upvalue
capture (M7+ / stdlib-content blocker)".  Multi-slot class bodies
and `this.method()` from method bodies join it on the same
backlog row — the three features are tightly coupled and would
need to ship together for the deeper overlays to compile.

(Phase 12) **`defer:M6` audit IDs migrated to v1.x** — 4 of 15
carry-forward IDs need architectural work that was not in v1.0
scope and were filed against the design-risks register:

- **GC-003 + GC-037 + VM-007 (clustered)** — 'UClosure cells
  GC-managed promotion + slot-write barrier real index'.  UClosure
  cells today are allocated through `vm->alloc_fn` directly (not
  `urbi_gc_alloc`) and threaded onto a per-strand `closure_list`
  legacy free-list; the missing UTYPE_CLOSURE walker is benign
  because cells are never enrolled on `all_cells_head`.  The
  proper fix promotes both allocation paths (`vm_alloc_closure` +
  `urbi_native_closure_create`) to `urbi_gc_alloc` + adds a
  `walk_uclosure` strand-walker entry; the same pass naturally
  closes the slot-write barrier placeholder index (VM-007).
  Filed in design-risks as 'v1.x: UClosure cells GC-managed
  promotion'.
- **GC-005** — Cross-references VM-007 (same root issue).
  Slow-path `OP_SETSLOT` calls `urbi_gc_slot_write` with hardcoded
  slot_index = 0; observer_dirty ignores the key argument entirely
  at v1.0, so the placeholder is benign.  Closes when v1.x
  preemptive scheduler or per-slot dirty-bit work needs the real
  index.  Filed in design-risks as 'v1.x: slot-write barrier
  placeholder index'.

### Changed

- (filled in)

### Fixed

- (Phase 12) **`defer:M6` cleanup absorption** — 11 of 15 carry-forward
  audit IDs closed under TDD-per-fix-commit discipline (Wave-5 G1):
  EMIT-030 (dead break_chain/continue_chain UBlockCtx fields removed),
  PARSE-032 (doc-only — lex absorbs time/angle suffix into TOK_INT),
  PARSE-033 (doc-only — tag-prefix onleave is v1.x scope; AST field
  retained on union variant for v1.x parse+emit addition),
  CHSTR-007 (regression net asserting REPL diagnostic NEVER falls
  back to literal "compile error" string; pinned `<stdin>:line:col:`
  format pass-through),
  CHSTR-009 (annotated existing load_module live-path test as
  closure pointer; M6 Wave 1 / API-021 had landed the real impl),
  CHSTR-026 + CHSTR-030 (regression net asserting urbi_run_chunk
  realm parameter is honored vs silently replaced by global; Wave 5
  API-004 had threaded realm through urbi_vm_run; new test installs
  K=1234 on a non-default realm and reads it back),
  CPPCHK-001 (annotated existing CPPCHK-011 inline-suppression as
  covering both IDs — duplicate filings),
  TAGCH-018 (annotated existing tag_enter_setter_throws_protected_slot
  test as the audit-ID closure pointer),
  VM-009 (doc-only — OP_CALL native-arm short-circuit prevents the
  audit's failure shape; comment block at the dispatch site pins the
  contract),
  REALM-002 (regression net pinning past-slot-7 install + best-effort
  const enforcement contract; v1.0 architectural limit on
  packed-nibble UShape.flags is documented).

  Per-ID disposition lines appended to the
  `2026-05-05-v0.5.x-cleanup-audit-findings.md` audit doc.  Phase 12
  closure summary in `docs/urbi-embedded-design-risks.md` 'v0.6.1-stdlib
  Phase 12' section.

- T41 install-time arity validation for `get` / `set`: rejects
  wrong-param-count getters / setters (getter must take 0 params,
  setter must take 1) with a clear ArityError diagnostic at
  `Object.setProperty` time instead of segfaulting in the dispatch
  arm later.
- T41 statement-start `get` / `set` form is now rejected at parse
  time with a dedicated `PARSE_TOPLEVEL_GETSET_NOT_SUPPORTED`
  diagnostic that points the user at the two legal forms
  (`recv.get x() {}` and `class C { get x() {} }`).  The bare
  statement-start form has no v1.0 implicit-`this` resolver and is
  deferred to v1.x.
- `emit_call_arm` register clobber on multi-arg calls with a
  trailing `AST_FUNCTION` argument: leaf-literal args ahead of the
  function literal bumped only `next_reg`, leaving `freereg` lagging,
  so the trailing `OP_CLOSURE` destination would land on an already-
  allocated arg slot.  Latent since v0.4.0-objects, surfaced under
  T41's setProperty desugar shape.

### Tooling

- (Phase 3) **`tools/urbi-compile-stdlib` build-time bake tool**
  (~110 LOC C; links host `liburbi.a`).  Walks
  `src/stdlib/STDLIB_ORDER.txt`, will compile each listed `.u` via
  the public Urbi compile API, and emits the bytecode blob as
  `src/stdlib/urbi_stdlib_bytecode.gen.c`.  Phase-3 baseline ships
  the empty-walk scaffold (0-length blob); Phase 10 fills in the
  compile loop.  Two-pass build wired in `Makefile`: pass 1 builds
  `liburbi.a` against the tracked placeholder `.gen.c`; pass 2
  links the bake tool; pass 3 regenerates `.gen.c` and re-links
  `liburbi.a` whenever `STDLIB_ORDER.txt` or any `.u` changes.
  Determinism gate (`tests/scripts/bake_smoke.sh`, 3-run
  byte-identity) wired into `make releasetest` as
  `test-bake-smoke`.  `make bake-clean` force-regenerates the
  blob.  Cross-arch builds consume the host-baked `.gen.c` source
  unchanged (the bake tool is host-only).  See
  `docs/internals/build-system.md`.
- (Phase 14) **Final footprint at `v0.6.1-stdlib`** (host gcc 14 -O2,
  arm-none-eabi-gcc -Os, riscv64-unknown-elf-gcc -Os):
  - host: 193 994 B / 198 882 B total (.text + .data) — vs v0.6.0
    149 360 B = +44 634 B (+30 %).  Master spec §7 Linux total cap
    400 KB: 49 % of budget consumed.
  - arm: 94 191 B — vs v0.6.0 68 908 B = +25 283 B (+37 %).  ARM
    cap 100 KB: 94 % of budget consumed.
  - riscv: 118 460 B — vs v0.6.0 88 579 B = +29 881 B (+34 %).
    RISC-V cap 130 KB: 91 % of budget consumed.
  Wave 2 expected delta per spec §7 was +25-40 KB host before
  contraction, +15-25 KB after.  Actual +44 KB host is over the
  contraction-adjusted target but well under the absolute cap; no
  contraction applied at v0.6.1 (room remains for future Wave 2
  bytecode-blob growth + Phase 10 deferred overlays + M7 C-API
  formalization).  Captured at
  `tests/golden/v0.6.1-stdlib-footprint.txt`.

## v0.6.0-stdlib-scaffold — 2026-05-09 — Wave 1 of M6 stdlib

**Theme:** Language scaffolding for the M6 standard library — string
literals, atom-method dispatch, Object root C-native methods, atom proto
stubs, `Class.new()` / `.clone()` semantics, class declarations with
multi-proto MRO and nested-class shadow scoping, and the scripted
`Event.new()` constructor. Activates eight deferred
`tests/chk/objects/` fixtures end-to-end. Absorbs ~12 `defer:M6`
cleanup IDs.

### Language

- (Phase 1) **String literals.** `TOK_STRING` lex with `\n` / `\t` /
  `\\` / `\"` / `\0` / `\xNN` escapes; adjacent-string concatenation
  (`"foo" "bar"` → `"foobar"`); `AST_STR` parse node; `OP_LOADK`
  emits via `UVAL_STR` constant-pool entry. Empty-string literal
  (`""`) lex-safe (null buffer guard). LEX-035 closed for ASCII /
  basic-escape coverage; angle literals (`180deg` / `200grad`)
  remain on the v1.x literal-suffix backlog.
- (Phase 6) **Class declarations.** `class Foo : public A, B { body }`
  parse + emit. Surface compiles to a clone-Object idiom + multi-proto
  insertFront in declaration order (left-most proto wins MRO). Body forms
  desugar inside the class
  scope. Nested-class shadow scoping (S-class-name-scope): a
  declaration-local class binding shadows an outer same-name binding
  for the duration of the enclosing scope. New keywords `class` and
  `public` (lex). New AST node `AST_CLASS_DECL = 34`.
- (Phase 5) **`Class.new()` / `.clone()` semantics.** `Object.new`
  C-native method implements the `Class.new()` idiom (closes T39):
  allocates a fresh clone of the receiver, returns it, no const-slot
  inheritance from the proto's locals (COW-cloned slots are mutable on
  the derived object). Added `Object.removeLocalSlot` and
  `Object.getSlotValue` legacy aliases (unbreaking existing third-
  party-corpus call sites).
- (Phase 7) **`Event.new()`.** Scripted Event constructor: clones the
  Event proto, the resulting handle works through the existing
  `at`/`waituntil`/`emit`/`syncEmit` watcher dispatch path. Unblocks
  the M5 `event_sync_emit.chk` deferred-fixture activation in Phase 8.

### Object model

- (Phase 2) **Atom-method dispatch.** `OP_GETSLOT` slow path now
  routes `UVAL_INT` / `UVAL_FLOAT` / `UVAL_STR` / `UVAL_BOOL` receivers
  through `urbi_atom_proto_for_value(...)` to the appropriate
  per-type proto when a slot lookup misses on the bare atom.
  Atom-proto pointer cache lives in the realm; lookups fold through
  the inline-cache machinery without per-call allocation. Slow-path
  return distinguishes OOM from const-write (new
  `URBI_SLOT_INVALID` sentinel; `URBI_ERR_SLOT_CONST_WRITE` retained
  for const-overwrite). `ic_fill_at_cursor` hoisted to `uic.h` for
  shared use across megamorphic-bail call sites.
- (Phase 3) **`Object` root C-native methods.** Nine methods land as
  C-native closures: `setSlot`, `getSlot`, `hasSlot`, `removeSlot`,
  `clone`, `addProto`, `removeProto`, `protos`, `setProtos`. Closes
  T30 + T33-T37 + T42. Introduces `UClosure.native_fn` (a
  `urbi_native_method_fn`-typed function pointer alongside the
  bytecode body); `OP_CALL` dispatch arm transparently routes
  native-fn closures to direct C invocation, skipping the bytecode
  arm entirely. Closures retain GC ownership of their UProto and
  realm-binding metadata.
- (Phase 3 follow-up) **VM-lifetime closure migration.** When a
  top-level run completes, any UClosures still reachable via the
  realm's globals are migrated from the run-scoped scratch frame to
  vm-lifetime ownership; heapified upvals follow the same migration.
  This closes a use-after-free that the M5 reactive-runtime
  closure-lifetime model couldn't reach, surfaced by exhaustive
  testing of the new C-native dispatch path.
- (Phase 4) **Atom protos as realm globals.** Boolean / Nil / Void
  protos now exist as first-class atom-proto singletons, exposed via
  realm globals (`Boolean`, `Nil`, `Void`; `Nil` retained as
  `Object`-style proto handle, lower-case `nil` remains the
  `UVAL_NIL` value singleton). New enum members
  `URBI_ATOM_BOOLEAN` / `URBI_ATOM_NIL` / `URBI_ATOM_VOID`. C-native
  method registration extended for atom protos. **Compatibility
  rename:** `Bool` → `Boolean` (v1.0 atom-proto naming convention).
- (Phase 5) **`Object.protos.insertFront`.** Synthetic protos-list
  surface gains `insertFront` (mutates the proto chain non-
  destructively from the front). Wave 1 stub — full `List`-shaped
  protos surface lands in Wave 2.

### Tests

- (Phase 8) **Eight deferred fixtures activated.**
  `tests/chk/objects/`: `lookup`, `inheritance` (subset; List-literal
  sections defer to Wave 2), `slot-cow-const`, `shared-protos`
  (insertFront proto-chain mutation), `class` (S-class-name-scope
  nested-shadow), `fallback` (`var Object.blurg` only; full
  fallback-chain semantics defer to Wave 2), `atom-clone`, `atoms`
  (subset; remainders defer to Wave 2). `tests/chk/objects/README.md`
  refreshed to reflect activation status.
- New `.chk` fixtures: `tests/chk/objects/atom_method_dispatch.chk`,
  `object_root_methods.chk`, `atom_proto_clone.chk`,
  `class_new_clone.chk`, `class_decl_basic.chk`,
  `class_decl_multi_proto.chk`, `class_decl_nested.chk`,
  `event_new_emit.chk`. Three new string-literal fixtures landed
  Phase 1.
- Co-located regression tests: `test_object_root.c`,
  `test_atom_proto_dispatch.c`, `test_atom_protos.c`,
  `test_class_decl.c`, `test_event_new.c`, `test_string_literal.c`,
  plus VM-lifetime UClosure migration coverage in `test_uvm.c`.
- Test corpus at ship: **1409 unit cases / 7933 checks / 168 `.chk`
  fixtures** (vs 1300 / 7400 / 148 at v0.5.8-cleanup). Coverage
  gates remain at the 85% line / 75% branch threshold.

### Cleanup

`defer:M6` audit IDs absorbed during Wave 1 polish phases:

- **TAGCH-013 / EVENT-013** (Phase 7) — emitter / runtime cleanups
  surfaced by Event.new() integration; closed with TDD.
- **REALM-016** (Phase 9) — `reflective_field_unused` dead code
  removed.
- **REALM-017** (Phase 9) — `urbi_realm_has_live_work` →
  `urbi_vm_has_live_work` rename (signature also `const`-qualified
  in the Phase 9 follow-up sweep).
- **API-005** (Phase 3) — `ULOAD_UNSUPPORTED_VERSION` now routes
  through `URBI_ERR_BYTECODE_VERSION_MISMATCH` at the public API
  surface.
- **API-021** (Phase 9) — `urbi_load_module` body implemented
  (was a stub).
- **FOUND-026 / FOUND-027** (Phase 7) — runtime stubs documented
  as Wave-2 deferred (audit IDs closed with explicit deferral
  rationale).
- **LEX-035** (Phase 1, partial closure) — string-literal lex
  landed; angle literals (`180deg` / `200grad`) defer to v1.x
  literal-suffix backlog; `pi` becomes `Math.pi` at Wave 2 (no
  lex change).
- Several smaller items: cppcheck suppressions refreshed for line-
  number drift after Phase 6 (`uunwind.c`); `parse_string_literal`
  null-buffer guard for empty strings; `parse_class` `proto_cap`
  scope-narrowed inside the colon-branch; const-qualifications on
  atom-proto pointers, `obj_protos_insertFront::sym_owner`, and
  `urbi_proto_list_create` recv-arg flow.

### Bytecode

Wire format stays at **v1.5** (`URBI_BYTECODE_VERSION_BYTE = 0x15`).
No new opcodes. The constant pool gains `UVAL_STR` support (closes
MOD-008 deferred at v0.5.6). Per-fixture wire-format hashes shift
only for fixtures that exercise the new string-literal emit path or
the activated objects fixtures; the canonical Wave 1 goldens are
captured at `tests/golden/v0.6.0-stdlib-scaffold-{bytecode,wire-
format}-hashes.txt`.

### Compatibility

- **Public C API additions:** `urbi_atom_proto_for_value`,
  `urbi_native_closure_create` (UClosure native_fn extension),
  `urbi_object_new`, atom-proto realm-global setters. No v0.5.x
  signature changes; the only signature touch is the
  `urbi_realm_has_live_work` → `urbi_vm_has_live_work` rename
  (closes REALM-017; in turn the `const`-qualification follow-up
  sweep is purely additive at the source level since the renamed
  function is the only callsite).
- **Atom-proto naming:** `Bool` → `Boolean` realm global (v1.0 naming
  convention). Old name not exported in
  v0.5.x.
- **Module wire format:** unchanged at v1.5. v1.4 modules continue
  to be rejected with `ULOAD_UNSUPPORTED_VERSION` (now mapped to
  `URBI_ERR_BYTECODE_VERSION_MISMATCH` at the API).

### Footprint

`size --total liburbi.a` at v0.6.0-stdlib-scaffold:

| Target | v0.5.8-cleanup | v0.6.0-stdlib-scaffold | Delta |
|---|---|---|---|
| host (Linux x86_64) | 135 022 B | 149 360 B | +14 338 B (+10.6 %) |
| ARM Cortex-M7 | 62 378 B | 68 908 B | +6 530 B (+10.5 %) |
| RISC-V rv32imc | 80 084 B | 88 579 B | +8 495 B (+10.6 %) |

Growth sources: `object_root.o` (~6.4 K host / 2.3 K arm / 3.1 K
riscv) is the largest single contributor — nine C-native methods
plus the dispatch tables. `atom_protos.o` (~1.3 K host / 0.7 K
arm / 0.8 K riscv) hosts the atom-proto registration. `utypes_init.o`
gains `URBI_ATOM_BOOLEAN/NIL/VOID` lazy singletons and the
keyword-table extension for `class` / `public`. The class-decl
emit arm adds ~340 LOC to the emit subsystem (still well under the
600-LOC per-file cap). Wave 2 (`v0.6.1-stdlib`) is expected to
ship the bulk of remaining stdlib content; footprint impact will
be re-baselined at that ship.

## v0.5.8-cleanup — 2026-05-09 — Pre-M6 cleanup ramp final wave (Wave 6 of 6)

**Theme:** Polish + dead-code + docs. Closes ~104 `wave-6-cleanup` audit IDs
from the v0.5.x cleanup audit. Reactive runtime polish bundle landed.
Footprint overage from Wave 5 addressed. Strict-tooling residuals
(cppcheck 135 + tidy-strict 23 informational tier at v0.5.7-fixes) driven
to zero — both gates now hard-fail across all categories in
`make releasetest`. Docstring coverage gate enforced on every
header-exposed declaration in `include/urbi/*.h`. Design-risks register
reaches a coherent state. Cleanup-ramp retrospective at
`docs/milestones/v0.5.x-cleanup.md` (workspace root).

### Reactive runtime

- (Phase 1, REACT-POLISH-001) Test-helper extraction: new
  `tests/unit/utest_e2e_helpers.{c,h}` consolidates the four scripted
  reactive end-to-end test setups (`test_at_scripted_e2e`,
  `test_at_sync_scripted`, `test_tag_stop_onleave_scripted`,
  `test_event_sync_emit_scripted`) onto a single helper API. Self-test
  covers the helper itself.
- (Phase 2, REACT-POLISH-002/004/005/006/007/008/009) Identifier
  naturalization (`__trigger__` / `__trigger2__` / `__post__` →
  natural test-fixture names; no-op onleave hooks substituted in
  walker-presence tests; comment + display-name normalization;
  `whenever_level.chk` fixture header corrected — three safepoints,
  not two; documentation of `run_event_body_on_scratch`
  in_watcher_scratch asymmetry; throwing-cond rationale captured in
  source). REACT-POLISH-003 retired (no `__trigger*__` left to rename
  in `test_event_sync_emit_scripted.c`).

### Bug fixes

- (Phase 4, CHSTR-019) `sched_strand_init` now always resets
  `s->cur_consts` on strand arm; previously a re-armed strand could
  observe stale per-closure const buffer pointers from the prior arm
  (TDD: `test_strand_consts_reset_on_arm`).
- (Phase 4, CPPCHK-002) Symbol-id rollover false-positive in object
  lookup explicitly suppressed at the call site with rationale
  pinning the documented behavior (32-bit lookup_id wraparound is
  a deliberate non-issue at v1.0 working-set scales).
- (Phase 4, MOD-014) `abs_lines` monotonic-check simplification:
  retire `first_checkpoint` flag; the `prev` initial value of `0` is
  itself the sentinel for the first call (TDD covers both
  first-call + monotonicity-violation paths).
- (Phase 4, TIDY-008) `cond_has_direct_side_effect`: AST_CALL collapses
  into the default branch (was a redundant duplicate of fallthrough).

### Unsafe-pattern hardening

Phase 5 used a "contract-pin TDD" pattern: each fix pins documented
post-condition behavior with a regression test rather than chasing a
crash. All six findings were benign-but-undocumented at audit time.

- (CHSTR-004) `ustrand_destroy` guards already-NULL stack; double-free
  no longer hits an undefined-behavior path on early-error teardown.
- (EMIT-001) `emit_push_line_delta` no-ops on `instr_count==0`; debug
  line table no longer emits a phantom delta before the first
  instruction.
- (EVENT-001) `gc_byte` allocator-ownership contract documented at
  the field declaration; allocator zero-init is the source of truth.
- (EVENT-007) `atomic_load` on `drain_handler` reads; observer
  registration is sequenced through release-store / acquire-load.
- (GC-007) `UGC_IS_FIXED` sweep re-paint contract documented; the
  flag's interaction with the next-cycle white tag is now explicit.
- (MOD-003) Documents nested[] grow-without-commit on UProto OOM;
  partial-state behavior of `umodule_deserialize` is the contract,
  not a bug (paired with v0.5.6 MOD-039 docstring rewrite).

### Lex

12 IDs closed (Phase 6, LEX-001..036).

- (LEX-001) `ulex_init` asserts `lex` and `src` preconditions.
- (LEX-002) `line_start == src + cur` invariant asserted on init.
- (LEX-003) Line/col are 1-based in caller-visible reports.
- (LEX-004) Unterminated block comment error spans the full comment.
- (LEX-012) Leading-zero path entry asserts `cur == start`.
- (LEX-013) `scan_radix` overflow recovery contract documented +
  test-pinned.
- (LEX-027/028/029) Docstring polish for `ulex_init` / `ulex_next` /
  `UToken` lifetime.
- (LEX-033) Regression test for `123ms_x` / `123sfoo` identifier-time
  interaction.
- (LEX-034) **New fixture** `tests/chk/lex/block_comment_no_nest.chk`
  documents and locks in the C-style (non-nesting) block-comment
  semantics. **Only diff against v0.5.7-fixes wire-format goldens.**
- (LEX-036) `var_async_as_ident_fails` comment matches v0.5.7
  lex/parse contract.

### Parse

(Closed in Phase 3 via dead-code sweep; Phase 7 a placeholder.)

- (PARSE-018) Empty `udesugar.c` retired.
- (PARSE-019) `PARSE_LAZY_PARAM_DEFAULT` unused error code removed.

### Emit

- (Phase 8, EMIT-021) `global_slot_reserved` / `references_global`
  state machine documented (audit's "three-flags-one-concept" claim
  was wrong; documented as tri-state lock-in instead of collapsed).
- (Phase 8, EMITR-003) `run_event_body_on_scratch` asserts
  pre-condition on entry.
- (Phase 8, EMITR-012) Rationale documented at `URBI_ASSERT_NOT_ISR`
  sites in `uevent_emit.c`.
- (Phase 8, EMITR-013) `urbi_emit_slot_change_slow` silent-return
  contract documented in header.

### VM / scheduler

7 IDs (Phase 9, VM-015/016, SCHED-005/007/009/010/012).

- (VM-015) `op_push_tag` enter_event branch promoted to assertion.
- (VM-016) `drain_deferred_slot_changes` early-returns on empty
  queue (avoids spurious atomic-load on the hot drain path).
- (SCHED-005) `sched_strand_make_runnable` asserts non-READY entry.
- (SCHED-007) `sched_strand_block` documented default arm + assert.
- (SCHED-009) `sched_destroy` zeros internal counters before free.
- (SCHED-010) `URBI_SCHED_RT` / `URBI_SCHED_DEADLINE` marked v1.x
  reserved.
- (SCHED-012) `strand_walk_roots` TODOs refreshed post-M5.

### GC

- (Phase 10, GC-011) `gc_atomic_finish_step` return-value sentinel
  documented (caller distinguishes "still working" from "atomic
  finish complete" via a documented sentinel value).

### Watcher

10 IDs (Phase 11, WATCH-003/004/005/009/010/011/029/031/032/036).

- (WATCH-003) `pool_destroy` explicitly NULLs `pending_head`.
- (WATCH-004) `proto_inst` contiguity at scratch range walk pinned
  with a `URBI_DEBUG`-only assertion (cross-MI pointer-range walk
  is allowed but fragile; assertion documents the invariant).
- (WATCH-005) `urbi_watcher_install` resets trace on pool-alloc
  fall-through.
- (WATCH-009) `observer_dirty` asserts non-ISR.
- (WATCH-010) `in_watcher_eval` drain-routing dependency documented;
  ghost `vm->dirty_set` reference removed from the WATCH-010
  invariant phrasing in a follow-up `4573f9c`.
- (WATCH-011) `urbi_run_closure_on_scratch` `in_watcher_scratch`
  caller-owned contract documented.
- (WATCH-029) `pool_init` zero-loop freestanding rationale expanded.
- (WATCH-031) `invoke_body_inline` yield-degrade contract documented.
- (WATCH-032) WATCH-023 historical note relocated to file header.
- (WATCH-036) `in_watcher_scratch` caller-owned contract documented
  at field decl (paired with WATCH-011 site).

### Event / tag / changed

13 IDs (Phase 12, EVENT-010/011/021/022/023/026,
TAGCH-003/006/007/008/009/010/017).

- (EVENT-010) `uevent_at_watchers_remove` `next_in_event` contract
  documented for miss-case.
- (EVENT-011) `urbi_native_event_new` uses `urbi_value_nil()` helper.
- (EVENT-021) `uevent.h` layout claim matches `_Static_assert`.
- (EVENT-022) `urbi_register_event_drain` asserts not-in-step.
- (EVENT-023) `uevent_subscribe` iteration-during-emit safety
  documented.
- (EVENT-026) Native dispatch exempt from v0.5.2 fix rationale
  documented.
- (TAGCH-003) Redundant zero-loop in `utag_create` retired
  (paired with EVENT-016 dropping pad0 in `urbi_event_create`).
- (TAGCH-006) UTag layout note matches `_Static_assert(sizeof == 56)`.
- (TAGCH-007) `UTag.gc_byte` GC-managed since M5 documented.
- (TAGCH-008) `UTag.flags` lists active `UTAG_FLAG_*` values.
- (TAGCH-009) `urbi_object_get_or_create_change_node` shade scope
  correction.
- (TAGCH-010) `UGC_HAS_SLOT_CHANGE_EVENT` sticky-bit semantics
  documented at call sites.
- (TAGCH-017) `walk_utag` double-visit is correctness, not perf
  (documents why the second visit is load-bearing for color-flip
  during sweep).

### Module

- (Phase 13, MOD-032) Helper extraction: `module_buf_free` shared
  static-inline replaces three slightly-divergent inline frees in
  `umodule_destroy`.

### Realm

4 IDs (Phase 14, REALM-008/025/026/034).

- (REALM-008) `realm_list_walk_roots` asserts UTag layout invariant
  via `_Static_assert(offsetof(UTag, type_tag) == 0)`.
- (REALM-025) `realm.c` row-reference cleanup post-M3.
- (REALM-026) UStrand GC-walker contract relocated to file-level
  comment.
- (REALM-034) `urbi_realm_has_live_work` NULL out-param test
  coverage.

### Public C API

6 IDs (Phase 15, API-024/025/026/028/029/030).

- (API-024) Consecutive `URBI_DEBUG` blocks merged.
- (API-025) `urbi_get_determinism_checksum` handles `UVAL_OBJECT` /
  `UVAL_EVENT` explicitly.
- (API-026) **int/float checksum hash unified via `memcpy`** —
  pointer-cast asymmetry between INT and FLOAT in the determinism
  checksum eliminated.
- (API-028) `urbi_get_determinism_checksum` documents 6 inputs in
  its docstring.
- (API-029) Module loader doc cites `ULOAD_UNSUPPORTED_VERSION`.
- (API-030) README public API surface claim corrected.

### Sched / strand (chstr)

6 IDs (Phase 16, CHSTR-025/032/034/035/036/043).

- (CHSTR-025) `wait_payload` reason-discriminator contract
  documented + `_Static_assert`-pinned.
- (CHSTR-032) `urbi_strand_attach_ambient_tags` zero-init via
  shared `urbi_zero` helper (replaces field-by-field volatile-byte
  loop).
- (CHSTR-034) `uchunk.c` M3-baseline comment refreshed (M3-era
  jargon replaced with current architecture text).
- (CHSTR-035) `ustrand.h` M2-field landing comment refreshed.
- (CHSTR-036) `ustrand.h` init/destroy described as live (was
  marked stub).
- (CHSTR-043) `ustrand` `module_instance` field GC-managed contract
  documented.

### Strict tooling

- (Phase 19) `make test-cppcheck` driven from 135 informational
  residuals to 0 and **promoted to releasetest hard-fail gate**
  (commit `2532858`). New project-level `.cppcheck.suppressions`
  for `unusedFunction`, `unusedLabelConfiguration`, and
  `assignBoolToPointer` (computed-goto false positives in
  `src/vm/uvm.c`). Per-line suppression `2952abc` documents an
  intentional `bugprone-branch-clone`. Const-pointer sweep across
  6 sites (`c0c6eb8`); see Compatibility note below.
- (Phase 20) `make test-tidy-strict` driven from 23 informational
  residuals to 0 and **promoted to releasetest hard-fail gate**
  (commit `54bfb61`). `bugprone-branch-clone` sweep (`2952abc`),
  per-line `TIDY-003` design pin for `UProtos` pointer encoding
  (`1469d3a`), `valist` false-positive suppress with rationale
  (`ad025dd`), residual fixes for loop-var / widening cast / macro
  parens / UVM padding (`3915f64`), CPPCHK-012 line-number realign
  (`afdb1b2`).

### Footprint

vs v0.5.7-fixes archive baseline (host 352 K / arm 174 K / riscv 324 K):

- host  liburbi.a: 346 K  (Δ −1.7 %)
- arm   liburbi.a: 169 K  (Δ −2.9 %)
- riscv liburbi.a: 315 K  (Δ −2.8 %)

Reductions sourced from Phase 3 dead-code removal, Phase 13 helper
extraction (`module_buf_free`), Phase 16 `urbi_zero` callsite sweep
(CHSTR-032), Phase 17 cppcheck-driven variable-scope narrowing, and
Phase 18 inline-helper tightening (UValue↔UEvent kind predicates).
Remaining overage vs the v0.5.6 baseline is documented as load-bearing
(M5 reactive-runtime IC tables, atom families, `urbi_opcode_shapes[]`
verifier table, deferred slot-change emit ring).

text+data+bss `size --total liburbi.a` post-tightening: host 135 022 B,
arm 62 378 B, riscv 80 084 B.

### Documentation

- (Phase 21) **Docstring coverage gate** at
  `tests/scripts/check_docstring_coverage.sh` enforces a `/** ... */`
  block above every header-exposed declaration in `include/urbi/*.h`
  (functions, structs, enums, macros). Awk-based; tracks brace
  depth, cascade rule for grouped docstrings, skips `_internal.h`.
  11 missing docstrings filled in `include/urbi/{gc,sched}.h`
  (`c6d78e5`); script (`7302690`); promoted to releasetest hard-fail
  gate (`6f36457`).
- (Phase 22) Design-risks register triaged: every row either
  closed-with-SHA or milestone-tagged. Workspace-root only;
  no urbi-embedded commits.
- (Phase 24) **CONTRIBUTING.md final read-through** (3 commits:
  strict-tooling all-categories hard-fail rewrite `b6503bb`,
  v0.5.x cleanup ramp summary `4061c09`, final read-through
  `02a4e7b`). Documents the four hard-fail gates (cppcheck-strict,
  tidy-strict, scan-build, docstring-coverage) joining the existing
  ASan / UBSan / valgrind / cross-arm / cross-riscv / determinism
  / docs-check / coverage gates.

### Build / tests / tools

- (Phase 17, CPPCHK-006) `urbi_object_install_property`: silence
  cppcheck `unreadVariable` on debug-only `found`.
- (Phase 17, CPPCHK-008) Variable scope narrowed at 5 cppcheck-flagged
  sites. Phase 17 T141 + T142 SKIPPED — `bugprone-switch-missing-default`
  and `readability-redundant-casting` not in `.clang-tidy.strict`.
- (Phase 18, footprint) `UValue ↔ UEvent` kind predicates inlined
  in header (`4ec563f`); the only Phase 18 codegen-change commit.
- (Phase 23) Manual REPL sanity confirmed via cross-arm, cross-riscv,
  and 7 representative .chk fixtures; no commits.

### Dead code removed

13 IDs closed in Phase 3.

- (CHSTR-028) Duplicate UTag forward decl in `ustrand.h`.
- (EVENT-015) `UEVENT_FLAG_RESERVED` dead macro.
- (EVENT-016) Redundant `pad0` zero-loop in `urbi_event_create`.
- (GC-023) `TYPE_HOST_BACKED` unused flag.
- (GC-024) `UType.payload_size` unused field.
- (GC-025) `walk_vm_globals` no-op stub.
- (GC-034) `UGC_IS_WEAK` defined to 0 under `URBI_GC_NONE`
  (eliminates compile-time mention from the `URBI_GC_NONE` profile).
- (MOD-030) `proto` NULL guard converted to
  `URBI_INTERNAL_ASSERT`.
- (OBJ-027) `urbi_module_instance_destroy` annotated as M7-reserved.
- (OBJ-029) `walk_noop` comment refreshed post-M4.
- (PARSE-018) Empty `udesugar.c` retired.
- (PARSE-019) `PARSE_LAZY_PARAM_DEFAULT` unused error code removed.
- (VM-023) `OP_TAG_STOP` stub comment refreshed post-M3.

T32 PARSE-020 + T33 VM-022 verified already-resolved upstream
(`label_m5_stub` retired with OP_INVOKE in v0.5.6).

### Bytecode

Bytecode-byte-identical AND wire-format-byte-identical against
`v0.5.7.1` for all 148 pre-existing `.chk` fixtures. The only diff in
the Wave-6 wire-format gate is the **new** `LEX-034` fixture
(`tests/chk/lex/block_comment_no_nest.chk`) which adds a 149th
hash entry.

### Compatibility

- Module wire format unchanged at v1.5 (no version bump).
- `urbi_run_chunk` / `urbi_run_script` parameter `module` widened
  to `const struct UModule *module` as part of the Phase 19
  cppcheck-driven const-pointer sweep (closes API-026 in spirit;
  technically source-breaking for any external caller that passes
  a non-const pointer; M7 will revisit the embedding ABI surface
  formally).
- All other public symbols, signatures, and enum values are
  source-compatible with `v0.5.7-fixes` / `v0.5.7.1`.

## v0.5.7.1 — 2026-05-08 — Wire-format hash gate determinism hotfix

Hotfix on top of `v0.5.7-fixes`.

### Fixed

- `tests/scripts/capture_wire_format_hashes.sh` was non-deterministic across
  runs: per-fixture extraction wrote source to `mktemp /tmp/urbi_chk_XXXXXX.u`
  and passed that random path to `urbi --dump-wire-format`, embedding it in
  the wire format's `source_name` field. Two consecutive captures of the same
  fixture produced different SHA256s. The wire-format hash gate added at
  `v0.5.7-fixes` Phase 22 was therefore not actually a useful gate — the
  checked-in `tests/golden/v0.5.7-fixes-wire-format-hashes.txt` was just one
  arbitrary instance of the noisy output.

  Surfaced at the start of Wave 6 (v0.5.8-cleanup) when the Phase-0 baseline
  sanity check (re-capture and diff against the golden) failed.

### Changed

- `capture_wire_format_hashes.sh` now feeds source via stdin (`-f -`) so the
  embedded `source_name` is the stable string `"-"` rather than a random
  per-run path. Output is now byte-identical across runs.
- `tests/golden/v0.5.7-fixes-wire-format-hashes.txt` regenerated from the
  fixed script (148 hashes; 5 COMPILE_ERROR + 143 deterministic SHA256s).

### Added

- `tests/scripts/check_wire_format_determinism.sh` runs the capture script
  twice and asserts byte-identical output. Hard-fail in `make releasetest`
  Phase 1 via the new `test-wire-format-determinism` target.

### Bytecode

Bytecode-byte-identical against `v0.5.7-fixes` (no codegen change). The
`tests/golden/v0.5.7-fixes-bytecode-hashes.txt` content matches.

## v0.5.7-fixes — 2026-05-08

Wave 5 of v0.5.x cleanup ramp.

### Fixed

- 123 audit findings dispositioned `wave-5-fixes` (53 bug + 39 unsafe + 11 cov +
  12 smell + 5 doc + 3 dead). Closing-commit table at
  the v0.5.x cleanup audit findings Wave-5 Resolutions section.
- 4 prior-wave carry-forwards: API-004 (`urbi_run_chunk` realm threading),
  WATCH-023 (test-seam removal — 47 call sites lifted to
  `tests/unit/twatcher_install_helper`), EMIT-019 underlying (JMP offset
  pc-based helper), FOUND-032 (`ustrand_consts_for_closure` shared helper).
- 3 Wave-4 forward-looking items: ic_names symbol-table verifier
  cross-validation (T77), deeply-nested closure verifier sanity (T78),
  nupvals/nparams range check at proto decode (T79).
- Emit register-allocation drift cluster (Phase 2): EMIT-009 through EMIT-018
  closed via the M2-NaryEmit pattern (replace `next_reg--` with
  `next_reg = freereg`). New helper `free_reg_freereg_synced` for
  watcher-install teardowns; new `uemit_jmp_offset` PC-based helper.
- VM dispatch ownership (Phase 5): VM-001/002/003/005/012/013 closed; new
  `vm_install_check_closure_operand` / `vm_install_check_event_operand` /
  `vm_install_fault` helpers centralize kind-checking and fault propagation.
- Object-model fixes (Phase 13): in-place barrier writes (OBJ-003), shape-clone
  on aliased mutation (OBJ-005), CoW props_table on cross-shape mutation
  (OBJ-018), idempotent install short-circuit (OBJ-041).
- Module loader hardening (Phase 15): MOD-001/002/004/007/017/018/019 closed;
  new `ULOAD_INVALID_ARG` and `ULOAD_OVERSIZED` error codes; explicit
  `URBI_MAX_INSTRS_PER_PROTO = 1<<20` cap.
- Foundations (Phase 16): 17 of 18 audit IDs closed (T89/FOUND-028 BLOCKED —
  audit assumption inverted; filed as backlog). New `urbi_value_nil()` helper
  in `<urbi/types.h>`.

### Added

- Strict-tooling gates: `make test-tidy-strict`, `make test-cppcheck`,
  `make test-scan-build`, `make test-corpus-sanitize`,
  `make test-branch-coverage`. `test-scan-build` and `test-corpus-sanitize`
  promoted to `make releasetest`; `test-tidy-strict` (25 residuals) and
  `test-cppcheck` (145 residuals) remain informational pending v1.x
  close-out (filed as backlog under "Strict-tooling residuals").
- `tests/scripts/capture_wire_format_hashes.sh` (Wave-4 deferral landed) +
  `--dump-wire-format` flag on `tools/urbi`.
- `tests/golden/v0.5.7-fixes-bytecode-hashes.txt` (post-Wave-5 disasm-text
  baseline) and `tests/golden/v0.5.7-fixes-wire-format-hashes.txt`
  (first-ever on-disk wire-byte baseline).
- Full-corpus sanitizer gate (`make test-corpus-sanitize`) — all 148 `.chk`
  fixtures × 2 sanitizers (ASan + UBSan) under one target. Promoted to
  releasetest Phase 2 (solo). valgrind memcheck is intentionally omitted
  per the project's "Not valgrind-wrapped" rationale (Makefile:88-90):
  the run_chk.sh wrapper-bash itself leaks ~520 bytes via yyparse on
  every fixture, drowning real urbi-side leak signal. Unit-test-binary
  valgrind coverage stays in `make test-valgrind`.
- Co-located regression tests for every `src/*.c` fix commit per Wave-5 Gate
  G1 (TDD-per-fix-commit discipline). New test files:
  `test_emit_freereg_drift.c`, `test_emit_error_paths.c`,
  `test_vm_dispatch_ownership.c`, `test_gc_scratch_rooting.c`,
  `test_gc_sweep_accounting.c`, `test_sched_state_aliasing.c`,
  `test_watcher_ownership.c`, `test_event_runtime.c`, `test_tag_barrier.c`,
  `test_object_in_place_barrier.c`, `test_realm_globals.c`,
  `test_module_loader_hardening.c`, `test_foundations.c`,
  `test_chunk_strand.c`, `test_public_api.c`.
- Coverage-gap tests (Phase 20, T119-T125, audit IDs COV-001..008):
  `src/urbi.c` 8 % → 100 %, `src/vm/uop_fork.c` 60 % → 91 %,
  `src/changed/uchanged.c` 77 % → 86 %, `src/emit/*` 84.1 % → 87.1 %.
- Two new `UErrCode` values: `URBI_ERR_SHAPE_BOUNDS`, `URBI_ERR_PROTO_DEPTH`
  (Phase 14 T68 — distinct error codes for `set_global` / `get_global`).
- Two new `UEmitError` values: `EMIT_TOO_MANY_ARGS` (T13/EMIT-014),
  `EMIT_TAG_SPILL_OUT_OF_RANGE` (T14/EMIT-015).
- Two new `UModuleLoadResult` values: `ULOAD_INVALID_ARG` (T73/MOD-007),
  `ULOAD_OVERSIZED` (T74/MOD-017).
- New scheduler helpers: `sched_strand_unbind_from_ready_queue`,
  `sched_strand_unbind_from_sleep_queue` (Phase 14 T69 + Phase 8 T41).
- New realm helper: `realm_install_const` (Phase 14 T70).
- New emit helpers: `uemit_jmp_offset`, `free_reg_freereg_synced`.
- New VM helpers: `vm_install_check_closure_operand`,
  `vm_install_check_event_operand`, `vm_install_result_is_fatal`,
  `vm_install_fault`.
- `event_sync_degradation_warned` UVM field (T28/EMITR-005 — one-shot warn).
- `gc_surviving_bytes` UVM field (T37/GC-015 — persistent sweep accumulator).
- `tests/unit/twatcher_install_helper.{c,h}` — test-only seam lifted from
  `src/watcher/uwatcher.c` per WATCH-023.
- `docs/internals/emit-correctness-notes.md` — verified-clean rationale for
  the OP_CLOSURE-clobber-event family (EMIT-043).

### Changed

- `urbi_run_chunk` and `urbi_vm_run` signatures: realm argument now threaded
  through (closes API-004; signature change cascades to ~80 callers across
  test files + `tools/urbi.c`).
- `URBI_VERSION` literal updated to "0.5.7-fixes" (closes API-011).
- Full-corpus ASan + UBSan + valgrind gate (all 148 `.chk` fixtures) is now
  standing CI; was a curated subset.
- AST_PROP_GET / AST_PROP_SET (arrow-access `obj.x->y`) now reject explicitly
  with `EMIT_UNSUPPORTED_AST` rather than fall through silently (T23/SCAN-001).
  Resolution B (lower to OP_GETSLOT/OP_SETSLOT) filed as v1.x backlog —
  legacy stdlib `profile.u`, `object.u`, `run-test.u` and third-party
  `jouve gsrapi.u` actively use this syntax.
- Named-function decl (`function f() { ... }` at top level) now rejected with
  `PARSE_NAMED_FUNCTION_NOT_SUPPORTED` (T25/PARSE-004; was silently discarded).
  Resolution B filed as v1.x backlog.
- `async` keyword consistently rejected as identifier in both var-decl and
  assignment contexts (T26/PARSE-007; was inconsistent — accepted in
  `var async = 1` but rejected at `async = 2`).

## v0.5.6-bytecode — 2026-05-07

Wave 4 of v0.5.x cleanup ramp.

### Changed (bytecode wire format — INTENTIONAL BREAK v1.4 → v1.5)

- (T18) Bytecode header version byte advances `0x14 → 0x15`. v1.4 modules
  are rejected with `ULOAD_UNSUPPORTED_VERSION`; rebuild from source to
  migrate. No live-system v1.4 → v1.5 upgrade path; this runtime does
  not promise bytecode stability before v1.0.
- (T16-T17) `OP_INVOKE` retired; M5 reactive opcodes 39-46 renumbered to
  38-45. `OP_MAX` shrinks from 47 to 46. Computed-goto + opcode-name +
  disassembler tables updated in lockstep.
- (T12-T14) Wire format extended: `nested[]` UProto array + per-proto +
  root-chunk `ic_count` + `ic_names` are now persisted. Pre-v1.5 modules
  silently dropped these on round-trip; the in-process emit-then-run
  path masked the gap (closures and IC names re-populated from emit
  state, not load).

### Fixed

- (T4 / MOD-009) Verifier replaces hardcoded M1-shaped operand checks
  with an opcode-shape table; legitimate v1.5 modules with `OP_LOADBOOL`,
  `OP_PUSH_TAG`, `OP_GETUPVAL`, etc. no longer wrongly flagged as
  `ULOAD_CORRUPT`.
- (T5 / MOD-010) `Bx` range checks added for `OP_CLOSURE` (against
  `nested_count`) and `OP_LOAD_REALM_GLOBAL` (against the runtime symbol
  table sentinel). `OP_JMP` Bx is signed and intentionally unbounded.
- (T8 / MOD-038) Header bytes 16-23 are now strictly enforced as zero
  on load. Forward-compat flags can no longer slip through silently.

### Internal

- (T3 / pre-T4) New file-private `src/module/uopcode_shape.h` data
  structure consumed by the verifier; future M6+ opcodes register here
  rather than extending an inline switch.
- (T7 / MOD-029) `kCanary[6]` consolidated into shared header
  `src/module/umodule.h`; serializer + deserializer share the
  definition.
- (T11) `UProto.ic_name_strs` + `UModule.ic_name_strs` companion fields
  added — populated at emit time by the emitter and at load time by the
  deserializer; lazily interned into `USymbol *` at first
  `urbi_module_instance_create`.

### Documentation

- (T9 / MOD-008 + MOD-039) `umodule_deserialize` docstring rewritten to
  state actual error-state behavior (the partial-state-on-error rough
  edge from MOD-001/MOD-002 is documented honestly; "module is left
  empty on error" is replaced with "module may hold partial buffers on
  error; `umodule_destroy` is safe in either case"). Stale "M1" pool
  comment retired.
- (T21) `docs/internals/bytecode-format.md` updated for v1.5 sections;
  `docs/internals/opcodes.md` reflects renumber + OP_INVOKE retirement.
- (T22) Compatibility-ledger gains S-bytecode-v1.5 row.

### Verification

- New golden baseline at `tests/golden/v0.5.6-bytecode-hashes.txt` (148
  fixture hashes) replaces `v0.5.3-bytecode-hashes.txt` as the operative
  gate going forward. `v0.5.5-bytecode-hashes.txt` retained as a
  historical anchor.
- All `make releasetest` gates green: host + ASan + UBSan +
  valgrind-fast + tidy + docs-check + coverage 85% + GC stress +
  URBI_GC_NONE smoke + 3-preset × 100-run determinism + cross-arm +
  cross-riscv + LOC-cap.
- Round-trip unit tests added (T6 + T15) covering verifier rejections
  for every shape-table category + nested-proto + ic_names persistence.

## v0.5.5-naming — 2026-05-07

Internal symbol + public C API naming pass per CONTRIBUTING.md §3.2 conventions.
Last opportunity to settle public API before M6 grows the surface.

### Changed (naming hygiene)

- (T7) Public VM lifecycle promoted: `uvm_init` → `urbi_vm_init`,
  `uvm_destroy` → `urbi_vm_destroy`, `uvm_run` → `urbi_vm_run`.
- (T8) `URBI_ERR_OUT_OF_MEMORY` (-10) collapsed into `URBI_ERR_OOM` (-3);
  native-code OOM no longer reports a distinct numeric.
- (T9) `URBIAtomFamilyTag` retired; public surface uses `URBIAtomFamily`
  directly. `URBI_ATOM_*_F` enumerators drop the `_F` suffix.
- (T10) `URBI_WATCHDOG_*` macros promoted to `UWatchdogMode` enum.
- (T11) `URBI_SCHED_CLASS_DEADLINE` → `URBI_SCHED_DEADLINE` (drop `_CLASS_`
  infix).
- (T15-T17) New `include/urbi/types.h` hosts UValue / UExecStatus / UErrCode
  / UVMError / UVMAllocFn + opaque struct fwd-decls; `include/urbi/urbi.h`
  no longer pulls in `src/sched/ustrand.h`.
- (T18) `URBI_ASSERT_NOT_ISR(vm)` macro now calls `urbi_in_isr(vm)`;
  embedders no longer need a complete `struct UVM` definition to use the
  macro.

### Internal

- (T6) Mass uppercase-literal-suffix sweep: `1u`/`0u` → `1U`/`0U`
  (~650 sites; clang-tidy `readability-uppercase-literal-suffix`).
- (T20-T31) Per-subsystem internal symbol renames per spec §3.2; ~70 audit
  IDs closed.
- (T32) `misc-include-cleaner` direct-include sweep (~241 sites) — every
  TU now declares its own includes rather than relying on transitive
  pulls.
- (T33) Const-correctness sweep (28 sites flagged by cppcheck
  `constParameterPointer` + `constVariablePointer`).

### Added

- `urbi-embedded/CONTRIBUTING.md` — naming + layout + commit conventions
  (will be finalized in Wave 6).
- `runtime/umacros.h` gains `urbi_memeq` static-inline helper retiring
  file-local `lex_memeq` + `module_memcmp` lookalikes.

### Fixed

- (T12) `urbi_step` declaration/definition argument-name drift — public
  header and impl now use `budget_instructions` consistently.
- (T13) 8 sites in uunwind public APIs had `strand` (decl) vs `s` (def)
  drift — settled to `strand`.
- (T14) `urbi_run_chunk` no longer collapses every non-OOM `uvm_run`
  error into `URBI_ERR_STRAND_FATAL`; the underlying `UErrCode` now
  propagates through. The `realm` argument is no longer silently
  discarded.

### Verification

- Bytecode byte-identical against `tests/golden/v0.5.3-bytecode-hashes.txt`
  (148 fixture hashes; the v0.5.3 baseline is the operative gate, not a
  fresh capture — no codegen changes in this wave).
- All `make releasetest` gates green: host, ASan, UBSan, valgrind-fast,
  tidy, docs-check, coverage 85%, GC stress, URBI_GC_NONE smoke,
  3-preset × 100-run determinism, cross-arm, cross-riscv, LOC-cap.

## v0.5.4-decompose — 2026-05-06

Wave 2 of the v0.5.x pre-M6 cleanup ramp: decomposes the four
translation units that exceeded the 1000-LOC soft cap into focused
per-concern files of ≤600 LOC; extracts the 14-site duplicated
volatile-byte-zero loop into a shared `urbi_zero` helper; lands
audit-driven cross-cutting refactors across all subsystems.  Bytecode
output is byte-identical to v0.5.3-layout.

### File decompositions

- **`uemit.c` 3563 LOC → 9 files** (EMIT-045): `uemit.c` retains top-level
  dispatch; `uemit_funcstate.c`, `uemit_expr.c`, `uemit_stmt.c`,
  `uemit_react.c`, `uemit_unwind.c`, `uemit_disasm.c`,
  `uemit_serialize.c`, `uemit_diag.c` extract per-concern logic.
  `uemit_internal.h` provides inter-TU linkage.
- **`uvm.c` 2314 LOC → 6 files** (VM decomposition): `uvm_init.c`,
  `uvm_diag.c`, `uvm_closure.c`, `uvm_run.c`, plus header-inlined
  `uvm_arith.h`.  `uvm_internal.h` provides inter-TU linkage.
- **`uparse.c` 1698 LOC → 6 files** (PARSE-021): `uparse_top.c`,
  `uparse_separators.c`, `uparse_stmt.c`, `uparse_react.c`,
  `uparse_expr.c`.  `uparse_internal.h` provides inter-TU linkage.
- **`uobject.c` 1157 LOC → 4 files** (OBJ-045): `uobject_proto.c`,
  `uobject_lookup.c`, `uobject_slot.c`.  `uobject_internal.h`
  provides inter-TU linkage.

### New CI gate

- `make test-loc-cap`: scans all `src/` translation units and fails on
  any file exceeding 1000 LOC.  One documented exception: `uvm.c`
  (dispatch loop, 1336 LOC — see `CONTRIBUTING.md`).

### Audit findings closed

**Decomposition** (EMIT-045, VM decomposition, PARSE-021, OBJ-045)

**Theme 4 — volatile-byte-zero dedup**: `urbi_zero` helper extracted;
14 open-coded volatile-byte-zero loops across 9 files swept to the
shared helper (FOUND-030).

**Lex** (LEX-019, LEX-020, LEX-024, LEX-025): duration-suffix dispatch
table-driven; single-char punctuation switch table-driven; UToken
init helpers consolidated; digit-accumulator loops unified.

**Watcher** (WATCH-018, WATCH-019, WATCH-024): header-init + pool-drain
deduplication; thin `run_closure_on_scratch_frame_with_result` wrapper
retired; pool_destroy loops consolidated.

**Event / EmitR** (EMITR-006, EMITR-008): waiter-wake loop deduped;
ring weak-ref contract clarified; payload coercion / pad-zero / proto
OOM propagation deduplicated (EVENT-019, EVENT-027).

**Realm** (REALM-020, REALM-021, REALM-022, REALM-028, REALM-029):
cleanup ladder consolidated; strlen dedup; dead resolvers removed;
snapshot-next teardown path fixed; zero-helper sweep applied.

**Chunk / strand** (CHSTR-020, CHSTR-021, CHSTR-022, CHSTR-029,
CHSTR-031, CHSTR-044): strncpy dedup; arm-init helper extracted;
REPL drain + error format consolidated; cleanup-stack OOM propagated;
destroy / counter / regstack lifecycle centralized.

**Module** (MOD-031): `umodule_deserialize` split into per-section
helpers.  MOD-027 investigated and not applicable (include order is
load-bearing).

**GC** (GC-027, GC-028): duplicate gray-drain loop collapsed; gc_byte
color-update pattern deduplicated.

**Foundation** (FOUND-020, FOUND-031): `utype.c` folded into
`uvalue.c`; `uvalue_le` 4-way dispatch simplified.

**VM** (VM-008): IC-resolve preamble extracted into shared helper
`ic_resolve_proto_inst`.

**Emit core** (EMIT-033, EMIT-035): AST_TRY three near-duplicate paths
collapsed to `emit_try_frame`; `uemit_disassemble` table-driven.

**Object** (OBJ-031, OBJ-032, OBJ-034): atom switch / walker /
module-instance init deduplicated.

**Parse** (PARSE-013, PARSE-014, PARSE-022, PARSE-023, PARSE-024,
PARSE-025): postfix-emit duplicate collapsed; IDENT-lookahead Pratt
duplicate removed; `parse_at` split into per-form helpers;
`reject_bare_function_forms` extracted; `parse_statement_or_expr`
decomposed; 4× arena-array doubling pattern collapsed.

**Tag / changed / event** (TAGCH-005): OOM-throw block collapsed; dead
placeholders removed.

**Cross-compile fix**: `ulex.c` duration-suffix table used `memcmp`
from `<string.h>`; replaced with local `lex_memeq` so the file
compiles under `-ffreestanding` (cross-arm / cross-riscv targets).

### Carried forward / deferred

- OBJ-041 (`urbi_object_install_property` spurious topology_gen bump) —
  wave-5-fixes; correctness impact requires shape-mutation audit.
- WATCH-023 (`urbi_watcher_install_internal` dead seam) — wave-6-cleanup.
- FOUND-032 (`pop_call_frame` cleanup-TU coupling) — wave-5-fixes.
- WATCH-017 (IC table walk hand-rolled): investigated; fixing requires
  proto_inst membership guarantee not yet established — defer to M6.
- EVENT-025 (subscriber-walk snapshot-next enforcement): design work
  needed; defer to M6.
- MOD-027 (umodule.h includes uframe.h mid-typedef): not applicable —
  the include is load-bearing.
- FOUND-029 (vm_alloc helper duplication): three sites differ in subtle
  ways; consolidation deferred to wave-5-fixes with test coverage.

### Verification

- 1138 unit cases / 6382 checks / 0 failed; 148 `.chk` fixtures
- Bytecode output byte-identical to v0.5.3-layout (all 148 fixtures)
- `make test-loc-cap`: EXEMPT 1336 `src/vm/uvm.c`; no FAILs
- `make releasetest`: all gates green (host + ASan + UBSan + tidy +
  lint + docs-check + coverage + stress + GC-none + cross-arm +
  cross-riscv + valgrind memcheck)

## v0.5.3-layout — 2026-05-06

Wave 1 of the v0.5.x pre-M6 cleanup ramp: pure mechanical layout
reorganization.  Every `.c`/`.h` under `src/` (except `urbi.c`) moves
into a per-subsystem folder; three filename renames; layout-flagged
doc + comment fixes; six `_Static_assert` layout pins; dead-code
removal of the orphaned `UScratchFrame` heap allocation.  No
behavioral change.  Bytecode-byte-identical to v0.5.2 — every
`.chk` fixture passes unchanged.  Binary footprint within 0.1 % of
v0.5.2 across host / arm-cortex-m7 / riscv-rv32imc.

### Layout

- Every source under `src/` (except the entrypoint `urbi.c`) moved
  into a per-subsystem folder.  Top-level `src/` now holds only
  `urbi.c` and subsystem directories.
- New folders: `lex/`, `parse/`, `emit/`, `vm/`, `event/`, `tag/`,
  `changed/`, `module/`, `value/`, `runtime/` — alongside the
  pre-existing `gc/`, `sched/`, `watcher/`, `realm/`, `object/`.
- `ustrand.{c,h}` joins the `sched/` folder (strand is the unit of
  scheduled execution).
- `urealm_globals.{c,h}` moves into `realm/` — closes REALM-012
  (the file was orphaned at `src/` top level while every other
  realm file lived under `src/realm/`).
- `uchunk.c` joins `module/` (chunks become modules at runtime).
- `uast.h` moves into `parse/` (co-located with the parser that
  produces it).
- `umacros.h` moves into `runtime/`; the audit verdict was "earns
  its keep, relocate not retire" (closes API-031, INC-002).

### Renames

- `src/event_native.{c,h}` → `src/event/uevent_native.{c,h}` —
  picks up the project `u` prefix (closes EVENT-012).
- `src/tag_native.{c,h}` → `src/tag/utag_native.{c,h}` (closes
  TAGCH-015).
- `src/object/umoduleinstance.{c,h}` → `src/object/umodule_instance.{c,h}`
  — snake-case for compound filenames (closes OBJ-022).
- File-private field `UStrand::is_uvm_run_transient` →
  `is_transient_strand` — the name no longer embeds the
  implementation function `uvm_run`; docstring rewritten to
  describe both transient strand sources (`uvm_run` and
  `urbi_run_closure_on_scratch`).  Closes CHSTR-023.

Function symbol names inside the renamed files (e.g.,
`event_native_register`, `walk_umoduleinstance`) stay unchanged —
symbol renames belong to wave-3-naming.

### Hygiene

- `_Static_assert` layout pins for `UStrand` (2880 B), `UWatcher`
  (240 B), and `UTag` (56 B) — the three v0.5.x runtime cell types
  that lacked compile-time size pins.  `UEvent` (40 B), `UObject`
  (56 B), `UChangedNode` (32 B) already had asserts.  All six are
  guarded on `__SIZEOF_POINTER__ == 8` so 32-bit cross targets
  build clean.  Closes CHSTR-041.
- Removed dead `UScratchFrame` heap allocation (~280 B / VM saved
  at runtime).  The v0.5.1-cond-unstub patch routed scratch frames
  onto the C stack via `urbi_run_closure_on_scratch`; the heap
  slot on UVM was orphaned.  Removed the struct definition,
  `vm->watcher_scratch_frame` field, init allocation, destroy
  free, and the defensive test
  `watcher_scratch_frame_allocated_at_init`.  The OOM-counter
  test `vm_oom_first_alloc_fails_second_would_succeed` adjusts
  its target from alloc #5 to alloc #4 to match the new init
  sequence.  Closes WATCH-022.
- `is_ident_cont` forward-decl in `src/lex/ulex.c` removed; the
  function reordered above its first caller.  Closes LEX-022.

### Documentation

- `gc/` header docstrings refreshed: `gc_byte` bit allocations
  enumerated bit-by-bit; `UTYPE_HOST_BASE = 64` claimed/reserved
  ID gap documented; `urbi_register_type` host-only contract
  stated; `urbi_gc_slice` per-phase termination documented;
  `urbi_gc_alloc` ATOMIC_FINISH role narrowed to match
  implementation; 1ms pause-time budget cited near
  `urbi_gc_slice`; `UGC_IS_WEAK` reservation note clarified;
  `ugc_none.h` "spec-only at M3" replaced with current
  `URBI_GC_NONE` compile-smoke description; stale path
  references updated to new subsystem prefixes.  Closes GC-014,
  GC-022, GC-026, GC-029, GC-031, GC-032, GC-033, GC-035, GC-036,
  GC-039.
- `runtime/uframe.h` indirect-`UValue`-dependency note rewritten
  to acknowledge the actual `uvalue.h → umodule.h → uframe.h`
  include cycle (the cycle prevents the obvious "just include
  `value/uvalue.h` here" fix).  Closes FOUND-006, FOUND-022.
- Public-header `include/urbi/urbi.h` `#include` paths adjusted
  to new subsystem-prefixed form (`sched/ustrand.h`); docstring
  updated to cite `sched/ustrand.h` and `vm/uvm.h`.
  `URBI_ASSERT_NOT_ISR` docblock now states why the macro lives
  in the public header (embedder-facing assertion surface) and
  acknowledges the `isr_check_fn` internal-field dependency for
  wave-3-naming follow-up.  `umacros.h` docstring clarifies that
  `URBI_ASSERT_NOT_ISR` is in the public header.  Path-only
  mechanical close for API-012, API-018, API-027, INC-003,
  GC-012; the deeper "public should not include internal" cleanup
  is a wave-3-naming carry-forward.

### Build

- Makefile `SRC` discovery extended with wildcards for the 10 new
  subsystem folders.

### Verification

- 1138 unit cases / 6382 checks / 0 failed (was 1139 / 6383 at
  v0.5.2; the −1 case + −1 assertion are the now-removed
  `UScratchFrame` defensive test).
- 148 / 148 `.chk` fixtures pass.
- ASan + UBSan + valgrind-fast + valgrind-deep clean across the
  full suite.
- Coverage 85 % (line); GC stress targets all PASS; GC pause max
  2.7 µs (target 1 ms — 370 × margin); barrier throughput 40 M
  ops/sec; event-emit throughput 11.8 M ops/sec.
- `make tidy` + `make docs-check` clean.
- `URBI_GC_NONE` compile smoke PASS.
- `make test-determinism` (3-preset × 100-run) PASS.
- Cross-arm + cross-riscv builds green.
- Binary footprint deltas vs v0.5.2 baseline:
  - host-x86_64:    `liburbi.a` text 135 973 B → 135 910 B  (−63 B, −0.05 %)
  - arm-cortex-m7:  `liburbi.a` text  55 710 B →  55 650 B  (−60 B, −0.11 %)
  - riscv-rv32imc:  `liburbi.a` text  69 774 B →  69 712 B  (−62 B, −0.09 %)
  All deltas trace to the removed `UScratchFrame` allocation
  paths; well within the cleanup-design §4.2 ±5 % gate.
- Bytecode output byte-identical to v0.5.2 across all 148
  fixtures (no codegen change in this wave).
- `git log --follow` traces every moved file back to its v0.5.2
  history (rename detection threshold satisfied for all 58
  file-rename entries across the 17 file-move/rename commits;
  similarity ≥ 94 %).

### Wave-1 audit IDs closed (28)

API-012, API-018, API-027, API-031, CHSTR-023, CHSTR-041, EVENT-012,
FOUND-006, FOUND-022, GC-012, GC-014, GC-022, GC-026, GC-029, GC-031,
GC-032, GC-033, GC-035, GC-036, GC-039, INC-002, INC-003, INC-004,
INC-005, LEX-022, OBJ-022, REALM-012, TAGCH-015, WATCH-022.

Carries forward to wave-3-naming: the deeper "public header should
not include internal types" hygiene (API-012/018/027 + INC-003 +
GC-012 in their structural form, beyond the path-fix mechanical
close landed here).

See the v0.5.x cleanup audit findings doc for full audit context.

---

## v0.5.2-scratch-frame-followup — 2026-05-05

Closes the four scratch-frame stub sites left hook-stubbed at
`v0.5.1-cond-unstub` (AT_SYNC body inline, falling-edge onleave inline,
drain-time onleave during tag-stop cascade, event sync-emit subscriber
body), plus a tidy baseline fix at `src/uchanged_emit.c:108` and a
bundled emit-time bug fix surfaced during execution.  All four wires
route through the v0.5.1 helper `urbi_run_closure_on_scratch` (or a
new payload variant added here for the event sync-emit body's
R[0]-payload contract).  Activates `at_onleave.chk` as a live
conformance fixture using the integer-counter pattern (M5 lacks
string-literal lex; deferred to backlog).

### Added

- `urbi_run_closure_on_scratch_with_payload` variant
  (`src/watcher/uwatcher.h`, `src/watcher/uwatcher_scratch.c`) —
  same shape as `urbi_run_closure_on_scratch` but writes a `payload`
  UValue into the closure's R[0] before dispatch.  Used by AT_EVENT_SYNC
  subscriber bodies to receive the emit payload as their first argument.
  Both public functions share a static `run_on_scratch_core` so the
  no-payload path is a thin shim.
- 4 new end-to-end unit tests exercising the wired paths through the
  production install + dispatch path with no test hooks:
  `tests/unit/test_at_sync_scripted.c`,
  `tests/unit/test_tag_stop_onleave_scripted.c`,
  `tests/unit/test_event_sync_emit_scripted.c`, plus 2 cases added to
  `tests/unit/test_uwatcher_scratch.c` for the payload variant.
- 3 regression tests for the AST_AT_EVENT emit register-allocation
  desync (`tests/unit/test_parse_at_event.c`,
  `tests/unit/test_emit_at_slot_change.c`) — each disassembles the
  install opcode and asserts `event_reg != body_reg`.
- Activated `tests/chk/reactive/at/at_onleave.chk` as a live
  conformance fixture covering the falling-edge onleave path
  (`URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE` guard).

### Fixed

- **AT_SYNC body inline dispatch** (`invoke_body_inline`,
  `src/watcher/uwatcher_eval.c`): replaced the M5 stub fall-through
  with a call to `urbi_run_closure_on_scratch`.  Throws are
  suppressed (eval pass cannot propagate per spec §6.4).  Test hook
  short-circuit preserved.
- **Falling-edge onleave dispatch** (`invoke_onleave_inline`,
  `src/watcher/uwatcher_eval.c`): same swap; onleave fires through
  real bytecode dispatch on falling edge after a prior body fire.
- **Drain-time onleave dispatch** (`run_watcher_onleave`,
  `src/watcher/uwatcher_drain.c`): tag-stop cascade now runs each
  member watcher's onleave handler through real bytecode dispatch
  via `urbi_run_closure_on_scratch`.
- **Event sync-emit subscriber dispatch** (`run_event_body_on_scratch`,
  `src/uevent_emit.c`): AT_EVENT_SYNC subscribers now run through
  real bytecode dispatch via `urbi_run_closure_on_scratch_with_payload`.
  Payload UValue arrives at the closure's R[0].  The `vm->in_watcher_scratch`
  re-entry guard at the top of the wrapper preserves the existing
  degrade-to-async-with-warn behaviour for nested sync emits.
- **AST_AT_EVENT emit register-allocation desync** (`src/uemit.c`):
  `emit_expr` for AST_IDENT global-fallback and AST_MEMBER_GET only
  bumped `e->next_reg` and not `e->current_fs->freereg`, so AST_AT_EVENT's
  subsequent `emit_function_literal` could allocate `body_reg` on top
  of `event_reg`.  OP_CLOSURE then clobbered the event pointer at
  runtime, and OP_AT_EVENT_SYNC_INSTALL tripped `R[A] == R[B]` under
  URBI_DEBUG asserts.  AST_WATCHER and AST_WAITUNTIL avoided this
  by wrapping cond in a closure (which routes through `emit_function_literal`
  symmetrically).  Fix syncs `freereg` to `next_reg` after `emit_expr`
  for the event expression at TWO sibling sites: AST_AT_EVENT (sync +
  async event install) and AT_SLOT_CHANGE (`obj.x.changed?` install).
  Affects scripted `at sync (X?) Y`, `at (X?) Y`, and
  `at sync (obj.x.changed?) Y` whenever the event_expr is non-trivial.
- **`make tidy` baseline failure** (`src/uchanged_emit.c:108`): clang-tidy
  under `-warnings-as-errors` flagged the `(UValue){0, {0}}` brace-init
  for missing `v` union initialiser.  Fixed via designated-init using
  the existing block-scoped `UValue nil = {0}` idiom (matches 14 other
  sites).  Pre-existing M5 baseline; failed identically on
  `v0.5.0-reactive` (`4faf5bc`) and `v0.5.1-cond-unstub` (`a1e8683`).

### Numbers

- 1139 unit cases / 6383 checks / 0 failed (was 1131 / 6314 at
  `v0.5.1-cond-unstub`).
- 148 `.chk` fixtures pass; `at_onleave.chk` activated as a live
  conformance fixture (joining `at_rising_edge.chk` and
  `whenever_level.chk` from v0.5.1).
- All `make releasetest` gates green: host + URBI_DEBUG + ASan +
  UBSan + valgrind (memcheck full leak-check) + determinism (3-preset
  × 100-run) + cross-arm + cross-riscv + tidy + docs-check + coverage
  (85% line) + GC stress + URBI_GC_NONE compile smoke.

## v0.5.1-cond-unstub — 2026-05-05

The M5 reactive runtime shipped with the scripted cond closure
hook-stubbed: scripted `at (cond) body`, `whenever (cond) body`, and
`waituntil (cond)` could not fire end-to-end because the install-time
and eval-time cond evaluation paths returned `UVAL_NIL` unless test
hooks were set.  This patch wires both paths to a new shared scratch-
frame runner, fixes the cascade of latent issues that surfaced once
real cond closures actually executed, and activates the first two
reactive `.chk` fixtures.

### Added

- New helper `urbi_run_closure_on_scratch` (`src/watcher/uwatcher_scratch.c`)
  — synthesizes a transient `UStrand` on the C stack, arms it from the
  closure via `urbi_strand_arm_from_closure`, runs `dispatch_loop_until_yield`
  with `URBI_SCRATCH_BUDGET_OPS` (default 4096) bound, and captures the
  `OP_RET` value plus a throw flag.  Mirrors `uvm_run`'s transient-strand
  pattern but scoped to single-closure cond eval with bounded budget +
  no-yield contract.
- Public macro `URBI_SCRATCH_BUDGET_OPS` (default 4096) — override at
  compile time for footprint targets.
- 5 unit tests in `tests/unit/test_uwatcher_scratch.c` covering integer
  return, throw detection, NULL-closure handling, nil-literal cond, and
  bool comparison conds.
- 1 integration test in `tests/unit/test_at_scripted_e2e.c` proving
  scripted `at (Realm.x > 5) body` fires through real dispatch with no
  test hooks.
- Activated `tests/chk/reactive/at/at_rising_edge.chk` and
  `tests/chk/reactive/at/whenever_level.chk` — first two live reactive
  conformance fixtures (the other 10 reactive fixtures remain deferred
  pending body-inline / onleave-inline / event-sync-emit unstubs or
  `Event.new()` / `Object.new()` stdlib at M6).

### Fixed

- **Install-time cond eval** (`run_closure_on_scratch_frame_with_result`,
  `src/watcher/uwatcher_install.c`): replaced the M5 stub fall-through
  with a call to `urbi_run_closure_on_scratch`.  Test hook short-circuit
  preserved so existing install-trace tests continue to inject specific
  cond results without going through real bytecode dispatch.
- **Eval-time cond eval** (`invoke_condition_closure`,
  `src/watcher/uwatcher_eval.c`): same swap; eval-time throws fail-soft
  as nil per the existing contract (caller `watcher_eval_dirty` is void
  and cannot propagate).
- **Closure ownership transfer at install** (`uwatcher_install.c`):
  `install_watcher_runtime` now calls `strand_closure_unlink` to move
  cond / body / onleave closures from the strand's `closure_list` to
  the watcher.  New `URBI_WATCHER_OWNS_COND` / `_BODY` / `_ONLEAVE`
  flag bits drive `pool_free`'s closure release.  Without this,
  `uvm_run`'s post-run cleanup freed the watcher's closures while
  still in use.
- **Body strand IC table wiring** (`uwatcher_spawn.c`):
  `do_spawn_body_coroutine` now wires `body->module_instance` by
  walking `vm->module_instances_head` to find the owning instance via
  pointer-range comparison on `proto_inst`.  Required because
  `urbi_strand_arm_from_closure` (the M3 helper) doesn't set
  `module_instance`, and OP_GETSLOT/SETSLOT at `frame_count==0` reads
  through it.  See backlog: `UClosure.owning_mi` field is the cleaner
  long-term shape (set at OP_CLOSURE — eliminates the pointer walk).
- **OP_GETSLOT/SETSLOT entry_closure fallback at frame_count==0**
  (`src/uvm.c`): the IC table is now resolved from
  `s->entry_closure->proto_inst->ic_table` when available, falling back
  to `s->module_instance->proto_instances->entries[0].ic_table`.  The
  former is the correct (non-root-chunk) IC table for body strands
  spawned from nested closures.
- **OP_SETSLOT slow-path write barrier** (`src/uvm.c`): the slow path
  through `urbi_slot_set_slow` now calls `urbi_gc_slot_write` (with
  conservative slot index 0 sentinel — observer_dirty ignores the key
  at M5; real index needed at M6).  Without this, COW writes never
  bumped `watcher_dirty_count` so watchers with read-sets that include
  slow-path receivers never fired.
- **`sched_strand_init` for `uvm_run` transient strand** (`src/uvm.c`):
  arms `instruction_budget_remaining` so the first safepoint hit
  inside the transient run actually crosses the dirty-walk path.
  Without this, the transient strand yielded at first safepoint with
  budget=0 and `watcher_eval_dirty` was missed.
- **REPL drain loop** (`tools/urbi.c`): the REPL now drains spawned
  body strands via `urbi_step` after each `uvm_run`.  Without this,
  body strands queued during a REPL line never executed before the
  next line ran.  Embedders driving `urbi_step` directly are
  unaffected — this only changes the REPL's host-driver shape.
- **`pending_onleave_head` drain at `pool_destroy`** (`uwatcher.c`):
  `urbi_tag_stop` (called from `urealm_teardown_all`) moves watchers
  from `active_watchers_head` to `pending_onleave_head`.  The pre-T12
  `pool_destroy` only drained the active list — pending entries with
  `OWNS_*` flags would have leaked their owned closures.  Now both
  lists are drained.
- **`vm->in_watcher_scratch` zero-init** (`src/uvm.c`): the field was
  declared in M5 (spec #3 §5.4) but missing from the `uvm_init`
  initialiser block.  Stack-allocated UVMs in tests left it
  uninitialised; valgrind flagged the read at `uevent_emit.c:140` and
  `uchanged_emit.c:36`.  Pre-existing M5 latent bug; surfaced when the
  cond-unstub work raised valgrind coverage.

### Deferred (separate follow-up patch)

The same `urbi_run_closure_on_scratch` primitive can wire four more
sites that are still hook-stubbed at this release.  Each is a 5-10 LOC
patch reusing the helper:

- `invoke_body_inline` (`src/watcher/uwatcher_eval.c`) — AT_SYNC body
  inline execution.
- `invoke_onleave_inline` (`src/watcher/uwatcher_eval.c`) — onleave
  handler on falling edge.
- Drain-time onleave (`src/watcher/uwatcher_drain.c`) — onleave during
  tag-stop cascade.
- Event sync-emit body (`src/uevent_emit.c`) — sync subscribers run
  inline on emit.

These are tracked as M6 prerequisites or `v0.5.2-scratch-frame-followup`
candidates.  Backlog also tracks two clean-up items:

- `UClosure.owning_mi` field set at OP_CLOSURE — replaces the pointer-
  range walk in `uwatcher_spawn.c` with a direct field read.
- Real slot index in slow-path `urbi_gc_slot_write` calls — needed at
  M6 when observer_dirty starts using the key.

### Numbers

- 1131 unit cases / 6314 checks / 0 failed (was 1124 / 6272 at v0.5.0).
- 148 chk fixtures pass; 2 reactive fixtures activated as live
  conformance tests (at_rising_edge, whenever_level).
- ASan + UBSan + valgrind-fast clean.

## v0.5.0-reactive — 2026-05-04

The M5 reactive runtime milestone. Persistent watchers, events, slot-change
subscriptions, tag enter/leave hooks, and the realm-global identifier
resolution that anchors them. Bytecode v1.3 → v1.4 hard break.

### Breaking changes

- **Bytecode v1.4**: version byte incremented; loader rejects v1.3 and earlier.
- **Reserved keywords**: `at`, `whenever`, `waituntil`, `onleave`, `sync` are
  hard keywords; `async` is a soft keyword (allowed as identifier at v1.0,
  deprecation warning v1.x). `var at = 1` raises
  `PARSE_RESERVED_KEYWORD_AS_IDENT`.
- **gc_byte bit 7** allocated as `UGC_HAS_SLOT_CHANGE_EVENT`; all 8 bits are
  now claimed. Future additions must multiplex or extend to gc_word.

### Language additions

- `at (cond) body [onleave handler]`, `at sync (cond) body`,
  `whenever (cond) body`, `waituntil (cond)` — cond watchers with rising/
  falling edge fire, level-triggered whenever, and one-shot waituntil
  strand-park.
- `at (e?) body [onleave]`, `at sync (e?) body [onleave]` — event subscribers
  via spec #3 `OP_AT_EVENT_INSTALL` / `OP_AT_EVENT_SYNC_INSTALL` dispatch.
- `at (obj.slot.changed?) body`, `at sync (obj.slot.changed?) body` — slot-
  change subscribers via spec #4 `OP_GETSLOT_CHANGE_EVENT` lookup-or-create.
- `at (mytag.enter?) body`, `at (mytag.leave?) body` — tag tier-2 hooks.
- Postfix `e!` and `e!(p)` for event emission (multi-arg parse error
  `PARSE_EMIT_MULTI_ARG_V1`); `e.syncEmit(p)` and `Event.waituntil(e)`
  native methods.
- Top-level identifiers (`Object`, `Tag`, `Event`, `Integer`, `Float`,
  `String`, `Bool`, `Nil`, `Void`, `List`, `Dict`, `Symbol`, `Realm`, `nil`,
  `void`) resolve to a 15-entry static built-in registry per realm.
- Top-level `var X = …` and `function f() { … }` write to realm-global slots
  (const-attributed for built-ins).

### New opcodes

- 39 `OP_AT_INSTALL` (cond watcher install).
- 40 `OP_AT_SYNC_INSTALL` (sync cond watcher install).
- 41 `OP_WHENEVER_INSTALL` (level-triggered watcher install).
- 42 `OP_WAITUNTIL_INSTALL` (one-shot strand-park install).
- 43 `OP_AT_EVENT_INSTALL` (event subscriber install, async).
- 44 `OP_AT_EVENT_SYNC_INSTALL` (event subscriber install, sync).
- 45 `OP_GETSLOT_CHANGE_EVENT` (per-object slot-change UEvent lookup-or-create).
- 46 `OP_LOAD_REALM_GLOBAL` (frame prologue load realm.global_object into
  reserved register).

(Plan template said opcodes 29-36 — wrong. M3 control-transfer + M4 INVOKE
already claimed those slots; M5 lands at 39-46.)

### Runtime additions

- `UEvent` (40 B) cell type with `at_watchers_head` (UWatcher chain) and
  `waiters_head` (UStrand chain) intrusive subscriber lists.
- `UChangedNode` (32 B host / 16 B 32-bit) cell type for per-UObject slot-
  change subscriber chain.
- `UTag` GC-promoted (was M3 host-managed); 48 → 64 B with `enter_event` and
  `leave_event` lazy-allocated event slots.
- `UWatcher` 200 → 240 B default (104 → 144 footprint preset); 6 modes
  total (AT, AT_SYNC, WHENEVER, WAITUNTIL, AT_EVENT, AT_EVENT_SYNC); 4 new
  flag bits.
- `UStrand` 256 → 288 B; 2 new wait states (`USTRAND_WAIT_WATCHER=0x32`,
  `USTRAND_WAIT_EVENT=0x33`); back-pointer to owning watcher; event-waiter
  fields.
- `UObject` 48 → 56 B with `changed_events_head` lazy-allocated chain head.
- `UVM` gains trace fields (16-entry read-set for cond install) + slot-change
  deferred-emit ring (default 64, footprint 16 entries × 24 B).

### Public C API additions

- `urbi_realm_set_global(realm, name, value)` — install non-const global.
- `urbi_realm_set_global_const(realm, name, value)` — install const global.
- `urbi_realm_get_global(realm, name, *out)` — read a realm global.
- `urbi_register_event_drain(vm, drain_fn)` — host callback for ISR-injected
  events; drains at safepoint via the M3 SPSC ring.

### Determinism

- `URBI_SCHED_COOPERATIVE × URBI_GC_INCREMENTAL` remains bit-for-bit
  reproducible. New rules (D-watcher-1/2, D-event-1/2, D-slotchange-1/2/3):
  watcher install + event subscribe + slot-change install all FIFO over
  registration order; sync subs run before async; deferred-emit ring drains
  at safepoint before `watcher_eval_dirty`.

### Gates

- 1124 unit cases / 6272 checks / 0 failed.
- 148 chk fixtures (was 127 pre-M5).
- ASan + UBSan + valgrind-fast + valgrind-deep all clean (a pre-existing
  `test_emit_diag` leak from M5's own T32 was fixed in R8 hardening).
- 3-preset × 100-run determinism gate green (cross-spec det fixtures
  deferred-stub at this release; activate post-M6).
- GC pause within budget; cross-arm + cross-riscv build green.

### Known limitations (deferred to v1.x or M6)

- **Scripted at/whenever/waituntil cond closure is hook-stubbed.** The
  watcher install path is real, the registry is real, the eval/fire-decision
  machinery is real — but `run_closure_on_scratch_frame_with_result` is an
  M5-baseline stub returning UVAL_NIL. C-level unit tests cover the logic
  via `vm->test_install_cond_hook` and `vm->test_watcher_fire_hook`.
  Unblocking the scripted .chk fixture path is a 1-2 commit M6 prerequisite.
- ~48 of the planned 60 reactive .chk fixtures deferred (R8 minimum-viable);
  full corpus is v1.x.
- 4 of the planned 5 stress targets deferred.
- 5th slot-change callsite (namespace_set) deferred to M6.
- 7 of 8 M4-era prototype-chain fixtures still T38/T39-blocked (atom-method
  dispatch, class declaration, `.new()` stdlib — M6 territory).

### Notes

- Bytecode v1.4 is a hard break; pre-M5 bytecode files refuse to load.
- 74 commits on `topic/m5-reactive` since `v0.4.0-objects` follow-up `83bab89`.
- See `docs/milestones/m5-reactive.md` for the full retrospective.

## v0.4.0-objects — 2026-05-02

The M4 object model milestone. Introduces a prototype-based object system
with hidden-class shape inference, per-call-site inline caches, copy-on-write
slot inheritance, and atom-family inheritance constraints. Bytecode bumped
to v1.3; earlier `.urb` files are rejected at load time.

### Breaking changes

- **Bytecode v1.3**: version byte incremented; loader rejects v1.2 and earlier
  modules with a diagnostic. `UProto` gains `ic_count` (uint16) + `ic_names`
  side table (USymbol** parallel array). Recompile all `.urb` files.

### Object model

- `UObject` (48 B header): `cell` (GC) + `shape` + `slots` + `protos`
  (tagged uintptr_t) + `object_id` (per-VM monotonic) + `lookup_stamp` (cycle
  guard) + `flags` (atom family low 4 bits + IS_PROTOTYPE / FROZEN /
  SANDBOX_RO bits).
- Tagged-pointer prototype chain with three storage forms: empty (`protos=0`),
  single (`(p<<1)|1` — most common in legacy code), and heap (`UProtos*`
  pointer with bit 0 clear). `UPROTOS_FOREACH` macro captures `obj->protos`
  once at iteration start (legacy semantics per spec §6.3).
- `UShape` (56 B): hidden-class with `name` (last-added slot), `index`,
  `count`, `flags` (4-bit per-slot nibbles for OGET/OSET/CONSTANT/LOCAL),
  `parent`, `transitions` (UShapeMap cache), `props_table` (lazy per-slot
  UProps* dense array via UPropsTable wrapper).
- `UShapeMap` transition cache (open-addressing hash, power-of-2 capacity,
  USymbol* identity hashing); `urbi_shape_transition_add_slot` shares
  child shapes for identical construction sequences.
- `USlot` collapsed to 16 B (typedef of `UValue`); slot storage is
  `USlotArray` wrapper (UCell + entries[]).
- `UProps` (48 B): per-slot getter/setter/constant flag side table; allocated
  lazily when any slot in the shape lineage installs a property; stored in
  per-shape `UPropsTable`.
- Atom-family singletons: 9 lazily-allocated per-VM prototypes (Object,
  Integer, Float, String, List, Dict, Tag, Event, Symbol). Atoms pinned via
  GC root provider, not manual handle-table pinning.

### Inline caches and dispatch

- `UIC` (144 B at `URBI_IC_ENTRIES_PER_SITE=4`, the default; 80 B at =2;
  48 B at =1): per-site inline cache with `name`, parallel arrays of
  `recv_shapes` / `topology_gen` / `slots` / `uprops` / `flags`, `n` valid
  count, and `replace_cursor`.
- `URBI_IC_ENTRIES_PER_SITE` compile-time tunable {1, 2, 4} drives IC site
  width; default 4, footprint preset binds 2.
- `UModuleInstance` + `UProtoInstance`: per-VM RAM tier separate from the
  read-only `UModule`. Two instances of the same module have independent
  IC tables; threaded onto `vm->module_instances_head`.
- Per-function emit-time IC bookkeeping: `UFuncState.ic_next` counter,
  `ic_names` dynamic array (16-slot growth chunks), capped at 256 sites
  per function (`EMIT_TOO_MANY_IC_SITES`).
- `urbi_object_resolve_slot`: cycle-safe DFS over prototype chain
  (left-first via `UPROTOS_FOREACH`), 64-deep stack bound, `lookup_id`
  stamping for re-entry guard with rollover handling.
- COW on assignment to inherited slot: `urbi_slot_set_slow` installs a
  local slot on receiver via `urbi_object_set_local_slot`, leaving the
  prototype's slot intact.

### Language and parser

- `OP_GETSLOT` (opcode 27) and `OP_SETSLOT` (opcode 28) ABC-encoded:
  A=dst (or src), B=recv, C=ic_index. IC tables looked up via
  `cur_closure->proto_inst->ic_table[ic_index]`.
- AST nodes: `AST_MEMBER_GET` / `AST_MEMBER_SET` for `obj.x` / `obj.x = v`;
  `AST_PROP_GET` / `AST_PROP_SET` for `obj.x->prop` / `obj.x->prop = v`.
- Lexer tokens: `TOK_DOT` (`.`) and `TOK_ARROW` (`->`).
- Parser preserves method-call syntax: `obj.method()` parses as
  `AST_CALL{callee=AST_MEMBER_GET}` not as `AST_MEMBER_GET` followed by
  `AST_CALL`.

### Public C API

- `include/urbi/object.h`: opaque `UObject` / `UShape` typedefs; atom-family
  enum (`URBIAtomFamilyTag` with `_F` suffix to avoid namespace collision
  with internal `URBIAtomFamily`); `urbi_object_root` /
  `urbi_object_atom` / `urbi_object_add_proto` / `urbi_object_remove_proto`
  / `urbi_object_set_protos`.
- `UModuleInstance` opaque typedef + `urbi_module_instance_create` /
  `urbi_module_instance_destroy`.
- `USlotHandle` wrapper: `urbi_object_get_slot` returns a handle pointing
  at the slot's current owner (may be the receiver or an inherited
  prototype). `urbi_slothandle_read_value` / `write_value` validate or
  refresh on access; handles become permanently invalid after the slot is
  removed.
- Fallback slot retry: `urbi_object_lookup` retries once with name=`fallback`
  on full-tree miss. Cycle-safe (no infinite recursion when looking up
  `fallback` itself).

### Inheritance semantics

- `valid_proto`: atom-family constraint at addProto/setProtos time. An atom
  can only inherit from its own family or root Object; root Object never
  blocks.
- `urbi_object_set_protos` is atomic: validate every survivor before any
  state change. Dedup first-occurrence-wins, capped at 64 unique items.
- `IS_PROTOTYPE` bit (`URBI_OBJ_FLAG_IS_PROTOTYPE`): set monotonically on
  any UObject when it joins another object's protos chain. Read by
  `urbi_object_set_local_slot` to drive the conditional topology bump.
- `urbi_object_clone` (atom-aware): preserves parent's atom family in the
  clone's flags low-4-bits and threads the parent into protos as the
  single-tag form.

### topology_gen mutation surfaces

Every IC-invalidating mutation surface bumps `vm->topology_gen` (u64 since
T2). 12 surfaces per the topology-generation spec §4.1:

- Slot install / remove on a prototype (rows 1, 4); slot install on a
  non-prototype receiver does NOT bump (caught by the IC's shape-mismatch
  check per §4.2 row 2).
- Property install / remove / in-place mutate (rows 5–7) via
  `urbi_object_install_property` / `_remove_property` / `_set_property_value`.
- Prototype-chain mutations (rows 8–12) via `urbi_object_set_protos_*`.

`urbi_get_determinism_checksum` (URBI_DEBUG) folds `topology_gen` +
`lookup_id` + `next_object_id` plus per-IC state (`n` + `replace_cursor` +
`recv_shapes[]` + `topology_gen[]`) into the per-step checksum. The
existing CI determinism gate (3 presets × 100 runs) remains green.

### Garbage collector

- New cell types: `UTYPE_PROTOS=9`, `UTYPE_SHAPE=10`, `UTYPE_PROPS=11`,
  `UTYPE_SLOTHANDLE=12`, `UTYPE_MODULE_INSTANCE=13`,
  `UTYPE_PROTO_INSTANCE=14`, `UTYPE_SHAPE_MAP=15`,
  `UTYPE_PROPS_TABLE=16`, `UTYPE_SLOT_ARRAY=17`. (`UTYPE_OBJECT=1` is the
  pre-existing M3 baseline tag.)
- Per-type mark walkers in `src/object/utypes_init.c`. `UObject` walker
  shades `shape` + `slots[i]` UValue payloads + `protos` chain via
  `UPROTOS_FOREACH`. `UShape` walker shades `parent` + `transitions` +
  `props_table` entries. Wrappers (UPropsTable, USlotArray,
  UProtoInstance bulk) reach their content via the owning UObject's
  walker through `offsetof` arithmetic.
- `UClosure` embeds `UCell` as first member (closes the M3 baseline
  TODO). `urbi_gc_upvalue_write` may now safely cast `UClosure*` →
  `UCell*` for the barrier color check; OP_SETUPVAL invokes the
  barrier before the actual store.
- New `UVAL_OBJECT` UValue kind (=8); incremental GC's `uvalue_is_heap`
  / `uvalue_as_cell` treat UVAL_OBJECT as heap-bearing.
- M4 GC root provider registered at `uvm_init`: walks the 9 atom
  singletons + root shape + every `UModuleInstance` on the per-VM list.
- GC strand walker swapped from `ready_head` + `sleep_q_head` to
  realm-hierarchy iteration (`vm->realms_head` → `realm.strands_head`).
  Closes the WAITING_JOIN root gap; generalizes to any wait state.
- Transient strands (uvm_run helpers) routed to `vm->global_realm` at
  creation; unlinked symmetrically at exit.

### Public-facing scheduler contract

- New `docs/internals/scheduler-design.md` documents the GC walker
  contract: every strand whose register window may hold GC-managed
  UValues MUST be reachable via `vm->realms_head → realm.strands_head`.
  Scheduler implementations are responsible; the GC walker assumes the
  invariant without re-verification.

### Tests and CI

- 884 unit cases / 5330 checks at default build; 902/5370 under
  URBI_DEBUG. Green under host + ASan + UBSan; cross-arm (Cortex-M7) +
  cross-riscv (rv32imc) build clean.
- New test suites: `test_uic`, `test_uslothandle`, `test_topology_gen`,
  `test_scheduler_invariant`, `test_gc_strand_walker`,
  `test_ugc_object_cells`. Existing `test_uobject` / `test_ushape` /
  `test_funcstate` extended.
- `make test-determinism` (3 presets × 100 runs) green.
- `make releasetest` aggregate target: all stages green.
- Footprint preset (`URBI_IC_ENTRIES_PER_SITE=2`) builds and tests pass.

### Known limitations / deferred

- **`UClosure.proto_inst` binding for transient strands**: the
  OP_GETSLOT / OP_SETSLOT dispatch arms are wired but the `proto_inst`
  field on a closure created by `vm_alloc_closure` is NULL (no
  UModuleInstance association). Affects urbiscript-level slot dispatch
  through transient strands; the dispatch arms diagnose with
  `TypeError: GETSLOT: no IC table bound`. Unblocking this requires 1–2
  commits in `src/uvm.c` near OP_CALL plus the strand-spawn path. All
  C-API paths (urbi_object_resolve_slot, urbi_slot_get_slow,
  urbi_slot_set_slow, urbi_slot_handle_*) work end-to-end.
- **Class declaration emit** (`class Foo : A, B { body }`),
  **Class.new() stdlib wiring**, **get/set parse sugar**: deferred
  pending the proto_inst binding above.
- **Legacy `.chk` fixture ports** (lookup, inheritance, slot-cow-const,
  shared-protos, class, fallback, atom-clone, atoms): deferred for the
  same reason.
- **Getter / setter dispatch macros** (`URBI_VM_DISPATCH_GETTER` /
  `_SETTER`): the IC entries' OGET/OSET flags are honored at slow-path
  fill time, but the dispatch macros themselves are deferred to land
  alongside the frame-push wrapper at `Class.new()`.
- **Wire-trailer reader / writer for v1.3 `UProto.ic_count` /
  `ic_names`**: in-memory foundation lands at T1; nested-proto
  serialization deferred to a future task that adds the symbol-pool
  framing.
- **`URBI_IC_ENTRIES_PER_SITE=1` build**: a pre-existing `replace_cursor`
  modulo-1 wraparound assumption (cursor=0 vs assertion of cursor=1) in
  one IC test is unrelated to T44; default (=4) and footprint (=2) both
  pass cleanly.
- **Determinism `.chk` fixture covering all 12 §4.1 surfaces**: deferred
  pending end-to-end runtime; URBI_DEBUG `determinism_checksum_includes_
  ic_state` test pins the IC-fold step at unit level.

## v0.3.0-concurrency — 2026-04-28

The M3 concurrency milestone. Adds six subsystems above the M2 expression
foundation: control transfer (exceptions, tags, unwind), chunk lifecycle
(realms, namespaces, step driver), cooperative scheduler (ISR-safe event ring,
strand C API), incremental tri-color GC (5-phase state machine, debt-triggered
slices, 3 barrier surfaces, host-handle pinning), tag/watcher data and eval
layer (UTag, UWatcher pool, read-set, watcher eval loop, pending-onleave drain),
and determinism infrastructure (checksum diagnostic, CI gate, time literals,
legacy corpus port). Bytecode bumped to v1.2; earlier `.urb` files are rejected
at load time.

### Breaking changes

- **Bytecode v1.2**: version byte incremented; loader rejects v1.1 and earlier
  modules with a diagnostic. Recompile all `.urb` files.

### Language

- Time and angle literals: `100ms`, `1s`, `2.5s`, `180deg` lexed to
  `TOK_DURATION` / `TOK_ANGLE`; `ms`/`s`/`m`/`h`/`d` suffixes emit
  microsecond integer values; `deg`/`rad` emit float radian values.
- `,` (parallel fire-and-forget) and `&` (parallel join) separator runtime
  activated. `,` spawns N-1 child strands + runs last child inline.
  `&` compiles rhs to closure, runs lhs inline, then OP_FORK_JOIN /
  OP_JOIN_WAIT; result is void. Child handles are `UVAL_STRAND` (kind=7).
- `try` / `catch` / `finally` / `throw` — full emit and runtime; exception
  value forwarded through catch register.
- Tag scopes — `mytag: { ... }` compiles to OP_PUSH_TAG / body / OP_POP_TAG;
  member-strand list maintained; `urbi_tag_stop` deposits UEXEC_TAG_STOP
  with C-1 priority.

### Unwind / exception model

- `urbi_unwind` walker: 5-kind absorption (OK / RETURN / THROW / TAG_STOP /
  CANCEL); replace-on-raise semantics; URBI_WARN_SUPPRESSED_UNWIND emitted
  via `host_log_fn`.
- `UExecStatus` enum (OK / RETURN / THROW / TAG_STOP / CANCEL / FATAL);
  `urbi_exec_status_name`.

### Chunk lifecycle and scheduler

- `URealm` + `UNamespace`: per-realm GC root provider, 4-function Realm C API,
  namespace resolution protocol.
- `urbi_step` 4-state driver (OK / QUIESCENT / FATAL / YIELD_BUDGET); 4
  chunk-execution wrappers.
- ISR-safe SPSC event ring: `urbi_inject_event` as the sole ISR-safe entry
  point; bounded drain at `urbi_step` entry.
- Strand C API: `urbi_strand_create` / `start` / `spawn` / `cancel` / `panic`
  / `reset`; ambient-tag attachment; cooperative FIFO run-queue.
- `URBI_DEBUG` build mode: ISR-safety assertions at all non-ISR entry points;
  callback watchdog (configurable warn / assert threshold).

### Incremental GC

- Tri-color mark-sweep with 5-phase state machine; `urbi_gc_slice(vm, budget)`
  incremental driver; `urbi_gc_force_full` synchronous path.
- Three barrier surfaces: `urbi_gc_slot_write` (forward Dijkstra + watcher
  dirty hook), `urbi_gc_register_write` (no-op), `urbi_gc_upvalue_write`.
- Root-provider registry: up to 8 providers; 5 registered at M3 (scheduler,
  realm list, intern table, host handles, watcher table).
- Host-handle table: `urbi_pin` / `urbi_unpin`; `urbi_register_type` with
  finalizer dispatch; `UType.destroy` called from sweep.
- GC pause max 2.8 µs measured (357× margin under 1 ms target).
- `make test-gc-pause` gated stress binary; `make test-stress` 4-program suite;
  `make test-gc-none-build` strategy-swap smoke; all wired into `make releasetest`.

### Tag / watcher subsystem

- `UTag` host-managed (via `alloc_fn`); ambient-tag inheritance via synthetic
  TAG_SCOPE cleanup entries; member-strand and member-watcher lists.
- `UWatcher` pool: 200-byte record, pre-allocated slab, freelist, `in_use` /
  `high_water` counters.
- Read-set capture: bit-6 (`UGC_HAS_WATCHER_OBSERVER`) lifecycle; tail-insert
  for deterministic eval order; install-time `last_value_cache` seed.
- Watcher eval loop: `watcher_eval_dirty` walks active list; edge/level firing
  per spec §6.2/§6.3; `UScratchFrame` (~280 B) allocated at `uvm_init`.
- Pending-onleave queue: drain reuses `in_watcher_eval` reentrancy guard;
  OP_POP_TAG and `urbi_tag_stop` cascade watchers before scope destruction.

### Determinism

- `urbi_get_determinism_checksum` (`URBI_DEBUG`): XOR-reduce over active-watcher
  list and dirty count; enables replay comparison.
- `make test-determinism` CI gate: two consecutive `urbi_step` sweeps with
  checksum equality assertion; wired into `make releasetest`.

### Tests

- Unit cases: 772 (up from 489, +283); debug variant 786.
- `.chk` fixtures: 127 (up from 18, +109) across 8 subdirectories
  (`control_transfer/`, `chunk_lifecycle/`, `scheduler/`, `gc/`, `tag/`,
  `separator/`, `time_literals/`, `determinism/`).
- Cross-build: ARM Cortex-M7 32 KB text / RISC-V rv32imc 41 KB text (host
  65 KB). All three targets verified at `make cross-arm` / `make cross-riscv`.
- All 8 gates green: `make test` / `test-debug` / `test-asan` / `test-ubsan` /
  `cross-arm` / `cross-riscv` / `test-stress` / `test-gc-none-build`.

### Known limitations / deferred

- **Watcher body and on-leave execution** deferred to M5. `spawn_body_coroutine`
  and `run_watcher_onleave` are M3 stubs; tests use `test_watcher_fire_hook`
  and `test_watcher_onleave_hook` on `UVM`.
- **`,` shared-frame semantics** (spec §7.1) deferred to M5+. Current
  implementation uses closure-spawn; correctness is unchanged, only
  per-child allocation overhead differs.
- **`at`/`whenever`/`waituntil`** — reactive runtime deferred to M5.
- **Object method dispatch** — deferred to M4.
- **UVM struct padding** — `clang-analyzer-optin.performance.Padding` reports
  36 bytes excess in `struct UVM`; full field reorder deferred to avoid
  destroying semantic row-grouping in the struct comments.
- **Most legacy `.chk` corpus fixtures** remain deferred (require M4 object
  model or M5 reactive runtime). 127 fixtures active or structured as
  deferred placeholders for future milestones.

## v0.2.0-expressions — 2026-04-25

The M2 expressions milestone. Adds the full expression language surface
above the M1 arithmetic core: variables, closures, control flow, function
definitions and calls, per-parameter lazy arguments, statement separators,
and multi-VM hardening. Bytecode bumped to v1.1; earlier `.urb` files are
rejected at load time.

### Language

- Bytecode v1.1: version byte incremented; loader rejects v1.0 modules
  with a diagnostic. Reserved opcode slots assigned for all M2 additions.
- Per-VM string interning table (`ustr_intern`): strings are canonical;
  pointer equality implies content equality within a VM.
- Lua-FuncState-adapted register allocator with named locals, lexical
  block scopes, and cascading upvalue capture across arbitrarily nested
  function definitions.
- Statement separators: `;` (sequential with yield) and `|` (sequential
  atomic) ship full runtime semantics. `,` (parallel fire-and-forget) and
  `&` (parallel join) are parsed and represented in the AST; runtime is
  deferred to M3.
- Per-parameter `lazy` keyword: `function f(lazy x) { ... }` compiles
  the argument to a sub-proto thunk; the callee's first read of `x`
  forces evaluation implicitly.
- Control flow: `if` / `else` with proper short-circuit jumps; `while`
  with back-edge `OP_CLOSE` for closure-in-loop correctness.
- Function definitions, calls, and `return`.
- Comparison operators (`==`, `!=`, `<`, `<=`, `>`, `>=`).
- Boolean and nil literals (`true`, `false`, `nil`).

### Multi-VM hardening

- Per-VM `intern_table` and `topology_gen` fields on `UVM`; no
  file-scope mutable state remains.
- `UModule` gains `origin_vm` field; stamped at compile time, checked at
  load time.
- 8-case isolation test matrix in `tests/unit/test_multi_vm.c`
  (3 cases deferred to M3+/M5+/M6+).
- `tools/audit-globals.sh` + `cppcoreguidelines-avoid-non-const-global-variables`
  clang-tidy check gated under `make lint`.

### Migration notes

- `bare function name { body }` → `function name() { body }`.
  The bare-function form (no formal parameter list) now produces
  `PARSE_BARE_FUNCTION` at parse time. The migration recipe is mechanical.
- `closure(x) { ... }` → `function(x) { ... }`.
  The `closure` keyword is retired; `function` captures lexical scope
  universally. Note the `this`-binding migration trap: legacy `closure`
  bound `this` to the definition site; v1.0 `function` binds `this` to
  the call site. Affected pattern: `var obj.m = closure(t) { this.f(t) }`.
  Migration recipe: capture the receiver explicitly before the closure:
  `var self = this; var obj.m = function(t) { self.f(t) }`.

### Build (infra)

- Added `make releasetest` aggregate target that runs every host-side
  CI gate in sequence (sanitizer matrix, valgrind memcheck, lint,
  docs-check, coverage). Invoked manually before tagging a release
  or pushing branches touching multiple subsystems. Cross-compile
  targets are excluded; CI remains authoritative for cross-compile
  verification.

### Documentation (test infra)

- Documented the tiered test-target convention in
  `docs/internals/test-harness.md` — `make test` and the three fast
  companion variants form the pre-commit gate (~30 s combined);
  `make releasetest` is the pre-release gate (~3–5 min). Extended
  the target-reference table with a Runtime column and the
  previously-undocumented `test-switch` and `releasetest` rows.
- Codified the `.chk` fixture header schema (`Milestone:` /
  `Covers:` comment lines) in the "Authoring a new fixture"
  subsection of `docs/internals/test-harness.md`. Applied the
  schema to `tests/chk/arithmetic/basic.chk`. Enables `grep`-based
  discovery across the corpus at scale.

### Tests (chk-layout)

- Reorganized `tests/chk/` into feature subdirectories. Moved
  `tests/chk/arithmetic.chk` → `tests/chk/arithmetic/basic.chk` and
  documented the `tests/chk/<feature>/<name>.chk` layout convention
  in `docs/internals/test-harness.md`. The `test-chk` Makefile target
  already uses `find ... -name` recursively; no build change required.

## v0.1.0-skeleton — 2026-04-24

The walking-skeleton milestone. A complete end-to-end compile-and-execute
pipeline — lexer, parser, arena allocator, bytecode emitter + module
format, register-based VM, interactive REPL, and the first `.chk`
conformance fixture (`arithmetic.chk`). Not a production runtime — the
language surface is an 8-opcode Int/Float arithmetic subset — but the
pipeline is genuinely end-to-end, and every subsystem is covered by
unit tests, sanitizers, a valgrind-gated memcheck job, and coverage
instrumentation. Freestanding-clean front end cross-compiles for
Cortex-M7 and RV32IMC.

### REPL

- New binary `urbi` — the M1 walking-skeleton REPL. Drives the full
  `ulex` → `uparse` → `uemit` → `uvm` pipeline. Five modes: `urbi -i`
  (interactive via vendored linenoise, with `~/.urbi_history` persistence
  and `[%08u] value` timestamp frames via `clock_gettime(CLOCK_MONOTONIC, …)`),
  `urbi -e <expr>` (evaluate and print), `urbi [-f] <file>` /
  `urbi <file>` (run script; no per-statement print per Unix convention),
  `urbi --dump-bytecode` (disassemble via `uemit_disassemble`, incompatible
  with `-i`), `urbi --version` / `urbi --help`. Persistent `UVM` across
  interactive lines; fresh `UModule` + `UArena` per line. Implicit `|`
  statement terminator appended if missing.
- Source in `tools/urbi.c`, outside `src/` to preserve the `cc src/*.c`
  drop-in invariant. Never built on cross-compile targets.

### Formatter

- New library module `src/uvalue.{c,h}` — `UValue`-to-string formatter
  with Lua-5.4-style number formatting. Integer via `%lld`, Float via
  `%.14g` (f64) or `%.7g` (f32) with trailing `.0` appended on
  whole-number floats for visual kind-distinction. Bool, Nil, Str also
  covered (Bool/Str/Nil unreachable from M1 source; ship complete for
  M2+). Buffer-based, no allocation, thread-safe.
  `__STDC_HOSTED__`-gated — contributes no symbols under freestanding.

### Vendored

- `tools/linenoise.c` / `tools/linenoise.h` — single-file line editor
  from `github.com/antirez/linenoise` at commit
  `a15597057991fc748b3759cc66e157c9ea8bdfff`, BSD-2, preserved verbatim.
  Provenance ledger at `tools/LINENOISE-UPSTREAM.md`. Only linked into
  the `urbi` binary; never enters `liburbi.a`.

### Build

- New Makefile targets: `$(BUILDDIR)/urbi` (the REPL binary),
  `urbi-bin` (phony), `test-integration` (phony running the shell
  harness). `test` aggregate now depends on `test-integration`, so
  unit and integration run together under every sanitizer variant.
- `make tidy` scope widened to include `tools/urbi.c`.
  `tools/linenoise.c` stays outside the first-party tidy scope.
- `tools/linenoise.c` compiles with `-D_POSIX_C_SOURCE=200809L
  -D_XOPEN_SOURCE=700 -w` to suppress vendored-code warnings; `tools/urbi.c`
  compiles under the standard strict `$(CFLAGS)` discipline.

### Tests

- Added `.chk` conformance-fixture runner at `tests/integration/run_chk.sh`
  and the first fixture `tests/chk/arithmetic.chk` covering the M1 8-opcode
  VM. Folded into the `test` aggregate via a new `test-chk` Make target,
  so every sanitizer variant runs the fixture corpus automatically.
- `tests/unit/test_uvalue.c` — ~25 unit cases covering all 5 UValKinds,
  edge cases (INT64_MAX/MIN, -0.0, NaN, Inf, whole-number floats,
  scientific notation), and truncation (cap=0/1/3).
- `tests/integration/repl_smoke.sh` — POSIX sh harness covering every
  CLI mode and error path (30 cases). Runs against `$(BUILDDIR)/urbi`
  as part of `make test`.

### VM

- New module `src/uvm.{c,h}` implements the M1 register-based bytecode
  interpreter. Handles the 8-opcode M1 set (LOADK, MOVE, ADD, SUB, MUL,
  DIV, NEG, RET) with type-dispatched arithmetic per
  `docs/LANG-CONVENTIONS.md` §1.3: Int+Int wraps two's-complement,
  Int+Float promotes to Float, DIV always produces Float.
- Persistent `UVM` struct with `init`/`run`/`destroy` lifecycle and a
  VM-owned allocator hook distinct from the UModule loader's allocator.
  The 128-byte fixed error-message buffer carries
  `source:line:`-prefixed diagnostics for `UVM_TYPE_ERROR` and
  `UVM_OOM`; freestanding-compilable with no dependency on `<stdio.h>`
  / `<string.h>` / `<stdlib.h>` (stdlib-realloc shim is
  `__STDC_HOSTED__`-gated).
- Dispatch macros (`CASE` / `DISPATCH` / `NEXT`) expand to computed-goto
  under `__GNUC__` / `__clang__` and to `switch`/`case`/`continue`
  otherwise. Opcode bodies are written once; a new
  `URBI_VM_FORCE_SWITCH` build flag overrides the detection to exercise
  the switch path on GCC/Clang hosts.

### Tests (VM)

- `tests/unit/test_vm.c` — new test suite covering lifecycle,
  per-opcode happy paths, arithmetic type matrix, wrap semantics
  (INT64_MAX+1, INT64_MIN*-1, etc.), IEEE 754 DIV corners (±Inf, NaN),
  TypeError paths, OOM path, diagnostic prefix variants (`source:line:`,
  `line N:`, `instr N:`), and DiagWriter truncation. Coverage on
  `src/uvm.c` reaches 97% line.
- `tests/fuzz/fuzz_vm.c` — libFuzzer harness deserializing arbitrary
  bytes and executing any accepted module. 100K-iteration smoke run
  passes clean under ASan+UBSan.

### VM build and tooling

- New Make targets: `test-switch` (build with `-DURBI_VM_FORCE_SWITCH=1`
  for switch-dispatch CI parity) and `fuzz-vm` (libFuzzer harness
  build + run). `fuzz-build` aggregate extended to include `fuzz_vm`.
- `.clang-tidy` — `-clang-diagnostic-gnu-label-as-value` suppressed
  for the intentional GCC/Clang computed-goto extension.
- CI `host` job matrix extended with `test-switch`, bringing the matrix
  to 5 host modes.
- `docs/internals/design-decisions.md` — new entry explaining the
  uniform `UValue` tagged-struct decision across all targets.
- `docs/internals/architecture.md` — VM marked shipped; source table
  updated with `uvm.{c,h}` and `test_vm.c`.

### Documentation

- New `docs/` tree covering the first tranche of audience-A / audience-B
  / audience-C docs per the documentation-strategy design: `docs/README.md`
  (hub), `docs/language/getting-started.md`, `docs/internals/architecture.md`,
  `docs/internals/bytecode-format.md`, `docs/internals/opcodes.md`,
  `docs/internals/test-harness.md`, `docs/internals/design-decisions.md`.
  ~1500 lines of prose total.
- `docs/internals/test-harness.md` gains a Conformance fixtures section
  covering the `.chk` format, normalization rule, `make test-chk` entry
  point, and authoring flow.
- `docs/internals/test-harness.md` and `CONTRIBUTING.md`: acknowledge
  `make test-valgrind` as CI-gating (too slow for every commit, required
  before a milestone tag).
- `docs/internals/test-harness.md` Coverage expectations block replaces
  the stale `gcov`-on-debug-build language with `make coverage` (gcovr +
  HTML report + advisory CI job).
- `make docs-check` infrastructure + gating CI job: markdownlint-cli2
  over the `docs/` tree + intra-repo link-check.
- `.markdownlint.yaml` ships the ruleset (MD013 off; MD025/MD040/MD041
  on; neutral ordered-list-increment and heading-style).
- `WORKFLOW.md` §7 milestone ritual gains a "docs-for-this-release"
  step; §9 CHANGELOG cadence gains a `Documentation` subsection rule.

### Fixed

- `uvarint_decode_u` now rejects 10-byte encodings whose terminal-byte
  payload exceeds `0x01` as `UVARINT_OVERSIZE`. The previous code
  silently truncated values like `0x02..0x7F` at the 10th byte (payload
  bit shifts fall off the end of `uint64_t`), which could mask a
  corrupt bytecode module during loader verification. Paired
  positive-boundary test (`UINT64_MAX` at 10 bytes must succeed) added.
  The fix is defense-in-depth only — the loader verifier would have
  caught the resulting mis-decoded value downstream via `LOADK Bx`
  bounds or opcode range checks.
- `uvarint_size_zz` and `uvarint_write_zz` replace the
  implementation-defined `(v >> 63)` signed shift with a portable
  sign-extended mask built from `(v < 0)`. Equivalent on every
  mainstream compiler; defined by the C standard on all conforming
  implementations.

### Portability

- Compiler front-end compiles under `-ffreestanding` on toolchains without a C library (e.g. `gcc-riscv64-unknown-elf` on Ubuntu). `uarena_init` and the internal stdlib-backed allocator pair are guarded behind `__STDC_HOSTED__`; `uarena_alloc` uses a local byte-fill in place of `memset`. Freestanding callers must use `uarena_init_ex` or `uarena_init_static`.
- `umodule.c` and `uemit.c` follow the same freestanding discipline: local `module_zero` / `module_memcpy` / `module_memcmp` helpers in place of `<string.h>`, `stdlib_alloc` and `vsnprintf`-based `set_errmsg` guarded behind `__STDC_HOSTED__`, pluggable allocator on `UModule` via `UModuleAllocFn`. UModules hot-loaded in embedded builds (future M7) use caller-supplied allocators.

### Tooling

- Static-analysis Make targets: `tidy` (gating clang-tidy via `run-clang-tidy --warnings-as-errors='*'`), `tidy-fix` (local `--fix` convenience), `cppcheck` (advisory), `analyzer` (advisory GCC `-fanalyzer` in dedicated `build/host-analyzer/`), and `lint` aggregate.
- CI `lint` job runs all three analyzers parallel to host and cross-compile jobs. Advisory-ness of cppcheck and `-fanalyzer` lives in their Makefile targets' exit codes; CI job itself is gating.
- `.clang-tidy` disables `cert-err33-c`, `bugprone-easily-swappable-parameters`, and `readability-identifier-length` with per-check rationale comments — these stay disabled even if the broader check set is later expanded.
- Correctness-tooling Make targets: `coverage` (gcovr-backed coverage summary + HTML report at `build/host-coverage/report.html`), `test-valgrind` (memcheck-gated; catches uninitialized reads ASan misses), `fuzz-lex` and `fuzz-parse` (clang libFuzzer harnesses over lexer and parser, local-only). CI gains a gating `valgrind` job and an advisory `coverage` job; the advisory-to-gating promotion for coverage follows the cppcheck/analyzer pattern once the noise floor is known. Bench + profile harness deferred to M2-era paired work — see an internal backlog entry.

### Added

- Bytecode emitter walks AST nodes into a `UModule`: register-based instruction stream (byte-aligned 8/8/8/8 encoding), single tagged constant pool with linear-scan dedup, Lua-5.5-style delta-encoded synclines with absolute-line checkpoints, stack-discipline register allocator with destination-reuse. 8-opcode M1 set (`LOADK`, `MOVE`, `ADD`, `SUB`, `MUL`, `DIV`, `NEG`, `RET`). Reserved opcode slots 8–255 for M2+ additions (locals, control flow, calls, reactive primitives).
- `.urb` on-disk format: 24-byte header (magic `"URBI"` + 16·major+minor version + 6-byte FTP/paste-corruption canary + 8-byte flavor descriptor) followed by varint-delimited sections (metadata, constants, 4-byte-aligned instruction stream, delta synclines). Per-target flavor pinned at compile time (`URBI_INT_WIDTH` / `URBI_FLOAT_TYPE` / `URBI_INSTR_WIDTH` / `URBI_ENDIANNESS`); loader refuses mismatches with field-specific diagnostics.
- Loader verifier sweep after byte-level decode: opcode range, register range, `LOADK` Bx bounds, terminal `OP_RET`, abs-line pc monotonicity, 4-byte instruction alignment. `OP_RET` B operand and `OP_MOVE`/`OP_NEG` C operand intentionally not enforced (unused bytes, no runtime effect).
- UEmitter and module APIs in new headers `uemit.h` / `umodule.h`: `UEmitter` accumulator (init / statement / finish), `UModule` struct, `umodule_deserialize`, `umodule_serialize`, `uemit_disassemble`, error-name tables. Compiler-internal — `urbi.h` unchanged.
- Streaming Pratt parser consumes the lexer's token stream and produces one `UAstNode` per statement (integer literal, identifier, unary, binary, error). Recursive-descent statements + precedence climber for `+ - * /` with parens, unary `+ -` (plus is parse-time no-op), panic-mode recovery via `|`, in-stream `AST_ERROR` nodes, OOM sentinel path. Public parser API in `uparse.h`: `UParser`, `uparse_init`, `uparse_next_statement`, `uparse_error_name`.
- Internal chunk-list bump-allocator arena (`uarena.h` / `uarena.c`) backing the AST and emit arenas. Three init variants — `uarena_init` (stdlib), `uarena_init_ex` (pluggable allocator for embedded), `uarena_init_static` (fixed caller buffer for freestanding) — plus `uarena_alloc`, `uarena_reset`, `uarena_destroy`. No copy between chunks; pointers stable across growth.
- Lexer scans integer literals (decimal, hex, binary, octal with underscores), identifiers, single-character operators (`+ - * /`), parentheses, and the statement separator `|`. Full synclines on every token.
- Structured lexer error codes: unknown character, unterminated block comment, ambiguous leading zero, empty radix, malformed hex/binary/octal, leading/trailing/adjacent underscores, integer overflow.
- Public lexer API in new header `ulex.h`: `UToken`, `ULexer`, `ulex_init`, `ulex_next`, `ulex_token_name`. No allocation; caller owns source buffer.

### Refactoring

- Added the `U` prefix to every public struct and enum in the source tree so
  host embedders can include any header without type-name collisions. `Lexer`
  → `ULexer`, `Token` → `UToken`, `TokenType` → `UTokenType`,
  `LexErrorCode` → `ULexError`, `Parser` → `UParser`,
  `ParseErrorCode` → `UParseError`, `AstNode` → `UAstNode`,
  `AstKind` → `UAstKind`, `UnaryOp` → `UAstUnaryOp`,
  `BinaryOp` → `UAstBinaryOp`, `Arena` → `UArena`,
  `ArenaChunk` → `UArenaChunk`, `Emitter` → `UEmitter`,
  `EmitError` → `UEmitError`, `AbsLine` → `UAbsLine`. Error-type suffix
  normalized: `LexErrorCode` and `ParseErrorCode` drop the redundant `Code`
  suffix to match the existing `UVMError`/`UEmitError` pattern. Enum tag
  values (`TOK_*`, `AST_*`, `LEX_*`, `PARSE_*`, `EMIT_*`, `OP_*`, `UOP_*`,
  `BOP_*`) are unchanged — they are namespaced by prefix already, and
  renaming them risks cross-family collisions (e.g. `UOP_*` already denotes
  unary-op values, so opcode values can't take the same prefix). `src/uvarint.h`'s
  include guard normalized from `URBI_UVARINT_H` to `UVARINT_H` to match
  every other header.
- Bytecode `Chunk` renamed to `UModule` across the codebase, and the
  `src/uchunk.{c,h}` + `tests/unit/test_chunk.c` module renamed to
  `src/umodule.{c,h}` + `tests/unit/test_module.c`. The type is the
  compilation-unit record — instructions + constants + synclines +
  metadata — so the new name reflects what it actually is. `UChunkLoadError`
  → `UModuleLoadError`, `UChunkAllocFn` → `UModuleAllocFn`, `uchunk_*`
  → `umodule_*`. The `ULOAD_*` error tags and the arena's internal
  chunk-list terminology are unchanged.
- `UConst` renamed to `UValue` across sources, tests, and internals docs. The
  type has always been the universal tagged-value cell — constants-pool entry,
  register-frame slot, arithmetic operand, `uvm_run` result — so the new name
  reflects what it actually is. The `UValKind` enum and `UVAL_*` tags are
  unchanged (they already wore the `val` prefix); `uconst_to_double` /
  `uconst_set_float` become `uvalue_to_double` / `uvalue_set_float`.
- LEB128 varint encode/decode extracted into a standalone freestanding module
  `uvarint.{c,h}` with its own error enum (`UVarintError`). `umodule.c` now
  consumes it via two translation wrappers that map `UVarintError` into
  `UModuleLoadError` at the boundary; `uemit.c` drops the four private `static`
  varint helpers and consumes the module directly. The test-only header
  `src/umodule_internal.h` is retired; varint coverage moves into a new
  `test_varint_suite` (11 cases) that exercises encode and decode directly,
  replacing the indirect serialize→deserialize-only encode coverage of prior
  state.

### Foundation

- Header-only test harness `utest.h` (zero dependencies, pure C99)
- Make targets: `test`, `test-asan`, `test-ubsan`, `test-debug`, `cross-arm`, `cross-riscv`
- GitHub Actions CI covering host (debug/release/ASan/UBSan) plus ARM Cortex-M7 and RISC-V rv32imc cross-compiles
- Initial placeholder API: `urbi_version()`

### Build system

- Per-TARGET build directories: all variants (release, debug, sanitizers, cross-compiles) land in `build/<TARGET>/` and coexist without requiring `make clean` between them
- `make all` as the default target
- `make compile_commands.json` — generates a clangd-compatible compilation database for LSP-based editors

### Developer environment

- `.editorconfig` — universal indent, newline, and charset rules
- Extended `.gitignore` covering editor state (JetBrains, VS Code, Vim, Emacs, Sublime, TextMate), tag databases (ctags, cscope, GNU Global), and IDE indexing artifacts (`compile_commands.json`, `.cache/`)
- `CONTRIBUTING.md` documents test modes, cross-compile, indexing database, and TARGET convention
