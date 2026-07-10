# Footprint tunables

Small-RAM targets tune the runtime's fixed per-strand and per-VM allocations
through a set of compile-time knobs. Each is an `#ifndef`-guarded macro with a
host-friendly default; embedded ports override it with `-D<KNOB>=<value>` at
compile time. The Makefile bundles the shipping cross builds' overrides into
one `FOOTPRINT_CFLAGS` preset so every non-host cross target tells the same
tuning story rather than drifting per-target.

## The FOOTPRINT_CFLAGS preset

| Knob | Define site | Default | Footprint | Effect |
|---|---|---|---|---|
| `UVM_STACK_CAP` | `runtime/uframe.h` | `2048` | `512` | register slots per strand |
| `UVM_MAX_FRAMES` | `runtime/uframe.h` | `64` | `24` | call frames per strand |
| `URBI_WATCHER_POOL_SIZE` | `watcher/uwatcher.h` | `64` | `16` | watcher slab entries |
| `URBI_EVENT_RING_DEPTH` | `event/uevent_ring.h` | `256` | `32` | ISR event ring depth (power of 2) |
| `URBI_IC_ENTRIES_PER_SITE` | `object/uic.h` | `4` | `2` | inline-cache ways per call site |
| `URBI_CLEANUP_MAX` | `runtime/ucleanup.h` | `64` | `16` | cleanup slots per strand |

The `make cross-arm`, `cross-riscv`, `cross-stm32f4`, `cross-pico`,
`cross-pico-repl`, and `cross-esp32s3-full` targets apply `FOOTPRINT_CFLAGS`
automatically. Custom embedder builds can pass the same `-D` set, or override
individual knobs to trade RAM for headroom.

## `UVM_STACK_CAP` — the per-port register-stack knob (H10)

Each strand owns a heap-allocated register array of `UVM_STACK_CAP` `UValue`
slots (16 B each). This is the single biggest per-strand cost, so it is the
first knob to tune on a RAM-constrained target. It is documented, per-port, and
`#ifndef`-guarded at its define site in `runtime/uframe.h`.

Per-strand cost is `sizeof(UStrand) + UVM_STACK_CAP * sizeof(UValue)`:

- **Default** (`UVM_STACK_CAP=2048`, `UVM_MAX_FRAMES=64`): UStrand is 3912 B,
  so a strand costs `3912 + 2048 * 16 = 36 680 B` — about **36.7 KB**.
- **Footprint, stack cap only** (`UVM_STACK_CAP=512`, frames unchanged):
  `3912 + 512 * 16 = 12 104 B` — about **12.1 KB**.
- **Full footprint preset** (`UVM_STACK_CAP=512` and `UVM_MAX_FRAMES=24`):
  the smaller frame array shrinks UStrand to ~1672 B (each `UCallFrame` is
  56 B; 64 - 24 = 40 fewer frames = 2240 B saved), so a strand costs
  `1672 + 512 * 16 = 9864 B` — about **9.9 KB**.

The `UStrand` size pin (CHSTR-041, `sched/ustrand.h`) is asserted only on
64-bit-pointer host builds where the footprint preset is not applied, so
reducing `UVM_MAX_FRAMES` on the 32-bit cross targets does not fire it.

`UVM_STACK_CAP` is an **interim, fixed-cap** knob. A `UVM_STACK_CAP` overflow
raises `urbi_vm_format_oom` rather than growing the stack. Grow-on-demand
register stacks (spill only what a deep call chain needs) are a v1.x item.
A shallow embedding — the mandelbrot and repl_demo workloads are ~5 nested
calls with ~10 locals each — is comfortable at 512 slots; deeply recursive
scripts should raise the cap per build.

## See also

- [`../internals/ports.md`](../internals/ports.md) — per-silicon footprint
  measurements and idiosyncrasies.
- [`../internals/build-system.md`](../internals/build-system.md) — per-target
  Make recipes.
