# Changelog

## Unreleased — Wave 3 of v0.5.x cleanup ramp (v0.5.5 candidate)

Internal symbol + public C API naming pass per CONTRIBUTING.md §3.2 conventions.
Last opportunity to settle public API before M6 grows the surface.

### Changed (naming hygiene)

- (T7) Public VM lifecycle promoted: `uvm_init` → `urbi_vm_init`,
  `uvm_destroy` → `urbi_vm_destroy`, `uvm_run` → `urbi_vm_run`.
- (T8) `URBI_ERR_OUT_OF_MEMORY` (-10) collapsed into `URBI_ERR_OOM` (-3);
  native-code OOM no longer reports a distinct numeric.
- (T9) `URBIAtomFamilyTag` retired; public surface uses `URBIAtomFamily`
  directly. `URBI_ATOM_*_F` enumerators drop the `_F` suffix.
- (T10) `URBI_WATCHDOG_*` macros promoted to `UWatchdogMode` enum.
- (T11) `URBI_SCHED_CLASS_DEADLINE` → `URBI_SCHED_DEADLINE` (drop `_CLASS_`
  infix).
- (T15-T17) New `include/urbi/types.h` hosts UValue / UExecStatus / UErrCode
  / UVMError / UVMAllocFn + opaque struct fwd-decls; `include/urbi/urbi.h`
  no longer pulls in `src/sched/ustrand.h`.
- (T18) `URBI_ASSERT_NOT_ISR(vm)` macro now calls `urbi_in_isr(vm)`;
  embedders no longer need a complete `struct UVM` definition to use the
  macro.

### Internal

- (T6) Mass uppercase-literal-suffix sweep: `1u`/`0u` → `1U`/`0U`
  (~650 sites; clang-tidy `readability-uppercase-literal-suffix`).
- (T20-T31) Per-subsystem internal symbol renames per spec §3.2; ~70 audit
  IDs closed.
- (T32) `misc-include-cleaner` direct-include sweep (~241 sites) — every
  TU now declares its own includes rather than relying on transitive
  pulls.
- (T33) Const-correctness sweep (28 sites flagged by cppcheck
  `constParameterPointer` + `constVariablePointer`).

### Added

- `urbi-embedded/CONTRIBUTING.md` — naming + layout + commit conventions
  (will be finalized in Wave 6).
- `runtime/umacros.h` gains `urbi_memeq` static-inline helper retiring
  file-local `lex_memeq` + `module_memcmp` lookalikes.

### Fixed

- (T12) `urbi_step` declaration/definition argument-name drift — public
  header and impl now use `budget_instructions` consistently.
- (T13) 8 sites in uunwind public APIs had `strand` (decl) vs `s` (def)
  drift — settled to `strand`.
- (T14) `urbi_run_chunk` no longer collapses every non-OOM `uvm_run`
  error into `URBI_ERR_STRAND_FATAL`; the underlying `UErrCode` now
  propagates through. The `realm` argument is no longer silently
  discarded.

### Verification

- Bytecode byte-identical against `tests/golden/v0.5.3-bytecode-hashes.txt`
  (148 fixture hashes; the v0.5.3 baseline is the operative gate, not a
  fresh capture — no codegen changes in this wave).
- All `make releasetest` gates green: host + ASan + UBSan + valgrind-fast
  + tidy + docs-check + coverage 85% + GC stress + URBI_GC_NONE smoke +
  3-preset × 100-run determinism + cross-arm + cross-riscv + LOC-cap.

## v0.5.4-decompose — 2026-05-06

Wave 2 of the v0.5.x pre-M6 cleanup ramp: decomposes the four
translation units that exceeded the 1000-LOC soft cap into focused
per-concern files of ≤600 LOC; extracts the 14-site duplicated
volatile-byte-zero loop into a shared `urbi_zero` helper; lands
audit-driven cross-cutting refactors across all subsystems.  Bytecode
output is byte-identical to v0.5.3-layout.

### File decompositions

- **`uemit.c` 3563 LOC → 9 files** (EMIT-045): `uemit.c` retains top-level
  dispatch; `uemit_funcstate.c`, `uemit_expr.c`, `uemit_stmt.c`,
  `uemit_react.c`, `uemit_unwind.c`, `uemit_disasm.c`,
  `uemit_serialize.c`, `uemit_diag.c` extract per-concern logic.
  `uemit_internal.h` provides inter-TU linkage.
- **`uvm.c` 2314 LOC → 6 files** (VM decomposition): `uvm_init.c`,
  `uvm_diag.c`, `uvm_closure.c`, `uvm_run.c`, plus header-inlined
  `uvm_arith.h`.  `uvm_internal.h` provides inter-TU linkage.
- **`uparse.c` 1698 LOC → 6 files** (PARSE-021): `uparse_top.c`,
  `uparse_separators.c`, `uparse_stmt.c`, `uparse_react.c`,
  `uparse_expr.c`.  `uparse_internal.h` provides inter-TU linkage.
- **`uobject.c` 1157 LOC → 4 files** (OBJ-045): `uobject_proto.c`,
  `uobject_lookup.c`, `uobject_slot.c`.  `uobject_internal.h`
  provides inter-TU linkage.

### New CI gate

- `make test-loc-cap`: scans all `src/` translation units and fails on
  any file exceeding 1000 LOC.  One documented exception: `uvm.c`
  (dispatch loop, 1336 LOC — see `CONTRIBUTING.md`).

### Audit findings closed

**Decomposition** (EMIT-045, VM decomposition, PARSE-021, OBJ-045)

**Theme 4 — volatile-byte-zero dedup**: `urbi_zero` helper extracted;
14 open-coded volatile-byte-zero loops across 9 files swept to the
shared helper (FOUND-030).

**Lex** (LEX-019, LEX-020, LEX-024, LEX-025): duration-suffix dispatch
table-driven; single-char punctuation switch table-driven; UToken
init helpers consolidated; digit-accumulator loops unified.

**Watcher** (WATCH-018, WATCH-019, WATCH-024): header-init + pool-drain
deduplication; thin `run_closure_on_scratch_frame_with_result` wrapper
retired; pool_destroy loops consolidated.

**Event / EmitR** (EMITR-006, EMITR-008): waiter-wake loop deduped;
ring weak-ref contract clarified; payload coercion / pad-zero / proto
OOM propagation deduplicated (EVENT-019, EVENT-027).

**Realm** (REALM-020, REALM-021, REALM-022, REALM-028, REALM-029):
cleanup ladder consolidated; strlen dedup; dead resolvers removed;
snapshot-next teardown path fixed; zero-helper sweep applied.

**Chunk / strand** (CHSTR-020, CHSTR-021, CHSTR-022, CHSTR-029,
CHSTR-031, CHSTR-044): strncpy dedup; arm-init helper extracted;
REPL drain + error format consolidated; cleanup-stack OOM propagated;
destroy / counter / regstack lifecycle centralized.

**Module** (MOD-031): `umodule_deserialize` split into per-section
helpers.  MOD-027 investigated and not applicable (include order is
load-bearing).

**GC** (GC-027, GC-028): duplicate gray-drain loop collapsed; gc_byte
color-update pattern deduplicated.

**Foundation** (FOUND-020, FOUND-031): `utype.c` folded into
`uvalue.c`; `uvalue_le` 4-way dispatch simplified.

**VM** (VM-008): IC-resolve preamble extracted into shared helper
`ic_resolve_proto_inst`.

**Emit core** (EMIT-033, EMIT-035): AST_TRY three near-duplicate paths
collapsed to `emit_try_frame`; `uemit_disassemble` table-driven.

**Object** (OBJ-031, OBJ-032, OBJ-034): atom switch / walker /
module-instance init deduplicated.

**Parse** (PARSE-013, PARSE-014, PARSE-022, PARSE-023, PARSE-024,
PARSE-025): postfix-emit duplicate collapsed; IDENT-lookahead Pratt
duplicate removed; `parse_at` split into per-form helpers;
`reject_bare_function_forms` extracted; `parse_statement_or_expr`
decomposed; 4× arena-array doubling pattern collapsed.

**Tag / changed / event** (TAGCH-005): OOM-throw block collapsed; dead
placeholders removed.

**Cross-compile fix**: `ulex.c` duration-suffix table used `memcmp`
from `<string.h>`; replaced with local `lex_memeq` so the file
compiles under `-ffreestanding` (cross-arm / cross-riscv targets).

### Carried forward / deferred

- OBJ-041 (`urbi_object_install_property` spurious topology_gen bump) —
  wave-5-fixes; correctness impact requires shape-mutation audit.
- WATCH-023 (`urbi_watcher_install_internal` dead seam) — wave-6-cleanup.
- FOUND-032 (`pop_call_frame` cleanup-TU coupling) — wave-5-fixes.
- WATCH-017 (IC table walk hand-rolled): investigated; fixing requires
  proto_inst membership guarantee not yet established — defer to M6.
- EVENT-025 (subscriber-walk snapshot-next enforcement): design work
  needed; defer to M6.
- MOD-027 (umodule.h includes uframe.h mid-typedef): not applicable —
  the include is load-bearing.
- FOUND-029 (vm_alloc helper duplication): three sites differ in subtle
  ways; consolidation deferred to wave-5-fixes with test coverage.

### Verification

- 1138 unit cases / 6382 checks / 0 failed; 148 `.chk` fixtures
- Bytecode output byte-identical to v0.5.3-layout (all 148 fixtures)
- `make test-loc-cap`: EXEMPT 1336 `src/vm/uvm.c`; no FAILs
- `make releasetest`: all gates green (host + ASan + UBSan + tidy +
  lint + docs-check + coverage + stress + GC-none + cross-arm +
  cross-riscv + valgrind memcheck)

## v0.5.3-layout — 2026-05-06

Wave 1 of the v0.5.x pre-M6 cleanup ramp: pure mechanical layout
reorganization.  Every `.c`/`.h` under `src/` (except `urbi.c`) moves
into a per-subsystem folder; three filename renames; layout-flagged
doc + comment fixes; six `_Static_assert` layout pins; dead-code
removal of the orphaned `UScratchFrame` heap allocation.  No
behavioral change.  Bytecode-byte-identical to v0.5.2 — every
`.chk` fixture passes unchanged.  Binary footprint within 0.1 % of
v0.5.2 across host / arm-cortex-m7 / riscv-rv32imc.

### Layout

- Every source under `src/` (except the entrypoint `urbi.c`) moved
  into a per-subsystem folder.  Top-level `src/` now holds only
  `urbi.c` and subsystem directories.
- New folders: `lex/`, `parse/`, `emit/`, `vm/`, `event/`, `tag/`,
  `changed/`, `module/`, `value/`, `runtime/` — alongside the
  pre-existing `gc/`, `sched/`, `watcher/`, `realm/`, `object/`.
- `ustrand.{c,h}` joins the `sched/` folder (strand is the unit of
  scheduled execution).
- `urealm_globals.{c,h}` moves into `realm/` — closes REALM-012
  (the file was orphaned at `src/` top level while every other
  realm file lived under `src/realm/`).
- `uchunk.c` joins `module/` (chunks become modules at runtime).
- `uast.h` moves into `parse/` (co-located with the parser that
  produces it).
- `umacros.h` moves into `runtime/`; the audit verdict was "earns
  its keep, relocate not retire" (closes API-031, INC-002).

### Renames

- `src/event_native.{c,h}` → `src/event/uevent_native.{c,h}` —
  picks up the project `u` prefix (closes EVENT-012).
- `src/tag_native.{c,h}` → `src/tag/utag_native.{c,h}` (closes
  TAGCH-015).
- `src/object/umoduleinstance.{c,h}` → `src/object/umodule_instance.{c,h}`
  — snake-case for compound filenames (closes OBJ-022).
- File-private field `UStrand::is_uvm_run_transient` →
  `is_transient_strand` — the name no longer embeds the
  implementation function `uvm_run`; docstring rewritten to
  describe both transient strand sources (`uvm_run` and
  `urbi_run_closure_on_scratch`).  Closes CHSTR-023.

Function symbol names inside the renamed files (e.g.,
`event_native_register`, `walk_umoduleinstance`) stay unchanged —
symbol renames belong to wave-3-naming.

### Hygiene

- `_Static_assert` layout pins for `UStrand` (2880 B), `UWatcher`
  (240 B), and `UTag` (56 B) — the three v0.5.x runtime cell types
  that lacked compile-time size pins.  `UEvent` (40 B), `UObject`
  (56 B), `UChangedNode` (32 B) already had asserts.  All six are
  guarded on `__SIZEOF_POINTER__ == 8` so 32-bit cross targets
  build clean.  Closes CHSTR-041.
- Removed dead `UScratchFrame` heap allocation (~280 B / VM saved
  at runtime).  The v0.5.1-cond-unstub patch routed scratch frames
  onto the C stack via `urbi_run_closure_on_scratch`; the heap
  slot on UVM was orphaned.  Removed the struct definition,
  `vm->watcher_scratch_frame` field, init allocation, destroy
  free, and the defensive test
  `watcher_scratch_frame_allocated_at_init`.  The OOM-counter
  test `vm_oom_first_alloc_fails_second_would_succeed` adjusts
  its target from alloc #5 to alloc #4 to match the new init
  sequence.  Closes WATCH-022.
- `is_ident_cont` forward-decl in `src/lex/ulex.c` removed; the
  function reordered above its first caller.  Closes LEX-022.

### Documentation

- `gc/` header docstrings refreshed: `gc_byte` bit allocations
  enumerated bit-by-bit; `UTYPE_HOST_BASE = 64` claimed/reserved
  ID gap documented; `urbi_register_type` host-only contract
  stated; `urbi_gc_slice` per-phase termination documented;
  `urbi_gc_alloc` ATOMIC_FINISH role narrowed to match
  implementation; 1ms pause-time budget cited near
  `urbi_gc_slice`; `UGC_IS_WEAK` reservation note clarified;
  `ugc_none.h` "spec-only at M3" replaced with current
  `URBI_GC_NONE` compile-smoke description; stale path
  references updated to new subsystem prefixes.  Closes GC-014,
  GC-022, GC-026, GC-029, GC-031, GC-032, GC-033, GC-035, GC-036,
  GC-039.
- `runtime/uframe.h` indirect-`UValue`-dependency note rewritten
  to acknowledge the actual `uvalue.h → umodule.h → uframe.h`
  include cycle (the cycle prevents the obvious "just include
  `value/uvalue.h` here" fix).  Closes FOUND-006, FOUND-022.
- Public-header `include/urbi/urbi.h` `#include` paths adjusted
  to new subsystem-prefixed form (`sched/ustrand.h`); docstring
  updated to cite `sched/ustrand.h` and `vm/uvm.h`.
  `URBI_ASSERT_NOT_ISR` docblock now states why the macro lives
  in the public header (embedder-facing assertion surface) and
  acknowledges the `isr_check_fn` internal-field dependency for
  wave-3-naming follow-up.  `umacros.h` docstring clarifies that
  `URBI_ASSERT_NOT_ISR` is in the public header.  Path-only
  mechanical close for API-012, API-018, API-027, INC-003,
  GC-012; the deeper "public should not include internal" cleanup
  is a wave-3-naming carry-forward.

### Build

- Makefile `SRC` discovery extended with wildcards for the 10 new
  subsystem folders.

### Verification

- 1138 unit cases / 6382 checks / 0 failed (was 1139 / 6383 at
  v0.5.2; the −1 case + −1 assertion are the now-removed
  `UScratchFrame` defensive test).
- 148 / 148 `.chk` fixtures pass.
- ASan + UBSan + valgrind-fast + valgrind-deep clean across the
  full suite.
- Coverage 85 % (line); GC stress targets all PASS; GC pause max
  2.7 µs (target 1 ms — 370 × margin); barrier throughput 40 M
  ops/sec; event-emit throughput 11.8 M ops/sec.
- `make tidy` + `make docs-check` clean.
- `URBI_GC_NONE` compile smoke PASS.
- `make test-determinism` (3-preset × 100-run) PASS.
- Cross-arm + cross-riscv builds green.
- Binary footprint deltas vs v0.5.2 baseline:
  - host-x86_64:    `liburbi.a` text 135 973 B → 135 910 B  (−63 B, −0.05 %)
  - arm-cortex-m7:  `liburbi.a` text  55 710 B →  55 650 B  (−60 B, −0.11 %)
  - riscv-rv32imc:  `liburbi.a` text  69 774 B →  69 712 B  (−62 B, −0.09 %)
  All deltas trace to the removed `UScratchFrame` allocation
  paths; well within the cleanup-design §4.2 ±5 % gate.
- Bytecode output byte-identical to v0.5.2 across all 148
  fixtures (no codegen change in this wave).
- `git log --follow` traces every moved file back to its v0.5.2
  history (rename detection threshold satisfied for all 58
  file-rename entries across the 17 file-move/rename commits;
  similarity ≥ 94 %).

### Wave-1 audit IDs closed (28)

API-012, API-018, API-027, API-031, CHSTR-023, CHSTR-041, EVENT-012,
FOUND-006, FOUND-022, GC-012, GC-014, GC-022, GC-026, GC-029, GC-031,
GC-032, GC-033, GC-035, GC-036, GC-039, INC-002, INC-003, INC-004,
INC-005, LEX-022, OBJ-022, REALM-012, TAGCH-015, WATCH-022.

Carries forward to wave-3-naming: the deeper "public header should
not include internal types" hygiene (API-012/018/027 + INC-003 +
GC-012 in their structural form, beyond the path-fix mechanical
close landed here).

See `docs/superpowers/specs/2026-05-05-v0.5.x-cleanup-audit-findings.md`
for full audit context.

---

## v0.5.2-scratch-frame-followup — 2026-05-05

Closes the four scratch-frame stub sites left hook-stubbed at
`v0.5.1-cond-unstub` (AT_SYNC body inline, falling-edge onleave inline,
drain-time onleave during tag-stop cascade, event sync-emit subscriber
body), plus a tidy baseline fix at `src/uchanged_emit.c:108` and a
bundled emit-time bug fix surfaced during execution.  All four wires
route through the v0.5.1 helper `urbi_run_closure_on_scratch` (or a
new payload variant added here for the event sync-emit body's
R[0]-payload contract).  Activates `at_onleave.chk` as a live
conformance fixture using the integer-counter pattern (M5 lacks
string-literal lex; deferred to backlog).

### Added

- `urbi_run_closure_on_scratch_with_payload` variant
  (`src/watcher/uwatcher.h`, `src/watcher/uwatcher_scratch.c`) —
  same shape as `urbi_run_closure_on_scratch` but writes a `payload`
  UValue into the closure's R[0] before dispatch.  Used by AT_EVENT_SYNC
  subscriber bodies to receive the emit payload as their first argument.
  Both public functions share a static `run_on_scratch_core` so the
  no-payload path is a thin shim.
- 4 new end-to-end unit tests exercising the wired paths through the
  production install + dispatch path with no test hooks:
  `tests/unit/test_at_sync_scripted.c`,
  `tests/unit/test_tag_stop_onleave_scripted.c`,
  `tests/unit/test_event_sync_emit_scripted.c`, plus 2 cases added to
  `tests/unit/test_uwatcher_scratch.c` for the payload variant.
- 3 regression tests for the AST_AT_EVENT emit register-allocation
  desync (`tests/unit/test_parse_at_event.c`,
  `tests/unit/test_emit_at_slot_change.c`) — each disassembles the
  install opcode and asserts `event_reg != body_reg`.
- Activated `tests/chk/reactive/at/at_onleave.chk` as a live
  conformance fixture covering the falling-edge onleave path
  (`URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE` guard).

### Fixed

- **AT_SYNC body inline dispatch** (`invoke_body_inline`,
  `src/watcher/uwatcher_eval.c`): replaced the M5 stub fall-through
  with a call to `urbi_run_closure_on_scratch`.  Throws are
  suppressed (eval pass cannot propagate per spec §6.4).  Test hook
  short-circuit preserved.
- **Falling-edge onleave dispatch** (`invoke_onleave_inline`,
  `src/watcher/uwatcher_eval.c`): same swap; onleave fires through
  real bytecode dispatch on falling edge after a prior body fire.
- **Drain-time onleave dispatch** (`run_watcher_onleave`,
  `src/watcher/uwatcher_drain.c`): tag-stop cascade now runs each
  member watcher's onleave handler through real bytecode dispatch
  via `urbi_run_closure_on_scratch`.
- **Event sync-emit subscriber dispatch** (`run_event_body_on_scratch`,
  `src/uevent_emit.c`): AT_EVENT_SYNC subscribers now run through
  real bytecode dispatch via `urbi_run_closure_on_scratch_with_payload`.
  Payload UValue arrives at the closure's R[0].  The `vm->in_watcher_scratch`
  re-entry guard at the top of the wrapper preserves the existing
  degrade-to-async-with-warn behaviour for nested sync emits.
- **AST_AT_EVENT emit register-allocation desync** (`src/uemit.c`):
  `emit_expr` for AST_IDENT global-fallback and AST_MEMBER_GET only
  bumped `e->next_reg` and not `e->current_fs->freereg`, so AST_AT_EVENT's
  subsequent `emit_function_literal` could allocate `body_reg` on top
  of `event_reg`.  OP_CLOSURE then clobbered the event pointer at
  runtime, and OP_AT_EVENT_SYNC_INSTALL tripped `R[A] == R[B]` under
  URBI_DEBUG asserts.  AST_WATCHER and AST_WAITUNTIL avoided this
  by wrapping cond in a closure (which routes through `emit_function_literal`
  symmetrically).  Fix syncs `freereg` to `next_reg` after `emit_expr`
  for the event expression at TWO sibling sites: AST_AT_EVENT (sync +
  async event install) and AT_SLOT_CHANGE (`obj.x.changed?` install).
  Affects scripted `at sync (X?) Y`, `at (X?) Y`, and
  `at sync (obj.x.changed?) Y` whenever the event_expr is non-trivial.
- **`make tidy` baseline failure** (`src/uchanged_emit.c:108`): clang-tidy
  under `-warnings-as-errors` flagged the `(UValue){0, {0}}` brace-init
  for missing `v` union initialiser.  Fixed via designated-init using
  the existing block-scoped `UValue nil = {0}` idiom (matches 14 other
  sites).  Pre-existing M5 baseline; failed identically on
  `v0.5.0-reactive` (`4faf5bc`) and `v0.5.1-cond-unstub` (`a1e8683`).

### Numbers

- 1139 unit cases / 6383 checks / 0 failed (was 1131 / 6314 at
  `v0.5.1-cond-unstub`).
- 148 `.chk` fixtures pass; `at_onleave.chk` activated as a live
  conformance fixture (joining `at_rising_edge.chk` and
  `whenever_level.chk` from v0.5.1).
- All `make releasetest` gates green: host + URBI_DEBUG + ASan +
  UBSan + valgrind (memcheck full leak-check) + determinism (3-preset
  × 100-run) + cross-arm + cross-riscv + tidy + docs-check + coverage
  (85% line) + GC stress + URBI_GC_NONE compile smoke.

## v0.5.1-cond-unstub — 2026-05-05

The M5 reactive runtime shipped with the scripted cond closure
hook-stubbed: scripted `at (cond) body`, `whenever (cond) body`, and
`waituntil (cond)` could not fire end-to-end because the install-time
and eval-time cond evaluation paths returned `UVAL_NIL` unless test
hooks were set.  This patch wires both paths to a new shared scratch-
frame runner, fixes the cascade of latent issues that surfaced once
real cond closures actually executed, and activates the first two
reactive `.chk` fixtures.

### Added

- New helper `urbi_run_closure_on_scratch` (`src/watcher/uwatcher_scratch.c`)
  — synthesizes a transient `UStrand` on the C stack, arms it from the
  closure via `urbi_strand_arm_from_closure`, runs `dispatch_loop_until_yield`
  with `URBI_SCRATCH_BUDGET_OPS` (default 4096) bound, and captures the
  `OP_RET` value plus a throw flag.  Mirrors `uvm_run`'s transient-strand
  pattern but scoped to single-closure cond eval with bounded budget +
  no-yield contract.
- Public macro `URBI_SCRATCH_BUDGET_OPS` (default 4096) — override at
  compile time for footprint targets.
- 5 unit tests in `tests/unit/test_uwatcher_scratch.c` covering integer
  return, throw detection, NULL-closure handling, nil-literal cond, and
  bool comparison conds.
- 1 integration test in `tests/unit/test_at_scripted_e2e.c` proving
  scripted `at (Realm.x > 5) body` fires through real dispatch with no
  test hooks.
- Activated `tests/chk/reactive/at/at_rising_edge.chk` and
  `tests/chk/reactive/at/whenever_level.chk` — first two live reactive
  conformance fixtures (the other 10 reactive fixtures remain deferred
  pending body-inline / onleave-inline / event-sync-emit unstubs or
  `Event.new()` / `Object.new()` stdlib at M6).

### Fixed

- **Install-time cond eval** (`run_closure_on_scratch_frame_with_result`,
  `src/watcher/uwatcher_install.c`): replaced the M5 stub fall-through
  with a call to `urbi_run_closure_on_scratch`.  Test hook short-circuit
  preserved so existing install-trace tests continue to inject specific
  cond results without going through real bytecode dispatch.
- **Eval-time cond eval** (`invoke_condition_closure`,
  `src/watcher/uwatcher_eval.c`): same swap; eval-time throws fail-soft
  as nil per the existing contract (caller `watcher_eval_dirty` is void
  and cannot propagate).
- **Closure ownership transfer at install** (`uwatcher_install.c`):
  `install_watcher_runtime` now calls `strand_closure_unlink` to move
  cond / body / onleave closures from the strand's `closure_list` to
  the watcher.  New `URBI_WATCHER_OWNS_COND` / `_BODY` / `_ONLEAVE`
  flag bits drive `pool_free`'s closure release.  Without this,
  `uvm_run`'s post-run cleanup freed the watcher's closures while
  still in use.
- **Body strand IC table wiring** (`uwatcher_spawn.c`):
  `do_spawn_body_coroutine` now wires `body->module_instance` by
  walking `vm->module_instances_head` to find the owning instance via
  pointer-range comparison on `proto_inst`.  Required because
  `urbi_strand_arm_from_closure` (the M3 helper) doesn't set
  `module_instance`, and OP_GETSLOT/SETSLOT at `frame_count==0` reads
  through it.  See backlog: `UClosure.owning_mi` field is the cleaner
  long-term shape (set at OP_CLOSURE — eliminates the pointer walk).
- **OP_GETSLOT/SETSLOT entry_closure fallback at frame_count==0**
  (`src/uvm.c`): the IC table is now resolved from
  `s->entry_closure->proto_inst->ic_table` when available, falling back
  to `s->module_instance->proto_instances->entries[0].ic_table`.  The
  former is the correct (non-root-chunk) IC table for body strands
  spawned from nested closures.
- **OP_SETSLOT slow-path write barrier** (`src/uvm.c`): the slow path
  through `urbi_slot_set_slow` now calls `urbi_gc_slot_write` (with
  conservative slot index 0 sentinel — observer_dirty ignores the key
  at M5; real index needed at M6).  Without this, COW writes never
  bumped `watcher_dirty_count` so watchers with read-sets that include
  slow-path receivers never fired.
- **`sched_strand_init` for `uvm_run` transient strand** (`src/uvm.c`):
  arms `instruction_budget_remaining` so the first safepoint hit
  inside the transient run actually crosses the dirty-walk path.
  Without this, the transient strand yielded at first safepoint with
  budget=0 and `watcher_eval_dirty` was missed.
- **REPL drain loop** (`tools/urbi.c`): the REPL now drains spawned
  body strands via `urbi_step` after each `uvm_run`.  Without this,
  body strands queued during a REPL line never executed before the
  next line ran.  Embedders driving `urbi_step` directly are
  unaffected — this only changes the REPL's host-driver shape.
- **`pending_onleave_head` drain at `pool_destroy`** (`uwatcher.c`):
  `urbi_tag_stop` (called from `urealm_teardown_all`) moves watchers
  from `active_watchers_head` to `pending_onleave_head`.  The pre-T12
  `pool_destroy` only drained the active list — pending entries with
  `OWNS_*` flags would have leaked their owned closures.  Now both
  lists are drained.
- **`vm->in_watcher_scratch` zero-init** (`src/uvm.c`): the field was
  declared in M5 (spec #3 §5.4) but missing from the `uvm_init`
  initialiser block.  Stack-allocated UVMs in tests left it
  uninitialised; valgrind flagged the read at `uevent_emit.c:140` and
  `uchanged_emit.c:36`.  Pre-existing M5 latent bug; surfaced when the
  cond-unstub work raised valgrind coverage.

### Deferred (separate follow-up patch)

The same `urbi_run_closure_on_scratch` primitive can wire four more
sites that are still hook-stubbed at this release.  Each is a 5-10 LOC
patch reusing the helper:

- `invoke_body_inline` (`src/watcher/uwatcher_eval.c`) — AT_SYNC body
  inline execution.
- `invoke_onleave_inline` (`src/watcher/uwatcher_eval.c`) — onleave
  handler on falling edge.
- Drain-time onleave (`src/watcher/uwatcher_drain.c`) — onleave during
  tag-stop cascade.
- Event sync-emit body (`src/uevent_emit.c`) — sync subscribers run
  inline on emit.

These are tracked as M6 prerequisites or `v0.5.2-scratch-frame-followup`
candidates.  Backlog also tracks two clean-up items:

- `UClosure.owning_mi` field set at OP_CLOSURE — replaces the pointer-
  range walk in `uwatcher_spawn.c` with a direct field read.
- Real slot index in slow-path `urbi_gc_slot_write` calls — needed at
  M6 when observer_dirty starts using the key.

### Numbers

- 1131 unit cases / 6314 checks / 0 failed (was 1124 / 6272 at v0.5.0).
- 148 chk fixtures pass; 2 reactive fixtures activated as live
  conformance tests (at_rising_edge, whenever_level).
- ASan + UBSan + valgrind-fast clean.

## v0.5.0-reactive — 2026-05-04

The M5 reactive runtime milestone. Persistent watchers, events, slot-change
subscriptions, tag enter/leave hooks, and the realm-global identifier
resolution that anchors them. Bytecode v1.3 → v1.4 hard break.

### Breaking changes

- **Bytecode v1.4**: version byte incremented; loader rejects v1.3 and earlier.
- **Reserved keywords**: `at`, `whenever`, `waituntil`, `onleave`, `sync` are
  hard keywords; `async` is a soft keyword (allowed as identifier at v1.0,
  deprecation warning v1.x). `var at = 1` raises
  `PARSE_RESERVED_KEYWORD_AS_IDENT`.
- **gc_byte bit 7** allocated as `UGC_HAS_SLOT_CHANGE_EVENT`; all 8 bits are
  now claimed. Future additions must multiplex or extend to gc_word.

### Language additions

- `at (cond) body [onleave handler]`, `at sync (cond) body`,
  `whenever (cond) body`, `waituntil (cond)` — cond watchers with rising/
  falling edge fire, level-triggered whenever, and one-shot waituntil
  strand-park.
- `at (e?) body [onleave]`, `at sync (e?) body [onleave]` — event subscribers
  via spec #3 `OP_AT_EVENT_INSTALL` / `OP_AT_EVENT_SYNC_INSTALL` dispatch.
- `at (obj.slot.changed?) body`, `at sync (obj.slot.changed?) body` — slot-
  change subscribers via spec #4 `OP_GETSLOT_CHANGE_EVENT` lookup-or-create.
- `at (mytag.enter?) body`, `at (mytag.leave?) body` — tag tier-2 hooks.
- Postfix `e!` and `e!(p)` for event emission (multi-arg parse error
  `PARSE_EMIT_MULTI_ARG_V1`); `e.syncEmit(p)` and `Event.waituntil(e)`
  native methods.
- Top-level identifiers (`Object`, `Tag`, `Event`, `Integer`, `Float`,
  `String`, `Bool`, `Nil`, `Void`, `List`, `Dict`, `Symbol`, `Realm`, `nil`,
  `void`) resolve to a 15-entry static built-in registry per realm.
- Top-level `var X = …` and `function f() { … }` write to realm-global slots
  (const-attributed for built-ins).

### New opcodes

- 39 `OP_AT_INSTALL` (cond watcher install).
- 40 `OP_AT_SYNC_INSTALL` (sync cond watcher install).
- 41 `OP_WHENEVER_INSTALL` (level-triggered watcher install).
- 42 `OP_WAITUNTIL_INSTALL` (one-shot strand-park install).
- 43 `OP_AT_EVENT_INSTALL` (event subscriber install, async).
- 44 `OP_AT_EVENT_SYNC_INSTALL` (event subscriber install, sync).
- 45 `OP_GETSLOT_CHANGE_EVENT` (per-object slot-change UEvent lookup-or-create).
- 46 `OP_LOAD_REALM_GLOBAL` (frame prologue load realm.global_object into
  reserved register).

(Plan template said opcodes 29-36 — wrong. M3 control-transfer + M4 INVOKE
already claimed those slots; M5 lands at 39-46.)

### Runtime additions

- `UEvent` (40 B) cell type with `at_watchers_head` (UWatcher chain) and
  `waiters_head` (UStrand chain) intrusive subscriber lists.
- `UChangedNode` (32 B host / 16 B 32-bit) cell type for per-UObject slot-
  change subscriber chain.
- `UTag` GC-promoted (was M3 host-managed); 48 → 64 B with `enter_event` and
  `leave_event` lazy-allocated event slots.
- `UWatcher` 200 → 240 B default (104 → 144 footprint preset); 6 modes
  total (AT, AT_SYNC, WHENEVER, WAITUNTIL, AT_EVENT, AT_EVENT_SYNC); 4 new
  flag bits.
- `UStrand` 256 → 288 B; 2 new wait states (`USTRAND_WAIT_WATCHER=0x32`,
  `USTRAND_WAIT_EVENT=0x33`); back-pointer to owning watcher; event-waiter
  fields.
- `UObject` 48 → 56 B with `changed_events_head` lazy-allocated chain head.
- `UVM` gains trace fields (16-entry read-set for cond install) + slot-change
  deferred-emit ring (default 64, footprint 16 entries × 24 B).

### Public C API additions

- `urbi_realm_set_global(realm, name, value)` — install non-const global.
- `urbi_realm_set_global_const(realm, name, value)` — install const global.
- `urbi_realm_get_global(realm, name, *out)` — read a realm global.
- `urbi_register_event_drain(vm, drain_fn)` — host callback for ISR-injected
  events; drains at safepoint via the M3 SPSC ring.

### Determinism

- `URBI_SCHED_COOPERATIVE × URBI_GC_INCREMENTAL` remains bit-for-bit
  reproducible. New rules (D-watcher-1/2, D-event-1/2, D-slotchange-1/2/3):
  watcher install + event subscribe + slot-change install all FIFO over
  registration order; sync subs run before async; deferred-emit ring drains
  at safepoint before `watcher_eval_dirty`.

### Gates

- 1124 unit cases / 6272 checks / 0 failed.
- 148 chk fixtures (was 127 pre-M5).
- ASan + UBSan + valgrind-fast + valgrind-deep all clean (a pre-existing
  `test_emit_diag` leak from M5's own T32 was fixed in R8 hardening).
- 3-preset × 100-run determinism gate green (cross-spec det fixtures
  deferred-stub at this release; activate post-M6).
- GC pause within budget; cross-arm + cross-riscv build green.

### Known limitations (deferred to v1.x or M6)

- **Scripted at/whenever/waituntil cond closure is hook-stubbed.** The
  watcher install path is real, the registry is real, the eval/fire-decision
  machinery is real — but `run_closure_on_scratch_frame_with_result` is an
  M5-baseline stub returning UVAL_NIL. C-level unit tests cover the logic
  via `vm->test_install_cond_hook` and `vm->test_watcher_fire_hook`.
  Unblocking the scripted .chk fixture path is a 1-2 commit M6 prerequisite.
- ~48 of the planned 60 reactive .chk fixtures deferred (R8 minimum-viable);
  full corpus is v1.x.
- 4 of the planned 5 stress targets deferred.
- 5th slot-change callsite (namespace_set) deferred to M6.
- 7 of 8 M4-era prototype-chain fixtures still T38/T39-blocked (atom-method
  dispatch, class declaration, `.new()` stdlib — M6 territory).

### Notes

- Bytecode v1.4 is a hard break; pre-M5 bytecode files refuse to load.
- 74 commits on `topic/m5-reactive` since `v0.4.0-objects` follow-up `83bab89`.
- See `docs/milestones/m5-reactive.md` for the full retrospective.

## v0.4.0-objects — 2026-05-02

The M4 object model milestone. Introduces a prototype-based object system
with hidden-class shape inference, per-call-site inline caches, copy-on-write
slot inheritance, and atom-family inheritance constraints. Bytecode bumped
to v1.3; earlier `.urb` files are rejected at load time.

### Breaking changes

- **Bytecode v1.3**: version byte incremented; loader rejects v1.2 and earlier
  modules with a diagnostic. `UProto` gains `ic_count` (uint16) + `ic_names`
  side table (USymbol** parallel array). Recompile all `.urb` files.

### Object model

- `UObject` (48 B header): `cell` (GC) + `shape` + `slots` + `protos`
  (tagged uintptr_t) + `object_id` (per-VM monotonic) + `lookup_stamp` (cycle
  guard) + `flags` (atom family low 4 bits + IS_PROTOTYPE / FROZEN /
  SANDBOX_RO bits).
- Tagged-pointer prototype chain with three storage forms: empty (`protos=0`),
  single (`(p<<1)|1` — most common in legacy code), and heap (`UProtos*`
  pointer with bit 0 clear). `UPROTOS_FOREACH` macro captures `obj->protos`
  once at iteration start (legacy semantics per spec §6.3).
- `UShape` (56 B): hidden-class with `name` (last-added slot), `index`,
  `count`, `flags` (4-bit per-slot nibbles for OGET/OSET/CONSTANT/LOCAL),
  `parent`, `transitions` (UShapeMap cache), `props_table` (lazy per-slot
  UProps* dense array via UPropsTable wrapper).
- `UShapeMap` transition cache (open-addressing hash, power-of-2 capacity,
  USymbol* identity hashing); `urbi_shape_transition_add_slot` shares
  child shapes for identical construction sequences.
- `USlot` collapsed to 16 B (typedef of `UValue`); slot storage is
  `USlotArray` wrapper (UCell + entries[]).
- `UProps` (48 B): per-slot getter/setter/constant flag side table; allocated
  lazily when any slot in the shape lineage installs a property; stored in
  per-shape `UPropsTable`.
- Atom-family singletons: 9 lazily-allocated per-VM prototypes (Object,
  Integer, Float, String, List, Dict, Tag, Event, Symbol). Atoms pinned via
  GC root provider, not manual handle-table pinning.

### Inline caches and dispatch

- `UIC` (144 B at `URBI_IC_ENTRIES_PER_SITE=4`, the default; 80 B at =2;
  48 B at =1): per-site inline cache with `name`, parallel arrays of
  `recv_shapes` / `topology_gen` / `slots` / `uprops` / `flags`, `n` valid
  count, and `replace_cursor`.
- `URBI_IC_ENTRIES_PER_SITE` compile-time tunable {1, 2, 4} drives IC site
  width; default 4, footprint preset binds 2.
- `UModuleInstance` + `UProtoInstance`: per-VM RAM tier separate from the
  read-only `UModule`. Two instances of the same module have independent
  IC tables; threaded onto `vm->module_instances_head`.
- Per-function emit-time IC bookkeeping: `UFuncState.ic_next` counter,
  `ic_names` dynamic array (16-slot growth chunks), capped at 256 sites
  per function (`EMIT_TOO_MANY_IC_SITES`).
- `urbi_object_resolve_slot`: cycle-safe DFS over prototype chain
  (left-first via `UPROTOS_FOREACH`), 64-deep stack bound, `lookup_id`
  stamping for re-entry guard with rollover handling.
- COW on assignment to inherited slot: `urbi_slot_set_slow` installs a
  local slot on receiver via `urbi_object_set_local_slot`, leaving the
  prototype's slot intact.

### Language and parser

- `OP_GETSLOT` (opcode 27) and `OP_SETSLOT` (opcode 28) ABC-encoded:
  A=dst (or src), B=recv, C=ic_index. IC tables looked up via
  `cur_closure->proto_inst->ic_table[ic_index]`.
- AST nodes: `AST_MEMBER_GET` / `AST_MEMBER_SET` for `obj.x` / `obj.x = v`;
  `AST_PROP_GET` / `AST_PROP_SET` for `obj.x->prop` / `obj.x->prop = v`.
- Lexer tokens: `TOK_DOT` (`.`) and `TOK_ARROW` (`->`).
- Parser preserves method-call syntax: `obj.method()` parses as
  `AST_CALL{callee=AST_MEMBER_GET}` not as `AST_MEMBER_GET` followed by
  `AST_CALL`.

### Public C API

- `include/urbi/object.h`: opaque `UObject` / `UShape` typedefs; atom-family
  enum (`URBIAtomFamilyTag` with `_F` suffix to avoid namespace collision
  with internal `URBIAtomFamily`); `urbi_object_root` /
  `urbi_object_atom` / `urbi_object_add_proto` / `urbi_object_remove_proto`
  / `urbi_object_set_protos`.
- `UModuleInstance` opaque typedef + `urbi_module_instance_create` /
  `urbi_module_instance_destroy`.
- `USlotHandle` wrapper: `urbi_object_get_slot` returns a handle pointing
  at the slot's current owner (may be the receiver or an inherited
  prototype). `urbi_slothandle_read_value` / `write_value` validate or
  refresh on access; handles become permanently invalid after the slot is
  removed.
- Fallback slot retry: `urbi_object_lookup` retries once with name=`fallback`
  on full-tree miss. Cycle-safe (no infinite recursion when looking up
  `fallback` itself).

### Inheritance semantics

- `valid_proto`: atom-family constraint at addProto/setProtos time. An atom
  can only inherit from its own family or root Object; root Object never
  blocks.
- `urbi_object_set_protos` is atomic: validate every survivor before any
  state change. Dedup first-occurrence-wins, capped at 64 unique items.
- `IS_PROTOTYPE` bit (`URBI_OBJ_FLAG_IS_PROTOTYPE`): set monotonically on
  any UObject when it joins another object's protos chain. Read by
  `urbi_object_set_local_slot` to drive the conditional topology bump.
- `urbi_object_clone` (atom-aware): preserves parent's atom family in the
  clone's flags low-4-bits and threads the parent into protos as the
  single-tag form.

### topology_gen mutation surfaces

Every IC-invalidating mutation surface bumps `vm->topology_gen` (u64 since
T2). 12 surfaces per the topology-generation spec §4.1:

- Slot install / remove on a prototype (rows 1, 4); slot install on a
  non-prototype receiver does NOT bump (caught by the IC's shape-mismatch
  check per §4.2 row 2).
- Property install / remove / in-place mutate (rows 5–7) via
  `urbi_object_install_property` / `_remove_property` / `_set_property_value`.
- Prototype-chain mutations (rows 8–12) via `urbi_object_set_protos_*`.

`urbi_get_determinism_checksum` (URBI_DEBUG) folds `topology_gen` +
`lookup_id` + `next_object_id` plus per-IC state (`n` + `replace_cursor` +
`recv_shapes[]` + `topology_gen[]`) into the per-step checksum. The
existing CI determinism gate (3 presets × 100 runs) remains green.

### Garbage collector

- New cell types: `UTYPE_PROTOS=9`, `UTYPE_SHAPE=10`, `UTYPE_PROPS=11`,
  `UTYPE_SLOTHANDLE=12`, `UTYPE_MODULE_INSTANCE=13`,
  `UTYPE_PROTO_INSTANCE=14`, `UTYPE_SHAPE_MAP=15`,
  `UTYPE_PROPS_TABLE=16`, `UTYPE_SLOT_ARRAY=17`. (`UTYPE_OBJECT=1` is the
  pre-existing M3 baseline tag.)
- Per-type mark walkers in `src/object/utypes_init.c`. `UObject` walker
  shades `shape` + `slots[i]` UValue payloads + `protos` chain via
  `UPROTOS_FOREACH`. `UShape` walker shades `parent` + `transitions` +
  `props_table` entries. Wrappers (UPropsTable, USlotArray,
  UProtoInstance bulk) reach their content via the owning UObject's
  walker through `offsetof` arithmetic.
- `UClosure` embeds `UCell` as first member (closes the M3 baseline
  TODO). `urbi_gc_upvalue_write` may now safely cast `UClosure*` →
  `UCell*` for the barrier color check; OP_SETUPVAL invokes the
  barrier before the actual store.
- New `UVAL_OBJECT` UValue kind (=8); incremental GC's `uvalue_is_heap`
  / `uvalue_as_cell` treat UVAL_OBJECT as heap-bearing.
- M4 GC root provider registered at `uvm_init`: walks the 9 atom
  singletons + root shape + every `UModuleInstance` on the per-VM list.
- GC strand walker swapped from `ready_head` + `sleep_q_head` to
  realm-hierarchy iteration (`vm->realms_head` → `realm.strands_head`).
  Closes the WAITING_JOIN root gap; generalizes to any wait state.
- Transient strands (uvm_run helpers) routed to `vm->global_realm` at
  creation; unlinked symmetrically at exit.

### Public-facing scheduler contract

- New `docs/internals/scheduler-design.md` documents the GC walker
  contract: every strand whose register window may hold GC-managed
  UValues MUST be reachable via `vm->realms_head → realm.strands_head`.
  Scheduler implementations are responsible; the GC walker assumes the
  invariant without re-verification.

### Tests and CI

- 884 unit cases / 5330 checks at default build; 902/5370 under
  URBI_DEBUG. Green under host + ASan + UBSan; cross-arm (Cortex-M7) +
  cross-riscv (rv32imc) build clean.
- New test suites: `test_uic`, `test_uslothandle`, `test_topology_gen`,
  `test_scheduler_invariant`, `test_gc_strand_walker`,
  `test_ugc_object_cells`. Existing `test_uobject` / `test_ushape` /
  `test_funcstate` extended.
- `make test-determinism` (3 presets × 100 runs) green.
- `make releasetest` aggregate target: all stages green.
- Footprint preset (`URBI_IC_ENTRIES_PER_SITE=2`) builds and tests pass.

### Known limitations / deferred

- **`UClosure.proto_inst` binding for transient strands**: the
  OP_GETSLOT / OP_SETSLOT dispatch arms are wired but the `proto_inst`
  field on a closure created by `vm_alloc_closure` is NULL (no
  UModuleInstance association). Affects urbiscript-level slot dispatch
  through transient strands; the dispatch arms diagnose with
  `TypeError: GETSLOT: no IC table bound`. Unblocking this requires 1–2
  commits in `src/uvm.c` near OP_CALL plus the strand-spawn path. All
  C-API paths (urbi_object_resolve_slot, urbi_slot_get_slow,
  urbi_slot_set_slow, urbi_slot_handle_*) work end-to-end.
- **Class declaration emit** (`class Foo : A, B { body }`),
  **Class.new() stdlib wiring**, **get/set parse sugar**: deferred
  pending the proto_inst binding above.
- **Legacy `.chk` fixture ports** (lookup, inheritance, slot-cow-const,
  shared-protos, class, fallback, atom-clone, atoms): deferred for the
  same reason.
- **Getter / setter dispatch macros** (`URBI_VM_DISPATCH_GETTER` /
  `_SETTER`): the IC entries' OGET/OSET flags are honored at slow-path
  fill time, but the dispatch macros themselves are deferred to land
  alongside the frame-push wrapper at `Class.new()`.
- **Wire-trailer reader / writer for v1.3 `UProto.ic_count` /
  `ic_names`**: in-memory foundation lands at T1; nested-proto
  serialization deferred to a future task that adds the symbol-pool
  framing.
- **`URBI_IC_ENTRIES_PER_SITE=1` build**: a pre-existing `replace_cursor`
  modulo-1 wraparound assumption (cursor=0 vs assertion of cursor=1) in
  one IC test is unrelated to T44; default (=4) and footprint (=2) both
  pass cleanly.
- **Determinism `.chk` fixture covering all 12 §4.1 surfaces**: deferred
  pending end-to-end runtime; URBI_DEBUG `determinism_checksum_includes_
  ic_state` test pins the IC-fold step at unit level.

## v0.3.0-concurrency — 2026-04-28

The M3 concurrency milestone. Adds six subsystems above the M2 expression
foundation: control transfer (exceptions, tags, unwind), chunk lifecycle
(realms, namespaces, step driver), cooperative scheduler (ISR-safe event ring,
strand C API), incremental tri-color GC (5-phase state machine, debt-triggered
slices, 3 barrier surfaces, host-handle pinning), tag/watcher data and eval
layer (UTag, UWatcher pool, read-set, watcher eval loop, pending-onleave drain),
and determinism infrastructure (checksum diagnostic, CI gate, time literals,
legacy corpus port). Bytecode bumped to v1.2; earlier `.urb` files are rejected
at load time.

### Breaking changes

- **Bytecode v1.2**: version byte incremented; loader rejects v1.1 and earlier
  modules with a diagnostic. Recompile all `.urb` files.

### Language

- Time and angle literals: `100ms`, `1s`, `2.5s`, `180deg` lexed to
  `TOK_DURATION` / `TOK_ANGLE`; `ms`/`s`/`m`/`h`/`d` suffixes emit
  microsecond integer values; `deg`/`rad` emit float radian values.
- `,` (parallel fire-and-forget) and `&` (parallel join) separator runtime
  activated. `,` spawns N-1 child strands + runs last child inline.
  `&` compiles rhs to closure, runs lhs inline, then OP_FORK_JOIN /
  OP_JOIN_WAIT; result is void. Child handles are `UVAL_STRAND` (kind=7).
- `try` / `catch` / `finally` / `throw` — full emit and runtime; exception
  value forwarded through catch register.
- Tag scopes — `mytag: { ... }` compiles to OP_PUSH_TAG / body / OP_POP_TAG;
  member-strand list maintained; `urbi_tag_stop` deposits UEXEC_TAG_STOP
  with C-1 priority.

### Unwind / exception model

- `urbi_unwind` walker: 5-kind absorption (OK / RETURN / THROW / TAG_STOP /
  CANCEL); replace-on-raise semantics; URBI_WARN_SUPPRESSED_UNWIND emitted
  via `host_log_fn`.
- `UExecStatus` enum (OK / RETURN / THROW / TAG_STOP / CANCEL / FATAL);
  `urbi_exec_status_name`.

### Chunk lifecycle and scheduler

- `URealm` + `UNamespace`: per-realm GC root provider, 4-function Realm C API,
  namespace resolution protocol.
- `urbi_step` 4-state driver (OK / QUIESCENT / FATAL / YIELD_BUDGET); 4
  chunk-execution wrappers.
- ISR-safe SPSC event ring: `urbi_inject_event` as the sole ISR-safe entry
  point; bounded drain at `urbi_step` entry.
- Strand C API: `urbi_strand_create` / `start` / `spawn` / `cancel` / `panic`
  / `reset`; ambient-tag attachment; cooperative FIFO run-queue.
- `URBI_DEBUG` build mode: ISR-safety assertions at all non-ISR entry points;
  callback watchdog (configurable warn / assert threshold).

### Incremental GC

- Tri-color mark-sweep with 5-phase state machine; `urbi_gc_slice(vm, budget)`
  incremental driver; `urbi_gc_force_full` synchronous path.
- Three barrier surfaces: `urbi_gc_slot_write` (forward Dijkstra + watcher
  dirty hook), `urbi_gc_register_write` (no-op), `urbi_gc_upvalue_write`.
- Root-provider registry: up to 8 providers; 5 registered at M3 (scheduler,
  realm list, intern table, host handles, watcher table).
- Host-handle table: `urbi_pin` / `urbi_unpin`; `urbi_register_type` with
  finalizer dispatch; `UType.destroy` called from sweep.
- GC pause max 2.8 µs measured (357× margin under 1 ms target).
- `make test-gc-pause` gated stress binary; `make test-stress` 4-program suite;
  `make test-gc-none-build` strategy-swap smoke; all wired into `make releasetest`.

### Tag / watcher subsystem

- `UTag` host-managed (via `alloc_fn`); ambient-tag inheritance via synthetic
  TAG_SCOPE cleanup entries; member-strand and member-watcher lists.
- `UWatcher` pool: 200-byte record, pre-allocated slab, freelist, `in_use` /
  `high_water` counters.
- Read-set capture: bit-6 (`UGC_HAS_WATCHER_OBSERVER`) lifecycle; tail-insert
  for deterministic eval order; install-time `last_value_cache` seed.
- Watcher eval loop: `watcher_eval_dirty` walks active list; edge/level firing
  per spec §6.2/§6.3; `UScratchFrame` (~280 B) allocated at `uvm_init`.
- Pending-onleave queue: drain reuses `in_watcher_eval` reentrancy guard;
  OP_POP_TAG and `urbi_tag_stop` cascade watchers before scope destruction.

### Determinism

- `urbi_get_determinism_checksum` (`URBI_DEBUG`): XOR-reduce over active-watcher
  list and dirty count; enables replay comparison.
- `make test-determinism` CI gate: two consecutive `urbi_step` sweeps with
  checksum equality assertion; wired into `make releasetest`.

### Tests

- Unit cases: 772 (up from 489, +283); debug variant 786.
- `.chk` fixtures: 127 (up from 18, +109) across 8 subdirectories
  (`control_transfer/`, `chunk_lifecycle/`, `scheduler/`, `gc/`, `tag/`,
  `separator/`, `time_literals/`, `determinism/`).
- Cross-build: ARM Cortex-M7 32 KB text / RISC-V rv32imc 41 KB text (host
  65 KB). All three targets verified at `make cross-arm` / `make cross-riscv`.
- All 8 gates green: `make test` / `test-debug` / `test-asan` / `test-ubsan` /
  `cross-arm` / `cross-riscv` / `test-stress` / `test-gc-none-build`.

### Known limitations / deferred

- **Watcher body and on-leave execution** deferred to M5. `spawn_body_coroutine`
  and `run_watcher_onleave` are M3 stubs; tests use `test_watcher_fire_hook`
  and `test_watcher_onleave_hook` on `UVM`.
- **`,` shared-frame semantics** (spec §7.1) deferred to M5+. Current
  implementation uses closure-spawn; correctness is unchanged, only
  per-child allocation overhead differs.
- **`at`/`whenever`/`waituntil`** — reactive runtime deferred to M5.
- **Object method dispatch** — deferred to M4.
- **UVM struct padding** — `clang-analyzer-optin.performance.Padding` reports
  36 bytes excess in `struct UVM`; full field reorder deferred to avoid
  destroying semantic row-grouping in the struct comments.
- **Most legacy `.chk` corpus fixtures** remain deferred (require M4 object
  model or M5 reactive runtime). 127 fixtures active or structured as
  deferred placeholders for future milestones.

## v0.2.0-expressions — 2026-04-25

The M2 expressions milestone. Adds the full expression language surface
above the M1 arithmetic core: variables, closures, control flow, function
definitions and calls, per-parameter lazy arguments, statement separators,
and multi-VM hardening. Bytecode bumped to v1.1; earlier `.urb` files are
rejected at load time.

### Language

- Bytecode v1.1: version byte incremented; loader rejects v1.0 modules
  with a diagnostic. Reserved opcode slots assigned for all M2 additions.
- Per-VM string interning table (`ustr_intern`): strings are canonical;
  pointer equality implies content equality within a VM.
- Lua-FuncState-adapted register allocator with named locals, lexical
  block scopes, and cascading upvalue capture across arbitrarily nested
  function definitions.
- Statement separators: `;` (sequential with yield) and `|` (sequential
  atomic) ship full runtime semantics. `,` (parallel fire-and-forget) and
  `&` (parallel join) are parsed and represented in the AST; runtime is
  deferred to M3.
- Per-parameter `lazy` keyword: `function f(lazy x) { ... }` compiles
  the argument to a sub-proto thunk; the callee's first read of `x`
  forces evaluation implicitly.
- Control flow: `if` / `else` with proper short-circuit jumps; `while`
  with back-edge `OP_CLOSE` for closure-in-loop correctness.
- Function definitions, calls, and `return`.
- Comparison operators (`==`, `!=`, `<`, `<=`, `>`, `>=`).
- Boolean and nil literals (`true`, `false`, `nil`).

### Multi-VM hardening

- Per-VM `intern_table` and `topology_gen` fields on `UVM`; no
  file-scope mutable state remains.
- `UModule` gains `origin_vm` field; stamped at compile time, checked at
  load time.
- 8-case isolation test matrix in `tests/unit/test_multi_vm.c`
  (3 cases deferred to M3+/M5+/M6+).
- `tools/audit-globals.sh` + `cppcoreguidelines-avoid-non-const-global-variables`
  clang-tidy check gated under `make lint`.

### Migration notes

- `bare function name { body }` → `function name() { body }`.
  The bare-function form (no formal parameter list) now produces
  `PARSE_BARE_FUNCTION` at parse time. The migration recipe is mechanical.
- `closure(x) { ... }` → `function(x) { ... }`.
  The `closure` keyword is retired; `function` captures lexical scope
  universally. Note the `this`-binding migration trap: legacy `closure`
  bound `this` to the definition site; v1.0 `function` binds `this` to
  the call site. Affected pattern: `var obj.m = closure(t) { this.f(t) }`.
  Migration recipe: capture the receiver explicitly before the closure:
  `var self = this; var obj.m = function(t) { self.f(t) }`.

### Build (infra)

- Added `make releasetest` aggregate target that runs every host-side
  CI gate in sequence (sanitizer matrix, valgrind memcheck, lint,
  docs-check, coverage). Invoked manually before tagging a release
  or pushing branches touching multiple subsystems. Cross-compile
  targets are excluded; CI remains authoritative for cross-compile
  verification.

### Documentation (test infra)

- Documented the tiered test-target convention in
  `docs/internals/test-harness.md` — `make test` and the three fast
  companion variants form the pre-commit gate (~30 s combined);
  `make releasetest` is the pre-release gate (~3–5 min). Extended
  the target-reference table with a Runtime column and the
  previously-undocumented `test-switch` and `releasetest` rows.
- Codified the `.chk` fixture header schema (`Milestone:` /
  `Covers:` comment lines) in the "Authoring a new fixture"
  subsection of `docs/internals/test-harness.md`. Applied the
  schema to `tests/chk/arithmetic/basic.chk`. Enables `grep`-based
  discovery across the corpus at scale.

### Tests (chk-layout)

- Reorganized `tests/chk/` into feature subdirectories. Moved
  `tests/chk/arithmetic.chk` → `tests/chk/arithmetic/basic.chk` and
  documented the `tests/chk/<feature>/<name>.chk` layout convention
  in `docs/internals/test-harness.md`. The `test-chk` Makefile target
  already uses `find ... -name` recursively; no build change required.

## v0.1.0-skeleton — 2026-04-24

The walking-skeleton milestone. A complete end-to-end compile-and-execute
pipeline — lexer, parser, arena allocator, bytecode emitter + module
format, register-based VM, interactive REPL, and the first `.chk`
conformance fixture (`arithmetic.chk`). Not a production runtime — the
language surface is an 8-opcode Int/Float arithmetic subset — but the
pipeline is genuinely end-to-end, and every subsystem is covered by
unit tests, sanitizers, a valgrind-gated memcheck job, and coverage
instrumentation. Freestanding-clean front end cross-compiles for
Cortex-M7 and RV32IMC.

### REPL

- New binary `urbi` — the M1 walking-skeleton REPL. Drives the full
  `ulex` → `uparse` → `uemit` → `uvm` pipeline. Five modes: `urbi -i`
  (interactive via vendored linenoise, with `~/.urbi_history` persistence
  and `[%08u] value` timestamp frames via `clock_gettime(CLOCK_MONOTONIC, …)`),
  `urbi -e <expr>` (evaluate and print), `urbi [-f] <file>` /
  `urbi <file>` (run script; no per-statement print per Unix convention),
  `urbi --dump-bytecode` (disassemble via `uemit_disassemble`, incompatible
  with `-i`), `urbi --version` / `urbi --help`. Persistent `UVM` across
  interactive lines; fresh `UModule` + `UArena` per line. Implicit `|`
  statement terminator appended if missing.
- Source in `tools/urbi.c`, outside `src/` to preserve the `cc src/*.c`
  drop-in invariant. Never built on cross-compile targets.

### Formatter

- New library module `src/uvalue.{c,h}` — `UValue`-to-string formatter
  with Lua-5.4-style number formatting. Integer via `%lld`, Float via
  `%.14g` (f64) or `%.7g` (f32) with trailing `.0` appended on
  whole-number floats for visual kind-distinction. Bool, Nil, Str also
  covered (Bool/Str/Nil unreachable from M1 source; ship complete for
  M2+). Buffer-based, no allocation, thread-safe.
  `__STDC_HOSTED__`-gated — contributes no symbols under freestanding.

### Vendored

- `tools/linenoise.c` / `tools/linenoise.h` — single-file line editor
  from `github.com/antirez/linenoise` at commit
  `a15597057991fc748b3759cc66e157c9ea8bdfff`, BSD-2, preserved verbatim.
  Provenance ledger at `tools/LINENOISE-UPSTREAM.md`. Only linked into
  the `urbi` binary; never enters `liburbi.a`.

### Build

- New Makefile targets: `$(BUILDDIR)/urbi` (the REPL binary),
  `urbi-bin` (phony), `test-integration` (phony running the shell
  harness). `test` aggregate now depends on `test-integration`, so
  unit and integration run together under every sanitizer variant.
- `make tidy` scope widened to include `tools/urbi.c`.
  `tools/linenoise.c` stays outside the first-party tidy scope.
- `tools/linenoise.c` compiles with `-D_POSIX_C_SOURCE=200809L
  -D_XOPEN_SOURCE=700 -w` to suppress vendored-code warnings; `tools/urbi.c`
  compiles under the standard strict `$(CFLAGS)` discipline.

### Tests

- Added `.chk` conformance-fixture runner at `tests/integration/run_chk.sh`
  and the first fixture `tests/chk/arithmetic.chk` covering the M1 8-opcode
  VM. Folded into the `test` aggregate via a new `test-chk` Make target,
  so every sanitizer variant runs the fixture corpus automatically.
- `tests/unit/test_uvalue.c` — ~25 unit cases covering all 5 UValKinds,
  edge cases (INT64_MAX/MIN, -0.0, NaN, Inf, whole-number floats,
  scientific notation), and truncation (cap=0/1/3).
- `tests/integration/repl_smoke.sh` — POSIX sh harness covering every
  CLI mode and error path (30 cases). Runs against `$(BUILDDIR)/urbi`
  as part of `make test`.

### VM

- New module `src/uvm.{c,h}` implements the M1 register-based bytecode
  interpreter. Handles the 8-opcode M1 set (LOADK, MOVE, ADD, SUB, MUL,
  DIV, NEG, RET) with type-dispatched arithmetic per
  `docs/LANG-CONVENTIONS.md` §1.3: Int+Int wraps two's-complement,
  Int+Float promotes to Float, DIV always produces Float.
- Persistent `UVM` struct with `init`/`run`/`destroy` lifecycle and a
  VM-owned allocator hook distinct from the UModule loader's allocator.
  The 128-byte fixed error-message buffer carries
  `source:line:`-prefixed diagnostics for `UVM_TYPE_ERROR` and
  `UVM_OOM`; freestanding-compilable with no dependency on `<stdio.h>`
  / `<string.h>` / `<stdlib.h>` (stdlib-realloc shim is
  `__STDC_HOSTED__`-gated).
- Dispatch macros (`CASE` / `DISPATCH` / `NEXT`) expand to computed-goto
  under `__GNUC__` / `__clang__` and to `switch`/`case`/`continue`
  otherwise. Opcode bodies are written once; a new
  `URBI_VM_FORCE_SWITCH` build flag overrides the detection to exercise
  the switch path on GCC/Clang hosts.

### Tests (VM)

- `tests/unit/test_vm.c` — new test suite covering lifecycle,
  per-opcode happy paths, arithmetic type matrix, wrap semantics
  (INT64_MAX+1, INT64_MIN*-1, etc.), IEEE 754 DIV corners (±Inf, NaN),
  TypeError paths, OOM path, diagnostic prefix variants (`source:line:`,
  `line N:`, `instr N:`), and DiagWriter truncation. Coverage on
  `src/uvm.c` reaches 97% line.
- `tests/fuzz/fuzz_vm.c` — libFuzzer harness deserializing arbitrary
  bytes and executing any accepted module. 100K-iteration smoke run
  passes clean under ASan+UBSan.

### VM build and tooling

- New Make targets: `test-switch` (build with `-DURBI_VM_FORCE_SWITCH=1`
  for switch-dispatch CI parity) and `fuzz-vm` (libFuzzer harness
  build + run). `fuzz-build` aggregate extended to include `fuzz_vm`.
- `.clang-tidy` — `-clang-diagnostic-gnu-label-as-value` suppressed
  for the intentional GCC/Clang computed-goto extension.
- CI `host` job matrix extended with `test-switch`, bringing the matrix
  to 5 host modes.
- `docs/internals/design-decisions.md` — new entry explaining the
  uniform `UValue` tagged-struct decision across all targets.
- `docs/internals/architecture.md` — VM marked shipped; source table
  updated with `uvm.{c,h}` and `test_vm.c`.

### Documentation

- New `docs/` tree covering the first tranche of audience-A / audience-B
  / audience-C docs per the documentation-strategy design: `docs/README.md`
  (hub), `docs/language/getting-started.md`, `docs/internals/architecture.md`,
  `docs/internals/bytecode-format.md`, `docs/internals/opcodes.md`,
  `docs/internals/test-harness.md`, `docs/internals/design-decisions.md`.
  ~1500 lines of prose total.
- `docs/internals/test-harness.md` gains a Conformance fixtures section
  covering the `.chk` format, normalization rule, `make test-chk` entry
  point, and authoring flow.
- `docs/internals/test-harness.md` and `CONTRIBUTING.md`: acknowledge
  `make test-valgrind` as CI-gating (too slow for every commit, required
  before a milestone tag).
- `docs/internals/test-harness.md` Coverage expectations block replaces
  the stale `gcov`-on-debug-build language with `make coverage` (gcovr +
  HTML report + advisory CI job).
- `make docs-check` infrastructure + gating CI job: markdownlint-cli2
  over the `docs/` tree + intra-repo link-check.
- `.markdownlint.yaml` ships the ruleset (MD013 off; MD025/MD040/MD041
  on; neutral ordered-list-increment and heading-style).
- `WORKFLOW.md` §7 milestone ritual gains a "docs-for-this-release"
  step; §9 CHANGELOG cadence gains a `Documentation` subsection rule.

### Fixed

- `uvarint_decode_u` now rejects 10-byte encodings whose terminal-byte
  payload exceeds `0x01` as `UVARINT_OVERSIZE`. The previous code
  silently truncated values like `0x02..0x7F` at the 10th byte (payload
  bit shifts fall off the end of `uint64_t`), which could mask a
  corrupt bytecode module during loader verification. Paired
  positive-boundary test (`UINT64_MAX` at 10 bytes must succeed) added.
  The fix is defense-in-depth only — the loader verifier would have
  caught the resulting mis-decoded value downstream via `LOADK Bx`
  bounds or opcode range checks.
- `uvarint_size_zz` and `uvarint_write_zz` replace the
  implementation-defined `(v >> 63)` signed shift with a portable
  sign-extended mask built from `(v < 0)`. Equivalent on every
  mainstream compiler; defined by the C standard on all conforming
  implementations.

### Portability

- Compiler front-end compiles under `-ffreestanding` on toolchains without a C library (e.g. `gcc-riscv64-unknown-elf` on Ubuntu). `uarena_init` and the internal stdlib-backed allocator pair are guarded behind `__STDC_HOSTED__`; `uarena_alloc` uses a local byte-fill in place of `memset`. Freestanding callers must use `uarena_init_ex` or `uarena_init_static`.
- `umodule.c` and `uemit.c` follow the same freestanding discipline: local `module_zero` / `module_memcpy` / `module_memcmp` helpers in place of `<string.h>`, `stdlib_alloc` and `vsnprintf`-based `set_errmsg` guarded behind `__STDC_HOSTED__`, pluggable allocator on `UModule` via `UModuleAllocFn`. UModules hot-loaded in embedded builds (future M7) use caller-supplied allocators.

### Tooling

- Static-analysis Make targets: `tidy` (gating clang-tidy via `run-clang-tidy --warnings-as-errors='*'`), `tidy-fix` (local `--fix` convenience), `cppcheck` (advisory), `analyzer` (advisory GCC `-fanalyzer` in dedicated `build/host-analyzer/`), and `lint` aggregate.
- CI `lint` job runs all three analyzers parallel to host and cross-compile jobs. Advisory-ness of cppcheck and `-fanalyzer` lives in their Makefile targets' exit codes; CI job itself is gating.
- `.clang-tidy` disables `cert-err33-c`, `bugprone-easily-swappable-parameters`, and `readability-identifier-length` with per-check rationale comments — these stay disabled even if the broader check set is later expanded.
- Correctness-tooling Make targets: `coverage` (gcovr-backed coverage summary + HTML report at `build/host-coverage/report.html`), `test-valgrind` (memcheck-gated; catches uninitialized reads ASan misses), `fuzz-lex` and `fuzz-parse` (clang libFuzzer harnesses over lexer and parser, local-only). CI gains a gating `valgrind` job and an advisory `coverage` job; the advisory-to-gating promotion for coverage follows the cppcheck/analyzer pattern once the noise floor is known. Bench + profile harness deferred to M2-era paired work — see an internal backlog entry.

### Added

- Bytecode emitter walks AST nodes into a `UModule`: register-based instruction stream (byte-aligned 8/8/8/8 encoding), single tagged constant pool with linear-scan dedup, Lua-5.5-style delta-encoded synclines with absolute-line checkpoints, stack-discipline register allocator with destination-reuse. 8-opcode M1 set (`LOADK`, `MOVE`, `ADD`, `SUB`, `MUL`, `DIV`, `NEG`, `RET`). Reserved opcode slots 8–255 for M2+ additions (locals, control flow, calls, reactive primitives).
- `.urb` on-disk format: 24-byte header (magic `"URBI"` + 16·major+minor version + 6-byte FTP/paste-corruption canary + 8-byte flavor descriptor) followed by varint-delimited sections (metadata, constants, 4-byte-aligned instruction stream, delta synclines). Per-target flavor pinned at compile time (`URBI_INT_WIDTH` / `URBI_FLOAT_TYPE` / `URBI_INSTR_WIDTH` / `URBI_ENDIANNESS`); loader refuses mismatches with field-specific diagnostics.
- Loader verifier sweep after byte-level decode: opcode range, register range, `LOADK` Bx bounds, terminal `OP_RET`, abs-line pc monotonicity, 4-byte instruction alignment. `OP_RET` B operand and `OP_MOVE`/`OP_NEG` C operand intentionally not enforced (unused bytes, no runtime effect).
- UEmitter and module APIs in new headers `uemit.h` / `umodule.h`: `UEmitter` accumulator (init / statement / finish), `UModule` struct, `umodule_deserialize`, `umodule_serialize`, `uemit_disassemble`, error-name tables. Compiler-internal — `urbi.h` unchanged.
- Streaming Pratt parser consumes the lexer's token stream and produces one `UAstNode` per statement (integer literal, identifier, unary, binary, error). Recursive-descent statements + precedence climber for `+ - * /` with parens, unary `+ -` (plus is parse-time no-op), panic-mode recovery via `|`, in-stream `AST_ERROR` nodes, OOM sentinel path. Public parser API in `uparse.h`: `UParser`, `uparse_init`, `uparse_next_statement`, `uparse_error_name`.
- Internal chunk-list bump-allocator arena (`uarena.h` / `uarena.c`) backing the AST and emit arenas. Three init variants — `uarena_init` (stdlib), `uarena_init_ex` (pluggable allocator for embedded), `uarena_init_static` (fixed caller buffer for freestanding) — plus `uarena_alloc`, `uarena_reset`, `uarena_destroy`. No copy between chunks; pointers stable across growth.
- Lexer scans integer literals (decimal, hex, binary, octal with underscores), identifiers, single-character operators (`+ - * /`), parentheses, and the statement separator `|`. Full synclines on every token.
- Structured lexer error codes: unknown character, unterminated block comment, ambiguous leading zero, empty radix, malformed hex/binary/octal, leading/trailing/adjacent underscores, integer overflow.
- Public lexer API in new header `ulex.h`: `UToken`, `ULexer`, `ulex_init`, `ulex_next`, `ulex_token_name`. No allocation; caller owns source buffer.

### Refactoring

- Added the `U` prefix to every public struct and enum in the source tree so
  host embedders can include any header without type-name collisions. `Lexer`
  → `ULexer`, `Token` → `UToken`, `TokenType` → `UTokenType`,
  `LexErrorCode` → `ULexError`, `Parser` → `UParser`,
  `ParseErrorCode` → `UParseError`, `AstNode` → `UAstNode`,
  `AstKind` → `UAstKind`, `UnaryOp` → `UAstUnaryOp`,
  `BinaryOp` → `UAstBinaryOp`, `Arena` → `UArena`,
  `ArenaChunk` → `UArenaChunk`, `Emitter` → `UEmitter`,
  `EmitError` → `UEmitError`, `AbsLine` → `UAbsLine`. Error-type suffix
  normalized: `LexErrorCode` and `ParseErrorCode` drop the redundant `Code`
  suffix to match the existing `UVMError`/`UEmitError` pattern. Enum tag
  values (`TOK_*`, `AST_*`, `LEX_*`, `PARSE_*`, `EMIT_*`, `OP_*`, `UOP_*`,
  `BOP_*`) are unchanged — they are namespaced by prefix already, and
  renaming them risks cross-family collisions (e.g. `UOP_*` already denotes
  unary-op values, so opcode values can't take the same prefix). `src/uvarint.h`'s
  include guard normalized from `URBI_UVARINT_H` to `UVARINT_H` to match
  every other header.
- Bytecode `Chunk` renamed to `UModule` across the codebase, and the
  `src/uchunk.{c,h}` + `tests/unit/test_chunk.c` module renamed to
  `src/umodule.{c,h}` + `tests/unit/test_module.c`. The type is the
  compilation-unit record — instructions + constants + synclines +
  metadata — so the new name reflects what it actually is. `UChunkLoadError`
  → `UModuleLoadError`, `UChunkAllocFn` → `UModuleAllocFn`, `uchunk_*`
  → `umodule_*`. The `ULOAD_*` error tags and the arena's internal
  chunk-list terminology are unchanged.
- `UConst` renamed to `UValue` across sources, tests, and internals docs. The
  type has always been the universal tagged-value cell — constants-pool entry,
  register-frame slot, arithmetic operand, `uvm_run` result — so the new name
  reflects what it actually is. The `UValKind` enum and `UVAL_*` tags are
  unchanged (they already wore the `val` prefix); `uconst_to_double` /
  `uconst_set_float` become `uvalue_to_double` / `uvalue_set_float`.
- LEB128 varint encode/decode extracted into a standalone freestanding module
  `uvarint.{c,h}` with its own error enum (`UVarintError`). `umodule.c` now
  consumes it via two translation wrappers that map `UVarintError` into
  `UModuleLoadError` at the boundary; `uemit.c` drops the four private `static`
  varint helpers and consumes the module directly. The test-only header
  `src/umodule_internal.h` is retired; varint coverage moves into a new
  `test_varint_suite` (11 cases) that exercises encode and decode directly,
  replacing the indirect serialize→deserialize-only encode coverage of prior
  state.

### Foundation

- Header-only test harness `utest.h` (zero dependencies, pure C99)
- Make targets: `test`, `test-asan`, `test-ubsan`, `test-debug`, `cross-arm`, `cross-riscv`
- GitHub Actions CI covering host (debug/release/ASan/UBSan) plus ARM Cortex-M7 and RISC-V rv32imc cross-compiles
- Initial placeholder API: `urbi_version()`

### Build system

- Per-TARGET build directories: all variants (release, debug, sanitizers, cross-compiles) land in `build/<TARGET>/` and coexist without requiring `make clean` between them
- `make all` as the default target
- `make compile_commands.json` — generates a clangd-compatible compilation database for LSP-based editors

### Developer environment

- `.editorconfig` — universal indent, newline, and charset rules
- Extended `.gitignore` covering editor state (JetBrains, VS Code, Vim, Emacs, Sublime, TextMate), tag databases (ctags, cscope, GNU Global), and IDE indexing artifacts (`compile_commands.json`, `.cache/`)
- `CONTRIBUTING.md` documents test modes, cross-compile, indexing database, and TARGET convention
