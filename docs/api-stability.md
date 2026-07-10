# C API stability policy

> Status: ABI pin at 0/23/4 (v0.13.3-scheduler-liveness PATCH — scheduler liveness + reactive correctness wave, zero public-surface change, NOT an escape; behavior-change note: `urbi_step` QUIESCENT now excludes armed/suspended/waiting work; `every()` bare-float argument = seconds + fixed/overrun cadence; `at sync` atomic-body `;`-sequence runs to completion; 0/23/3 = v0.13.2-gc-soundness PATCH — internal GC-soundness wave, zero public-surface change, NOT an escape; 0/23/2 = v0.13.1-unwind-and-frontend PATCH — unwind + frontend correctness wave, zero public-surface change, NOT an escape; 0/23/1 = v0.13.0-test-honesty PATCH — test/build/CI honesty wave, zero public-surface change, NOT an escape; previous pin 0/23/0 = escape #33, the M10 B6a API-freeze sweep:
> `urbi_set_time_us` renamed to `urbi_set_clock_fn`, internal symbols hidden
> via `-fvisibility=hidden`, bidirectional manifest gate).  MINOR bump from
> v0.12.4-stdlib-completeness (0/22/2).  Wire format unchanged at v1.9 / 0x19.
> Shipped on `main` at v0.12.5-m10-prep ahead of the v0.13.x pre-release
> hardening arc; the 1/0/0 freeze lands at the end of that arc.  See §6
> escape ledger (#33).  The freeze pin is a forcing function (deliberate
> intent at every bump), not a hard cap.  Any
> further MINOR/MAJOR change after this tag must follow §3.

## 1. The freeze pin

`include/urbi/version.h` contains a `_Static_assert` pinning the public
ABI to the current `URBI_API_VERSION_*` triple in include/urbi/version.h.
Any change to the public C API after v0.10.6-stabilization
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

## 5. Argument count types

The public C API uses two calling conventions for argument counts,
depending on the callback type.

**`urbi_native_method_fn` (internal native-method protocol):**

```c
typedef int (*urbi_native_method_fn)(struct UVM *vm,
                                     UValue self,
                                     UValue *args,
                                     uint8_t nargs,
                                     UValue *out);
```

The argument count is `uint8_t nargs` (range 0–255).  This matches
urbiscript's internal arity limit.  Functions registered via
`urbi_install_native_methods` use this signature.

**`UHostFn` (host-callable function protocol):**

```c
typedef UValue (*UHostFn)(struct UStrand *s, int argc, UValue *argv);
```

The argument count is `int argc`.  This matches the POSIX `main()`
convention and is used by `urbi_make_native_closure` and
`urbi_strand_call_host`.

**Which to use for new code:**

- Registering methods on a script-visible prototype: use
  `urbi_native_method_fn` / `urbi_install_native_methods`.
- Wrapping a C function as a first-class script callable: use
  `UHostFn` / `urbi_make_native_closure`.

The two signatures are not interchangeable.  The `uint8_t` truncation
in `urbi_native_method_fn` is intentional: the bytecode CALL opcode
packs argument count into a single byte, so nargs ≤ 255 by construction.

## 6. References

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

### Escape #28 — v0.11.4-cat-f (PATCH)

Catchable structured exceptions.  VM-internal error sites that previously
fatal-HALTed the strand or printed diagnostic strings to stderr now raise
catchable typed `Exception` instances: the native-method raise helpers
(`urbi_raise_type` / `_arity` / `_oom` / `_lookup`) and the 6 slot-access
TypeError sites (getter/setter not-a-closure / raised, slot-not-found,
slot-write-failed).  A new internal helper clones a cached Exception-subclass
proto and binds a `message` slot; the cached
`vm->{typeerror,arityerror,lookuperror,oomerror}_proto` are resolved by name
after the stdlib bake (mirroring the channel-proto pattern) and added to the GC
root set, and the strand's `catch_value` is added to the strand GC root walker
(a freshly-cloned thrown instance was being collected between unwind-bind and
the catch handler).  Four new `Exception` subclasses: `RuntimeError`,
`SchedulingError`, `SyntaxError`, `OutOfMemoryError`.  Uncaught typed throws at
top level now print a `!!! <message>` diagnostic (reading the instance's
`message` slot) instead of bare `nil`; string/scalar throws unchanged.  Plus two
test-only chk host-driver directives (`## host: advance-clock` /
`## host: expect-host-call`) in `tests/integration/chk_host_driver.c`.  The
structured-throw work added two internal exported symbols (`urbi_raise_typed`,
`urbi_exception_subclass_protos_resolve`) already manifested in Tier 4 of
`docs/api-surface-tiers.md` — internal-leak, not public surface.  **Zero new
public C API symbols**, so PATCH.  No new opcodes; wire unchanged at v1.9 / 0x19.
PATCH-only, not a §3 freeze-override.

### Escape #29 — v0.12.0-ros-foundation (MINOR)

Optional ROS2 bridge foundation.  Three new public C API symbols declared in
`include/urbi/ros.h`, all compile-gated behind `URBI_ENABLE_ROS2`:
`urbi_ros_register` (allocates + installs the `ros` native namespace proto on
the VM), `urbi_ros_register_globals` (binds `ros` as a realm global),
`urbi_ros_pump` (drains the transport incoming queue once per `urbi_step` and
emits events).  The bridge is a self-contained optional component; the core VM
has zero reference to it.  Per the v0.11.0-trace-spine precedent, new public
symbols behind a compile gate count as MINOR.  Wire format unchanged at
v1.9 / 0x19; no new opcodes.  29th use of pre-v1.0 escape clause.
MINOR bump: 0/20/4 to 0/21/0.  §3 freeze-override.

### Escape #30 — v0.12.1-ros-dds (PATCH)

Real rcl/rclc/Fast-DDS ROS2 transport behind `URBI_ROS_BACKEND=rcl`.  NO new
public C API symbols: the rcl backend (`src/ros/uros_rcl.c`), the
rosidl-targeting codegen mode (`tools/urbi-rosgen.py --target rcl`), the
internal List C-builder (`src/value/ulist_build.h`), and the object-based
`URosTransport` seam revision are all internal — `include/urbi/ros.h` is
unchanged.  Wire format unchanged at v1.9 / 0x19; no new opcodes.  30th use of
pre-v1.0 escape clause.  PATCH bump: 0/21/0 to 0/21/1.  PATCH-only, not a §3
freeze-override.

### Escape #31 — v0.12.2-urobotics (MINOR)

Optional Standard Robotics API facet overlay (`URBI_ENABLE_UROBOTICS`).  Two new
public C API symbols in the new gated header `include/urbi/urobotics.h`:
`urbi_urobotics_register` and `urbi_urobotics_run`.  Both exist only in
`URBI_ENABLE_UROBOTICS` builds (absent from the default gate-off `liburbi.a`) and
are called only from stdlib boot + realm-globals population, never by an
embedder — but per the v0.11.0-trace-spine / v0.12.0-ros-foundation precedent
(escape #24 / #29), new public symbols behind a compile gate count as MINOR.
The `Robotics` facet namespace itself is pure urbiscript baked into a separate
bytecode blob (`urbi_urobotics_bytecode`); the core VM has zero reference to it.
Wire format unchanged at v1.9 / 0x19; no new opcodes.  31st use of pre-v1.0
escape clause.  MINOR bump: 0/21/1 to 0/22/0.  §3 freeze-override (consistent
with escape #29's gated-public-symbol MINOR ruling).

### Escape #32 — v0.12.3-ros-demo-and-contract (PATCH)

Facet↔ROS2 binding contract.  NO new public C API symbols: the binding
(`Robotics.bindInput` / `bindOutput`) is pure urbiscript baked into the urobotics
overlay blob, gated on both `URBI_ENABLE_UROBOTICS` and `URBI_ENABLE_ROS2`.  The
two new natives (`ros.__injectMsg` / `ros.__lastPublished`) are mock-only gated
test hooks living inside the ROS2 component, not in `include/urbi/` — they are
absent from the default gate-off build and from the public manifest
(`docs/api-surface-tiers.md`).  Wire format unchanged at v1.9 / 0x19; no new
opcodes.  32nd use of pre-v1.0 escape clause.  PATCH bump: 0/22/0 to 0/22/1.
PATCH-only, not a §3 freeze-override.

(v0.12.4-stdlib-completeness bumped 0/22/1 → 0/22/2 PATCH — NOT an escape: no
public C symbol change, stdlib-only.  No ledger entry by design.)

### Escape #33 — v1.0.0 / M10 (B6a) (MINOR) — FINAL escape before the freeze

Two freeze-window ABI changes, deliberately landed under the escape clause
immediately before the 1/0/0 freeze (after the freeze, each would need a
deprecation cycle):

1. **Rename `urbi_set_time_us` → `urbi_set_clock_fn`.**  It installs a
   `urbi_time_us_fn` clock-source callback, but the `_us` suffix read like an
   integer setter, violating the `_fn` convention of `urbi_set_wake_fn` /
   `urbi_set_isr_check_fn`.  A remove+add at the public surface.  The callback
   typedef `urbi_time_us_fn` is unchanged.

2. **Export-surface narrowing via `-fvisibility=hidden`.**  The library is now
   compiled with `-fvisibility=hidden`; the `include/urbi/*.h` public headers are
   wrapped in `#pragma GCC visibility push(default)` (and `URBI_PUBLIC` is
   defined for explicit re-export).  Result: the ~423 internal cross-TU symbols
   (Tier 4 in `docs/api-surface-tiers.md` — `dispatch_loop_until_yield`,
   `consume`, the stdlib/parse/vm helpers, etc.) no longer escape when an
   embedder links `liburbi.a` into a shared object; only the ~105 documented
   public `urbi_*` symbols (Tier 1/2/3) carry default visibility.  Static linking
   into an executable (the dominant embedded case) is unaffected — hidden symbols
   still resolve at static-link time.  NOTE: `liburbi.a` is not built `-fPIC`, so
   producing a `.so` from it as-shipped still requires an embedder rebuild; the
   narrowing takes effect at that rebuild.  `nm` still lists internal symbols
   (visibility ≠ binding), so the `test-api-manifest` forward gate is unchanged;
   that gate also gained a bidirectional check (Tier-1/2 surface must not shrink).

Wire format unchanged at v1.9 / 0x19; no new opcodes.  33rd and FINAL use of the
pre-v1.0 escape clause.  MINOR bump: 0/22/2 → 0/23/0.  The next bump is the
1/0/0 freeze, after which §3 (no break without MAJOR + deprecation) governs.

(v0.13.0-test-honesty through v0.13.3-scheduler-liveness bumped 0/23/0 → 0/23/4
in four PATCH steps — NOT escapes: all changes were internal-only or script-side,
zero new public C API symbols in any of the four tags.  No ledger entries by
design.)

(v0.13.4-error-surfacing bumped 0/23/4 → 0/23/5 PATCH — NOT an escape.
One new `UErrCode` enumerator: `URBI_ERR_UNCAUGHT_THROW = -18`.  This fills the
previously-reserved slot -18 in the enum.  The addition is a pre-freeze
sanction: `urbi_vm_run`, `urbi_run_chunk`, and the CLI `-e`/file entry points
now return this code when a script throw is uncaught; the interactive REPL's
nil-recovery path is unchanged.  No new public C functions; wire format unchanged
at v1.9 / 0x19.  ABI triple after this tag: 0/23/5.)

(v0.13.5-conformance-and-stdlib bumped 0/23/5 → 0/23/6 PATCH — NOT an escape.
No new public C API symbols, no new opcodes, wire format unchanged at v1.9 /
0x19.  Tag 6 of the v0.13.x pre-release hardening arc: legacy-conformance pass
(truthiness, statement-operand fold, unbraced bodies, switch default, default
parameters, universal asString, String %, compat aliases, List.sort(comparator),
typed exceptions from div-by-zero, RegExp budget, emit diagnostics with
positions, message polish, tag-watcher persistence, REPL line-cap).  ABI triple
after this tag: 0/23/6.)

(v0.13.6-consistency bumped 0/23/6 → 0/23/7 PATCH — NOT an escape.
No new public C API symbols, no new opcodes, wire format unchanged at v1.9 /
0x19.  Tag 7 of the v0.13.x pre-release hardening arc: internal-consistency
pass (cross-TU internals namespaced under `urbi_` — two internal-leak allowlist
rows removed; duplicated internals consolidated; dead code removed with UStrand
3920 → 3912; fork-operand register-allocation fix; Boolean/Nil asString; unified
positioned division/modulo error prefixes; comment/doc truthfulness program +
source-comment lint gate; internals-docs accuracy sweep; GC rooting-matrix
additions; embedded footprint preset + per-port stack-cap knob + firmware size
gate; bytecode-verifier and emitter control-flow-arm decompositions).  The XC-04
public shims are untouched — deprecation banner comment only.  ABI triple after
this tag: 0/23/7.)
