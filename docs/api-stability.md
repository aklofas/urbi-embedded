# C API stability policy

> Status: ABI pin at v0.11.3-memory-debug (0/20/3) — PATCH bump from
> v0.11.2-host-tooling (0/20/2).  27th use of pre-v1.0 escape clause.
> Fourth and final tag of the v0.11.x tooling arc — on-target memory debugging
> behind the new URBI_MEM_DEBUG compile gate (off by default, zero bytes when
> off): allocation owner-tagging on the existing GC sidecar, trailing redzones,
> poison-on-free + freed-cell quarantine, heap-lock violation recording, and
> host-handle/pin leak detection.  Surfaced GDB-first (a full urbi-heap cell
> walk plus urbi-allocs and urbi-leaks) and one Debug.memCheck() script trigger.
> All new functions are internal; zero new public C API symbols, so PATCH.  No new
> opcodes, wire unchanged.
> PATCH-only, not freeze-override
> under §3 (the ledger numbers every bump; only MINOR/MAJOR bumps
> require §3's freeze-override review).  The freeze pin is a forcing
> function (deliberate intent at every bump), not a hard cap.  Any
> further MINOR/MAJOR change after this tag must follow §3.

## 1. The freeze pin

`include/urbi/version.h` contains a `_Static_assert` pinning the public
ABI to `0/19/3`.  Any change to the public C API after v0.10.6-stabilization
either bumps the macros AND the assert in lockstep (deliberate intent), or
fails to compile.

The freeze does NOT mean "no more ABI changes ever before v1.0."  It means
each change must be deliberate and enumerated.

## 2. What counts as a breaking change

Per the leading comment in `version.h`:

- **MAJOR bump:** removed function, changed signature, removed/renumbered
  enum value, struct-layout change visible across the boundary, removed
  `URBI_ERR_*` slot.
- **MINOR bump:** additive — new function, new enum value appended at the
  next free index, new `URBI_ERR_*` slot, new build flag.
- **PATCH bump:** bug fix only, no header change.

Pre-v1.0 escape clause: while `URBI_API_VERSION_MAJOR == 0`, MINOR or PATCH
bumps MAY break ABI per standard semver convention.  Each bump enumerates
breakages in CHANGELOG.

## 3. Post-freeze breaking change policy

To break the freeze after v0.10.6-stabilization:

1. **CHANGELOG.md entry** — add a numbered "post-freeze break" entry under
   the new tag's section.  Cite this document and explain why the break
   is necessary.
2. **Static-assert bump** — update the expected values in version.h's
   freeze-pin assert.  The build will not compile until this is done.
3. **Macros bump** — update `URBI_API_VERSION_MAJOR/MINOR/PATCH` per §2.
4. **Code review** — at least one reviewer must explicitly OK the freeze
   break.

## 4. What v1.0 promises

The v1.0 ABI promise — once `URBI_API_VERSION_MAJOR` increments to 1 —
is full semantic versioning per §2.  MAJOR breaks require a deprecation
cycle.  MINOR adds may not remove or rename anything.  PATCH bumps must
be header-stable.

The pre-v1.0 escape clause expires at v1.0.0.

## 5. References

- `include/urbi/version.h` — the freeze pin + macros + history.
- `CHANGELOG.md` — per-tag enumeration of breaking changes.
- `docs/api-surface-tiers.md` — public/experimental/advanced tier
  classification of the API surface.

## 6. Post-freeze escape ledger

Summary of every ABI change since the v0.10.6-stabilization freeze pin.
Detail in `include/urbi/version.h` ledger comment and `CHANGELOG.md`.

### Escape #16 — v0.10.6-stabilization (MINOR — freeze-pin tag)

UReplConfig gains `rate_limit_per_second` field (struct-layout change,
additive at tail).  Symbolic freeze pin; declared "16th and FINAL" at
time of writing.  0/17/0 to 0/18/0.

### Escape #17 — v0.10.9-tag-state (MINOR — first post-freeze break)

4 new public C API symbols: `urbi_tag_block` / `_unblock` / `_freeze` /
`_unfreeze`.  UStrand gains `unblock_value` field (+16 B).  UTag gains
`UTAG_FLAG_BLOCKED` (0x04).  D1 SUSPENDED-machinery ratification.
Supersedes "16th and FINAL" framing.  0/18/2 to 0/19/0.

### Escape #18 — v0.10.10-job-introspection (PATCH-only)

D7 full-ship Cat. E ratification: Job proto, detach/disown, scopeTag,
Lobby.connectionTag.  All script-side; zero new public C API symbols.
0/19/0 to 0/19/1.  Recorded for ledger completeness; NOT a
freeze-override under §3.

### Escape #19 — v0.10.11-channel-and-isA (PATCH-only)

D6 + isA + D5 Cat. E ratification, plus bundled v0.10.10 carry-overs
(-MMD -MP auto-deps + test_repl_stop_path UAF fix).  Three new
urbiscript surface constructs (Channel proto, `<<` infix, isA method)
plus one policy change (Object atom proto unfrozen) — all script-side,
no new public C API symbols.  PATCH bump only: 0/19/1 to 0/19/2.
Recorded for ledger completeness; this is NOT a freeze-override
under §3.

### Escape #20 — v0.10.12-cat-e-activation (PATCH-only)

Final tag of the 4-tag Cat. E ratification arc.  Fixture-and-doc-
only tag: D2 cross-spec at→event chain activation (W1), at.sync
keyword normalization (W2), Cat. E close-out (W3).  No new public
C API symbols — only the ABI macro bump.  PATCH bump: 0/19/2 →
0/19/3.  Recorded for ledger completeness; this is NOT a freeze-
override under §3.

### Escape #21 — v0.10.13-hygiene (PATCH-only)

Post-Cat. E hygiene + one targeted runtime bug fix.  Two parallel
worktrees: W2 bundled markdownlint MD004 per-file override for
CHANGELOG, `make all` dep on the urbi CLI binary (closes the
v0.10.7-H stale-binary trap), and a `String.asString` stdlib overlay
(closes v0.10.11-A); W3 suppressed the slot-change emit on first
slot-install (closes v0.10.7-C — install is creation, not change).
The only runtime semantic change is W3's two-site suppression:
removed the direct emit in `urbi_object_set_local_slot` Case 2 (leaf-
shape-add path), and added a shape-snapshot gate in `vm_setslot_slow`
that suppresses emit when `recv->shape` changes (= new local slot
installed via the COW or miss-install path).  No new public C API
symbols.  Two new chk fixtures (`string_asstring`,
`slot_change_no_install_emit`), one replaced unit test.  PATCH bump:
0/19/3 → 0/19/4.  Recorded for ledger completeness; this is NOT a
freeze-override under §3.

### Escape #22 — v0.10.14-prerc-infra (PATCH-only)

First tag of the pre-v1.0-rc stabilization arc.  Three file-isolated
worktrees: W1 corrected the stale flat-layout claims in `docs/STYLE.md`;
W2 reworked the REPL reader-thread output flush from an EAGAIN
`nanosleep` spin to a per-session staging buffer + `POLLOUT`-driven
retry (a teardown-latency/liveness fix — the prior loop did not drop
bytes), with a new `test_repl_backpressure.c` regression suite; W3
added a C `.chk` host-driver (`tests/integration/chk_host_driver.c`)
with `## host: realm`/`run`/`step` directives over the existing public
embedding API, activating 5 previously-blocked scheduler/multi-realm
conformance fixtures.  No new public C API symbols — all changes are
internal (REPL), test-side (host-driver), or documentation.  PATCH
bump: 0/19/4 → 0/19/5.  Recorded for ledger completeness; this is NOT
a freeze-override under §3.

### Escape #23 — v0.10.15-vm-decomp-2 (PATCH-only)

Final tag of the pre-v1.0-rc stabilization arc.  Internal VM dispatch
extraction round 2: `OP_PUSH_TAG`/`OP_POP_TAG` moved into
`src/vm/uvm_tag_scope.{c,h}` and the seven reactive-install opcodes into
`src/vm/uvm_reactive_install.{c,h}`, behavior-preserving under a per-stage
zero-delta gate (`uvm.c` 1785 → 1428 LOC).  Two tag/unwind semantic fixes
landed on the extracted seam: `OP_PUSH_TAG` now binds the `t:` scope to the
user tag in `R[tag_reg]` (closes design-risks v0.10.9-B), and `tag.stop()`
inside `try`/`finally` runs the finally during the TAG_STOP unwind (closes
design-risks v0.10.7-B, latent-fixed by the binding).  A new cleanup-entry
flag `FLAG_TAG_USER_OWNED` is internal (`src/runtime/ucleanup.h`), not public.
No new public C API symbols; no new opcodes; wire unchanged at v1.9 / 0x19.
PATCH bump: 0/19/5 → 0/19/6.  Recorded for ledger completeness; this is NOT
a freeze-override under §3.

### Escape #24 — v0.11.0-trace-spine (MINOR)

First tag of the v0.11.x tooling arc.  Adds a new public (EXPERIMENTAL)
header `include/urbi/trace.h`: the trace control/drain/stats API
(`urbi_trace_set_level` / `urbi_trace_get_level` / `urbi_trace_set_level_all` /
`urbi_trace_snapshot` / `urbi_trace_stats` / `urbi_trace_channel_name`,
`utrace_format` / `urbi_trace_flush_to_writer`) plus the `URBI_TP` tracepoint
macro family and bring-up primitives.  The subsystem is compile-gated by
`URBI_TRACE` (default off ⇒ zero text/`.bss`, byte-identical `struct UVM`); the
control API is linkable in both modes via no-op stubs.  Tracepoints are
instrumented across scheduler / GC / watcher / event / tag / REPL lifecycle;
`Debug.trace("…")` adds a script-side USER-channel marker (REPL-gated).
MINOR bump: 0/19/6 → 0/20/0 — new public surface.  No new opcodes; wire
unchanged at v1.9 / 0x19.  EXPERIMENTAL: the trace API may change before v1.0.

### Escape #25 — v0.11.1-perf-counters (PATCH)

Second tag of the v0.11.x tooling arc.  Adds VM-domain performance counters
behind the `URBI_PERF_COUNTERS` compile gate: opcodes retired, calls/returns,
slot get/set, IC hit/miss, native calls, scheduler context-switches/yields/
blocks, watcher install/fire, and event emits.  Default off ⇒ `URBI_PERF_INC`
is `(void)0` and the gated `UPerfCounters` field is absent from `struct UVM`
(zero hot-path cost, zero `UVM` delta).  Always-on GC cycle/slice counts plus
cycle timing (`gc_cycles` / `gc_slices` / `last_gc_us` / `total_gc_us`) feed
`Debug.gc()`; the deliberately-stubbed `Debug.profile()` seam is filled with the
counters object + `epoch` (or `counters:null` when the gate is off), and a new
`Debug.profileReset()` zeroes the counters + bumps the epoch.  ALL counters and
GC timing are excluded from `urbi_get_determinism_checksum` (proven by the
`test-determinism-perf` 100-run gate).  The counters live in the internal header
`src/runtime/uperf.h` and are surfaced only through `Debug.*` script methods —
**zero new public C API symbols**, so PATCH.  No new opcodes; wire unchanged at
v1.9 / 0x19.  PATCH-only, not a §3 freeze-override.

### Escape #26 — v0.11.2-host-tooling (PATCH)

Third tag of the v0.11.x tooling arc.  Host-side tooling only: a Python
Perfetto/Chrome-Trace decoder (`tools/urbi-trace-decode.py`) for the binary
`URBT` trace dump, GDB pretty-printers + heap/strand/handle/trace walkers
(`tools/gdb/urbi.py`, including a one-shot `urbi-dump`), a `--trace`/`--trace-out`
capture path on the `urbi` CLI (built via `make urbi-trace` with `-DURBI_TRACE=1`),
and a `--dump-on-fatal` best-effort host dump.  The CLI flags use only
already-exported public symbols (`urbi_trace_set_level` / `_set_level_all` /
`_snapshot` / `_stats` / `_channel_name` plus `urbi_repl_eval` /
`urbi_realm_global`); the decoder and GDB scripts are host artifacts that link
nothing.  Also corrected the long-standing `UTraceRecord` "24-byte" comment
drift to its real 32-byte size (comment-only — the `uint64_t ts_us` aligns the
struct to 8 bytes, so the 28 named bytes pad to 32).  **Zero new public C API
symbols**, so PATCH.  No new opcodes; wire unchanged at v1.9 / 0x19.  PATCH-only,
not a §3 freeze-override.

### Escape #27 — v0.11.3-memory-debug (PATCH)

Fourth and final tag of the v0.11.x tooling arc.  On-target memory debugging
behind the new `URBI_MEM_DEBUG` compile gate (off by default, zero bytes when
off — `UAllCellsNode` and `UVM` are byte-identical): allocation owner-tagging on
the existing `UAllCellsNode` GC sidecar (sequence, bytecode PC + opcode, C return
address, strand id), trailing redzones, poison-on-free + freed-cell quarantine
(use-after-free detection where ASan/Valgrind can't run), heap-lock violation
recording, and host-handle/pin leak + double-release detection.  Surfaced
GDB-first (`tools/gdb/urbi.py` gains a full `urbi-heap` live-cell walk plus
`urbi-allocs` and `urbi-leaks`, reading the owner sidecar + handle-owner array)
plus one `Debug.memCheck()` script trigger (graceful note when built without the
gate).  All new functions are internal (`umemdbg_*` in `src/runtime/umemdebug.c`,
`urbi_gc_mem_validate` / `urbi_gc_count_pinned` in `src/gc/ugc_incremental.c`);
the substate is a lazily-heap-allocated `+8 B` UVM pointer in debug builds only.
The memdbg state is excluded from `urbi_get_determinism_checksum` (proven by the
new `test-determinism-memdebug` 100-run gate, which also serves as the
UVM-layout-perturbation canary).  **Zero new public C API symbols**, so PATCH.
No new opcodes; wire unchanged at v1.9 / 0x19.  PATCH-only, not a §3
freeze-override.
