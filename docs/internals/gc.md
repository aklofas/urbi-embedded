# Garbage collector

This document describes the garbage collector that ships in `urbi-embedded`:
its strategy, the `gc_byte` bit layout, the write-barrier protocol, the
safe-point discipline that drives slices, the strand-walker contract that the
mark phase relies on, and the inventory of GC-managed cell types.

For higher-level context see [architecture.md](architecture.md). The GC walker
contract is also discussed from the scheduler side in
[scheduler-design.md](scheduler-design.md). Cell layouts referenced here are
detailed in `object-model.md` (object cells) and `reactive-runtime.md`
(`UEvent`, `UChangedNode`, the `UTag` promotion).

## Strategy

The default strategy is **tri-color incremental mark-sweep**, selected by
compiling with `URBI_GC == URBI_GC_INCREMENTAL` (the default). The
implementation lives in `src/gc/ugc_incremental.{c,h}` and uses the classic
two-white scheme: `vm->current_white` alternates between `UGC_COLOR_WHITE0`
(`0x00`) and `UGC_COLOR_WHITE1` (`0x01`) at the end of each cycle, so every
existing cell becomes "other white" (i.e. dead unless re-marked) at the start
of the next cycle without a separate clear pass.

A second strategy, `URBI_GC_NONE`, is maintained as a no-op smoke build so
that the strategy-router include path and the three barrier surfaces stay
honest under "no GC at all" configurations. Its header
`src/gc/ugc_none.h` defines all barrier inlines as no-ops and zeroes every
`gc_byte` flag. The smoke build is exercised in CI via
`make test-gc-none-build`, which is part of `make releasetest`. A real
freeze-arena allocator is deferred; today `URBI_GC_NONE` is compile-only.

Reserved-but-not-implemented values `URBI_GC_GENERATIONAL` and
`URBI_GC_ARENA_PER_TAG` are defined in `src/gc/ugc.h` for future expansion.

## `gc_byte` bit layout

Every GC-managed cell starts with a 2-byte `UCell` header:

```c
typedef struct UCell {
    uint8_t  type_tag;   /* offset 0 */
    uint8_t  gc_byte;    /* offset 1 — strategy-private */
} UCell;
```

Concrete cell types embed `UCell` as their first struct member, so the cast
from a typed cell pointer to `UCell *` is well-defined.

Under `URBI_GC_INCREMENTAL` all eight bits of `gc_byte` are claimed
(`src/gc/ugc_incremental.h`):

| Bit(s) | Macro | Meaning |
|--------|-------|---------|
| `[1:0]` | `UGC_COLOR_MASK` | Tri-color mark: `WHITE0`, `WHITE1`, `GRAY`, `BLACK` |
| `2` | `UGC_HAS_FINALIZER` | Mirrors `UType.flags & TYPE_HAS_FINALIZER` for hot-path sweep |
| `3` | `UGC_IS_WEAK` | Reserved; weak references not implemented at v1.0 |
| `4` | `UGC_IS_PINNED` | Cell exempt from sweep (host-pinned value) |
| `5` | `UGC_IS_FIXED` | Pool-managed cell; never freed by sweep |
| `6` | `UGC_HAS_WATCHER_OBSERVER` | Object has at least one watcher in the read-set |
| `7` | `UGC_HAS_SLOT_CHANGE_EVENT` | At least one slot has a slot-change subscriber |

Bit 7 is the most recent allocation and currently fills the byte. Any new
GC-bit allocation will need a wider header field; do not silently steal bits
from `UGC_IS_WEAK` (bit 3) without first deciding whether weak refs ship at
v1.x as planned.

Helpers `urbi_gc_set_color`, `IS_DEAD`, `IS_BLACK`, `IS_WHITE`, `IS_GRAY` live
alongside the layout macros in `ugc_incremental.h`.

## Write-barrier protocol

The barrier is a **forward (Dijkstra) shade-the-target** barrier. When a
black parent is about to point at a white child, the child is shaded gray so
the mark phase will visit it before sweep.

There are three barrier surfaces, all defined as `static inline` in
`src/gc/ugc_incremental.h` (and as no-ops in `src/gc/ugc_none.h`):

- `urbi_gc_slot_store(vm, parent, key, dst, child)` — object slot or realm
  namespace store; performs the barrier and the store. The barrier-only
  variant `urbi_gc_slot_pre_store(vm, parent, key, child)` exists for
  callers whose store target is not a `UValue *` or whose store already
  happened; those callers perform the actual store themselves. Both combine
  the GC barrier with the watcher dirty-set hook (`observer_dirty`) when
  `UGC_HAS_WATCHER_OBSERVER` is set on the parent. The post-store deferred
  slot-change emit is also wired here when bit 7 is set.
- `urbi_gc_register_write(vm, strand, reg_idx, child)` — strand register
  store. **Intentionally a no-op:** registers are roots, walked at
  `MARK_ROOTS` *and re-walked at `ATOMIC_FINISH` with the mutator stopped*
  (refactor-3 GC-02). The atomic-phase full root-provider re-scan — not a
  per-write barrier — is the soundness mechanism: a white value stored into
  an already-scanned register is re-discovered before `SWEEP`.
- `urbi_gc_upvalue_pre_store(vm, cell, child)` — barrier-only hook for
  heapified-upvalue stores (`OP_SETUPVAL`'s on-heap arm and
  `vm_close_upvalues`); the caller performs the store. The Dijkstra parent
  is the `UUpvalCell`'s embedded `UCell` header — not the executing closure,
  because sibling closures share the cell and its color diverges from any
  one closure's (refactor-3 GC-07). No watcher hook (closures are not
  directly watchable in v1.0).

`UClosure` and `UObject` both embed `UCell` at offset 0, which is what makes
the cast `(UCell *)closure` and `(UCell *)object` safe inside the inline
bodies. The size pin is enforced by `_Static_assert` in the corresponding
header.

There is no `URBI_WRITE_BARRIER` macro; the barriers are real (inline)
functions so that argument types are checked. `src/runtime/umacros.h` is
unrelated and holds freestanding helpers like `urbi_zero` and `urbi_strlen`.

## Safe-point discipline

The collector advances cooperatively. The dispatcher reaches the
`safepoint:` label in `src/vm/uvm.c` on three triggers:

1. **Backward branches** (loops, `while`, `for`).
2. **Calls and non-top `OP_RET`** (every non-leaf call boundary).
3. **Explicit yields** at statement-separator boundaries (`;`, `,`, `&`).

At each safepoint the dispatcher runs:

```c
if (vm->gc_pending) urbi_gc_slice(vm, URBI_GC_SLICE_BUDGET);
```

before draining the deferred-slot-change ring and the watcher dirty-set.
`urbi_gc_slice` is the single non-inline collector entry point — it advances
the state machine `IDLE → MARK_ROOTS → MARK_INCREMENTAL → ATOMIC_FINISH →
SWEEP → IDLE` by approximately `byte_budget` bytes of work and returns.
Driver loops that run outside the dispatcher (custom embeddings, REPL
servers, host-side test harnesses) call `urbi_gc_slice` directly to advance
the collector at their own safe points.

`vm->gc_pending` is set when the allocator notices `gc_debt > 0` (live bytes
crossed `gc_threshold`) and the GC is not paused. `urbi_gc_pause(vm, true)`
inhibits new cycles; `urbi_gc_force_full(vm)` runs the state machine to
completion synchronously and is intended for tests and shutdown only.

`urbi_gc_slice` is **not ISR-safe**. The barrier inlines are also not
ISR-safe — host bridges that need to deposit data from interrupt context
must use the SPSC event ring, which is.

### Pause budget

The slice budget defaults to `URBI_GC_SLICE_BUDGET = 4096` bytes of work
(MCU default; Linux builds typically override to `16384`). The CI gate
`make test-gc-pause` recompiles `tests/stress/gc_pause_time.c` with
`-DGC_PAUSE_ASSERT_NS=1000000` and fails if any single slice exceeds the
1 ms (1 000 000 ns) target. Measured maximum on the host runner sits at
≈2.1 µs — roughly a 600× margin against the 1 ms target.

## Strand-walker contract

The mark phase reaches every live coroutine through the realm hierarchy:

```text
vm->realms_head → realm.strands_head → strand → register window
                                              → unwind / cleanup state
                                              → frames[]
```

Every `UStrand` whose register window may contain GC-managed `UValue`s MUST
be reachable from this list. Scheduler implementations are responsible for
maintaining the invariant; the GC walker assumes it without re-verification.
Implications:

- Cooperative-scheduler queues (`ready_head`, `sleep_q_head`) are
  scheduler-private; the walker does not consult them.
- `DEAD` strands stay on `realm.strands_head` until the realm tears down;
  the walker filters them inside `strand_walk_roots`
  (`src/sched/usched_cooperative.c`).
- Transient strands (e.g. the stack-local strand used by `urbi_vm_run`) MUST
  be threaded onto the global realm's `strands_head` for the duration of
  their run, and unlinked before they leave scope.

This contract lets future schedulers (priority bands, work-stealing,
preemptive) use arbitrary internal queues without affecting GC correctness.
See [scheduler-design.md](scheduler-design.md) for the matching scheduler
side and the unit suites that verify the invariant.

## GC-cell inventory

The cells the collector currently knows about, with their type tag and
size pin (host = 64-bit pointers; embedded targets noted separately):

| Cell | `type_tag` | Header | Size pin (host) | Notes |
|------|-----------|--------|-----------------|-------|
| `UObject` | `UTYPE_OBJECT` (1) | yes | 56 B | `_Static_assert` in `src/object/uobject.h` |
| `UClosure` | `UTYPE_CLOSURE` (2) | yes | variable + `(nupvals - 1) * sizeof(UUpvalCell*)` | `UCell` first member |
| `UStrand` | `UTYPE_COROUTINE` (7) | yes | 3896 B | bulk is the embedded `frames[UVM_MAX_FRAMES]` array; `_Static_assert` in `src/sched/ustrand.h` |
| `UVM` | n/a | n/a | n/a | not a GC cell — owns the heap, walked as the root container |
| `UWatcher` | `UTYPE_WATCHER` (6) | pool | 240 B | pool-managed (`UGC_IS_FIXED`); `_Static_assert` in `src/watcher/uwatcher.h` |
| `UEvent` | `UTYPE_EVENT` (18) | yes | 40 B | `_Static_assert` in `src/event/uevent.h` |
| `UChangedNode` | `UTYPE_CHANGED_NODE` (19) | yes | 32 B (host) / 16 B (32-bit) | `_Static_assert` in `src/changed/uchanged_node.h`, guarded on pointer width |
| `UTag` | `UTYPE_TAG` (5) | yes | 64 B | promoted to a GC cell in the `v0.5.0-reactive` release; `_Static_assert` in `src/tag/utag.h` |

`UTag` was a pool-managed value before `v0.5.0-reactive`; promoting it to a
proper GC cell let it own GC-managed `UValue` slots safely. `UEvent` and
`UChangedNode` are the reactive-runtime cells that flow through the
slot-change pipeline; see `reactive-runtime.md`.

Type tags 21–63 are reserved for future runtime cells; host types start at
`UTYPE_HOST_BASE = 64` and are registered through `urbi_register_type`.

## Interned strings never evict (v1.0)

Strings are not GC cells at v1.0. Every distinct string — identifiers, string
literals, and every result of runtime `String + String` concatenation (the
`OP_ADD` atom fast path interns each result) — lands in the per-VM intern
table (`src/value/uintern.c`) as a raw `vm->alloc_fn` allocation that is freed
only at `urbi_vm_destroy`. There is no unintern and the collector never scans
the table for dead entries (`intern_table_walk_roots` is a no-op by design);
a string-building loop therefore grows RAM monotonically for the life of the
VM. Because the table allocates via the raw allocator rather than
`urbi_gc_alloc`, intern growth also never triggers a collection (and
`URBI_GC_STRESS` does not fire on the intern path).

The v1.0 mitigation is observability: `urbi_intern_bytes(vm)` (internal,
`src/value/uintern.h`) reports total intern-subsystem bytes — every live
string block plus the current entries array — and `Debug.gc()` exposes it as
`intern_bytes` alongside `intern_count`. Embedders with string-churn
workloads should monitor it and bound churn (precompute strings, avoid
unbounded concat loops). Heap-allocated, collectable string cells are the
v1.x fix (design-risks: GC-08); the no-op root walker above is the seam where
that migration lands.

## Configurable knobs

| Macro | Default | Effect |
|-------|---------|--------|
| `URBI_GC` | `URBI_GC_INCREMENTAL` | Selects strategy. `URBI_GC_NONE` is a smoke build; other values reserved. |
| `URBI_GC_SLICE_BUDGET` | `4096` | Bytes of work per `urbi_gc_slice` call. Linux builds typically override to `16384`. |
| `URBI_GC_PAUSE_RATIO` | `200` | Threshold = `live_bytes * RATIO / 100` (i.e. allow 2× live before next cycle). |
| `URBI_GC_INITIAL_THRESHOLD` | `16 * 1024` | First cycle's allocation threshold. |
| `URBI_GC_HAS_FINALIZERS` | `1` | Compile finalizer-tracking code paths. |
| `URBI_GC_HAS_PINNING` | `1` | Compile `urbi_pin` / `urbi_unpin`. |
| `URBI_IC_ENTRIES_PER_SITE` | `4` | Inline-cache entries per `OP_GETSLOT` / `OP_SETSLOT` site. Bound to `{1, 2, 4}`; embedded-footprint preset uses `2`. Affects cell sizes only indirectly (smaller `UIC`). |

The strategy header re-`#define`s feature flags as zero where they do not
apply (e.g. `URBI_GC_NONE` zeroes `URBI_GC_HAS_FINALIZERS` and
`URBI_GC_INCREMENTAL_BARRIER`).

## Cross-references

- [architecture.md](architecture.md) — runtime overview.
- [scheduler-design.md](scheduler-design.md) — scheduler-side strand-walker
  contract and the invariant-checking unit suites.
- `object-model.md` — `UObject`, slot layout, shape transitions, the
  `UCell` embedding pattern.
- `reactive-runtime.md` — `UEvent`, `UChangedNode`, the `UTag` GC-promotion,
  and the deferred slot-change emit ring driven from the safe-point loop.
