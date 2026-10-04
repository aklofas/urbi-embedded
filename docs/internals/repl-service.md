# REPL service

The REPL service is a cooperative NDJSON eval service: one JSON request
per line in, one or more JSON lines out, over a byte stream the host
supplies. It never starts a thread and never opens a socket. The host
calls `urbi_repl_serve_step` from the same loop that calls `urbi_step`,
so every VM touch stays on one thread and the service runs on a
microcontroller with no scheduler of its own.

The public surface is [`include/urbi/repl.h`](../../include/urbi/repl.h).
Read [`runtime.md`](./runtime.md) first for realms and the per-realm
writer the service is built on.

## The four files

| File | Role |
| --- | --- |
| `src/repl/urepl.c` | The public entry points and the per-session sweep. |
| `src/repl/urepl_dispatch.c` | Session create and destroy, the output buffer, the op handlers. |
| `src/repl/urepl_ndjson.c` | The request scanner and the four envelope emitters. |
| `src/repl/urepl_buffer_transport.c` | An in-process transport made of two byte queues. |

Each has a header of the same name. `src/repl/urepl.h` holds the server
and session structs. The service is hosted-only and is compiled into
every build except `URBI_BYTECODE_ONLY=1`, which has no compiler to eval
with.

## The sweep

`urbi_repl_serve_step` makes one non-blocking pass over every session.
Each session goes through four phases in order:

1. **Read.** One `read` call on the transport, up to 4 KiB, appended to
   the session's line accumulator.
2. **Run.** Every complete line in the accumulator is parsed and
   dispatched before the next one is looked at. A trailing CR is
   tolerated. A partial line waits for the next sweep.
3. **Write.** The output buffer is offered to `write` until it drains or
   the transport takes nothing more.
4. **Close.** A session whose stream has ended is destroyed once its
   output has drained.

The ordering guarantee follows from this shape. Every response to
request N is in the stream before any response to request N+1, and an
eval's own output precedes its result. `timeout_us` is accepted and
ignored: the sweep never waits, so pacing an idle loop is the caller's
business.

## Transports

`UTransport` is four members: `ctx`, `read`, `write` and `close`. A
`read` returning zero means "nothing now, still open"; a negative return
from either `read` or `write` ends the session. `close` is called
exactly once per registered transport.

Transports live outside the library. The service never accepts, resolves
or dials; it is handed a connected stream by `urbi_repl_register_transport`
and copies the vtable. The POSIX transports belong to the
`tools/urbi-server.c` tool, and a microcontroller's UART or USB transport
belongs to that port's example.

## The session

Each registered transport is one session, and each session owns one
realm. The realm's globals object is the session's own; the built-ins
below it are shared. The realm's writer frames everything the session's
code echoes as an `output` envelope for that session's client only. The
server config's `default_budget` is applied to every session's realm as
its compile limits.

**The output buffer is lazy.** It is not allocated until the first
write. It then grows by doubling from 256 bytes, never past the server
config's `output_buf_cap` (64 KiB when that is zero). A session that
never speaks holds no buffer at all.

**A write that does not fit is dropped.** The session latches a
`dropped` flag instead of truncating or blocking the VM on a client that
has stopped reading. Once the buffer has drained, the next write phase
sends one `output_dropped` error envelope with id zero, so the client
knows a gap happened.

**A request line is capped** at `UREPL_MAX_LINE` (1 MiB). An oversized
line is discarded up to its terminating newline, the client gets a
`line_too_long` error, and any request packed behind it is still run.

When a session closes, the service runs `Realm.handleDisconnect()` in its
realm, detaches the writer, frees the realm and calls the transport's
`close`. The default `handleDisconnect` emits `Lobby.onDisconnect`; a
session may replace it.

## The wire

Two ops exist. Any other op, or a line that does not parse, is answered
with a `parse` error.

```json
{"id":1,"op":"eval","code":"1+2"}
{"id":2,"op":"introspect","what":"coros"}
```

Four envelope kinds come back, as `urepl_ndjson.c` emits them:

```json
{"id":1,"kind":"output","channel":"clog","msg":"..."}
{"id":1,"kind":"result","value":"3"}
{"id":1,"kind":"error","code":"runtime","message":"..."}
{"id":1,"kind":"done"}
```

- **`output`** carries the id of the eval running when it was written.
  Output written after that eval's `done`, by a watcher or a timer it
  armed, carries no `id` key at all.
- **`result`** for an eval holds the value rendered the way the REPL
  prints it, as a JSON string. For an introspect it holds the answer
  inline, because that answer is JSON already.
- **`error`** omits `id` when there is no request to correlate to, and
  omits `message` when it is empty. The codes are `parse`,
  `budget_depth`, `budget_nodes`, `budget_source`, `runtime`, `oom`,
  `error`, `response_too_large`, `line_too_long`, `output_dropped`,
  `unknown_introspect` and `introspect_failed`.
- **`done`** ends every eval, success or failure. An introspect gets no
  `done`.

`introspect` answers one question, `coros`: a `{"coros":[...]}` object
with one `{"id","state","realm"}` entry per strand.

## The `Debug` namespace

`Debug.coros()` returns, as a String, the same JSON the `coros`
introspect op answers. It lives in `src/stdlib/debug_namespace.c`, so the
dependency runs from the REPL service to the standard library and not
the other way. A freestanding build has no formatter, and `Debug.coros()`
raises there.

## The in-process buffer transport

`urepl_buffer_transport.h` is a loopback made of two byte queues: what
the client writes is what the service reads, and what the service writes
is what the client reads back. `urepl_buffer_client_finish` declares end
of stream, which makes the disconnect path reachable. It is also the
reference implementation of the `UTransport` contract.

`build/host/repl-chk-driver` runs the NDJSON fixtures under
`tests/chk/repl/` through it, one buffer transport per session. A
fixture line `> {...}` sends a request, `< tok | tok` requires every
token in the next response, and `@sessions=2` opens a second session so
isolation, `Global` sharing and `wall` delivery are checkable. At the end
of every fixture each client finishes, so every fixture also exercises
`handleDisconnect` and realm teardown. `tests/unit/test_repl_outbuf.c`
drives the same transport to pin the output buffer's lazy growth, its
cap and the dropped report.
