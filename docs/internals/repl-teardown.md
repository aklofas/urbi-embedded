# REPL session teardown ownership model

This document is the authoritative contract for REPL session lifetime in
`src/repl/`.  Code must stay consistent with it; audits check it first.

Introduced as a pre-fix document in v0.10.7 W3 to capture the single-owner
model that W1 of v0.10.6 established, and to name the three concrete defects
that W3 corrects.

Source: `src/repl/urepl_dispatch.{c,h}`, `src/repl/urepl_listener.{c,h}`,
`src/repl/urepl.h`.

---

## 1. Roles

| Actor | Identity | May call `urepl_session_destroy`? |
|-------|----------|----------------------------------|
| **VM thread** | The single thread that called `urbi_repl_serve` / drives `urbi_step` | YES — sole owner |
| **POSIX reader thread** | Per-session `pthread` spawned by `spawn_reader` | NO |
| **Cooperative sweep** | `urbi_repl_serve_step` called from the VM thread | YES (it IS the VM thread) |
| **Listener thread** | The `pthread` that calls `accept()` | NO |
| **Host via `urbi_repl_stop`** | Any thread (typically test thread or signal handler) | NO — delegates to VM thread |

The invariant: `urepl_session_destroy` must only be called from the VM
thread.  Every other path requests teardown via `urepl_request_teardown`
and lets the VM thread reap.

---

## 2. Fields and their owners

### `s->needs_teardown`

- **Type:** `bool` in `struct UReplSession` (declared in `src/repl/urepl_dispatch.h:71`).
- **Writer (flag set to true):**
  - POSIX reader thread: `urepl_request_teardown` — must use
    `__atomic_store_n(..., __ATOMIC_RELEASE)`.
  - Cooperative (VM thread) sweep: `urepl_read_sweep_nonpollable`,
    `urepl_write_sweep_nonpollable` — also use
    `__atomic_store_n(..., __ATOMIC_RELEASE)` for field consistency.
  - VM thread stop path: `urepl_listener_stop_and_join` —
    `urepl_request_teardown` (same atomic store).
- **Reader:**
  - VM thread reaper (`urepl_session_reap_pending`): `__atomic_load_n(..., __ATOMIC_ACQUIRE)`.
  - Cooperative sweep (`urepl_disconnect_sweep`): `__atomic_load_n(..., __ATOMIC_ACQUIRE)`.
  - Cooperative write sweep (skip-flag check): `__atomic_load_n(..., __ATOMIC_ACQUIRE)`.
- **Synchronizes-with guarantee:** The `__ATOMIC_RELEASE` store in
  `urepl_request_teardown` + the `__ATOMIC_ACQUIRE` load in
  `urepl_session_reap_pending` form a release/acquire pair.  All writes
  made by the requesting thread before the store are visible to the
  reaping thread after the load observes `true`.
- **NO plain (non-atomic) access anywhere.**  Cooperative-only builds are
  single-threaded by the contract in `docs/internals/repl-service.md`, but
  the same struct field is shared with POSIX paths; mixed access (some
  atomic, some plain) is undefined behaviour under the C11 memory model.

### `r->session` (UReplReader.session)

- **Written to NULL by:**
  - VM-thread reaper (`urepl_session_reap_pending`, `urepl_disconnect_sweep`):
    while holding `sessions_mutex`.
  - Reader thread exit (`reader_main`): while holding `sessions_mutex`
    (added in W3/v0.10.7 — previously unguarded; was a data-race bug).
  - Stop path (`urepl_listener_stop_and_join`): while holding
    `sessions_mutex` (after routing through reaper).
- **Read:**
  - Reader thread: only before the teardown request (reads its own local
    pointer; the NULL assignment is synchronized by the mutex).
  - VM thread: under `sessions_mutex`.
- **Invariant:** every write to `r->session` must occur under
  `sessions_mutex`.

### `head->session` (UReplReader.session, stop-path context)

Same field as `r->session`; the stop path walks the readers list.  Writes
from `urepl_listener_stop_and_join` are under `sessions_mutex` (see § 5).

---

## 3. Lifecycle states

```text
ACCEPT → ACTIVE → FLAGGED → UNLINKED → FREED
```

| State | Who effects the transition |
|-------|---------------------------|
| ACCEPT → ACTIVE | Listener thread or VM thread (cooperative); `spawn_reader` links session into `sessions_head` under `sessions_mutex`. |
| ACTIVE → FLAGGED | Reader thread calls `urepl_request_teardown`; atomic release store on `needs_teardown`. |
| FLAGGED → UNLINKED | VM thread's `urepl_session_reap_pending` or `urepl_disconnect_sweep`; session removed from `sessions_head` under `sessions_mutex`. |
| UNLINKED → FREED | VM thread calls `urepl_session_destroy` AFTER releasing `sessions_mutex`.  Reader thread is joined first. |

Key rule: `urepl_session_destroy` is only called after the session is
unlinked from `sessions_head`.  Its internal unlink pass is a no-op when
the session is already removed.

---

## 4. Synchronizes-with edges

The critical release/acquire pair:

```c
/* Writer (reader thread or VM-thread request path): */
__atomic_store_n(&s->needs_teardown, true, __ATOMIC_RELEASE);

/* Reader (VM thread, urepl_session_reap_pending): */
bool flagged = __atomic_load_n(&s->needs_teardown, __ATOMIC_ACQUIRE);
```

When the acquire load in `urepl_session_reap_pending` observes `true`,
all memory writes by the requesting thread before the release store are
visible to the reaping thread.  This covers at minimum: the client_fd
close (in `reader_main`) and any parse-buffer state changes.

The `sessions_mutex` provides a separate synchronization domain for
structural mutations (pointer rewrites in the linked lists).

---

## 5. Stop path (`urepl_listener_stop_and_join`)

The stop path is called during `urbi_repl_stop` after the embedding loop
has ended.  By the time it reaches session cleanup:

- The listener pthread has been joined (no new accepts).
- Each reader pthread has been joined (no concurrent session access).
- `urbi_step` is no longer running (no background reaper firing).

Because no other thread is active, the stop path IS the effective sole
owner of destruction at this point.  It calls `urepl_request_teardown`
(idempotent atomic release store) then `urepl_session_destroy` directly:

```c
if (head->session != NULL) {
    urepl_request_teardown(head->session);   /* idempotent; sets flag */
    head->session->reader = NULL;
    urepl_session_destroy(server, head->session);
    head->session = NULL;
}
```

The pre-W3 bug was not in calling `urepl_session_destroy` per se but in
the stale comment claiming "session destroyed by reader_main's exit path"
(contradicting the W1 model) and in the missing `urepl_request_teardown`
before the destroy call.  W3 corrects both.

The background reaper (`urepl_session_reap_pending`) is NOT called from
the stop path; it is designed for the concurrent normal case where
`urbi_step` is running and reader threads are live.

---

## 6. Known limitations (v1.x)

- **TSAN coverage absent.** The `claude-yolo-cross` dev image does not
  include the Thread Sanitizer runtime.  Baking TSAN is deferred to v1.x.
  The ASan 100-trial stress CI job (`repl-multi-client-stress`) catches
  use-after-free but not data races.
- **Cooperative-only builds** are single-threaded by contract (`POSIX
  reader pthread` path is `#ifdef`-excluded).  The atomic operations on
  `needs_teardown` in cooperative sweeps are a consistency requirement, not
  a functional threading requirement.  They add no overhead in practice.
