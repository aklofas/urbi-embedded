# Loader Strand

This document describes the loader-strand model introduced in v0.8.0.  It
covers allocation, the `urbi_run_chunk` park-or-die contract, UModule refcount
mechanics, the asymmetry with `urbi_vm_run`'s transient strand, and the forward
path toward the canonical UModule layout refactor.

---

## 1. Overview

Before v0.8.0, `urbi_run_chunk` executed a loaded chunk on a **stack-local
transient strand** — a strand that was not enqueued in the scheduler and had no
scheduler context.  The transient model worked well for simple sequential
scripts, but it made chunk-top `&` (fork-join) and `,` (fork-detach) fatal:
both opcodes need `urbi_step` to run child strands, and the transient strand
had no such driver.  The legacy urbiscript spec allows `&` and `,` at chunk-top
(the original runtime always ran chunks under a scheduler-managed lobby strand),
so the v0.7.x transient model was a known divergence.

At v0.8.0, `urbi_run_chunk` switches to a **persistent scheduler-managed
loader strand**.  The strand is a real heap-allocated `UStrand` object,
enqueued in `realm->strands_head`, with `is_transient_strand = 0`.  A short
internal driver loop (`uchunk_loader_drive`) pumps `urbi_step` until the
strand parks (hits a sleep / join-wait / event-wait) or dies (OP_RET or
fatal).  Once `urbi_run_chunk` returns, the host's normal `urbi_step` loop
continues advancing the strand alongside any background work (at-handlers,
every loops, etc.).

This restores legacy parallel-by-syntax semantics.  The legacy
`tests/2.x/separator/` conformance fixtures (canonical demonstrations of `&`,
`,`, and their interaction with scoping) become runnable for the first time in
the v1.0 implementation.

The design-risks entry "v0.7.x — `&` fork-join requires urbi_step driver" is
deleted; the constraint is structurally resolved.

---

## 2. Lifecycle

### Allocation and arming

`urbi_run_chunk(vm, realm, module, out_result)` performs the following sequence:

1. **Refcount the module.**  Calls `umodule_refcount_inc(module)` — the module's
   `refcount` field (a new runtime-only `uint16_t`) is incremented before any
   strand creation.  This prevents a caller who calls `umodule_destroy` from a
   signal handler or a second thread from freeing the module while the loader
   strand still executes it.

2. **Allocate the loader strand.**  Calls
   `urbi_strand_create_for_module(vm, realm, module)`.  This helper
   pool-allocates a `UStrand`, sets `s->module = module`,
   `s->is_transient_strand = 0`, arms the strand at the root chunk's entry
   point (PC = 0, call stack empty, register bank zeroed), and links it into
   `realm->strands_head`.

3. **Drive until park or die.**  Calls `uchunk_loader_drive(vm, realm)`.  This
   is a bounded outer loop that calls `urbi_step(vm, budget, NULL)` and checks
   whether the loader strand has reached a stable intermediate state:
   - **Park:** the strand is in `USTRAND_STATE_SLEEPING`,
     `USTRAND_STATE_JOIN_WAIT`, or `USTRAND_STATE_EVENT_WAIT` — it is waiting
     on something the host will drive later.  `uchunk_loader_drive` returns
     `URBI_OK`; `urbi_run_chunk` returns to the host.
   - **Die:** the strand's state is `USTRAND_STATE_DEAD` (OP_RET reached) or
     `USTRAND_STATE_FATAL`.  `uchunk_loader_drive` returns `URBI_OK` (dead) or
     the fatal error code; `urbi_run_chunk` propagates the code.
   - **Budget exhausted without park or die:** `uchunk_loader_drive` returns
     `URBI_ERR_LOADER_BUDGET` after the outer iteration cap is exhausted.  See
     §3 for the semantics of this case.

   The realm-walk to detect loader death is UAF-safe: `uchunk_loader_drive`
   walks `realm->strands_head` by pointer — the strand is on the realm list,
   not on the stack — so there is no dangling pointer even if the strand is
   reaped between `urbi_step` calls.

### Persistence

Once `urbi_run_chunk` returns (park or die), the strand remains in
`realm->strands_head` if it is still alive.  The host's main `urbi_step` loop
advances it on subsequent calls.  Any background work forked at chunk-top
(detached `,` forks, `at`-handlers installed, `every` loops started) also
persists and is driven by the host's step loop.

### Module decrement and deferred destroy

When the loader strand reaches `USTRAND_STATE_DEAD`, the strand's cleanup path
calls `umodule_refcount_dec(module)`.  If this brings `module->refcount` to 0
and `module->destroy_requested` is set, `umodule_destroy(module, vm)` is
called immediately.  If `refcount` reaches 0 but `destroy_requested` is clear,
the module memory is left alive for the host to destroy manually.

The host's `umodule_destroy(module, vm)` call is therefore safe in both the
typical case:

- **Host calls destroy after `urbi_vm_destroy`:** at that point all strands are
  dead and `refcount` is 0 — immediate destroy.
- **Host calls destroy while loader strand is still alive:** `refcount > 0`,
  so destroy sets `module->destroy_requested = 1` and returns immediately.
  The actual destruction happens when the strand's cleanup path decrements to
  0.

---

## 3. `urbi_run_chunk` Contract

### Return semantics

`urbi_run_chunk` returns when the loader strand reaches one of two stable
states: **parked** (sleeping / waiting on a join or event) or **dead** (OP_RET
/ fatal unwind completed).  In both cases the return value is `URBI_OK` for
normal completion, or a negative error code for fatal or OOM conditions.

### `out_result`

If `out_result` is non-NULL and the loader strand dies normally (OP_RET), the
final value in the strand's R[0] register is written to `*out_result`.  If the
strand parks instead of dying, `*out_result` is written as `urbi_make_nil()` to
indicate a pending state rather than a completed result.

If `out_result` is NULL the behavior is identical — no write occurs.

### Error codes

| Code | Meaning |
|---|---|
| `URBI_OK` (0) | Loader strand parked or died normally. |
| `URBI_ERR_OOM` | Allocation failure during strand creation or loader drive. |
| `URBI_ERR_LOADER_BUDGET` (-20) | Inner driver exhausted the iteration cap without the strand reaching park or dead.  The strand is still alive in `realm->strands_head`; the host's `urbi_step` loop will continue advancing it.  This is not a fatal condition — it means the chunk-top has more work than fits in the internal drive budget. |
| (strand fatal code) | The loader strand hit a fatal opcode (`OP_FATAL`, unhandled exception propagating past chunk-top, etc.).  The strand is in `USTRAND_STATE_FATAL`.  The host can inspect with `urbi_last_error`. |

### `URBI_ERR_LOADER_BUDGET` handling

This code is returned when the internal `uchunk_loader_drive` loop finishes all
its allowed `urbi_step` budget calls but the loader strand has not yet reached
a park or die state.  This can happen if:

- The chunk-top spawns many detached `,` forks that all become immediately
  runnable, filling the scheduler queue beyond the budget.
- The chunk-top is a long computation that exceeds the budget.

In both cases the strand is still live in `realm->strands_head` and the host's
normal step loop will drain it.  The host should not treat
`URBI_ERR_LOADER_BUDGET` as an error — it means "the chunk is still running,
drive it via `urbi_step`."

---

## 4. UModule Refcount Mechanics

### Fields

Two new runtime-only fields are added to `UModule`:

```c
uint16_t refcount;         /* number of live strands referencing this module */
bool     destroy_requested; /* umodule_destroy called while refcount > 0 */
```

Both fields are runtime-only and not serialized to the wire format.  The wire
format remains at v1.6 / 0x16.

### Helpers

`umodule_refcount_inc(m)` and `umodule_refcount_dec(m, vm)` in
`<src/module/umodule.h>` manage the counter:

- **Saturation at UINT16_MAX:** if `refcount` would overflow, the increment is
  suppressed and a `URBI_LOG_WARN` is emitted.  The module is then treated as
  permanently live (never freed), which is a correct-if-leaky fallback.  This
  mirrors the v0.7.3 UProto refcount saturation policy.
- **Underflow assert:** in debug builds, decrementing below 0 is an assertion
  failure.  In release builds the decrement is suppressed (module stays alive).

### Bump sites

`refcount` is bumped in three places:

1. **`urbi_run_chunk`** — before `urbi_strand_create_for_module`, ensuring the
   module outlives the strand allocation even if the strand creation itself
   fails.
2. **`op_fork.c` (OP_FORK_DETACH / OP_FORK_JOIN)** — when a chunk-top fork
   spawns a child strand referencing the same module.
3. **`urbi_strand_create_for_module`** — the helper itself bumps on behalf of
   the newly created strand's reference to `s->module`.

### Decrement sites

`refcount` is decremented in the strand's cleanup path:

- **`strand_cleanup` / `urbi_strand_destroy`** — after the strand's register
  bank and cleanup stack are freed, `umodule_refcount_dec` is called.  If this
  brings refcount to 0 and `destroy_requested` is set, `umodule_destroy` is
  called immediately.

### Interaction with v0.7.3 UProto refcount

v0.7.3 added a `uint16_t refcount` to `UProto` for closure-aliasing safety
(WATCH-015).  The v0.8.0 UModule refcount is a parallel mechanism — same
saturation / underflow policy, same bump-at-bind / dec-at-death discipline,
but at the module granularity rather than the per-proto granularity.  The two
counters are independent; the canonical end-state (M8 UModule layout refactor)
fuses them into a single field on `UProto *root_proto`.

---

## 5. Transient Strand Asymmetry

`urbi_vm_run` in `src/vm/uvm_run.c` is **preserved** and continues to use a
stack-local transient strand.  The transient path is kept for:

- **Test helpers** that need a synchronous, stack-scoped execution context.
- **Future REPL one-shot expression eval** — short expressions that should
  complete synchronously without persisting in the scheduler.
- **Debugger expression evaluation** — the debugger may need to evaluate an
  expression in a controlled context without perturbing the scheduler queue.

The difference in behavior:

| Property | `urbi_run_chunk` (loader strand) | `urbi_vm_run` (transient strand) |
|---|---|---|
| Allocation | Pool-allocated, heap lifetime | Stack-local, call-frame lifetime |
| `is_transient_strand` flag | 0 (false) | 1 (true) |
| Enqueued in `realm->strands_head` | Yes | No |
| Survives past return | Yes (if parked) | No |
| Chunk-top `&` / `,` | Works | Fatal (`URBI_ERR_STRAND_FATAL`) |
| UModule refcount bump | Yes | No |

The transient path is only exposed to low-level callers.  Normal embedders use
`urbi_run_chunk` exclusively.

---

## 6. Defense-in-Depth Runtime Guards

OP_FORK_JOIN and OP_FORK_DETACH still contain an `is_transient_strand` check:

```c
if (s->is_transient_strand) {
    strand_fatal(s, URBI_ERR_STRAND_FATAL,
        "OP_FORK_JOIN: '&' requires urbi_step driver");
    return;
}
```

These guards remain in place as defense-in-depth.  Under the persistent-loader
path they are never reached from normally-loaded chunks, because loader strands
have `is_transient_strand = 0`.

The guards catch misuse of the transient path — for example, if a test helper
uses `urbi_vm_run` to evaluate a chunk that contains a chunk-top `&`.  The
runtime error message is clear and actionable; the guards do not need to be
removed to support the v0.8.0 feature.

---

## 7. CHSTR-051 Fix Context

CHSTR-051 was a latent bug in `strand_unlink_from_tags`: it scanned only
`[0..cleanup_depth)` rather than `[0..cleanup_cap)`.  When `strand_cleanup_pop`
decrements `cleanup_depth` to pop a TAG_SCOPE entry, the entry's slot is cleared
but the strand is NOT unlinked from `tag->member_strands_head`.  On subsequent
tag stop or tag sweep, the tag walks its member list and dereferences a pointer
to the now-dead strand entry — UAF.

This bug was latent under the v0.7.x transient-strand model because transient
strands are never enqueued and the GC sweep never reached their cleanup stack.
Under the persistent-strand model, strands live long enough for the tag machinery
to encounter the stale link.

The fix is `[0..cleanup_cap)` — scan every slot, not just the live prefix.  Slots
above `cleanup_depth` are zero-initialized; the unlink is a no-op if the slot
was already cleared by the pop.

Fixed in commit `9e2c725`.  `tests/chk/exceptions/throw_in_finally.chk` exercises
the failing path under the persistent-strand model.

---

## 8. Forward Path

The v0.8.0 persistent loader strand is a structural improvement but not the
canonical end-state.  The next step in the trajectory is the **UModule layout
refactor**, described in:

> the v0.8.0 persistent-loader-strand design spec §11

Summary of that refactor:

- `UModule` becomes a thin shell: `{ UProto *root_proto; UProto **nested; size_t nested_count; UModuleHeader header; }`.
- The root chunk's instructions, constants, line_deltas, and IC metadata move
  into `root_proto` — the same per-proto layout already used for function
  literals.
- `UModule.refcount` fuses with `root_proto->refcount` — one field, one
  mechanism.
- Strands bind `root_proto` directly via a new `s->root_proto` field instead of
  `s->module`.
- The wire format gains a version bump; ABI gains a MINOR bump.

This refactor is a pre-M8 prerequisite and is tracked in the design-risks
register under "M8 — UModule canonical-trajectory refactor."  The realm-owned
`loaded_protos[]` registry (multi-chunk REPL eval, disconnect-cleanup) follows
as the M8 REPL milestone milestone work.

---

## 9. References

- **Spec:** v0.8.0 persistent-loader-strand design — full design with
  state-machine diagrams, refcount policy, and the §11 forward path.
- **S-loader-strand** — compatibility-decisions ledger entry for the
  language-level commitment to persistent loader semantics.
- **v0.7.3 UProto refcount precedent** — CHANGELOG entry `v0.7.3-bugfixes`
  "Changed" section; design-risks entry "WATCH-015 cascade-wake".
- **CHSTR-051** — commit `9e2c725`, `tests/chk/exceptions/throw_in_finally.chk`.
- **Test files:**
  - `tests/unit/test_loader_strand_persistence.c` (7 cases)
  - `tests/unit/test_module_refcount.c` (6 cases)
  - `tests/chk/separator/and-environment.chk`, `comma.chk`, `comma-environment.chk`
    (3 activated legacy separator fixtures)
