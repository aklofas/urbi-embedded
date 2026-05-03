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
