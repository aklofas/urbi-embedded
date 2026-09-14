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
rather than silently dropping work. Pacing is by bytes allocated since
the last cycle, and `bytes_live` is a number the collector WRITES at the
end of a cycle, not one the allocator maintains — read it without
collecting first and you get the previous cycle's answer.

`URBI_GC_STRESS=1` collects before every cell allocation. It is the
highest-leverage way to find a rooting gap, and it runs the whole suite
in CI.

## Objects

A slot is a name and a value, and an object is a parallel array of them
plus a list of prototypes. No shapes, no transition trees, no inline
caches: lookup is a linear scan of the local slots by interned pointer,
then a depth-first walk of the proto graph guarded by a per-VM visit
stamp so a diamond is searched once.

That is a deliberate simplification and it costs measurable speed — see
`tests/probes/lookup_bench.c`, which pins the cost and says what it would
take to get it back.

Multiple prototypes are supported and `addProto` prepends, which is what
the legacy implementation did. Slot flags carry constness and getter or
setter behaviour.

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
it grows — and a strand parked on a `sleep` costs 615 bytes all in, which
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

## The scheduler

One run queue, one timer heap, and `park`/`wake`, in `src/rt/usched.c`.
`usched_step` gives one strand a slice of `USCHED_SLICE` (256)
instructions and returns, so a host that wants to interleave the VM with
its own work calls `urbi_step` in a loop and sleeps on `next_wake_us`
in between. Only backward jumps consume budget, so a straight-line strand
runs to its next park or death regardless.

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
to run it.

Forks are strands. `a , b` spawns and forgets; `a & b` spawns and joins.
A strand nobody awaits reports an uncaught throw through the diagnostic
hook, because there is no caller to return a code to.

## Errors

**One channel.** Every runtime failure is a throw: a value lands in
`s->transfer`, `s->unwind` becomes `UUNWIND_THROW`, and the cleanup-stack
walker in `src/rt/uunwind.c` is the only mechanism that moves control
anywhere else. `return` and a tag stop travel the same stack, which is
what makes a `return` out of a `try` run its `finally` and a stopped tag
scope fire its `leave`.

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
