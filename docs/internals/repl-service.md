# REPL service

This document covers the v0.9.1 networked REPL service: thread model,
NDJSON wire shape, session lifecycle, MPSC eval queue + SPSC output
ringbuf, the pluggable `UTransport` adapter, and the `Debug` urbiscript
namespace. The subsystem is opt-in via `URBI_ENABLE_REPL=1`; default
builds omit `src/repl/` entirely.

Read [`realm-and-chunks.md`](./realm-and-chunks.md) first — the REPL
service builds on `urbi_realm_create_repl` and the per-realm writer
plumbing introduced in v0.9.0 / Phase 1 of v0.9.1.

Source: `include/urbi/repl.h`, `src/repl/urepl_*.{c,h}`, `src/repl/ujson.{c,h}`,
`src/stdlib/lobby.u`, `tools/urbi-server.c`, `tools/urbi-send.c`.

## Thread model (Linux)

```text
                        ┌─────────────────────────────┐
                        │ pthread L: listener         │
                        │ accept() on each registered │
                        │ transport (TCP / Unix / ...) │
                        └──────────────┬──────────────┘
                                       │ spawns
       ┌───────────────────────────────┼─────────────────────────────┐
       ▼                               ▼                             ▼
┌────────────┐                  ┌────────────┐                ┌────────────┐
│ pthread R1 │                  │ pthread R2 │      ...       │ pthread Rk │
│ reader for │                  │ reader for │                │ reader for │
│ client A   │                  │ client B   │                │ client K   │
└──────┬─────┘                  └──────┬─────┘                └──────┬─────┘
       │ NDJSON parse                  │                             │
       │ + queue.push(UReplJob)        │                             │
       └────────────────────┬──────────┴─────────────────────────────┘
                            ▼
                    ┌───────────────┐
                    │  MPSC queue   │  one queue per UReplServer
                    │  (mutex+cond) │  intrusive singly-linked list
                    └───────┬───────┘
                            │ drain at urbi_step boundaries
                            ▼
                    ┌───────────────┐
                    │ pthread 0     │
                    │ VM thread     │  single-threaded; the thread
                    │               │  that called urbi_repl_serve
                    │ realm writer  │  writes output bytes onto
                    │ pushes bytes  │  session->output_ringbuf
                    └───────┬───────┘
                            │ SPSC ringbuf (lock-free)
       ┌────────────────────┼─────────────────────────────┐
       ▼                    ▼                             ▼
   ringbuf A            ringbuf B                     ringbuf K
       │                    │                             │
       │ each reader pthread polls its own ringbuf        │
       │ + socket fd in one poll() syscall                │
       ▼                    ▼                             ▼
   client A             client B                      client K
```

The VM thread is whichever thread called `urbi_repl_serve`. It remains the
single VM-owning thread for the entire server lifetime. Listener and
reader subthreads do **no VM work** — they parse NDJSON, allocate
`UReplJob` envelopes, push onto the MPSC queue, and pump the SPSC ringbuf
out to sockets. All bytecode execution, GC, slot lookups, and watcher
firing run exclusively on the VM thread.

The reader subthread does **not** consume responses by id-correlation.
Each session's output flows back via that session's own SPSC ringbuf,
which the same reader subthread also drains. This sidesteps cross-thread
response routing — the NDJSON parser and the output framer share state
machine + buffer state owned exclusively by one thread.

## Thread model (embedded / step-driven)

Hosts without `pthread_create` use the manual driver:

```c
urbi_repl_serve_init(vm, &cfg, &server);
urbi_repl_register_transport(server, &UREPL_PICO_UART_TRANSPORT, state);

while (running) {
    urbi_repl_serve_step(server, 1000 /*us*/);
    urbi_step(vm, 1024, NULL);
}

urbi_repl_serve_shutdown(server);
```

`serve_step` performs at most one accept + read + dispatch + write cycle
across all registered transports, returning when no progress is possible
or `timeout_us` elapses. On RTOS targets, `xTaskCreate` (FreeRTOS) /
`xTaskCreatePinnedToCore` (ESP-IDF) replace `pthread_create` and the
threaded model still applies — the MPSC queue is implemented over the
RTOS primitive (`QueueHandle_t`) instead of pthread mutex + cond.

## Cooperative drive via `urbi_repl_serve_step`

The threaded model above assumes a kernel-pollable listener fd per
transport. Pi Pico USB CDC, UART (Pico SDK / ESP-IDF / FreeRTOS), and
the in-process buffer test transport all have `pollable_fd_fn == NULL`
or `pollable_fd_fn(fd) < 0` — they do not surface a kernel fd that
`poll()` understands. v0.9.4 added a fully functional cooperative data
plane so these transports work without any pthread at all.

`urepl_listener_start` short-circuits when no registered transport has
a pollable listener fd (`src/repl/urepl_listener.c:634-648`): the
listener pthread is never created, no `eventfd` is allocated, and
`urbi_repl_serve_step` becomes the sole data-plane driver. The
embedder calls it from its own loop.

`urbi_repl_serve_step` runs four non-blocking sweeps in order
(`src/repl/urepl.c:235`):

1. `urepl_accept_sweep_nonpollable` — one `accept_fn` attempt per
   non-pollable transport; queued onto the existing accept queue so
   the dispatch drain creates the session on the VM thread.
2. `urepl_read_sweep_nonpollable` — one `read_fn` per session; bytes
   feed the NDJSON line parser, and complete lines push `UReplJob`
   onto the MPSC queue. `urepl_dispatch_drain` runs inline so the
   embedder doesn't have to also drive `urbi_step` just to get
   dispatch.
3. `urepl_write_sweep_nonpollable` — one `write_fn` per session
   draining `session->output_ringbuf`; partial writes stage in a
   per-session `coop_outbuf` and retry on the next sweep.
4. `urepl_disconnect_sweep` — reaps sessions whose `needs_teardown`
   flag was set by the read or write sweep (clean EOF, hard transport
   error); fires the v0.9.1 disconnect-cleanup sequence.

Mixed-mode is supported: a single `UReplServer` can have TCP (driven
by the listener pthread) AND USB CDC (driven by `serve_step`)
registered, and both work in the same process.

`spawn_reader` (`src/repl/urepl_listener.c:342`) skips
`pthread_create` for non-pollable transports — it sets
`reader->cooperative = true` instead. `urepl_listener_stop_and_join`
skips `pthread_join`, `shutdown(client_fd)`, and the `wake_eventfd`
signal for cooperative readers.

Source of truth: commits `4aa2bfa..fd1619f` (accept / read / write /
close sweeps + cooperative documentation + pthread-skip).

## MPSC eval queue

Source: `src/repl/urepl_queue.{c,h}`.

```c
typedef struct UReplJob {
    uint64_t        id;          /* client-assigned correlation id */
    uint32_t        session_id;  /* internal session handle */
    UReplOp         op;          /* eval / introspect / cancel / lobby_new / lobby_close / auth */
    char           *payload;     /* op-specific; owned by job */
    size_t          payload_len;
    struct UReplJob *next;
} UReplJob;
```

The queue is an intrusive singly-linked list protected by a mutex with a
condition variable for the VM thread's drain wait. Reader subthreads
allocate `UReplJob` + payload, hold them across the push, and never touch
them again. The VM thread frees them after dispatch.

The VM thread drains at every `urbi_step` call boundary: pop all pending
jobs under one lock, then dispatch outside the lock. Job dispatch is
synchronous — each job runs to a yield point (or completion for short
ops) before the next is picked up. Long-running jobs (`every(1s) ...`)
leave persistent strands that continue to run between drains.

## SPSC output ringbuf

Source: `src/repl/urepl_queue.{c,h}` (per-session field).

One ringbuf per session: writer is the VM thread, reader is that
session's reader pthread. Default capacity is 64 KiB per session
(tunable via `UReplConfig.output_ringbuf_cap`); overflow drops oldest
bytes and emits `{kind:"error", code:"output_overflow"}` the next time
the reader pulls. 64 KiB is enough for ~1000 typical `echo` lines.

The reader subthread polls its ringbuf in the same `poll()` syscall as
its socket read fd. The ringbuf writer signals readiness via `write` on
an eventfd (Linux) or equivalent on other platforms.

## Session lifecycle

```text
TCP accept()                  → urepl_session_create on the VM thread
                                (queued from listener pthread; created
                                 next urbi_step drain)
new session                   → URealm = urbi_realm_create_repl(vm)
                                writer = per-session output ringbuf
                                compile_budget = URBI_DEFAULT_REPL_BUDGET
session ↔ realm               → 1:1 strong binding for session lifetime
Lobby.lobbies.push(realm)     → C-side list mutation; visible to urbiscript
client sends {op:lobby_new}   → creates a sub-realm under the same session
                                (rare; mostly for editor tools)
EOF / close / urbi_repl_stop  → run handleDisconnect hook in lobby's realm
                                → tag.stop() the realm's root tag
                                → urbi_realm_destroy
                                → Lobby.lobbies.remove(realm)
                                → free UReplSession + ringbuf
```

The default behavior is **one lobby per connection**. Multi-lobby-per-
connection is supported via `{op:"lobby_new"}` returning a new lobby_id
the client can target subsequent `eval` ops to. The legacy idiom (one
TCP connection = one Lobby) is the default.

Session creation runs on the VM thread, not the listener pthread, to
avoid making URealm allocation thread-safe. The listener queues a
`session_create` job; the VM thread's drain creates the realm at the
next step boundary.

## Transport adapter pattern

Source: `include/urbi/repl.h`, `src/repl/urepl_transport_*.c`.

```c
typedef struct UTransport {
    const char *name;
    int  (*accept_fn)      (void *listener_state, int *out_client_fd);
    int  (*read_fn)        (int client_fd, void *buf, size_t n);
    int  (*write_fn)       (int client_fd, const void *buf, size_t n);
    void (*close_fn)       (int client_fd);
    int  (*pollable_fd_fn) (int client_fd);
} UTransport;
```

Each transport is a vtable + opaque listener state. Concrete transports:

| Source | Transport | Notes |
|---|---|---|
| `urepl_transport_tcp.c` | `UREPL_TCP_TRANSPORT` | POSIX sockets, IPv4, non-blocking accept |
| `urepl_buffer_transport.c` | `UREPL_BUFFER_TRANSPORT` | In-process loopback; used by unit tests |
| `urepl_transport_pty.c` | `UREPL_PTY_TRANSPORT` | Linux openpty; used by the CI UART harness |
| `urepl_transport_uart_freertos.c` | `UREPL_FREERTOS_UART_TRANSPORT` | `#if FreeRTOS` |
| `urepl_transport_uart_esp_idf.c` | `UREPL_ESP_IDF_UART_TRANSPORT` | `#if ESP_PLATFORM` |
| `urepl_transport_uart_pico.c` | `UREPL_PICO_UART_TRANSPORT` | `#if PICO_BOARD` |
| `urepl_transport_uart_linux.c` | `UREPL_LINUX_UART_TRANSPORT` | `#if __linux__` POSIX termios |

UART transports are single-client: `accept_fn` returns success once on
first call (the pre-opened UART fd) then returns "would block" forever
after. No auth is required on UART by default (physical access is
treated as auth).

`accept_fn` returning non-zero is interpreted as "no client waiting"
(treat as `EAGAIN`). `read_fn` / `write_fn` return positive transferred
bytes, 0 on EOF (client disconnect), or negative on error. All transports
are non-blocking-safe — the listener and reader pthreads poll-driven.

## NDJSON wire shape

Source: `src/repl/urepl_ndjson.{c,h}`.

One JSON document per line, terminated by `\n`. Server accepts both `\n`
and `\r\n` from client; server always emits `\n`. Maximum line length is
1 MiB (matches `max_source_bytes`); longer lines trigger `{kind:"error",
code:"frame_too_large"}` and the connection closes.

### Client → server ops

```jsonc
{"id":1, "op":"auth",        "token":"hunter2"}
{"id":2, "op":"eval",        "lobby":"a3f2", "code":"1 + 2"}
{"id":3, "op":"introspect",  "what":"coros"}
{"id":4, "op":"introspect",  "what":"stack",  "coro_id":42}
{"id":5, "op":"introspect",  "what":"slots",  "lobby":"a3f2", "obj":"Robot.left_arm"}
{"id":6, "op":"cancel",      "lobby":"a3f2", "tag":"experiment_42"}
{"id":7, "op":"lobby_new"}
{"id":8, "op":"lobby_close", "lobby":"a3f2"}
```

`id` is a client-assigned uint64 used for response correlation. The
client guarantees uniqueness within its connection. Server echoes the
`id` on every correlated response.

`lobby` defaults to the connection's implicit lobby (the first lobby
auto-created on accept).

### Server → client kinds

```jsonc
/* On successful connection, before any client op: */
{"kind":"hello", "version":"v0.9.1", "lobby":"a3f2", "synclines":true,
 "auth_required":false}

/* After client sends 'auth': */
{"id":1, "kind":"auth_ok"}
{"id":1, "kind":"error", "code":"auth_failed"}

/* eval response: */
{"id":2, "kind":"result", "value":3, "ts":1234567}
{"id":2, "kind":"done"}

/* eval with streaming output: */
{"id":9, "kind":"output", "channel":"clog", "msg":"sensor: 0.42", "ts":1234600}
{"id":9, "kind":"output", "channel":"clog", "msg":"sensor: 0.43", "ts":1235600}
{"id":9, "kind":"done"}

/* persistent watcher output (eval already completed): */
{"kind":"output", "lobby":"a3f2", "channel":"clog", "msg":"every-tick", "ts":1236600}

/* introspect: */
{"id":3, "kind":"result", "value":{"coros":[...]}}

/* event broadcast: */
{"kind":"event", "lobby":"a3f2", "name":"battery_low", "payload":{"level":18}, "ts":1234700}

/* error: */
{"id":2, "kind":"error", "code":"parse",
 "msg":"unexpected token at line 3, column 12",
 "loc":{"file":"<stdin>","line":3,"col":12}}

/* server-initiated shutdown: */
{"kind":"goodbye", "reason":"server_stopping"}
```

### Id correlation rules

- `result` / `done` / `error` **from the operation itself** carry the
  originating client op's `id`.
- `output` from **inside an eval's bytecode frame** (echo / Stream.write
  in the running code) carries the eval's `id`.
- `output` from **a strand spawned by an eval but outliving it** (e.g.,
  a watcher body installed by `at(...) { echo ... }`) carries `lobby` +
  `channel` but **no `id`** — the originating eval has already produced
  `done`.
- `event` kind never carries `id`; uses `lobby` + `name`.

Each strand carries `originating_eval_id` (0 if none / expired). On
`done`, the dispatcher zeros `originating_eval_id` on every strand that
was spawned by the just-finished eval. Subsequent output from those
strands flows as lobby-scoped.

### Synclines

The server wraps multi-line eval text with synclines reflecting the
client-provided file/line metadata. Client sends:

```jsonc
{"id":2, "op":"eval", "lobby":"a3f2",
 "file":"/home/dev/behaviors/explore.u", "line":1,
 "code":"function explore() { ... }\nloop { explore(); }"}
```

The server hands to `urbi_repl_eval`:

```text
//#push 1 "/home/dev/behaviors/explore.u"
function explore() { ... }
loop { explore(); }
//#pop
```

Runtime errors then report `/home/dev/behaviors/explore.u:2:N` rather
than `<stdin>:42:N`. The syncline mini-parser was introduced in v0.9.0
(`src/lex/ulex.c`).

## Auth flow

Source: `src/repl/urepl_auth.{c,h}`.

`urbi_repl_serve` refuses to start with `URBI_ERR_INSECURE_CONFIG` when
`bind_addr` is non-loopback and `auth_token` is NULL. Loopback addresses
are `127.0.0.1`, `::1`, NULL, and any `bind_addr` starting with `/`
(Unix socket).

On the wire:

```text
SERVER → CLIENT: {"kind":"hello", "version":"v0.9.1", "lobby":"a3f2",
                  "synclines":true, "auth_required":true}
CLIENT → SERVER: {"id":1, "op":"auth", "token":"hunter2"}
SERVER → CLIENT (success): {"id":1, "kind":"auth_ok"}
SERVER → CLIENT (failure): {"id":1, "kind":"error", "code":"auth_failed"}
                            (server closes connection after 1 s)
```

Token comparison uses `urepl_auth_constant_time_compare` (byte-XOR
accumulator across the full token length, regardless of mismatch
position) to avoid timing side-channel leaks.

Per-source rate limiter: 5 failed `auth` attempts within 30 s from the
same peer locks that peer out for 60 s. Implemented as an 8-entry LRU
table in the listener pthread. Local Unix-socket peers are tracked by
pid instead of IP.

mTLS / client-cert pinning is not in v0.9.1 — for untrusted networks,
operators SSH-tunnel the loopback TCP port.

## Dispatcher

Source: `src/repl/urepl_dispatch.{c,h}`.

The dispatcher's drain hook is registered at `urbi_repl_serve_init` time
and runs on the VM thread at every `urbi_step` boundary. The hook pops
the entire MPSC queue under one lock, then dispatches each job:

| Op | Handler |
|---|---|
| `auth` | `dispatch_auth` — constant-time compare + state transition |
| `eval` | `dispatch_eval` — `urbi_repl_eval` under the session's lobby realm |
| `cancel` | `dispatch_cancel` — `urbi_tag_stop` on a named tag |
| `introspect` | `dispatch_introspect` — switches on `what` to one of 9 primitives |
| `lobby_new` | creates a fresh URealm linked to the same session |
| `lobby_close` | destroys a sub-realm; primary lobby cannot be closed via this op |

Output produced by the dispatched op flows through the session's
per-realm writer back to the session's output ringbuf.

## Introspection primitives

Source: `src/repl/urepl_introspect.{c,h}`.

Nine read-only primitives, each walking VM-internal linked lists on the
MAIN thread and emitting one JSON object into a caller-provided buffer:

```c
int urbi_introspect_coros    (UVM *vm,                          char *buf, size_t cap, size_t *out_n);
int urbi_introspect_tags     (UVM *vm,                          char *buf, size_t cap, size_t *out_n);
int urbi_introspect_watchers (UVM *vm,                          char *buf, size_t cap, size_t *out_n);
int urbi_introspect_events   (UVM *vm,                          char *buf, size_t cap, size_t *out_n);
int urbi_introspect_stack    (UVM *vm, uint32_t coro_id,        char *buf, size_t cap, size_t *out_n);
int urbi_introspect_slots    (UVM *vm, URealm *realm, const char *obj_path,
                                                                char *buf, size_t cap, size_t *out_n);
int urbi_introspect_profile  (UVM *vm,                          char *buf, size_t cap, size_t *out_n);
int urbi_introspect_gc       (UVM *vm,                          char *buf, size_t cap, size_t *out_n);
int urbi_introspect_lobbies  (UVM *vm,                          char *buf, size_t cap, size_t *out_n);
```

Wire JSON shape is locked at v0.9.1 and frozen forward to v1.0 — only
additive evolution permitted.

Carry-forward notes:

- `urbi_introspect_profile` emits empty arrays + `note:"profiling
  deferred to v1.x"`. No profiling infrastructure exists in v0.9.1; the
  wire shape is locked for forward compatibility.
- `urbi_introspect_tags` walks `vm->realms_head` and emits one entry per
  realm root tag. Host-created child tags (via `urbi_tag_create`) are
  not centrally enumerated at v0.9.1; a central tag registry is in the
  v1.x design-risks register.
- `coro_id` is `(uint32_t)(uintptr_t)strand & 0xFFFFFFFF`. `UStrand` has
  no explicit id field at v0.9.1; identity is stable per VM run (no
  strand recycling within one run).

## `Debug` urbiscript namespace

Source: `src/repl/urepl_introspect.c`, `src/stdlib/lobby.u`.

Each of the 9 introspect primitives is exposed as a method on a
singleton `Debug` proto bound on `Global` at stdlib bootstrap:

```urbi
Debug.coros()          // string of JSON: {"coros":[...]}
Debug.tags()
Debug.watchers()
Debug.events()
Debug.stack(coro_id)
Debug.slots(obj_path)
Debug.profile()
Debug.gc()
Debug.lobbies()
```

Each method calls the matching introspect primitive, runs the returned
bytes through `ujson_parse` to validate, then returns the bytes as an
urbi `String`. `Debug.*()` returns a `String` rather than a structured
`Dict` / `List` in v0.9.1 because there is no `UVAL_LIST` / `UVAL_DICT` —
lists / dicts are UObjects in the `URBI_ATOM_LIST` / `URBI_ATOM_DICT`
families, and building one from C requires walking `containers.c`
internals. The wire JSON shape is locked, so any v1.x upgrade to
first-class structured returns is transparent to clients that already
parse the string.

`vm->debug_proto` is a single per-VM singleton; GC reachability is via
`object_roots_walker`.

## Tiny JSON parser

Source: `src/repl/ujson.{c,h}`.

Recursive-descent JSON parser shipped to back the `Debug` namespace's
parse-back step and the dispatcher's `lobby` / `obj` field extraction.
Supports object, array, string (with `\uXXXX` and surrogate pairs), int
/ double, bool, null. DoS-bounded: `UJSON_MAX_DEPTH=32`,
`UJSON_MAX_NODES=10000`, `UJSON_MAX_LEN=1 MiB`. Returns a `UJsonNode`
tree freed via `ujson_free_node`.

No external dependencies; self-contained ~600 LOC. Not part of the
public API.

## Lobby stdlib overlay

Source: `src/stdlib/lobby.u` (baked into `stdlib_boot.o` via the M6 bake
tool).

```urbi
// Lobby.echo: formatted message via the current realm's writer
function Lobby.echo(msg, tag = "", prefix = "***") {
  __builtin_lobby_send(asString(msg), tag, prefix)
};

// Lobby.wall: broadcast to every other lobby
function Lobby.wall(msg, tag = "") {
  for| (var l in Lobby.lobbies - [this])
    l.echo(msg, tag);
};

// Lobby.handleDisconnect: called by the C dispatcher on disconnect
function Lobby.handleDisconnect() {
  onDisconnect!(this)
};

var Lobby.onDisconnect = Event.new();
```

`Lobby.lobbies` is mutated from C at session create / destroy:
`urbi_lobby_lobbies_push(vm, lobby_value)` / `_remove`. The list is
visible to urbiscript but the slot itself is C-managed; urbiscript-side
assignment to `Lobby.lobbies` is rejected by the readonly bit on the
`Lobby` proto.

`__builtin_lobby_send` is one new C-native registered on `Object`. It
formats `"[TTTTTTTT:tag] PREFIX msg\n"` (or no `:tag` segment if tag is
empty) and writes through `strand->realm->writer` → `vm->writer` →
default-writer fallback chain.

## Threading abstraction (`src/repl/urepl_threading.h`)

All `src/repl/*.c` files that need synchronization use the
`urbi_mutex_t` / `urbi_cond_t` / `urbi_thread_t` typedefs +
`UREPL_*` macros defined in `urepl_threading.h`. The header
selects between two expansions:

- **Default (POSIX):** typedefs resolve to `pthread_mutex_t` /
  `pthread_cond_t` / `pthread_t`; macros expand to the matching
  `pthread_*` calls.
- **`URBI_REPL_COOPERATIVE_ONLY=1`:** typedefs become 1-byte empty
  stubs; macros become no-ops (`UREPL_COND_WAIT` returns immediately,
  callers must already poll the empty-state predicate;
  `UREPL_THREAD_CREATE` returns `-1` as a defensive measure since
  live thread-create callers live in `urepl_listener.c`'s POSIX-guarded
  sections which are skipped in cooperative mode).

The cooperative mode is for freestanding targets without pthread
(Pi Pico via RP2040, bare-metal STM32F4, FPU-less ARM). Listener +
socket transport TUs (`urepl_transport_tcp.c`,
`urepl_transport_unix.c`, `urepl_transport_pty.c`, `urepl_auth.c`)
get filtered out of `REPL_SRCS` so they don't pull in
`<sys/eventfd.h>` / `<sys/socket.h>` / `<sys/un.h>`.

`urepl_listener.c` is the exception — it stays in the build because
it also hosts the cooperative `urepl_accept_sweep_nonpollable`,
`urepl_read_sweep_nonpollable`, `urepl_write_sweep_nonpollable`,
and `urepl_disconnect_sweep` functions used by `urbi_repl_serve_step`.
Its POSIX-only sections (eventfd helpers, listener_main thread,
reader_main thread, full spawn_reader, real
drain_accepts / wake_all_readers) are guarded with
`#ifndef URBI_REPL_COOPERATIVE_ONLY`; cooperative stubs preserve
the dispatcher's call surface.

`urepl_dispatch.c`'s `dispatch_auth` auto-approves on cooperative
builds because the auth TU (`urepl_auth.c` with the rate-limiter
state machine) is not compiled in. Cooperative transports (USB CDC,
UART) have no network threat model; the v1.x `cooperative_auth_token`
opt-in is filed in design-risks for future cooperative network
embedders.

The contract for embedders: `URBI_REPL_COOPERATIVE_ONLY` must match
the library build. See `docs/embedding-guide.md` §12 for the
trap-class warning.

## Carry-forward to v1.x

Two known issues filed in `docs/urbi-embedded-design-risks.md`:

- **Closure-body bare-name resolution doesn't walk `this`.** Unqualified
  identifiers inside a closure body resolve through the realm-global
  fallback (`OP_GETSLOT` on the realm's `global_object`) in v0.9.1 —
  they do NOT walk the closure's `this` proto chain. This makes
  `Lobby.echo` / `Lobby.wall` non-functional from urbiscript because
  the body references `__builtin_lobby_send` unqualified. Workaround
  in the v0.9.1 `.chk` corpus + multi-client integration test: call
  `Lobby.__builtin_lobby_send(...)` directly. Disposition: v1.x emit
  follow-up (implicit-this fallback in `emit_ident_arm`).
- **Listener-teardown race under multi-client stress.** The 4-client
  multi-client integration test (`tests/unit/test_repl_multi_client.c`)
  intermittently segfaults (~50% rate) during harness teardown — race
  between `urepl_session_destroy` running on the listener pthread (on
  EOF) and `urbi_repl_stop` running on the test thread. Mitigation in
  v0.9.1: the suite is opt-in via `URBI_TEST_MULTI_CLIENT=1`; default
  `make test URBI_ENABLE_REPL=1` stays green. The scenarios' semantic
  coverage is covered by green unit tests. Disposition: v0.9.x or
  v1.0-rc — investigate under `--tool=helgrind`.

## See also

- `docs/embedding-guide.md` §12 — public-facing how-to for embedders.
- `docs/internals/realm-and-chunks.md` — URealm, the per-realm writer,
  and `urbi_realm_create_repl` (v0.9.0 foundation).
- `<urbi/repl.h>` — public API surface.
- `<urbi/repl.h>` — transport pluggability and default-secure posture rationale in header comments.
