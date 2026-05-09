# Reactive runtime

This document covers the reactive subsystem: condition watchers (`at` /
`whenever` / `waituntil`), event watchers (`at (e?)`), slot-change watchers
(`at (obj.x.changed?)`), and the scratch-frame primitive that backs every
synchronous fire path. Read [architecture.md](./architecture.md) first.

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
| `AST_AT_EVENT`       | `at (e?) body`, `at sync (e?) body`         | `OP_AT_EVENT_INSTALL` / `OP_AT_EVENT_SYNC_INSTALL`      |
| `AST_AT_SLOT_CHANGE` | `at (obj.x.changed?) body` and sync variant | `OP_GETSLOT_CHANGE_EVENT` then `OP_AT_EVENT_INSTALL`    |

Every install opcode is ABC-encoded. `AST_WATCHER` lays out
`(cond_reg, body_reg, onleave_or_FF)`; the `0xFF` sentinel in the C slot
signals "no onleave" to the dispatcher. `AST_AT_EVENT` and
`AST_AT_SLOT_CHANGE` build a one-parameter body closure whose `R[0]`
receives the emit payload.

## Watcher record

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
`UWATCHER_AT_EVENT_SYNC`.

## Lifecycle

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
`AT_EVENT` / `AT_EVENT_SYNC`. It skips the trace probe (events fire on
emit, not on slot writes) and links onto `event->at_watchers_head`
instead of `vm->active_watchers_head`.

The result enum is `UWatcherInstallResult` —
`URBI_INSTALL_OK`, `_OOM_POOL`, `_READSET_OVER`, `_TRACE_FAULT`,
`_RECURSIVE`.

### Fire

`watcher_eval_dirty` (`src/watcher/uwatcher_eval.c`) walks
`active_watchers_head` whenever `vm->watcher_dirty_count > 0` at a
safepoint. For each non-pending watcher it computes
`rising = truthy(new) && !truthy(old)` and dispatches by mode:

- `AT` rising — `spawn_body_coroutine` (async strand spawn).
- `AT_SYNC` rising — `invoke_body_inline` (synchronous, no yield).
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

## Closure ownership: `URBI_WATCHER_OWNS_*`

When `OP_CLOSURE` allocates a heap closure during the install run, the
closure is parked on `s->closure_list` so `urbi_vm_run`'s post-run
cleanup can free it. A watcher needs to outlive that cleanup, so
install transfers ownership via `strand_closure_unlink`: the closure is
spliced off `s->closure_list` and its proto is detached from
`module->nested[]`. Each successful unlink sets one of:

- `URBI_WATCHER_OWNS_COND` (0x20)
- `URBI_WATCHER_OWNS_BODY` (0x40)
- `URBI_WATCHER_OWNS_ONLEAVE` (0x80)

The three bits are independent. `pool_free` reads each bit and frees
the matching `(closure, proto, sub-buffers)` tuple before recycling the
slot. Closures *not* unlinked (test sentinels, already-freed) keep their
ownership bit clear and are left to whatever path created them. The
free path zeroes the pointer and clears the bit before recycling, so a
defensive double-free is impossible even if `pool_free` were re-entered
on the same slot.

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

## Strand-scheduler integration

Async fires (`AT`, `WHENEVER`, `AT_EVENT`) call `do_spawn_body_coroutine`
(`src/watcher/uwatcher_spawn.c`), which allocates a body strand via
`urbi_strand_create`, optionally attaches `owning_tag` distinct from
`realm->tag`, arms from the closure, wires
`body->watcher_body_owner = w` and `w->body_strand = body`, and starts
the strand (`DORMANT` → `READY`). Body completion is reported via
`urbi_watcher_body_completed` from the dispatcher's strand-`DEAD` path,
which honours the `URBI_WATCHER_PENDING_REFIRE` flag for queued
re-spawns. See [scheduler-design.md](./scheduler-design.md) for the
strand state machine and run-queue contract.
