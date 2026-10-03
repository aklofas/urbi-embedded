# The runtime

Everything under `src/rt/`: what a value is, who owns memory, what a
strand is, how the scheduler decides what runs, where an error goes, and
how a watcher fires. The compiler frontend that feeds it is a separate
story — see [architecture.md](architecture.md) for the lexer, parser,
emitter and chunk format, and [reactive-runtime.md](reactive-runtime.md)
for the watcher chapter in full.

This replaces eight documents that described the runtime this one does
not: a generational collector, a module system, a loader strand, a
scratch-frame primitive, two watcher types. None of those exist. If you
find a reference to them anywhere, it is stale.

## The shape of it

The runtime is one archive layer with a strict include order, and the
order is the design:

    uvalue -> ugc -> ustr -> uobj -> ulist -> ustrand -> usched -> uexec -> uwatch -> urealm -> uboot

A header may include only headers to its left. `tests/scripts/check_rt_layering.sh`
enforces it, along with two other rules: `src/rt/` reaches outside itself
only to `chunk/` (the bytecode it runs), `urbi/` (the API it implements),
`stdlib/` (the boot table's installers) and `emit/ufront.h` (the one
compile entry point `load` needs; nothing else in the frontend); and it
uses no libc beyond `stdint.h`, `stddef.h`, `stdbool.h`, `string.h` and
`math.h`. The second rule is what lets the same source build for a
microcontroller.

The rule binds `src/rt/`, not the whole archive. Where a public entry point
is inherently hosted it lives in `src/host/` instead — `urbi_value_to_string`
needs `snprintf`'s `%.14g` — and a freestanding build omits that directory.
Three other directories in the archive do use hosted libc today and are not
covered by the rule: the cooperative eval service (`src/repl/`, four files,
unconditional), the `Debug` namespace (`src/stdlib/debug_namespace.c`), and
the AST arena (`src/util/uarena.c`). Narrowing those is Phase 5 work.

`struct UVM` is completed in `rt/uexec.h` and holds every subsystem by
value: the collector, the symbol table, the scheduler, the watcher state,
the prototype table, the realm list. There is no global mutable state,
so two VMs in one process share nothing.

## Values

A `UValue` is 16 bytes: a one-byte kind and an eight-byte payload union,
declared in the PUBLIC header `include/urbi/types.h` so the frontend, the
runtime and the embedder all agree on the layout without a second
definition. `src/rt/uvalue.h` adds the core's own `UV_*` spellings for
the same numbers and the `uv_*` constructors and predicates.

Floats are always `double`. The old `URBI_FLOAT_TYPE` build knob is gone:
an embedder compiling the library with one width and their own code with
another silently zeroed every float, and one number that cannot be got
wrong is worth more than the two bytes the knob saved.

Two of the kinds are strings, and the distinction matters:

- `UV_SYM` is an interned symbol. It lives in the VM's one string table,
  is hashed once, compares by pointer, and is **immortal** — the table
  never shrinks. Slot keys are symbols. So are identifiers, which is why
  the emitter interns through the same table (`ustr_intern` in
  `src/emit/ufront.c` is implemented over it): a name the compiler wrote
  down and a name the runtime looks up are the same pointer.
- `UV_STR` is a garbage-collected string cell. Concatenation produces
  one. So does `asString`, `charAt`, `toUpper` and every other method
  whose result is a value rather than a key — anything else would make a
  program that prints a changing number grow without bound.

## Memory

One collector: stop-the-world mark-sweep, in `src/rt/ugc.c`. Not
generational, not incremental, no write barriers to get wrong. It runs at
allocation points and at `urbi_gc_collect`, never underneath a native
that is halfway through building a value.

Every heap thing is a `UCell` with a type byte and mark bits: strings,
objects, closures, upvalues, lists, dicts, tags, events, strands,
watchers, bound protos. A cell is reached from a root or it is not.

Roots are: the realm list and each realm's globals, the run queue and the
timer heap, every live strand's register stack and cleanup stack, the
prototype table, the spare-strand free list (capped at four, so one deep
nesting does not become a permanent memory floor), and the C-root stack a
native pushes when it holds a value across an allocation.

Pinned cells are roots too, and there are two pin bits, deliberately kept
apart. `UCELL_F_PINNED` is the HOST's, taken by `urbi_ref` and released
only by `urbi_unref`; the runtime never touches it, which is what makes
that guarantee in `<urbi/urbi.h>` true. `UCELL_F_RTPIN` is the runtime's
own short-lived hold — a fresh chunk or closure reachable from nothing
yet, an awaited strand kept addressable across a pump, a user object held
across the two allocations that install its `changed?` event. It is one
bit and therefore not nestable: every site releases it before returning,
and a hold that must outlive a call goes on the C-root stack instead.

Marking is tri-state with an explicit gray list, and the gray list can
overflow: when it does the collector rescans until the graph is clean
rather than silently dropping work.

Pacing: the bytes allocated since the last cycle — cells and the raw
arrays cells own (slot tables, list items, strand stacks and frames)
alike — are compared with twice the live size the last cycle left, or
16 KB if that is larger. The comparison is made at each cell allocation
and after each scheduler slice, and a cycle starts there when the count
is over; a raw allocation counts but never starts a cycle itself, because
its callers hold unrooted cells across it. The baseline is a snapshot
(`pace_base`) and does not move between cycles. It used to be
`bytes_live`, which a raw allocation raised as it was made; for garbage
whose raw block is at least as big as its cell, which is every list,
every object with slots and every strand, the limit then grew faster
than the count it was compared with, and a loop making such garbage never
collected at all. `bytes_live` itself is the cell bytes that survived the
last cycle plus the raw arrays live now: the cell half is written only by
a cycle, so read it without collecting first and you get the previous
cycle's answer.

`URBI_GC_STRESS=1` collects before every cell allocation. It is the
highest-leverage way to find a rooting gap, and it runs the whole suite
in CI.

## Objects

A slot is a name and a value, and an object is a parallel array of them
plus a list of prototypes. No shapes, no transition trees: lookup itself
is still a linear scan of the local slots by interned pointer, then a
depth-first walk of the proto graph guarded by a per-VM visit stamp so a
diamond is searched once. A per-site cache (below) sits in front of that
walk; it does not replace it.

Multiple prototypes are supported and `addProto` prepends, which is what
the legacy implementation did. Slot flags carry constness and getter or
setter behaviour.

### The slot cache

`OP_GETSLOT`, `OP_SELF`, `OP_SETSLOT` and `OP_SETSLOT_UPDATE` each carry
a site index the chunk format assigns, and `src/rt/uslotcache.h` keeps
one `USlotCache` entry per site: the receiver it was filled for, the
object the slot was found on, and — for an inherited hit — the VM's
slot epoch at fill time. A hit skips the walk; a miss falls back to it
and refills the entry.

- **Own slot** (the receiver holds it directly): the entry is checked
  against the LIVE receiver — same object, the index still in range, the
  name at that index unchanged — so it can never be stale and needs no
  invalidation, even when a freed receiver's address is reused.
- **Inherited slot** (found on a prototype): the entry is checked
  against the VM's one slot epoch. The epoch is bumped by `src/rt/uobj.c`
  on a structural change — a slot added or removed, a slot's attributes
  changed, or the proto list edited — to any object flagged
  `UOBJ_F_CACHED`. `UOBJ_F_CACHED` is set on every object a
  cache-filling walk passes through: the receiver, each intermediate
  proto, and the owner. The one attribute set outside `uobj.c` bumps
  nothing: the change-event bit `src/rt/uwatch.c` sets when a watcher
  subscribes to a slot, or when the slot it subscribed to is created. No
  hit's validity depends on it, and a write hit reads it live.
  An object no walk has visited is unflagged, so building and populating
  fresh objects in a loop bumps nothing. The flag is never cleared, and
  a collection bumps the epoch too; once the epoch passes `0x80000000` a
  collection resets it to 1 and clears every proto's cache array, so a
  stale inherited entry can never meet its own epoch again after a wrap.
- Either kind of hit reads the slot's attributes live: a slot carrying a
  getter, a setter, or — on a write — a constant flag takes the walk
  instead. A write hit is own-slot only (a write that resolves on a
  proto still creates a local slot the existing way) and checks
  `UOBJ_F_READONLY` on the receiver live.
- Entries are **weak** — the collector does not trace them — which is
  what makes the epoch bump enough: one bump retires every inherited
  entry at once rather than needing each one visited.
- The array is allocated lazily, on a proto's first slot operation
  *after* `vm->stdlib_booted` is set, so nothing the boot table touches
  is ever cached and the boot-heap number does not move. Allocation
  failure is not an error: the site just runs uncached.
- `URBI_SLOT_CACHE_VERIFY=1` makes every hit also run the uncached
  resolve and trap if owner or index disagree with it; `make
  test-cache-verify` builds and runs the whole suite that way.

`tests/probes/lookup_bench.c` is what the cache is for: it pins the
lookup cost against the old core's, on the dev box.

The distinction between creating a slot and updating one is visible in
the bytecode: `var x = 1` emits `OP_SETSLOT`, and a bare `x = 1` emits
`OP_SETSLOT_UPDATE`, which resolves the name through the proto chain and
raises `LookupError` when nothing answers. Whether a name resolves is a
question about the object graph, so it is a runtime question, and the
compiler does not pretend to answer it.

## Strands

A strand is a coroutine: a growable register stack, an array of call
frames, a list of open upvalues, and a cleanup stack. `sizeof(UStrand)`
is 192 bytes on a 64-bit host — the fixed struct, not counting the arrays
it grows — and a strand parked on a `sleep` costs 616 bytes all in, which
`tests/probes/strand_cost.c` measures on every build.
`tests/rt/test_strand.c` pins the struct size exactly, so a field added
without thinking about the idle-strand budget fails the build rather than
drifting.

The register stack grows on demand. The old core had a fixed per-VM cap
(`UVM_STACK_CAP`) and overflowing it raised an out-of-memory error the
program could do nothing about.

The cleanup stack is the whole of non-local control flow. Each entry
guards either a `try` frame or a tag scope, and carries the flag bits the
bytecode wrote — `FLAG_HAS_CATCH`, `FLAG_HAS_FINALLY`, `FLAG_HAS_ONLEAVE`
in `src/chunk/uchunk.h`, restated in `rt/ustrand.h` as `UCLEAN_F_*`
because the runtime sits below the chunk format in the include order.

Upvalues are open until the frame that owns them is popped, then closed
in place. `ustrand_pop_frame` closes at the popped base, so `OP_RET` must
not close them a second time.

## Dispatch

`uexec_run_inner`, in `src/rt/uexec_ops.c`, dispatches through a label
table on GCC and Clang — one address per opcode, generated from the
`chunk/uopcodes.def` X-macro so a new opcode cannot be left out of it —
and falls back to a plain `switch` everywhere else, and under
`URBI_VM_FORCE_SWITCH`, which `make test-switch` builds so the portable
path stays honest. One set of opcode bodies, written against
`OPCASE`/`NEXT`/`NEXT_RELOAD`, serves both forms; there is nothing to
drift apart because there is only one copy.

The current frame, its register window and its constants pool live in
three locals (`f`, `R`, `K`) between instructions. They are loaded once
on entry and reloaded — `NEXT_RELOAD`, where a plain `NEXT` ends an arm
that provably cannot have moved them — only after an instruction that
can push or pop a frame, unwind, resume, or grow the strand's register
stack, including a slot read that runs a getter.

### The yield fast path

`OP_YIELD` — the `;` sequence point — normally hands the strand back to
the scheduler, which re-enqueues and re-dispatches it, even when it was
the only runnable strand. A strand that may not be descheduled (a spare,
or one inside a synchronous call such as a getter or a comparator) never
reaches the fast path at all: for it a yield is the plain sequence point
it also is, and nothing more.

For everyone else, the arm stays on the same strand for the next
instruction — instead of making the round trip through the scheduler —
whenever ALL of the following hold, checked after the drain that a dirty
watch state always gets first:

- the ready queue is empty (`vm->sched.run_head == NULL`);
- nothing is dirty (`vm->watch.ndirty == 0`), which is true either
  because nothing was dirty to begin with or because the drain that just
  ran cleared it;
- the strand itself is not unwinding, carries no gate, and is still
  `RUNNING` — nothing the drain (or anything else at this safepoint) did
  left it with somewhere else to be;
- the ISR injection ring is empty;
- fewer than `UEXEC_FAST_YIELD_CAP` (64) fast yields have been taken in a
  row on this strand. The counter resets wherever the scheduler
  dispatches the strand (`usched_step`, `usched_run_inline`).

The instruction budget is untouched — only a backward jump spends it.
The consequence is for budgeted stepping. A strand that has nobody to
yield to runs up to 65 statements (64 fast yields, then the `;` that
returns) before going back to the scheduler, where it used to go back
at every `;`. Under `urbi_step` with a budget, a lone strand therefore
advances up to 65 statements per slice where it advanced one. That holds
for a loop with a `;` in its body as well as for straight-line code; a
loop without a `;` is still bounded per slice by the backward-jump
budget.

Timers are fired and the ISR ring drained only at the start of a step
(`usched_step`, before its dispatch loop), and the host writes only
between steps. A step with budget B dispatches a lone strand in
ceil(B / 256) slices, so up to 65 × ceil(B / 256) of its statements run
before a timer that came due is fired or a host write is seen, where it
was ceil(B / 256): 65 for a budget of 256, 130 for 512. The bound is per
slice, so it grows with the budget; a host that wants timers noticed
sooner passes a smaller budget. While an injection is pending, the ring
check refuses the fast path, so the strand stops at every `;` as before.

Strands that are READY TOGETHER keep their relative order, because the
fast path is only ever taken when no other strand is ready. Strands that
become ready at different times can interleave differently than before:
two strands whose timers are 1 ms apart, stepped with a budget of 256 and
the clock advanced 1 ms per step, logged
`a1 a2 b1 a3 b2 a4 b3 a5 b4 b5` and now log
`a1 a2 a3 a4 a5 b1 b2 b3 b4 b5`. Unbudgeted stepping runs until nothing
is runnable and gives the same results as before.

## The scheduler

One run queue, one timer heap, and `park`/`wake`, in `src/rt/usched.c`.
`usched_step` dispatches strands from the queue until its budget is
spent or nothing is runnable. Under a budget each dispatch gets a slice
of at most `USCHED_SLICE` (256); an unbudgeted step's slices have no
limit. A host that wants to interleave the VM with its own work calls
`urbi_step` in a loop with a budget and sleeps on
`next_wake_us` in between. Within a slice only backward jumps consume
budget. A strand returns from its slice at a `;` (after the fast path
above has let up to 64 of them pass), at a park, at its death, or when
the slice's backward jumps run out; a `READY` return is charged the whole
slice and a park or death is charged 1.

A host that wants every unbudgeted `urbi_step` to be a bounded slice sets
`UVMConfig.step_budget` at `urbi_open` rather than repeating the number at
each call; an explicit budget always wins, and leaving it unset keeps a
zero budget meaning "until nothing is runnable".

`urbi_step` returns one of three answers: it ran something, it is idle
until a deadline, or it is quiescent — nothing will ever make it runnable
again without the host doing something. Liveness is DERIVED from the
queue and the heap, never tracked as a separate counter that can drift: a
strand parked on an event nobody will emit is not live work.

Tags are cancellation handles. A tag scope is a cleanup entry, so
`tag.stop()` from another strand unwinds the target through its finallys
and its leave handlers rather than dropping it. `block` and `freeze` are
independent gate bits on the strand, checked when the scheduler is about
to run it — RECOMPUTED, not merely cleared, on a release and on leaving a
tag scope (`OP_SCOPE_POP`), from every tag still covering the strand. A
strand inside two blocked tags therefore stays held until both let go
rather than losing the bit when either one does.

Forks are strands. `a , b` spawns and forgets; `a & b` spawns and joins.
A strand nobody awaits reports an uncaught throw through the diagnostic
hook, because there is no caller to return a code to.

A strand that dies goes on the dead list, which roots it only until the
scheduler reaps it: right after the slice it died in, before the
collection check that follows every slice, and at both ends of a step.
When a parent that cannot park (a spare, or a synchronous call) joins,
the child runs on the spot (`usched_run_inline`) until it ends or parks;
one that ended is reaped there, one that parked is neither finished nor
reaped, and the join goes on without it. Reaping unlinks the strand from
its realm; whatever still refers to it — the joining
parent's register, a `Job` value, the pin `urbi_run` holds on the strand
it awaits — keeps it, and the rest is garbage. Reaping only at the ends of
a step kept every strand an unbudgeted step saw die, however often the
collector ran.

A watcher whose body or else arm cannot be spawned for want of memory
says so the way a detached strand's uncaught throw does: through the
diagnostic hook and `urbi_last_error`, with `URBI_ERR_OOM` and a message
naming the construct as the script wrote it and the arm ("at body",
"at onleave", "whenever body", "whenever else": out of memory), cleared by
the next step. The same report is made once per step however often the
spawn is refused again. The watcher stays armed, and every drain that
follows retries:

- a condition watcher's body that could not start leaves its rising edge
  unserved, so the next drain sees the condition rise again;
- an else arm that could not start is still owed: the next drain that
  finds the condition false runs it. A condition that has risen again by
  then has fallen and risen unseen, and the pair collapses into the rise.
  A `whenever` whose re-fire was refused keeps, the same way, the else
  arm its earlier bodies earned;
- an event's emission cannot be asked again, and is only reported.

## Errors

**One channel.** Every runtime failure is a throw: a value lands in
`s->transfer`, `s->unwind` becomes `UUNWIND_THROW`, and the cleanup-stack
walker in `src/rt/uunwind.c` is the only mechanism that moves control
anywhere else. `return` and a tag stop travel the same stack, which is
what makes a `return` out of a `try` run its `finally` and a stopped tag
scope fire its `leave`.

A chunk-integrity failure is the one throw a script cannot handle. The
load-time verifier checks one proto at a time, so a callee that closes or
resumes a scope its caller opened, or an `OP_UNWIND_TO` deeper than its
frame's scopes, is caught at run time instead, in every build:
`uexec_fatal` builds the same exception object a `TypeError` would, with a
message starting `chunk integrity:`, and sets `s->unwind` to
`UUNWIND_FATAL`. The walker carries it out through every frame without
letting a `catch` take it or running a `finally` body or a tag `leave`,
and it is reported like any uncaught throw.

Nothing is swallowed. An uncaught throw sets `URBI_ERR_UNCAUGHT_THROW`
and renders the value into `vm->last_error`, whatever the value was — the
old core answered `nil` for a scalar throw, which is how errors vanished
on the batch path.

`vm->last_error` is ONE buffer, which has a consequence worth knowing: a
detached strand dying later in the same pump would otherwise overwrite
what an awaited strand left there, so `uexec_run_chunk` renders the
awaited strand's escape again once its pump is over — at the one point
where the strand is still pinned and the caller has not yet read. A detached strand's
message goes to the diagnostic hook at the moment it dies.

Rendering a thrown value happens under the freestanding rule, so the core
can only spell what it can build from integer digits. A hosted build
hands it `urbi_value_to_string` through `vm->render_value`, which is
where `"%.14g"` comes from; without the hook a non-integral float renders
as `<?>`. An exception object is the one shape that never defers: it
contributes its `message`, where the host formatter would print an
address that is not reproducible across runs.

## Watchers

One watcher type. `at`, `at sync`, `whenever` and `waituntil` differ by a
mode byte and by which of `cond` or `event` is set. There is no separate
event-watcher struct, no per-watcher read set, no cascade rescan and no
generation stamp; the old core had all four, and they are where its
reactive bugs lived.

A slot read inside a running condition marks its object watched, and a
write to a watched object marks the VM dirty. One drain per step
re-evaluates the conditions. [reactive-runtime.md](reactive-runtime.md)
has the full account, including what `at` edges mean and why `whenever`
is level-triggered.

## Realms

A realm is one script world: its own globals object, its own strands, its
own output writer. Every realm's globals inherits from the single per-VM
`root_globals` that holds every built-in, so creating a realm allocates
the realm cell and one object — the standard library is shared through
the proto chain, not copied. That is what makes a per-session REPL realm
affordable.

## Booting

One table. `src/rt/uboot.c` has a row per built-in naming its global, its
slot in `vm->protos[]`, its parent, and its method list, and `uboot_init`
walks it in six labelled passes:

1. allocate every prototype, so no row can depend on another's position;
2. link the parents;
3. install the methods (`isA` goes on the Object root here);
4. build the shared `root_globals` object every realm inherits from;
5. install the constant and default SLOTS the table has no column for,
   and run the six per-proto init hooks;
6. run the `stdlib.u` script overlay, then apply the read-only seal.

The seal is last for a reason: passes 1–5 and the overlay all need to
write to objects that are read-only from the moment it lands. The old
core booted through eighteen hand-ordered registration functions whose
ordering constraints lived only in comments.

The standard library reaches the runtime through exactly one header,
`src/rt/ustdlib_glue.h`. A native method never includes `rt/uobj.h` or
`rt/ugc.h` directly; the layering gate checks that too.

Part of the standard library is urbiscript rather than C:
`src/stdlib/stdlib.u` is compiled at build time by
`tools/urbi-compile-stdlib` into a tracked bytecode blob that
`uboot_init` loads. Iteration (`map`, `filter`, `each`, `foldl`) lives
there, where it needs no C rooting at all.

## The C API

44 functions in `include/urbi/urbi.h`, plus the four-function eval
service in `include/urbi/repl.h` and the inline value helpers in
`include/urbi/types.h`. `docs/embedding-guide.md` walks a
complete program; `docs/api-surface-tiers.md` is the manifest the
`test-api-manifest` gate checks.

The shape an embedder sees: `urbi_open` takes the allocator (there is no
default — the library never calls `malloc` on its own), `urbi_run`
compiles and runs source on a realm, `urbi_step` advances the scheduler
one slice, `urbi_register` binds a C function to a name, and
`urbi_close` takes everything back. `UVM` is opaque.

## What is not here

Parked until Phase 5, in the tree but out of the build: the networked
REPL server and its transports (`src/repl/`, all but four files), the
ROS2 bridge (`src/ros/`), the Standard Robotics overlay
(`src/urobotics/`), and every hardware port under `examples/` and
`components/`. See [ports.md](ports.md).

Gone with the old runtime and not parked: the trace spine, the
performance counters and the memory-debug sidecar. Phase 5 re-derives
whatever the new core wants of them from git history rather than from a
translation unit that no longer compiles.
