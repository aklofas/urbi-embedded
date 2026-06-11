# Reactive runtime

This document covers the reactive subsystem: condition watchers (`at` /
`whenever` / `waituntil`), event watchers (`at (e?)`), slot-change watchers
(`at (obj.x.changed?)`), periodic execution (`every`), and the scratch-frame
primitive that backs every synchronous fire path. Read
[architecture.md](./architecture.md) first.

All headline reactive language primitives are fully wired as of v0.10.2-reactive.
Legacy gap markers (Findings 1–6 from the v0.10.x arc) are documented as CLOSED
inline with their shipping milestone.

## Reactive primitives surface

| Primitive | Emitter status | Runtime status |
|-----------|---------------|----------------|
| `at (cond) body` | emits `OP_AT_INSTALL` | working; edge-triggered async body spawn |
| `at sync (cond) body` | emits `OP_AT_SYNC_INSTALL` | working; inline scratch execution |
| `whenever (cond) body` | emits `OP_WHENEVER_INSTALL` | working; level-triggered async re-spawn |
| `waituntil (cond)` | emits `OP_WAITUNTIL_INSTALL` | working; blocking until rising edge |
| `at (e?) body` | emits `OP_AT_EVENT_INSTALL` | working; fires on `Event.emit` |
| `at sync (e?) body` | emits `OP_AT_EVENT_SYNC_INSTALL` | working with sync-degradation caveat (Finding 7) |
| `whenever (e?) body` | emits `OP_WHENEVER_EVENT_INSTALL` (W0/v0.10.2) | working; perpetual event subscriber — re-fires on every emission (not one-shot like `at (e?)`) |
| `every (period) body` | desugars to `every(period_us, fn)` C-native call | working; re-spawn cadence via `UPeriodic` |
| `tag.stop()` from script | emits `OP_TAG_STOP` (opcode 30) | working — **CLOSED W3/v0.10.2**: `Tag.new()` + `OP_TAG_STOP` + script-level `.stop()`, `.freeze()`, `.unfreeze()`, `.block()`, `.unblock()` all shipped. Bare-prefix `mytag: stmt` and member-expr `Tag.scope: body` (W6/v0.10.5) also working. C-level `urbi_tag_stop` remains the ISR-safe path. |

## Reactive vocabulary at the lexer

Three reactive keywords are reserved at the lexer (`src/lex/ulex.c`):
`at`, `whenever`, `waituntil`. The supporting modifiers `sync`, `async`,
and `onleave` are also reserved keywords. The names `every`, `enter`, and
`leave` are *not* reserved at the lexer — they are stdlib-bound identifiers
exposed as methods on the `Tag` proto (see `src/tag/utag_native.c`'s
registration of `enter` and `leave` getter stubs). Source that names a
reserved keyword as a variable raises `PARSE_RESERVED_KEYWORD_AS_IDENT`.

## AST node kinds and emit shapes

Reactive forms parse to four AST node kinds, all handled in
`src/emit/uemit_react.c`:

| AST node             | Source forms                                | Install opcode                                          |
| -------------------- | ------------------------------------------- | ------------------------------------------------------- |
| `AST_WATCHER`        | `at`, `at sync`, `whenever`                 | `OP_AT_INSTALL` / `OP_AT_SYNC_INSTALL` / `OP_WHENEVER_INSTALL` |
| `AST_WAITUNTIL`      | `waituntil (cond)`                          | `OP_WAITUNTIL_INSTALL`                                  |
| `AST_AT_EVENT`       | `at (e?) body`, `at sync (e?) body`, `whenever (e?) body` | `OP_AT_EVENT_INSTALL` / `OP_AT_EVENT_SYNC_INSTALL` / `OP_WHENEVER_EVENT_INSTALL` |
| `AST_AT_SLOT_CHANGE` | `at (obj.x.changed?) body` and sync variant | `OP_GETSLOT_CHANGE_EVENT` then `OP_AT_EVENT_INSTALL`    |

Every install opcode is ABC-encoded. `AST_WATCHER` lays out
`(cond_reg, body_reg, onleave_or_FF)`; the `0xFF` sentinel in the C slot
signals "no onleave" to the dispatcher. `AST_AT_EVENT` and
`AST_AT_SLOT_CHANGE` build a one-parameter body closure whose `R[0]`
receives the emit payload.

## UWatcher lifecycle

### Record layout

`UWatcher` (`src/watcher/uwatcher.h`) is the central reactive record.
Layout pins (guarded on `__SIZEOF_POINTER__ == 8`):

```c
_Static_assert(sizeof(UWatcher) == 240,
               "UWatcher size pin on 64-bit (URBI_WATCHER_READSET_MAX=16)");
```

At the embedded footprint preset (`URBI_WATCHER_READSET_MAX=4`) the size
shrinks to 144 B — the only variable-size field is the `cells[]` read-set
array. The fixed portion is 112 B; cells contribute `8 × cap` bytes.

Watchers live in a fixed pool (`UGC_IS_FIXED`) sized
`URBI_WATCHER_POOL_SIZE` (default 64). The pool slab is allocated once at
`urbi_vm_init` via `vm->alloc_fn` and freed at `urbi_vm_destroy`; the GC
marks active watchers but does not reclaim slots.

### Mode discriminator

`UWatcher.mode` selects the firing behaviour:
`UWATCHER_AT` (edge), `UWATCHER_WHENEVER` (level),
`UWATCHER_AT_SYNC`, `UWATCHER_WAITUNTIL`, `UWATCHER_AT_EVENT`,
`UWATCHER_AT_EVENT_SYNC`, `UWATCHER_WHENEVER_EVENT` (perpetual event subscriber;
re-fires on every emission, added W0/v0.10.2).

### Flag bits

Active flag bits in `UWatcher.flags`:

- `URBI_WATCHER_ACTIVE` (0x01) — installed and live.
- `URBI_WATCHER_PENDING_UNREGISTER` (0x02) — stop requested; drain before free.
- `URBI_WATCHER_FIRED_DURING_EVAL` (0x04) — condition fired while eval in progress.
- `URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE` (0x10) — body fired at least once since
  last onleave check.

Bits 0x20 / 0x40 / 0x80 are **available for reuse** — they held
`URBI_WATCHER_OWNS_COND` / `_OWNS_BODY` / `_OWNS_ONLEAVE` until v0.8.4
Step C-3, when the manual ownership model was deleted in favour of
GC-managed closure lifetime. Watcher closures are now roots yielded by
`watcher_table_walk_roots`; there is no per-closure ownership bit.

### Refire queue

`UWatcher.pending_refire_count` (uint8_t) and `max_refire_queue` (uint8_t,
initialised to `URBI_WATCHER_REFIRE_QUEUE_DEFAULT` = 15) replaced a single
`URBI_WATCHER_PENDING_REFIRE` flag bit in v0.7.x. When a body strand is
already in flight and `exhaust_policy == URBI_EXHAUST_QUEUE`, incoming fires
increment the counter up to the cap; on body completion,
`urbi_watcher_body_completed` decrements by one and calls
`respawn_body_coroutine` if the counter is still positive.

### Install

`install_watcher_runtime` (`src/watcher/uwatcher_install.c`) runs as a
single linear sequence: re-entry guard → `resolve_owning_tag` → arm the
`OP_GETSLOT` trace probe → run the cond closure on the scratch frame →
check overflow / cond-throw → pool-alloc → wire fields → mark
`UGC_HAS_WATCHER_OBSERVER` (bit 6) on each traced cell → tail-append to
`vm->active_watchers_head` and to the owning tag's member chain.
Tail-append (not prepend) preserves the determinism contract: install
order equals eval order.

`install_at_event_runtime` is the thinner sibling for
`AT_EVENT` / `AT_EVENT_SYNC` / `WHENEVER_EVENT`. It skips the trace probe
(events fire on emit, not on slot writes) and links onto
`event->at_watchers_head` instead of `vm->active_watchers_head`.

The result enum is `UWatcherInstallResult` —
`URBI_INSTALL_OK`, `_OOM_POOL`, `_READSET_OVER`, `_TRACE_FAULT`,
`_RECURSIVE`, `_NO_OBSERVABLE_CELLS` (W0/v0.10.2: empty read-set rejected
for `AT`/`WHENEVER` watchers; `WAITUNTIL` is exempt).

The `_NO_OBSERVABLE_CELLS` result closes reactive audit **Finding 1** (the
legacy `whenever (e?)` silently installed a no-op cond watcher with an empty
read-set). Now `whenever (e?)` routes to `OP_WHENEVER_EVENT_INSTALL` and any
`AT`/`WHENEVER` watcher reaching `install_watcher_runtime` with an empty
read-set is rejected as a programming error.

### Fire (eval pass)

`watcher_eval_dirty` (`src/watcher/uwatcher_eval.c`) walks
`active_watchers_head` whenever `vm->watcher_dirty_count > 0` at a
safepoint. For each non-pending watcher it computes
`rising = truthy(new) && !truthy(old)` and dispatches by mode:

- `AT` rising — `spawn_body_coroutine` (async strand spawn).
- `AT_SYNC` rising — `invoke_body_inline` (synchronous scratch frame; no yield).
- `WHENEVER` truthy — `spawn_body_coroutine` every dirty pass.
- `WAITUNTIL` rising — wake `waiter_strand`, unregister self.

Falling-edge `onleave` runs inline via `invoke_onleave_inline` when
`URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE` is set.

### Drain

`drain_pending_onleave_queue` (`src/watcher/uwatcher_drain.c`) runs at
the safepoint *before* `watcher_eval_dirty`. The pending FIFO is fed by
`pending_onleave_queue_push`, which transfers a watcher off the active
list and the tag member chain when its owning tag stops. Drain pops in
FIFO order, runs `onleave` if present, and calls
`urbi_watcher_unregister_internal`. Watchers whose body strand is still
alive are deferred to the next safepoint.

**CLOSED W3/v0.10.2 (Finding 2)** — `pending_onleave_queue_push` now
synchronously calls `uevent_at_watchers_remove` for AT_EVENT, AT_EVENT_SYNC,
and WHENEVER_EVENT before appending to the pending-onleave FIFO.  This closes
the UAF window where `c_event_emit_async`/`_sync` would dispatch a
logically-dead AT_EVENT watcher.  Defence-in-depth
`URBI_WATCHER_PENDING_UNREGISTER` checks remain in both emit paths.

### Unbind

`urbi_watcher_unregister_internal` performs scan-on-unregister to clear
bit 6 from any read-set cell no other watcher observes, unlinks from the
tag member chain and the appropriate watcher list (active or
event-subscriber), then `pool_free`s the slot.

## Scratch-frame primitive

`urbi_run_closure_on_scratch` (and the `_with_payload` variant) in
`src/watcher/uwatcher_scratch.c` is the shared backbone of every
synchronous closure run inside the reactive subsystem. It allocates a
transient `UStrand` on the C stack mirroring `urbi_vm_run`'s pattern,
arms it from the closure, threads it onto `global_realm->strands_head`
so the GC walker visits its register window, dispatches up to
`URBI_SCRATCH_BUDGET_OPS` (default 4096) instructions, then unlinks and
tears down. Cond closures must not yield — yield/block/budget exhaustion
all degrade to "throw" and are reported via `*out_threw`.

The five sync sites in the reactive runtime that route through this
primitive:

1. `install_watcher_runtime` — install-time cond eval.
2. `invoke_condition_closure` — eval-time cond eval.
3. `invoke_body_inline` — `AT_SYNC` body fire.
4. `invoke_onleave_inline` and `run_watcher_onleave` — falling-edge
   `onleave` (eval) and tag-stop drain `onleave` (drain).
5. `run_event_body_on_scratch` — `AT_EVENT_SYNC` body on `c_event_emit_sync`,
   via the `_with_payload` variant so `R[0]` carries the emit payload.

Sites (1)–(4) rely on caller-owned `vm->in_watcher_eval` /
`in_watcher_install` for re-entry protection;
`run_event_body_on_scratch` owns its own `vm->in_watcher_scratch` flag
because sync emit can fire from contexts that have not entered eval.

## Closure lifecycle and GC roots

Watcher closures (`condition`, `body`, `onleave`) are GC-managed
`UClosure*` pointers. The GC root provider `watcher_table_walk_roots`
(`src/watcher/uwatcher_gc.c`) walks the whole pool slab: every in-use
slot (`URBI_WATCHER_ACTIVE` set — set by `uwatcher_pool_alloc`, cleared
only by `pool_free`) yields each non-NULL closure pointer to the mark
callback as a `UVAL_CLOSURE` value, plus `last_value_cache`, and shades
`owning_tag` and `event` directly. Rooting is a property of "slot is in
use", not of list topology, so AT_EVENT / WHENEVER_EVENT watchers stay
rooted even when their owning event is otherwise unreachable, and the
subscribed event itself is immortal while subscribed (refactor-3
GC-05/GC-03).

Fields intentionally NOT walked by `watcher_table_walk_roots`:

- `body_strand` / `waiter_strand` — reached via `realm->strands_head` by
  the scheduler's root walker.
- `realm` — host-allocated; not GC-managed at v1.0.

The read-set `cells[]` remains a v1.x deferral: concrete cell types are
reached indirectly through closures and slot-tables.

## UEvent lifecycle

`UEvent` (`src/event/uevent.h`) is a 40-byte GC-managed cell
(`UTYPE_EVENT`) with two intrusive subscriber lists:

- `at_watchers_head` — persistent AT_EVENT / AT_EVENT_SYNC watcher chain
  (linked via `UWatcher.next_in_event`). Subscribed watchers are rooted
  by the pool-wide provider in `uwatcher_gc.c`, not by the event's walker.
- `waiters_head` — one-shot `UStrand` chain (linked via
  `UStrand.next_event_waiter`); strands self-walk via the realm hierarchy.

`c_event_emit_async` fans out payload to both lists: spawns body
coroutines for at-watchers, wakes and makes-runnable all waiters.
`c_event_emit_sync` runs AT_EVENT_SYNC subscribers inline on the scratch
frame (before returning); degrades to async with a one-shot warn when any
scratch re-entry flag is set.

`c_event_waituntil` tail-appends the current strand to `waiters_head` and
transitions it to `USTRAND_WAIT_EVENT`; the T53 opcode handler reads
`last_event_payload` after the strand is woken.

Named events (bound to Lobby slots or realm globals) are reachable via
the event registry (`src/event/uevent_registry.{h,c}`). ISR-safe
injection goes through the SPSC ring (`src/event/uevent_ring.{h,c}`) and
is drained on the main thread before reaching `c_event_emit_async`.

## UTag lifecycle

`UTag` (`src/tag/utag.h`) is a 64-byte GC-managed cell (`UTYPE_TAG`).
Fields of interest to the reactive runtime:

- `member_watchers_head` — watchers scoped to this tag (via
  `UWatcher.next_in_tag`).
- `member_strands_head` — `UCleanupEntry` instances for strands inside a
  TAG_SCOPE for this tag.
- `enter_event` / `leave_event` — lazily allocated `UEvent*` by
  `tag_enter_getter` / `tag_leave_getter` (`src/tag/utag_native.c`) on
  first subscriber access.
- `parent` — points to the realm-root tag for host-created child tags
  (set by `urbi_tag_create`, v0.7.1 Gap M).
- `flags` — `UTAG_FLAG_STOPPED` (0x02) set by `urbi_tag_stop`.

Ambient-tag inheritance at a strand's scope is via `UCleanupEntry`
TAG_SCOPE entries on the strand's cleanup stack. `OP_PUSH_TAG` pushes a
TAG_SCOPE entry; `OP_POP_TAG` pops it, fires `leave_event` if any
subscriber is installed, then walks `tag->member_watchers_head` and calls
`pending_onleave_queue_push` for each watcher.

`urbi_tag_stop` (`src/runtime/uunwind.c`) is the only current path that
deposits `PENDING_UNWIND=UEXEC_TAG_STOP` on member strands, walks
`member_watchers_head` for the onleave cascade, and sets
`UTAG_FLAG_STOPPED`. It is documented as NOT ISR-safe and is only callable
from host C code.

**CLOSED W3/v0.10.2 (Finding 3)** — Tag manipulation is fully
script-accessible.  `UVAL_TAG = 12` is a first-class `UValKind` variant;
`Tag.new()` creates tags; `OP_TAG_STOP` (opcode 30) calls `urbi_tag_stop`;
`.freeze()`, `.unfreeze()`, `.block()`, `.unblock()`, `.enter`, `.leave`
are registered on `vm->tag_proto`.  Bare-prefix `mytag: stmt` (v0.10.2-W4)
and member-expression `Tag.scope: body` (v0.10.5-W8) are parser-accepted.
`urbi_tag_stop` remains the ISR-safe C path; script-level `.stop()` routes
through it.

## UPeriodic lifecycle (`every`)

`UPeriodic` (`src/stdlib/temporal.h`) is a host-allocated record per
`every(period_us, body_closure)` call, threaded onto `vm->periodics_head`.

Fields:

- `body` — body closure (GC root via `urbi_periodic_table_walk_roots`).
- `period_us` / `next_fire_us` — timing fields; `next_fire_us` compared
  against `vm->host_time_us()` each `urbi_periodic_pump` call.
- `realm` / `owning_tag` — realm and ambient tag at install time.
- `current_strand` — non-NULL while a body strand is in flight.
- `module_instance` — `UChunkInstance` resolved at install time (O(N)
  walk of `vm->module_instances_head`; cached once).
- `unregister_pending` — set on tag cancel, uncaught throw, or realm
  destroy; the next `urbi_periodic_pump` call frees the record.

`urbi_periodic_pump` is called from `urbi_step` after the sleep-queue
drain. It walks the list: frees any with `unregister_pending` set; for
any whose `current_strand == NULL` and `next_fire_us <= now`, spawns a
fresh body strand via `spawn_periodic_body` and arms the next fire time.

Body completion is notified via `urbi_periodic_body_completed` from the
`exit_strand` path in `uvm.c`, which mirrors `urbi_watcher_body_completed`
in structure.

**CLOSED W3/v0.10.2 (Finding 4)** — body strands spawned by
`spawn_periodic_body` and `do_spawn_body_coroutine` now bind the body
closure's proto as `root_proto` in the entry frame, so `OP_CLOSURE` inside
`every`/`at`/`whenever` bodies correctly resolves `nested[]` children.
Function literals and nested reactive primitives inside reactive bodies work.

## Per-VM trace read-set + deferred slot-change ring

Two VM-level data structures support the reactive subsystem
(`src/vm/uvm.h`):

- **Trace read-set.** Fixed array `vm->trace_read_set[URBI_WATCHER_READSET_MAX]`
  with `trace_read_set_count` and `trace_overflow`. Armed by setting
  `vm->in_watcher_install` around the install-time cond eval; the
  `OP_GETSLOT` slow path records every cell read. Capacity is 16 entries
  at the default build, 4 at the embedded footprint preset.

- **Deferred slot-change emit ring.** Heap-allocated SPSC ring of
  `UDeferredSlotChange` entries sized by `URBI_DEFERRED_SLOT_CHANGE_RING_SIZE`
  (default 64). When a sync slot-change body re-enters the slot-write path,
  `urbi_emit_slot_change_slow` calls `urbi_defer_slot_change` to enqueue
  `(parent, key, new_value)`. `urbi_drain_deferred_slot_changes` runs at
  every safepoint *before* `watcher_eval_dirty`. Ring-full silently drops
  with a one-shot warn. The cap is reduced for footprint targets.

**CLOSED W3/v0.10.2 (Finding 6)** — `urbi_deferred_slot_changes_walk_roots`
is registered with the GC root registry at `urbi_vm_init`, making deferred
ring entries explicit GC roots.  The cooperative safepoint ordering
(GC before drain) remains the primary invariant, but the root walker
removes the foot-gun for future contributors who might reorder safepoint
actions or introduce preemptive GC.

## Slot-change events

`obj.x.changed?` reads or lazily creates a per-`(object, slot-name)`
`UEvent` via `urbi_object_get_or_create_change_event`
(`src/changed/uchanged.c`). Each subscribed object hosts an intrusive
chain of `UChangedNode` cells (`src/changed/uchanged_node.h`,
32 B / 16 B) keyed by interned `USymbol*`. The first prepend sets
`UGC_HAS_SLOT_CHANGE_EVENT` (bit 7) on the object. The bit is
sticky — never cleared at v1.0 — so the slow path stays reachable on
every slot write of a once-subscribed object; `urbi_emit_slot_change_slow`
tolerates an empty / unmatched chain by silent return. See
[object-model.md](./object-model.md) for slot-write barrier interaction
and the per-object cell header layout.

## Register-allocation invariant at sibling emit sites

`AST_AT_EVENT` and `AST_AT_SLOT_CHANGE` both contain a load-bearing
guard immediately before the body-closure allocation:

```c
if (e->current_fs->freereg < e->next_reg)
    e->current_fs->freereg = e->next_reg;
```

Without this `freereg`/`next_reg` synchronisation,
`emit_function_literal` can allocate `body_reg` on top of `event_reg`,
causing `OP_CLOSURE` to clobber the event pointer at runtime.
`AST_WATCHER` and `AST_WAITUNTIL` avoid this hazard by routing their
condition through `emit_function_literal` symmetrically. Future
reactive-emit work that introduces a new install opcode MUST audit
this invariant. See [emit-correctness-notes.md](./emit-correctness-notes.md)
for the full register-allocation rubric.

## Cooperative dispatch ordering

At each safepoint (`src/vm/uvm.c` label `safepoint:`) the actions fire
in this order:

1. Unwind check (`s->pending_unwind != UEXEC_OK` → `urbi_unwind`).
2. Per-strand instruction budget.
3. VM-wide step budget.
4. `urbi_gc_slice` if `vm->gc_pending`.
5. `drain_pending_onleave_queue` if `vm->pending_onleave_head`.
6. `urbi_drain_deferred_slot_changes` (spec §5.4 — must precede step 7).
7. `watcher_eval_dirty` if `vm->watcher_dirty_count > 0`.

The ordering of 4 → 6 → 7 is the cooperative invariant that keeps
deferred ring entries live across a GC slice (see Finding 6 above): slots
are deferred only from within a scratch context (step 7 and its
sub-calls), and the GC slice at step 4 fires before any slot-change body
can re-defer. This invariant is implicit; there is currently no assertion
enforcing it.

## Strand-scheduler integration

Async fires (`AT`, `WHENEVER`, `AT_EVENT`) call `do_spawn_body_coroutine`
(`src/watcher/uwatcher_spawn.c`), which allocates a body strand via
`urbi_strand_create`, optionally attaches `owning_tag` distinct from
`realm->tag`, arms from the closure, wires
`body->watcher_body_owner = w` and `w->body_strand = body`, and starts
the strand (`DORMANT` → `READY`). Body completion is reported via
`urbi_watcher_body_completed` from the dispatcher's strand-`DEAD` path,
which honours `pending_refire_count` for queued re-spawns.

Body-strand `module_instance` wiring: the spawn path walks
`vm->module_instances_head` to find the `UChunkInstance` whose
`proto_instances->entries[]` array contains the body closure's
`proto_inst` pointer (pointer-range comparison). The result is cached on
`body->module_instance` so `OP_GETSLOT` at `frame_count == 0` resolves
the IC table correctly. A v1.x backlog item (`UClosure.owning_mi` field)
would remove the O(N) walk.

See [scheduler-design.md](./scheduler-design.md) for the strand state
machine and run-queue contract.

## Reactive arc findings — all closed v0.10.2

The following reactive runtime gaps were tracked in the v0.10.x
architectural refactor arc.  All were closed in Wave 3 (v0.10.2-reactive)
unless noted; the inline sections above have been updated accordingly.

- **Finding 1** (`whenever (e?)` silent no-op): **CLOSED W0/v0.10.2** —
  `parse_whenever` now produces `AST_AT_EVENT` with `is_whenever=true`
  when the condition ends with `?`; `OP_WHENEVER_EVENT_INSTALL` (opcode 48,
  wire v1.9) routes to `UWATCHER_WHENEVER_EVENT`; empty-read-set installs
  rejected as `URBI_INSTALL_NO_OBSERVABLE_CELLS`.
- **Finding 2** (AT_EVENT dangling on `event->at_watchers_head` after
  tag-stop): **CLOSED W3/v0.10.2** — `pending_onleave_queue_push`
  synchronously calls `uevent_at_watchers_remove` for AT_EVENT,
  AT_EVENT_SYNC, and WHENEVER_EVENT before appending to the FIFO.
- **Finding 3** (`OP_TAG_STOP` reserved stub; no script-level tag
  cancellation): **CLOSED W3/v0.10.2** — `Tag.new()`, `OP_TAG_STOP`,
  bare-prefix `mytag: stmt`, and full `vm->tag_proto` method suite shipped.
  Member-expression `Tag.scope: body` closed in W8/v0.10.5.
- **Finding 4** (`OP_CLOSURE` in reactive body strands): **CLOSED
  W3/v0.10.2** — `spawn_periodic_body` and `do_spawn_body_coroutine` bind
  body closure proto as entry-frame `root_proto`.
- **Finding 6** (deferred slot-change ring weakly rooted): **CLOSED
  W3/v0.10.2** — `urbi_deferred_slot_changes_walk_roots` registered with
  GC root registry at `urbi_vm_init`.
