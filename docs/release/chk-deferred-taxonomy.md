# `.chk` fixture deferred-fixture taxonomy

> Closes audit-2 finding #8. Documents the four status buckets that replace the
> retired `defer-to: M*/T*` label scheme.

## Overview

Starting with v0.10.7, every non-active `.chk` fixture carries one of four
status annotations. The old `# defer-to: M4`, `# defer-to: T59`, etc. labels
are **retired** and must not appear in new fixtures.

`rg -n 'defer-to:' tests/chk/` should return nothing in v0.10.7 and later.

---

## Four taxonomy buckets

### `active`

```text
# active
```

The fixture runs in the normal `make test-chk` pass without any skip gate.
The `//# active` header is optional (absence implies active), but is written
on fixtures that were previously deferred to document that the feature shipped.

**Matrix-row link:** not required for active fixtures.

---

### `deferred: v1.x`

```text
//# deferred: v1.x — <reason>
//# matrix-row: <row-id>
```

The feature is **consciously out of v1.0 scope**. The design decision was made
explicitly (documented in `docs/language-compatibility-matrix.md`). The fixture
will be revisited in a post-v1.0 point release.

The `<reason>` field is a one-line human-readable description of why the feature
is deferred. The `<row-id>` is the corresponding row identifier from the
language compatibility matrix.

Examples:

- `deferred: v1.x — tag.enter?/tag.leave? events not yet implemented (T55)`
- `deferred: v1.x — onleave clause parse/emit not implemented; PARSE-033 open`
- `deferred: v1.x — URBI_SCHED_RT real-time scheduler is post-v1.0 scope`

---

### `dropped: <rationale>`

```text
//# dropped: <rationale>
//# matrix-row: <row-id>
```

The feature is **permanently unsupported** in urbi-embedded v1.0 (and beyond,
unless a specific future version lifts the decision). The fixture exists as a
specification record but the code under test will never produce the expected
output.

Examples:

- `dropped: CallMessage / evalArgAt permanently removed; see docs/migration/callmessage-migration.md`
- `dropped: bare-brace expression {} not in v1.0 parser; use Object.new()`

---

### `blocked: <work-item>`

```text
//# blocked: <work-item>
//# matrix-row: <row-id>
```

The feature is in v1.0 scope but **cannot be activated yet** because a specific
work item is not complete. The fixture WILL activate before the v1.0-rc milestone
opens. The `<work-item>` is a one-line description of the blocker that links to
an open tracking item.

Examples:

- `blocked: T39 chk-runner tunables/host-driver extension not implemented`
- `blocked: tag.stop() inside try/finally does not run finally clause — v1.0-rc bug`
- `blocked: slot-change fires on first slot-install as well as write — double-fire bug`

---

## Retirement of `defer-to:` labels

The old scheme used historical task IDs (`T59`, `T29`, `T39`) and milestone
labels (`M4`, `M5`, `M6`) as defer targets. These became meaningless after
those milestones/waves shipped or the task taxonomy changed. Every occurrence
has been replaced by one of the four buckets above during v0.10.7 W7.

The mapping between old labels and new buckets is documented in the
per-fixture table below.

---

## Per-fixture classification table

Columns: **fixture** | **old label** | **new bucket** | **notes**

### chunk_lifecycle/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `lobby_alias.chk` | M6 | blocked | Lobby proto is readonly (UPROTO_READONLY); script-side mutation not available |
| `module_load_isolation.chk` | M6 | blocked | `import` keyword / import-table surface not shipped — module-into-realm isolation unexpressible (multi-realm gap now closed by chk-host-driver) |
| `realm_global_default.chk` | T39+T20 | **active** | Activated v0.10.14: chk-host-driver `## host: run` under the default (NULL/global) realm; two chunks share bindings |
| `realm_isolation.chk` | T39+T20 | **active** | Activated v0.10.14: chk-host-driver `## host: realm`+`run`; two named realms keep `x` isolated |
| `repl_session_persistence.chk` | T20 | **active** | Feature shipped (M8/v0.9.x); REPL var persistence works |
| `script_at_persists.chk` | M5 | **active** | at(cond) watcher shipped (M5/v0.5.x); activated with trigger-function pattern |

### closure/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `closure.chk` | M4 | dropped | Requires CallMessage / evalArgAt (permanently dropped) |
| `read.chk` | M4 | dropped | Requires call.argAt() / Lobby.create() / CallMessage (permanently dropped) |
| `scopes.chk` | M4 | blocked | Requires mutable Object proto; Object is UPROTO_READONLY — v1.0-rc open |

### control_transfer/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `force_in_unwind.chk` | T29 | deferred: v1.x | Requires onleave clause (PARSE-033, deferred-v1.x) |
| `onleave_normal_exit.chk` | T29 | deferred: v1.x | Requires onleave clause (PARSE-033, deferred-v1.x) |
| `onleave_on_stop.chk` | T29 | deferred: v1.x | Requires onleave clause (PARSE-033, deferred-v1.x) |
| `tag_stop_no_target.chk` | T29 | blocked | tag.stop() outside scope is silent (not fatal); expected semantics not implemented |
| `tag_stop_skips_catch.chk` | T29 | **active** | TAG_STOP skips catch correctly; updated v0.10.15 for bound-scope (clean nil, no D3 fatal) |
| `tag_stop_with_finally.chk` | T29 | **active** | Activated v0.10.15: finally runs during TAG_STOP unwind (closes v0.10.7-B, latent-fixed by v0.10.9-B binding) |
| `scope_binds_user_tag.chk` | v0.10.9-B | **active** | New v0.10.15: `t: {}` binds the user tag; t.stop() inside is a clean in-scope stop (closes v0.10.9-B) |

### exceptions/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `exceptions.chk` | M4 | blocked | Requires `isA()` method on exceptions (not implemented) |
| `regressions.chk` | M4 | blocked | Requires `isA()` guard + freeze (not implemented) |

### gc/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `finalizer_native.chk` | T39+M4 | blocked | T39 chk-runner host-driver not implemented |
| `long_running.chk` | M4 | dropped | Bare `{}` brace-block expression not in v1.0 parser; use Object.new() |
| `mem-check.chk` | M6 | blocked | Requires System.memCheck() / System.status() / System.Platform (not implemented) |
| `reset-stats.chk` | M4 | blocked | Requires stats() / resetStats() stdlib functions (not implemented) |
| `tfail-mem-check.chk` | M6 | blocked | Requires System.memCheck() + at(e?) + Event type (System not implemented) |
| `gc_slice_at_safepoint.chk` | M4 | **active** | GC-managed cells allocatable from REPL (M4/M6 shipped); activated with while-loop pattern |

### migration/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `v11_rejected.chk` | T44 | blocked | T44 chk-runner binary-blob injection not implemented |

### mutex/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `basic.chk` | M6 | deferred: v1.x | Mutex stdlib type not in v1.0 scope |
| `queued.chk` | M6 | deferred: v1.x | Mutex stdlib type not in v1.0 scope |
| `tagged.chk` | M6 | deferred: v1.x | Mutex stdlib type not in v1.0 scope |

### reactive/cross-spec/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `at_then_event_chain.chk` | T59+M5 | blocked | at(cond) body event chain does not propagate to event subscriber — cross-spec chaining gap |
| `det/det_event_chain.chk` | T59+M5 | blocked | Cross-spec chain gap — at(cond) body event chain does not propagate to event subscriber |
| `det/det_slot_change_burst.chk` | T69+M5 | blocked | Requires at.sync (not implemented) |
| `event_driven_slot_write.chk` | T59+T69+M5 | blocked | Cross-spec chain gap; Event.new()+Object.new() available but chain broken |

### reactive/event/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `event_emit_async.chk` | T59+M5 | blocked | spawn_body_coroutine subscriber dispatch — log-write ordering not yet observable from script |
| `event_multi_sub_fifo.chk` | T59+M5 | blocked | Subscriber walk order not observable across spawn — needs spawn_body_coroutine activation |
| `event_sync_emit.chk` | T59+M5 | blocked | Event.syncEmit() method not yet bound on Event stdlib type |

### reactive/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `event_fifo_determinism.chk` | T59 | blocked | T39 determinism multi-preset chk-runner mode not implemented |
| `slot_change_fifo_determinism.chk` | T69 | blocked | at.sync not implemented + T39 multi-preset chk-runner mode |
| `slot_change_reentrancy_determinism.chk` | T69 | blocked | at.sync not implemented + T39 multi-preset chk-runner mode |
| `tag_enter_leave_determinism.chk` | T59 | deferred: v1.x | Requires tag.enter?/tag.leave? events (T55, not implemented) |

### reactive/slot-change/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `slot_change_basic.chk` | T69+M5 | blocked | Double-fire on first slot-install; pre-init workaround possible but semantics gap |
| `slot_change_sync.chk` | T69+M5 | blocked | Requires at.sync (not implemented) |

### reactive/tag/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `tag_enter.chk` | T55+T59 | deferred: v1.x | Requires tag.enter? events (T55, not implemented) |
| `tag_leave.chk` | T55+T59 | deferred: v1.x | Requires tag.leave? events (T55, not implemented) |

### scheduler/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `budget_soft_yield.chk` | T39 | blocked | per-strand soft yield needs a runtime budget knob; URBI_STRAND_BUDGET_MAX is compile-time only (chk-host-driver controls step budget, not strand budget) |
| `budget_step_exhausted.chk` | T39 | blocked | mid-flight URBI_STEP_RUNNING not observable; urbi_run_chunk/urbi_repl_eval drive the loader to quiescence/park internally |
| `cross_strand_cancel.chk` | T31+T39 | blocked | T39 host-driver not implemented; T31 Strand.cancel() not implemented |
| `cycles.chk` | M4 | blocked | Requires System.cycle counter (not implemented) |
| `dispatch_safepoint_pending_unwind.chk` | T39 | **active** | Activated v0.10.14: try/catch/throw-in-loop via OP_THROW direct path; safepoint pending_unwind branch is internal-only, behavioral correctness observable |
| `dormant_attach_tag_then_start.chk` | T39+T29 | blocked | T39 host-driver not implemented |
| `dormant_basic.chk` | T39+T20 | blocked | T39 host-driver not implemented |
| `dormant_pool_recycle.chk` | M5/v1.x | deferred: v1.x | Dormant pool recycle semantics post-v1.0 |
| `fifo_yield_order.chk` | M5 | blocked | forked strands run to completion before yielding (compile-time strand budget); no observable yield boundary to assert FIFO requeue order |
| `firing_order_registration.chk` | M5 | blocked | chk-runner urbi_step driver path not implemented (T39) |
| `flat_fifo_basic.chk` | T39+T20 | blocked | T39 host-driver not implemented |
| `gc_slice_at_safepoint.chk` | M4 | **active** | GC cells allocatable from REPL; activated with while-loop |
| `groups.chk` | M6 | deferred: v1.x | Group stdlib type not in v1.0 scope |
| `jobs-destruction.chk` | M4 | blocked | Requires Job.jobs introspection (not implemented) |
| `long_pipe_chain_yields.chk` | M5 | blocked | chk-runner urbi_step driver path not implemented (T39) |
| `multiple-visit.chk` | M5 | blocked | Requires `&` separator runtime in chk driver (hangs on `&` at top level) |
| `priorities.chk` | v1.x | deferred: v1.x | URBI_SCHED_RT real-time scheduler is post-v1.0 scope |
| `quiescent_clean.chk` | T39 | **active** | Activated v0.10.14: chk-host-driver `## host: step` on a fresh VM with no live work returns URBI_STEP_QUIESCENT |
| `quiescent_with_sleep_q.chk` | T39 | blocked | needs `## host: advance-clock` (mock-clock directive not implemented) to observe a sleep-queue wakeup |
| `safepoint_backward_branch.chk` | T39 | **active** | Activated v0.10.14: 5-iteration counting loop; safepoint fires on each backward branch (budget-decrement is internal); behavioral result is observable |
| `safepoint_call_return.chk` | T39 | blocked | instruction_budget_remaining has no public read path; per-call decrement unassertable |
| `stack-exhausted.chk` | M4 | blocked | Requires Exception.Scheduling type hierarchy + isA() (not implemented) |
| `tag_stop_mid_pipe.chk` | M4 | blocked | Candidate for activation followup — String + concat shipped v0.10.8; verify pipe-safepoint delivery body shape end-to-end |
| `wait_event_basic.chk` | M5 | **active** | waituntil(e?) works; activated with Realm.fired counter replacing echo |
| `wait_sleep_basic.chk` | T43+T39 | blocked | T39 tunables/clock-tick control not implemented |
| `wake_after_currently_running.chk` | M5 | blocked | chk-runner urbi_step driver path not implemented (T39) |

### semaphore/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `basic.chk` | M6 | deferred: v1.x | Semaphore stdlib type not in v1.0 scope |
| `bug.chk` | M6 | deferred: v1.x | Semaphore stdlib type not in v1.0 scope |
| `critical-exception.chk` | M6 | deferred: v1.x | Semaphore stdlib type not in v1.0 scope |
| `critical.chk` | M6 | deferred: v1.x | Semaphore stdlib type not in v1.0 scope |

### separator/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `andexp-pipeexp.chk` | M5 | blocked | `&` separator hangs chk driver; top-level `&` needs urbi_step driver (T39) |
| `atomic.chk` | M5 | blocked | Requires System.cycle + CallMessage + `&` runtime (CallMessage dropped) |
| `barrier.chk` | M6 | deferred: v1.x | Barrier stdlib type not in v1.0 scope |
| `detach-error.chk` | M4 | blocked | detach() builtin not implemented |
| `detach-many.chk` | M4 | blocked | detach() builtin not implemented |
| `disown.chk` | M4 | blocked | disown() / detach() / Job.current.tags not implemented |
| `inplace-atomic.chk` | M4 | blocked | Requires cout/Channel I/O (not implemented) |
| `non-interruptible.chk` | M6 | deferred: v1.x | nonInterruptible keyword not in v1.0 scope |
| `noop-andexp.chk` | M5 | blocked | `&` separator hangs chk driver (T39) |

### tag/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `ambient_inherit_separator.chk` | T38 | blocked | Requires `&` separator chk driver (T39) |
| `begin-end.chk` | M4 | blocked | mytag.begin:/mytag.end: notation not implemented |
| `block-propagation.chk` | T39 | blocked | Needs `&`-separator chk-driver (v0.10.9-B binding resolved v0.10.15; tag.block()/unblock() shipped v0.10.9) |
| `block.chk` | T39 | blocked | Needs T39 parallel-`,` chk-driver + `loop {}` (v0.10.9-B binding resolved v0.10.15; tag.block()/unblock() shipped v0.10.9) |
| `blocked.chk` | M4 | blocked | tag.block() + at() watcher needs tag.u child-slot (not implemented) |
| `connection.chk` | M4 | blocked | Job.current introspection + connectionTag builtin not implemented |
| `enter-leave.chk` | M5 | deferred: v1.x | Requires at(tag.enter?)/at(tag.leave?) events (T55, not implemented) |
| `freeze.chk` | M5 | deferred: v1.x | tag.freeze()/unfreeze() methods not implemented; SUSPENDED strand state |
| `freezeif.chk` | M5 | deferred: v1.x | freezeif keyword not in v1.0 scope |
| `hierarchical.chk` | M5 | blocked | at sync watcher not implemented + tag.freeze not implemented |
| `implicit.chk` | M4 | blocked | Implicit tag creation via Lobby scope lookup not implemented |
| `nested_scope_unwind.chk` | M5 | blocked | Requires `&` separator chk driver (T39) |
| `scope-tag.chk` | M5 | blocked | scopeTag builtin not implemented |
| `scope-tag2.chk` | M5 | blocked | scopeTag builtin not implemented |
| `start-and-stop.chk` | M4 | blocked | `&` separator hangs chk driver (T39) |
| `state.chk` | M4 | blocked | tag.freeze()/block()/frozen/blocked property not implemented |
| `stop-depth.chk` | M4 | blocked | Requires `,` concurrent stop; complex concurrent fixture |
| `stop.chk` | M4 | blocked | `&` separator hangs chk driver (T39) |
| `stopif.chk` | M5 | deferred: v1.x | stopif keyword not in v1.0 scope |
| `stopif2.chk` | M5 | deferred: v1.x | stopif keyword not in v1.0 scope |
| `tag_stop_cascades.chk` | M5 | deferred: v1.x | Requires onleave clause (PARSE-033, deferred-v1.x) |
| `tag_stop_value_opaque.chk` | M5 | deferred: v1.x | Requires onleave clause (PARSE-033, deferred-v1.x) |
| `unscoping.chk` | M5 | blocked | `&` separator hangs chk driver (T39) + complex cooperative semantics |
| `valued-block.chk` | M4 | blocked | tag.block(val) not implemented |
| `valued-stop.chk` | M4 | blocked | tag.stop(val) + `,` concurrent fixture; needs chk driver stepping |

### temporal/

| Fixture | Old label | New bucket | Notes |
|---------|-----------|------------|-------|
| `sleep_tag_stop.chk` | W4-merge-followup | blocked | Cooperative timing: sleep+tag-stop parallel dispatch requires T39 chk-runner multi-step |

---

## Bucket summary

> Numbers track `grep -rln '^# blocked:' tests/chk/` (blocked) and
> `grep -rln '^# deferred:' tests/chk/` (deferred). Updated each time
> fixtures activate or deferred decisions are locked.

### Post-v0.10.14 W3b (current)

> Counts verified against `grep -rln '^# blocked:' tests/chk/` etc.  The prior
> "Post-v0.10.14" entry recorded blocked=65; the true count at that point was
> 67 (the figure was stale).  W3b drops 3 → 64.

| Bucket | Count |
|--------|-------|
| deferred: v1.x | 24 |
| dropped | 3 |
| blocked (open v1.0-rc work items) | 64 |

The **3 newly active fixtures in v0.10.14 W3b** (chk-host-driver):

1. `chunk_lifecycle/realm_isolation.chk`
2. `chunk_lifecycle/realm_global_default.chk`
3. `scheduler/quiescent_clean.chk`

These previously passed *vacuously* (comment-only stubs, empty→empty diff).
They now carry live `## host:` directives driven by `chk-host-driver` and
were confirmed to FAIL against a wrong expectation before being pinned.

### Post-v0.10.14 (T39 MVP audit)

| Bucket | Count |
|--------|-------|
| deferred: v1.x | 24 |
| dropped | 3 |
| blocked (open v1.0-rc work items) | 67 |

The **2 newly active fixtures in v0.10.14** (T39 MVP audit):

1. `scheduler/safepoint_backward_branch.chk`
2. `scheduler/dispatch_safepoint_pending_unwind.chk`

### Post-v0.10.7 (historical baseline)

| Bucket | Count |
|--------|-------|
| active (newly activated in W7) | 6 |
| deferred: v1.x | 25 |
| dropped | 3 |
| blocked (open v1.0-rc work items) | 73 |
| **Total** | **107** |

The **6 newly active (v0.10.7 W7)** fixtures were:

1. `chunk_lifecycle/repl_session_persistence.chk`
2. `chunk_lifecycle/script_at_persists.chk`
3. `control_transfer/tag_stop_skips_catch.chk`
4. `gc/gc_slice_at_safepoint.chk`
5. `scheduler/gc_slice_at_safepoint.chk`
6. `scheduler/wait_event_basic.chk`

The dominant open work items for blocked fixtures are:

- **T39** (chk-runner `## tunables:` / `## host:` extension): the `## host:` C host-driver landed in v0.10.14 W3b (`tests/integration/chk_host_driver.c`) using only the public embedding API — it implements `realm`/`run`/`step` and activated the 3 multi-realm + quiescence fixtures above.  The remaining T39 residual (`v0.10.7-G`) is the per-strand/loader **runtime budget knob**: `URBI_STRAND_BUDGET_MAX` is compile-time only, and the public API drives strands to a stable observable (quiescence/park), so mid-flight RUNNING / soft-yield / budget-decrement states stay unobservable without a new public symbol.  Also unimplemented: `## host: advance-clock` (mock clock) and `## expect-host-call:`.
- **tag.freeze()/block()** methods: blocks ~8 tag fixtures
- **onleave clause** (PARSE-033, deferred-v1.x): blocks 5 control_transfer + tag fixtures
- **`&` separator top-level chk driver** (T39): blocks ~10 fixtures
- **isA() method dispatch**: blocks exceptions + scheduler/stack-exhausted

---

## How to activate a blocked fixture

1. Open the work item referenced in `//# blocked: <item>`.
2. Implement the missing feature / fix the bug.
3. Write the activated urbiscript code in the fixture (using the commented-out source as a guide).
4. Write the expected output lines.
5. Run `URBI_BUILD_PRESET=default tests/integration/run_chk.sh build/host/urbi <fixture>` to confirm PASS.
6. Change the header from `//# blocked:` + `//# matrix-row:` to just (no header, active by default).
7. Update `docs/language-compatibility-matrix.md` denominator.
8. Update `docs/release/release-readiness.md` active-fixture count.
