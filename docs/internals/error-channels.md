# Error channels

`urbi-embedded` reports failures through several distinct channels. They are
not interchangeable: each answers a different question ("did this C call
succeed?", "did the script raise?", "what should a REPL client show?"), and
mixing them is a common source of confusion. This document maps the channels
and states which one to use when.

For the canonical enum definitions see `include/urbi/types.h`; the internal
strand-unwind taxonomy lives in `src/sched/ustrand.h`.

## The channels

### 1. `UErrCode` — public C return codes

Every public `urbi_*` function returns `int`: `URBI_OK` (`0`) on success or a
negative `URBI_ERR_*` code on failure (`include/urbi/types.h`, `UErrCode`).
This is the channel a C embedder checks after every API call. It answers "did
this C call succeed?" — it does **not** carry a script-level exception value.
A negative return is also published to the per-VM error ring (channel 4).

New internal code that needs to name one of these codes should use the
`URBI_OK` / `URBI_ERR_*` spellings directly. The retired `UVM_OK` /
`UVM_TYPE_ERROR` / `UVM_OOM` / `UVMError` compatibility aliases in `types.h`
map onto `URBI_OK` / `URBI_ERR_STRAND_FATAL` / `URBI_ERR_OOM` / `int`
respectively and exist only for host source compatibility (see the
deprecation note in `types.h`).

### 2. `UCallbackSignal` — positive host-callback returns

Host callbacks (`urbi_native_method_fn`, `urbi_watcher_fn`) return a
**positive** `UCallbackSignal`: `URBI_CB_OK` (`0`), `URBI_CB_UNREGISTER` (`1`,
watcher auto-unregister after this firing), or `URBI_CB_THROW` (`2`, the host
wants to raise a script exception). The sign convention keeps callback signals
disjoint from the negative `URBI_ERR_*` failure space, so a single `int`
return can be read unambiguously. `URBI_ERR_WATCHER_UNREGISTER` is a retired
alias for `URBI_CB_UNREGISTER`.

### 3. `vm->last_error` — the VM's internal fatal slot

`vm->last_error` (a plain `int`) is where the interpreter records the code of
the fatal that terminated a strand — typically `URBI_ERR_STRAND_FATAL` for an
uncaught throw or type error, or `URBI_ERR_OOM`. It is an internal field, not
a public API; the fixed error-message buffer is formatted alongside it for
`source:line:`-prefixed diagnostics. `urbi_vm_run` / `urbi_run_chunk` surface
the same code through their `int` return (channel 1).

### 4. Per-VM error ring — `urbi_last_error`

`urbi_last_error(vm, &info)` reads the most-recent failure from a small per-VM
ring, filling a `urbi_error_info_t` (code, source line, context string).
`urbi_clear_error(vm)` empties it. This is the channel for retrieving detail
*after* an API call has already returned its `UErrCode`; the ring persists the
last failure so the host can inspect it without threading an out-parameter
through every call.

### 5. Catchable raises — typed `Exception` instances

Script-level errors that a `try` / `catch` can intercept travel as **values**,
not codes. The internal `urbi_raise_*` helpers (`src/stdlib/object_root.c`)
build a typed `Exception` instance (`TypeError`, `ArityError`, `LookupError`,
`OutOfMemoryError`, …) and deposit it on the running strand as `UEXEC_THROW`
(channel 6). The dispatcher's safepoint walks the cleanup stack to a
`try`-handler; if none exists the throw becomes the strand's fatal and surfaces
through channels 1/3. This is the only channel that carries a script-visible
value.

### 6. Strand unwind status — `UExecStatus` / `UEXEC_*`

Internally, control transfer out of a strand is one of five kinds:
`UEXEC_OK`, `UEXEC_RETURN`, `UEXEC_THROW`, `UEXEC_TAG_STOP`, `UEXEC_CANCEL`
(`src/sched/ustrand.h`, mirrored publicly as `UStrandUnwind` /
`URBI_UNWIND_*`). This is not an error channel per se — `UEXEC_RETURN` and
`UEXEC_OK` are normal exits — but `UEXEC_THROW` and `UEXEC_TAG_STOP` are how
raises and tag cancellation propagate through the unwind walker before they
reach channels 5 and 1.

### 7. REPL NDJSON envelopes

The REPL service (`src/repl/`) reports per-request outcomes as NDJSON
envelopes: a `result` envelope on success or an `error` envelope carrying a
short string `code` (`parse`, `runtime`, `budget_depth`, `oom`, …) mapped from
the underlying `UErrCode` (`src/repl/urepl_dispatch.c`). This is the wire-level
channel a remote REPL client consumes; it is a projection of channels 1/5 into
a client-friendly, transport-stable form.

## Which channel when

| Situation | Channel |
|-----------|---------|
| Checking whether a C API call succeeded | 1 — `UErrCode` return |
| Returning from a host callback | 2 — positive `UCallbackSignal` |
| Retrieving failure detail after a call | 4 — `urbi_last_error` ring |
| Raising a script-catchable error from host C | 5 — `urbi_raise_*` → `UEXEC_THROW` |
| Propagating a raise / return / tag-stop internally | 6 — `UEXEC_*` |
| Reporting a result to a remote REPL client | 7 — NDJSON envelope |

The internal fatal slot (channel 3) is written by the interpreter, not by
embedders; read it only through the public surface (channel 1 return or
channel 4 ring).
