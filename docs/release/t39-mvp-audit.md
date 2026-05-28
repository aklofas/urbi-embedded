# T39 chk-runner MVP Audit

> Target: v0.10.14-prerc-infra  
> Scope: per-fixture classification of all T39-blocked fixtures; finalized MVP
> directive set; activation list with residual-blocker narrowing.

---

## Background

T39 was filed to track "chk-runner host-driver / tunables extension" — a
general-purpose mechanism to let fixtures configure runtime behavior or inject
C-level host operations during a REPL run.  The original framing was broad
(mock clock, multi-step drive, realm creation, budget tuning).  This audit
scopes the v0.10.14 MVP to the smallest set of directives that unlocks actual
fixture activations.

### Key constraints discovered

1. **`URBI_STRAND_BUDGET_MAX` is compile-time only.**  The per-strand
   instruction budget is assigned from `usched_cooperative.c`
   `sched_strand_init` using the compile-time `#ifndef URBI_STRAND_BUDGET_MAX`
   macro.  There is no runtime environment variable read-path.  Any fixture
   claiming `## tunables: URBI_STRAND_BUDGET_MAX=N` requires either a
   purpose-built binary variant or a `tools/urbi.c` + scheduler change to
   read `URBI_STRAND_BUDGET_MAX` from the environment at startup.  Both are
   out of MVP scope.

2. **`## host:` directives require a C host-driver loop.**  The chk runner
   (`run_chk.sh`) is POSIX sh; it drives the binary via a single `urbi -i <
   inputs.txt` invocation.  Injecting multi-step VM calls, realm creation, or
   clock advances between script lines requires either (a) a purpose-built test
   binary that speaks a control protocol, or (b) a bespoke stdin multiplexer.
   Both are out of MVP scope.

3. **`URBI_BUILD_PRESET` is runner-internal only.**  The env var is consumed
   by `run_chk.sh` to implement the SKIP gate for `# tunables:` headers.  The
   binary (`build/host/urbi`) does not read it.

4. **`&` top-level separator needs the realm-managed scheduler path.**  The
   REPL (`urbi_vm_run`) passes `realm == NULL`; `OP_FORK_JOIN` and
   `OP_FORK_DETACH` return `TypeError` on that path.  Enabling `&`/`,` in
   chk fixtures requires the binary to instantiate a `URealm` and drive via
   `urbi_step`.

---

## Finalized MVP directive set

**Zero new directives.**  The two activatable fixtures (see below) need no
new runner directives — their behavioral outcomes are testable with the
existing single-pass `urbi -i` REPL path.  The `# tunables:` skip-gate and
`##`-prefixed comment-pass-through already handle everything they need.

The proposed `## tunables:` (double-hash, configure-and-run) and `## host:`
(C hook injection) directives are NOT implemented in this MVP.  The full
specification of those directives is deferred; see `v0.10.7-G` in
`docs/urbi-embedded-design-risks.md`.

---

## Per-fixture classification

### Candidate seed list (from plan)

| Fixture | Host power claimed | Verdict |
|---------|-------------------|---------|
| `scheduler/realm_global_default.chk` | multi-realm C host-driver | **stays blocked** — needs `urbi_run_chunk(vm, NULL, ...)` C setup |
| `scheduler/realm_isolation.chk` | multi-realm C host-driver | **stays blocked** — needs two `URealm*` instances |
| `scheduler/module_load_isolation.chk` | multi-realm + M6 stdlib | **stays blocked** — needs import-table + dual-realm host setup |
| `scheduler/quiescent_with_sleep_q.chk` | `host: step` + `host: advance-clock` | **stays blocked** — needs `sched_quiescent()` C observation |
| `scheduler/budget_step_exhausted.chk` | `host: rc = urbi_step(vm, 1)` + RC check | **stays blocked** — needs `urbi_step` return-code assertion |
| `reactive/event_fifo_determinism.chk` | `100x3` multi-preset run + T59 globals | **stays blocked** — T59 globals + multi-preset mode both unshipped |
| `reactive/cross-spec/det/det_slot_change_burst.chk` | `100x3` multi-preset run + T69 globals | **stays blocked** — T69 `Object.new()` globals + multi-preset mode |

Note: `realm_global_default.chk`, `realm_isolation.chk`, `module_load_isolation.chk`
are in `tests/chk/chunk_lifecycle/`, not `tests/chk/scheduler/`.

### Extended grep set — all T39-tagged fixtures

The extended grep (`grep -rlE 'T39|chk-driver|host:|mock clock|determinism|tunables' tests/chk/`) found 52 fixtures.  Classification below.

#### Can activate in v0.10.14 (no new runner directive needed)

| Fixture | Claimed blocker | Actual situation | Activation path |
|---------|----------------|-----------------|----------------|
| `scheduler/safepoint_backward_branch.chk` | `## tunables: URBI_STRAND_BUDGET_MAX=2` | The proposed source `var i = 0; while (i < 5) { i = i + 1 }; i` produces observable output `5` without a tiny budget.  The safepoint fires on every backward branch regardless; the budget-assertion is unobservable from pure urbiscript.  The behavioral correctness is testable now. | Write live source; expected `[...] 5`; remove `blocked:` header |
| `scheduler/dispatch_safepoint_pending_unwind.chk` | `## tunables: URBI_STRAND_BUDGET_MAX=2` | The proposed source (`try { while(i<1){i=i+1; throw 99} } catch(e){echo(e)}`) works without tiny budget.  The `echo` call fails but the Realm-capture pattern works.  The `throw` inside a loop body goes through `OP_THROW` → `urbi_unwind()` directly; the catch at the outer scope absorbs it.  No tiny budget needed to observe the catch result. | Write live source with `Realm.caught` pattern; expected output includes `nil` (try-catch block value) and `99` (Realm.caught) |

#### Stays blocked — `## tunables: URBI_STRAND_BUDGET_MAX=N` needed

These all need per-strand budget observable from the OUTSIDE (multi-turn `urbi_step` calls or multi-output runs that require the strand to ACTUALLY yield and resume):

- `scheduler/budget_soft_yield.chk` — expected output implies two separate results from a single strand yielding and resuming; needs `## host:` multi-step drive
- `scheduler/budget_step_exhausted.chk` — needs `urbi_step` return code
- `scheduler/quiescent_clean.chk` — needs `urbi_step` return code
- `scheduler/safepoint_call_return.chk` — needs budget observation mid-call
- `scheduler/dispatch_safepoint_pending_unwind.chk` — *see "can activate" above; the comment is misleading but the DIRECT behavioral outcome is testable*

#### Stays blocked — `## host: step` / multi-strand drive needed

- `scheduler/fifo_yield_order.chk`
- `scheduler/firing_order_registration.chk`
- `scheduler/long_pipe_chain_yields.chk`
- `scheduler/wake_after_currently_running.chk`
- `scheduler/cross_strand_cancel.chk`
- `scheduler/dormant_basic.chk`
- `scheduler/dormant_attach_tag_then_start.chk`
- `scheduler/flat_fifo_basic.chk`
- `scheduler/jobs-destruction.chk`
- `scheduler/multiple-visit.chk`

#### Stays blocked — `## host: advance-clock` needed

- `scheduler/wait_sleep_basic.chk`
- `scheduler/quiescent_with_sleep_q.chk`
- `temporal/sleep_tag_stop.chk`

#### Stays blocked — multi-realm C host-driver

- `chunk_lifecycle/realm_global_default.chk`
- `chunk_lifecycle/realm_isolation.chk`
- `chunk_lifecycle/module_load_isolation.chk`
- `chunk_lifecycle/realm_destroy_cancels_watchers.chk`

#### Stays blocked — `## expect-host-call:` needed

- `gc/finalizer_native.chk`

#### Stays blocked — `&` top-level separator requires urbi_step + realm

- `separator/andexp-pipeexp.chk`
- `separator/noop-andexp.chk`
- `tag/ambient_inherit_separator.chk`
- `tag/nested_scope_unwind.chk`
- `tag/start-and-stop.chk`
- `tag/stop.chk`
- `tag/unscoping.chk`

#### Stays blocked — T39 cited but primary blocker is a DIFFERENT unshipped feature

These fixtures mention T39 but their actual primary blocker is another feature:

| Fixture | Primary blocker | T39 role |
|---------|----------------|---------|
| `scheduler/cycles.chk` | `System.cycle` counter not implemented | secondary |
| `gc/reset-stats.chk` | `stats()` / `resetStats()` stdlib not implemented | secondary |
| `reactive/event_fifo_determinism.chk` | T59 globals + `Event.new()` not accessible | secondary (multi-preset mode) |
| `reactive/slot_change_fifo_determinism.chk` | T69 `Object.new()` globals not accessible | secondary (multi-preset mode) |
| `reactive/slot_change_reentrancy_determinism.chk` | T69 globals not accessible | secondary (multi-preset mode) |
| `reactive/cross-spec/det/det_slot_change_burst.chk` | T69 + multi-preset | secondary |
| `closure/scopes.chk` | T39 chk driver (multi-line error line matching) | sole blocker; consider error-line normalization path |
| `objects/class_new_clone.chk` | **already active** — no blocked header | n/a |
| `tag/scope_tag_basic.chk` | **already active** — no blocked header | n/a |
| `tag/valued-stop.chk` | `,` separator + T39 chk driver | separator |
| `tag/valued-block.chk` | `,` separator + v0.10.9-B | separator |
| `tag/freeze.chk` | `,` separator + T39 chk driver | separator |
| `tag/hierarchical.chk` | `at sync` + `tag.freeze` + `,` | separator |
| `tag/block.chk` | `tag.block()/unblock()` not implemented | secondary |
| `tag/connection.chk` | `&` separator + T39 chk driver | separator |
| `separator/detach-error.chk` | T39 chk driver + script error format | primary |
| `separator/detach-many.chk` | T39 chk driver (wall-clock sleep) | primary |
| `separator/disown.chk` | T39 chk driver (tag scope timing) | primary |
| `separator/inplace-atomic.chk` | T39 chk driver (interleaved cout output) | primary |
| `tag/scope-tag.chk` | v1.x OGET bridge + T39 (`,` separator) | secondary |
| `tag/scope-tag2.chk` | v1.x OGET bridge + T39 (`,` separator) | secondary |

#### Vacuous passes (comment-only, 0 input lines)

- `temporal/sleep_tag_stop.chk` — all comments, no input lines; passes vacuously as empty→empty diff.  Remains `blocked:` in header; this is the known vacuous-fixture phenomenon (Cat. E retrospective).

---

## v0.10.7-G design-risks closure assessment

`v0.10.7-G` covers "T39 chk-runner tunables/host-driver extension."  The MVP
ships **zero new runner directives** (the activation targets need none).  The
`v0.10.7-G` entry therefore remains OPEN.  The controller should update the
register entry to narrow the residual: the `## tunables: URBI_STRAND_BUDGET_MAX=N`
and `## host:` directive forms are the open portion; the two activatable fixtures
activate without any new directive.

---

## Activated fixtures summary

| # | Fixture | Notes |
|---|---------|-------|
| 1 | `scheduler/safepoint_backward_branch.chk` | Loop correctness; safepoint fires on backward branch (budget-assertion is internal) |
| 2 | `scheduler/dispatch_safepoint_pending_unwind.chk` | try/catch/throw-in-loop via OP_THROW direct path; catch absorbs exception |

**Net activation: +2.  Active count: 290 → 292.**

---

## Residual blocker narrowing (for `blocked:` header updates)

For fixtures that stay blocked, narrow the header from the generic "T39 not
implemented" to the specific residual:

- `## tunables: URBI_STRAND_BUDGET_MAX=N` group → `blocked: runtime strand-budget tunable not implemented (URBI_STRAND_BUDGET_MAX is compile-time only)`
- `## host: step` group → `blocked: T39 chk-runner urbi_step multi-turn driver not implemented`
- `## host: advance-clock` group → `blocked: T39 chk-runner advance-clock directive not implemented`
- multi-realm group → `blocked: T39 chk-runner multi-realm C host-driver not implemented`
- `## expect-host-call:` group → `blocked: T39 chk-runner expect-host-call directive not implemented`
- `&` top-level separator group → `blocked: top-level & separator requires realm-managed urbi_step path (T39 chk-runner)`

---

## W3b addendum — chk-host-driver shipped (v0.10.14)

The MVP above concluded "zero new directives."  W3b revisits that: a bounded
C **test host-driver** (`tests/integration/chk_host_driver.c`) was built using
**only the existing public embedding API** — no new public symbol, ABI stays
PATCH.  The original audit conflated "the chk *runner* (POSIX sh) cannot drive
this" with "no host power exists."  The public API (`urbi_repl_eval`,
`urbi_realm_create`, `urbi_step`) already exposes enough for a subset of the
blocked fixtures; they just needed a small C binary the sh runner routes to.

### Final driver directive set (implemented)

| Directive | Effect | Backing public API |
|-----------|--------|--------------------|
| `## host: realm <name>` | create-or-select a named realm; `default` aliases the NULL/global realm | `urbi_realm_create` |
| `## host: run <source>` | compile + run one source line under the current realm; emit the framed result | `urbi_repl_eval` |
| `## host: step <budget>` | `urbi_step(vm, budget)`; emit `step: <STATE>` | `urbi_step` |

The driver frames each observable as `[00000000] <text>` so `run_chk.sh`'s
existing `[...]`-prefix normalization diffs it identically to the REPL path.
`run_chk.sh` detects `## host:` directives and routes only those fixtures to
the driver (path derived from the urbi-binary dir, so sanitizer variants pick
up their own instrumented driver); all other fixtures keep the REPL path.

`## host: advance-clock` was **not** implemented — no activation target needed
it (the sleep/clock fixtures stay blocked, see below).

### Activation outcome — net +3 genuine (was vacuous)

| Fixture | Outcome | Observable |
|---------|---------|-----------|
| `chunk_lifecycle/realm_isolation.chk` | **ACTIVATED** | two named realms bind `x` independently; reads stay isolated |
| `chunk_lifecycle/realm_global_default.chk` | **ACTIVATED** | two `run` chunks under the default (NULL/global) realm share bindings |
| `scheduler/quiescent_clean.chk` | **ACTIVATED** | fresh VM with no live work → `URBI_STEP_QUIESCENT` |

All three previously passed *vacuously* (comment-only → empty diff).  Each was
confirmed to FAIL against a deliberately-wrong expectation before its correct
expected lines were pinned (LIVE-FIXTURE rule).

### Stays blocked — narrowed residual (still v0.10.7-G)

| Fixture | Narrowed residual |
|---------|-------------------|
| `scheduler/budget_step_exhausted.chk` | mid-flight `URBI_STEP_RUNNING` not observable — `urbi_run_chunk`/`urbi_repl_eval` drive the loader to quiescence/park internally |
| `scheduler/budget_soft_yield.chk` | per-strand soft yield needs a runtime budget knob; `URBI_STRAND_BUDGET_MAX` is compile-time only |
| `scheduler/safepoint_call_return.chk` | `instruction_budget_remaining` has no public read path; per-call decrement unassertable |
| `scheduler/fifo_yield_order.chk` | forked strands run to completion before yielding (compile-time budget); no observable yield boundary |
| `chunk_lifecycle/module_load_isolation.chk` | no `import` keyword / import-table surface — module-into-realm isolation unexpressible (import-table gap, NOT the multi-realm gap, which is now closed) |

The common thread for the four scheduler residuals: the public embedding API
intentionally drives strands to a *stable* observable point (quiescence or a
park state).  Catching a strand mid-execution to observe a budget-exhausted
RUNNING/soft-yield state needs a runtime per-strand budget knob, which would
be a **new public symbol** — out of scope for this PATCH tag.  `v0.10.7-G`
therefore stays OPEN with this narrowed residual.
