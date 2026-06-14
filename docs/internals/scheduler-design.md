# Scheduler design

This document captures the contract that every scheduler implementation must
satisfy. The cooperative scheduler at `URBI_SCHED_COOPERATIVE` is the only
implementation today; the contract is written so that future schedulers
(priority bands, work-stealing, preemptive) can satisfy the same invariants
without GC walker changes.

For a higher-level overview of how the scheduler fits into the runtime, see
[architecture.md](architecture.md). For the cooperative scheduler interface
itself, see `src/sched/usched_cooperative.h`.

## GC walker contract

Every UStrand whose register window may contain GC-managed UValues MUST be
reachable from `vm->realms_head → realm.strands_head → strand`. Scheduler
implementations are responsible for maintaining this invariant; the GC walker
assumes it without re-verification.

Implications:

- The cooperative scheduler's `ready_head` and `sleep_q_head` are
  scheduler-private. The GC walker does not consult them.
- Future schedulers (`URBI_SCHED_PRIORITY`, `URBI_SCHED_PREEMPTIVE`) may use
  arbitrary internal structures (multiple ready queues, mutex/semaphore wait
  queues, work-stealing per-CPU queues, timer wheels) without affecting GC
  correctness.
- DEAD strands remain on `realm.strands_head` until `urbi_realm_destroy`
  reclaims them; the walker filters DEAD via the existing per-strand guard
  inside `strand_walk_roots` (see `src/sched/usched_cooperative.c`).
- Transient strands (e.g. `uvm_run`'s stack-local strand) MUST be threaded
  onto `vm->global_realm->strands_head` for the duration of their run, and
  unlinked before they leave scope. The convenience getter is
  `urbi_realm_global(vm)`, which lazy-creates the global realm on first call.

The contract is verified by the unit suite in
`tests/unit/test_scheduler_invariant.c`, which sweeps READY → WAITING_SLEEP
→ READY → RUNNING → WAITING_JOIN → DEAD and asserts `strand` remains on
`realm.strands_head` after every transition. A second suite in
`tests/unit/test_gc_strand_walker.c` confirms the walker visits WAITING_JOIN
strands, filters DEAD strands, reaches strands that sit on no scheduler
queue, and round-trips the `uvm_run` transient through the global realm.

## Runnable-count ownership (v0.13.3)

`vm->strand_runnable_count` is a **single-writer counter**: its only legitimate
mutators are `urbi_sched_runnable_inc` and `urbi_sched_runnable_dec`, both in
`src/sched/usched_cooperative.c`, plus zero-init at VM creation/reset.

**Invariant:** `strand_runnable_count == |READY queue| + (1 if a non-transient
strand is RUNNING, else 0)`.

WAITING and SUSPENDED strands are **not** counted — they represent
external-input-dependent work (parked on sleep, event, join, or tag
gate). Transient strands (`is_transient_strand = 1`, e.g. scratch and
`urbi_vm_run`) never participate in the count even while RUNNING.

## Liveness formula and QUIESCENT (v0.13.3)

A single function `urbi_vm_liveness` (`src/sched/usched_liveness.c`)
computes all liveness state. Three subordinate views read from it:

- `sched_quiescent` — the cooperative scheduler's internal idle predicate.
- `urbi_step`'s post-loop result ladder — selects `URBI_STEP_QUIESCENT /
  WAKE_AT / RUNNING`.
- `urbi_vm_has_live_work` — the host-facing inclusive liveness query.

`UVmLiveness` has four fields:

| Field | Meaning |
|---|---|
| `runnable` | `strand_runnable_count` — READY + RUNNING non-transient |
| `pending` | internal work the next step performs without external input (ISR ring, host_call_pending, dirty watcher count, pending-onleave queue) |
| `armed` | external-input-dependent work (active_count watchers + SUSPENDED strand count + WAITING strand count) |
| `timed` | 1 if a sleep-queue or periodic wake deadline is pending |

**QUIESCENT** (`URBI_STEP_QUIESCENT`) is returned when `runnable == 0 &&
pending == 0 && timed == 0`. The `armed` count is **excluded** from the
QUIESCENT test — armed watchers and parked strands are re-armed by host
slot writes or injected events, and QUIESCENT is the signal to the host
that no internal progress is possible without external input.

`urbi_vm_has_live_work` is **inclusive**: it returns `true` when
`runnable + pending + armed + timed > 0`, allowing the host to distinguish
a fully-dead VM (all zero) from an armed-but-idle one that is waiting for
external input.

## Block/freeze suspension gates (v0.13.3)

Each strand carries two independent gate bits in `suspend_gates`:

| Bit | Macro | Set by | Cleared by |
|---|---|---|---|
| 0x01 | `USTRAND_GATE_BLOCK` | `urbi_tag_block` | `urbi_tag_unblock` |
| 0x02 | `USTRAND_GATE_FREEZE` | `urbi_tag_freeze` | `urbi_tag_unfreeze` |

A strand in `USTRAND_SUSPENDED` resumes only when **both** bits are clear
(`urbi_strand_resume_if_ungated`). Clearing one gate while the other is
still set re-stamps the reason nibble and keeps the strand SUSPENDED.
A tag-stop or cancel on a SUSPENDED member unconditionally clears both
gates and resumes the strand (stop wins over suspension).

The `USTRAND_REASON_BLOCK` / `REASON_FREEZE` nibble on the state byte is a
diagnostic decode of the gate-priority rule (`BLOCK` outranks `FREEZE`);
the gate bits in `suspend_gates` are authoritative for resume eligibility.

**Known limitation (v1.x):** block and freeze gates are per-strand bits,
not per-tag refcounts. If two different tags block/freeze the same strand,
one tag's unblock can resume a strand the other tag still wants blocked.
Filed as a design-risk for a future release (per-tag gate refcount).

See `src/sched/ustrand.h` (`USTRAND_GATE_*` macros + `urbi_strand_suspend` /
`urbi_strand_resume_if_ungated`) and `src/sched/usched_cooperative.h` for
the full transition contract.
