# The eval-service fixtures

NDJSON, not urbiscript. Everything else under `tests/chk/` is a program and
its expected output, diffed line by line. These are a conversation: requests
in, response envelopes out, checked by substring because an id, a strand
number or an address is not reproducible across runs.

That is why they have their own driver. `tests/integration/run_chk.sh` routes
any fixture carrying `## mode: repl` to `build/<target>/repl-chk-driver`,
which does the matching itself; the driver's exit status is the verdict and
nothing is diffed. Under the sanitizers the driver is rebuilt with them, so
the whole service is exercised instrumented.

## Format

```
## mode: repl

@sessions=2

> {"id":1,"op":"eval","code":"1+2"}
< "id":1 | "kind":"result" | "value":"3"
< "id":1 | "kind":"done"

>2 {"id":2,"op":"eval","code":"Global.x"}
<2 "id":2 | "kind":"result"
```

- `#` starts a comment; blank lines are ignored.
- `## mode: repl` is required. Without it the runner treats the file as
  urbiscript and every line fails to compile.
- `>` sends one request and runs one service sweep plus one VM step, so
  anything the eval armed that is due immediately has already run when the
  next request arrives.
- `<` claims the next response line on that session. Every `|`-separated
  token must appear somewhere in the line; order within the line is free.
  Omit the fields that are not stable and match the parts that are.
- A digit directly after `>` or `<` names a session: `>2` sends on session
  two, `<2` reads from it. No digit means session one.
- **A response no `<` line claims fails the fixture.** That is what makes a
  negative claim checkable: `wall_reaches_other_sessions.chk` proves the
  sender does not receive its own broadcast by never claiming one.

## Pragmas

Each on its own line, anywhere before the first request.

| Pragma | Effect |
|---|---|
| `@sessions=<n>` | Open n sessions (1–4). Default 1. |
| `@budget-depth=<n>` | `max_parser_depth` for every session's realm |
| `@budget-nodes=<n>` | `max_ast_nodes` |
| `@budget-source=<n>` | `max_source_bytes` |

## What every fixture gets for free

The driver finishes each client and sweeps once more before tearing down, so
every fixture in this directory runs the disconnect path: the lobby's
`handleDisconnect` fires and the session's realm is freed and unregistered
from `Lobby.lobbies`. A leak or a use-after-free there surfaces under the
sanitizers across the whole directory rather than needing a fixture of its
own.

## What changed when these came back

The sixteen original fixtures were written against the v0.9.1 networked
server and its unit-test driver, which the Phase 0 test cleanup removed.
Their **request and expectation lines are unchanged**; what changed is:

- Each gained a `## mode: repl` line, which is the routing directive the
  new runner needs.
- Header comments were rewritten where they described machinery that no
  longer exists (`urbi_repl_eval`, `urbi_vm_write_in_realm`, the removed
  `tests/unit/test_repl_multi_client.c`) or claimed a gap that is now
  covered.
- `@no-auto-session`, which was reserved for two-session fixtures and never
  used, is gone; `@sessions=<n>` is how a fixture asks for more than one.

Two fixtures are new, and they carry the claims a single session could only
gesture at: `wall_reaches_other_sessions.chk` and
`two_session_isolation.chk`.

## What the service does not do here

The networked server — listener, per-connection threads, auth, rate limits,
the TCP/Unix/UART transports — is parked during the core re-foundation, and
with it the `auth`, `cancel`, `lobby_new` and `lobby_close` ops and eight of
the nine introspection primitives. `coros` is the one that remains. Nothing
in this directory tests what is parked; when the server returns, so do its
fixtures.
