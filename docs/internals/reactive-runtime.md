# Reactive runtime

`at`, `at sync`, `whenever`, `waituntil`, `at (e?)` and `at (obj.x.changed?)`
are one subsystem: one watcher type, one dirty count, one drain. This document
describes the runtime in `src/rt/uwatch.c`; the header block in
`src/rt/uwatch.h` is the same material in the code, and the two are meant to
be read together. Read [architecture.md](./architecture.md) first.

`every (period) body` is NOT part of this subsystem. It is a periodic timer on
the scheduler's heap — see [runtime.md](./runtime.md) — and
appears here only because a tag stop cancels both kinds of thing at once.

## One watcher type

```c
struct UWatcher {
    UCell     cell;
    uint8_t   mode;         /* AT, AT_SYNC, WHENEVER, WAITUNTIL, ONCE */
    uint8_t   armed, last, fired;
    UClosure *cond;         /* NULL for an event watcher */
    UEvent   *event;        /* NULL for a condition watcher */
    UClosure *body, *onleave;
    UTag     *tag;
    URealm   *realm;
    UStrand  *body_strand;  /* the `whenever` body currently in flight */
    UStrand  *waiters;      /* strands parked in waituntil */
    UValue    payload;
    int     (*host_cb)(UVM *, void *, UValue);
    void     *host_ud;
    UWatcher *next, *next_on_event;
};
```

The five modes and the `cond`/`event` discriminator are the whole taxonomy.
There is no second struct for event subscriptions, no per-watcher read set, no
cascade rescan and no generation stamp.

A watcher is a GC cell (`UCELL_WATCHER`) and lives on `vm->watch.all`, which
`uvm_gc_mark_fixed` treats as a fixed root; `uwatch_trace` marks its condition,
event, body, onleave, tag, realm, in-flight body strand, every waiter, and
`payload`. That list is a watcher's ONLY root, which is why the walk discipline
below matters.

Per-VM state is four fields, by value inside `UVM`:

```c
typedef struct UWatchState {
    UWatcher *all;        /* every live watcher; a fixed GC root */
    uint32_t  ndirty;     /* writes to watched objects since the last drain */
    uint8_t   draining;   /* a drain or an event fan-out is walking the lists */
    uint8_t   observing;  /* a condition is running: slot reads mark their object */
} UWatchState;
```

`UWatchState` and `UWatchMode` are declared in `rt/uexec.h` rather than
`rt/uwatch.h`: the state is a by-value member of `UVM` and the install opcodes
name the modes, so both have to be complete at exec rank. The exec core, the
scheduler and the GC hooks reach the rest of the subsystem through a declared
seam in `rt/uexec.h` labelled *provided by the layer above (uwatch)* — the same
arrangement `uexec_run` has in `rt/usched.h`, in the other direction.

## The dirty set is a count

A slot read inside a running condition marks its object `UOBJ_F_WATCHED`
(`uwatch_observe`, a `static inline` on the resolved-read path in
`slot_get`). The bit is sticky and is never cleared. A slot write to an object
carrying it bumps `UWatchState.ndirty` (`uwatch_mark_dirty`, reached from
`uexec_note_write`, which OP_SETSLOT, OP_SETSLOT_UPDATE, `urbi_global_set` and
`urbi_slot_set` all go through).

The set really is a counter and not a list of objects. Because the bit is
per-object and sticky, once a condition has read a realm's globals every global
write marks dirty and every drain re-evaluates every armed condition anyway; an
object array would be a second copy of that fact with a rooting problem
attached, since a dirty-marked object the script has since dropped would either
be kept alive by the array or dangle in it. `uwatch_mark_dirty` keeps its
`UObject *` parameter as the seam for a later per-realm filter.

The trade this buys is the ratified one: no per-watcher read sets, at the cost
of a coarse re-evaluation.

## Where a drain happens

`uwatch_drain` with a non-zero count evaluates EVERY armed condition watcher
exactly once and acts on the edge against `last`. `ndirty` is cleared BEFORE
the walk, so a write made by a condition or by an inline body arms the NEXT
drain rather than extending the one in progress — that is what bounds a pass.

Two call sites, both safepoints:

- **`usched_step`**, once before the run queue and once after every strand
  slice. The pre-queue drain is what makes a HOST slot write between two steps
  reach a parked `waituntil`; the post-slice drain is what runs a watcher body
  after the strand that flipped its condition has finished.
- **`OP_YIELD`**, i.e. the `;` separator, on a strand that may be descheduled.
  This is what `at sync` means: the body runs to completion before the next
  statement of the installing strand. A strand that may not be descheduled is
  inside somebody's synchronous call and drains nothing, which is also what
  keeps a drain from nesting.

## Edges

| Mode | Rising edge | Falling edge |
|---|---|---|
| `AT` | spawn `body` as a strand under `w->tag` | spawn `onleave`, once, and only after a body has run |
| `AT_SYNC` | run `body` inline on a spare strand | run `onleave` inline |
| `WHENEVER` | spawn `body`; re-fire on body death while the condition still holds | spawn `onleave` |
| `WAITUNTIL` | wake every waiter, disarm | — |
| `ONCE` | wake every waiter, disarm | — |

`whenever` is a reactive LOOP, not a per-safepoint counter: `uwatch_body_done`,
called from the strand-death path in `usched_on_death`, re-asks the condition
and re-spawns while it holds. The legacy manual is explicit that this is the
construct's meaning and that the number of body evaluations is not something a
program may depend on, so a `whenever` whose body cannot falsify its guard does
not terminate — exactly as `while (true)` does not.

A condition, a body and an onleave all run on a SPARE strand acquired from
`uvm_spare_acquire`. A spare may not be descheduled, so a `;` inside one of
them is a plain sequence point and `sleep` is a no-op: this is the recorded
answer to REVIVAL §14 `S-atsync-atomic` — **at-sync bodies run to completion on
the spare strand**.

`uexec_call` takes no budget, so a non-terminating condition hangs the drain.
That is the same contract a non-terminating getter already has, and it is a
known limitation rather than a design intent.

## The walk discipline

**Cancelling a watcher only clears `armed`.** `uwatch_sweep` — the one place
that unlinks — is a no-op while `UWatchState.draining` is set, and is called
only from outside a walk. So a walk's `next` pointer is always valid, and a
watcher is never unlinked from the list that roots it while somebody still
holds it.

This single rule replaces three pieces of old machinery: the
`PENDING_UNREGISTER` flag, the cascade rescan that flag needed, and the
per-pass evaluation stamp the rescan needed to stop a level-triggered
`whenever` firing twice on a re-visit.

Every function that walks a list and runs script holds `draining` for the
duration: `uwatch_drain`, `uwatch_event_fired`, and `uwatch_body_done`. The
last one is not optional — a condition is arbitrary script, and a `t.stop()`
inside it reaches `uwatch_tag_stopped` and then the sweep. Without the bracket
the watcher is unlinked mid-use, the next allocation inside that same condition
collects it, and the caller reads freed memory. `make test-gc-stress` (in
`releasetest`) is the gate that catches this class;
`tests/rt/test_watch.c:a_condition_may_cancel_its_own_watcher` is the case.

Both `vm->watch.all` and `e->watchers` are APPEND lists, because the corpus
pins registration order on a fan-out. The "a subscription made during a
fan-out does not receive it" rule is kept by capturing the tail before the walk
rather than by relying on where the head is.

## Cancellation

`tag.stop()` cancels every watcher whose `w->tag` is that tag — `utag_stop`
calls `uwatch_tag_stopped` right after it drops the tag's periodics, so
`mytag: at (c) body` and `mytag: every(P) body` die the same way. Realm
teardown calls `uwatch_realm_dropped`, because a condition whose realm has no
globals left would raise on every drain for the life of the VM.

A watcher installed at chunk top takes the realm's connection tag, so
`urbi_tag_stop(vm, urbi_realm_tag(vm, realm))` is how a host cancels a realm's
whole reactive surface — including the main realm's, which `urbi_realm_free`
refuses to touch.

## A condition that throws

Absorbed at the spare-strand boundary, reported once through the diag hook as
`at condition raised: <message>`, and the watcher is DISARMED. Left armed it
would raise on every drain for the life of the VM, so "once" would not be true.

`uwatch_absorb` saves and restores `vm->last_error` around every watcher-run
boundary: a watcher's private failure is not the host's last error.

## Events

`uevent_emit_to(vm, e, payload, sync)` fans out to `e->watchers` first and then
wakes `e->waiters`, so a `waituntil (e?)` that resumes with the payload does
not observe a fan-out that has not happened yet. `sync` is what
`e.syncEmit(p)` passes; it makes an `AT_SYNC` subscriber run inline and leaves
every other subscriber with a spawned strand. "Synchronous" is a property of
the subscription, not of the emit.

A tag's `enter` and `leave` are plain events, reachable as PROPERTIES whose
getters allocate on first ask (`t.enter`, not `t.enter()`), so a tag nobody
subscribes to allocates neither. Crossing a scope boundary fires them
synchronously on the crossing strand, which is the only way `at sync
(t.enter?)` can mean "before the scope body".

`waituntil (e?)` desugars to `e.waituntil()`, which parks on the event's wait
list and resumes with the emission payload. The payload reaches the call's own
destination register through `UStrand.resume_slot`: `do_call` has already
written the native's nil there by the time the strand parks, so the native
records the destination via `ustrand_want_payload` (which reads its own call
site — `pc[-1]` is the OP_CALL) and `uexec_run_inner` delivers `transfer` into
it on the way back in.

## `x.changed?`

The event for `(object, slot)` lives in a HIDDEN SLOT on the object itself,
named `"\x01" + name`. A side table keyed by object would have to either keep
every subscribed object alive for the life of the VM or find out when one died;
a slot dies with its owner, is traced as an ordinary slot value, and needs no
bookkeeping. `\x01` is a byte no identifier can contain, and
`collect_local_slot_names` skips any name that starts with it, so neither
`slotNames` nor `localSlotNames` leaks it.

`uobj_set_local` preserves `USLOT_CHANGED_EVENT` across an overwrite: a
subscription is not a property of the value being written.

**Declaring a slot is not changing it.** A write that CREATES a slot arms a
pending subscription (`uwatch_slot_installed`) instead of firing it;
`uexec_note_write` takes an `existed` flag, which OP_SETSLOT computes from the
lookup it already does and `urbi_global_set` / `urbi_slot_set` compute the same
way.

## Liveness

`urbi_has_live_work` is true when the VM has a runnable strand, a pending
timer, or an armed `at`/`whenever` watcher. A watcher counts because a host
slot write between two steps is what it exists to notice and nothing else in
the VM records that such a write could matter.

**A wait never counts**, however it is spelled: a strand parked in `waituntil
(cond)`, in `waituntil (e?)`, or on an event nobody will emit, is quiescent.
That is the ratified rule — wait lists do not count — and one construct must
not answer two ways depending on its spelling.

This predicate is **not a loop condition**. `while (urbi_has_live_work(vm))
urbi_step(...)` spins at full CPU on any program that installs a watcher,
because an armed watcher is permanently live while `urbi_step` keeps returning
`URBI_STEP_QUIESCENT`. An event loop sleeps on the step result;
`tests/chk/scheduler/waituntil_is_not_live_work.chk` pins the two predicates
apart.

## The install opcodes

| Opcode | Operands | Mode |
|---|---|---|
| `OP_AT_INSTALL` | A = cond closure, B = body, C = onleave | `AT` |
| `OP_AT_SYNC_INSTALL` | same | `AT_SYNC` |
| `OP_WHENEVER_INSTALL` | same; C doubles as the `else` body | `WHENEVER` |
| `OP_WAITUNTIL_INSTALL` | A = cond closure | `WAITUNTIL` |
| `OP_AT_EVENT_INSTALL` | A = event, B = body, C = onleave | `AT` |
| `OP_AT_EVENT_SYNC_INSTALL` | same | `AT_SYNC` |
| `OP_WHENEVER_EVENT_INSTALL` | same | `AT` |
| `OP_GETSLOT_CHANGE_EVENT` | A = dst, B = receiver, C = IC index | — |

`0xFF` in B or C means "absent". An event subscription fires per emission, so
`whenever (e?)` and `at (e?)` are the same watcher; only the SYNC form differs,
by running its body inline under `syncEmit`.

`OP_WAITUNTIL_INSTALL` evaluates its condition on the WAITING strand rather
than a spare, because it is the caller's own code in the caller's own
statement: a throw there propagates instead of being absorbed. A condition that
already holds does not park. A strand that may not park treats the wait as
already satisfied rather than wedging a C frame that is waiting for a value.

## Host entry points

`urbi_watch(vm, realm, expr, cb, ud)` compiles `expr` as an ordinary chunk and
installs an `AT` watcher whose condition is its root closure — a chunk's value
is its last statement's value, which for a one-expression source is the
expression. The callback takes the place of a body closure: there is nothing
for a native closure to be spawned ON, and routing it through one would only
add a cell whose job is to find `cb` and `ud` again. `cb` receives the
condition's VALUE, which is why `uwatch_eval_cond` leaves it in `w->payload`
before releasing the spare strand. Its return value is ignored.

## Where to look

| Question | File |
|---|---|
| The model, in the code | `src/rt/uwatch.h` |
| Drain, edges, fan-out, sweep | `src/rt/uwatch.c` |
| The install opcodes, the write notification | `src/rt/uexec_ops.c` |
| Events, tags, cross-strand stop | `src/rt/utag.c` |
| Drain sites, strand death | `src/rt/usched.c` |
| `Event` / `Tag` script surface | `src/rt/usched_natives.c` |
| `urbi_watch`, `urbi_realm_tag` | `src/rt/uapi.c` |
| Tests | `tests/rt/test_watch.c`, `tests/chk/reactive/**` |
