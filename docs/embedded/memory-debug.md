# Memory debugging

The memory-debug subsystem (`URBI_MEM_DEBUG`, v0.11.3) brings on-target
allocation tracking and corruption detection to targets where AddressSanitizer
and Valgrind cannot run. It compiles out to **zero bytes** by default — with the
gate off, the GC sidecar (`UAllCellsNode`) and `struct UVM` are byte-identical to
a normal build and every `URBI_MEM_*` macro is a no-op.

It is **EXPERIMENTAL** and **debug-only** — never ship it in a release build. It
requires the incremental GC (`URBI_GC_INCREMENTAL`, the default).

## Configuration flags

| Flag | Default | Effect |
|---|---|---|
| `URBI_MEM_DEBUG` | off (`0`) | master gate; off ⇒ zero code, zero data, no symbols |
| `URBI_MEM_QUARANTINE_DEPTH` | `16` | freed cells held (poisoned) before physical release |
| `URBI_MEM_REDZONE_BYTES` | `16` | trailing guard bytes appended to every allocation |

Build a debug runtime by defining the gate (use `-O0 -g` so GDB can read the
owner sidecar):

```sh
make TARGET=host-memdbg \
    CFLAGS="-std=c99 -O0 -g -DURBI_MEM_DEBUG=1" test
```

The gate is independent of `URBI_TRACE`, `URBI_PERF_COUNTERS`, and `URBI_DEBUG`,
and is allowed without `URBI_ENABLE_REPL` and under `URBI_BYTECODE_ONLY` —
embedded bring-up is exactly where it earns its keep.

## What each detector catches

- **Allocation owner tags.** Every GC allocation records, in the existing
  per-cell sidecar (debug builds only), a monotonic sequence number, the
  bytecode PC and decoded opcode that were executing, the C return address of
  the allocating call site, and the current strand id. This answers "what is
  consuming the heap / what allocated this object" on-target — the question
  ASan cannot answer in production. The C return address is best-effort (build
  `-O0`/`-Og`); the bytecode PC is the deterministic complement.
- **Trailing redzones.** Each allocation is over-sized by
  `URBI_MEM_REDZONE_BYTES` and the trailing guard is checked on demand; a
  mismatch is a buffer overflow past the cell. (Leading-redzone underflow
  detection is deferred to a later release.)
- **Poison-on-free + quarantine.** Freed cells are filled with a poison pattern
  and held in a quarantine ring instead of being returned to the allocator
  immediately. When a cell is evicted from quarantine (or at VM teardown) its
  poison is verified — a mismatch is a use-after-free, reported with the owner
  site captured at allocation.
- **Heap-lock violations.** Allocations attempted after `urbi_lock_heap()`
  already return `NULL`; in a debug build the denied attempt's owner site is
  recorded so you can see *which* call tried to allocate past the lock.
- **Host-handle + pin leaks.** Each `urbi_handle_create` records its creation
  site; releasing an already-free slot is flagged as a double-release. Cells
  pinned with `urbi_pin` that are never unpinned are reported as leaks.

All bookkeeping lives in a lazily-heap-allocated substate reached through a
single pointer on `struct UVM` (debug builds only), and is excluded from
`urbi_get_determinism_checksum` — a `URBI_MEM_DEBUG` build still passes the
determinism presets (see `make test-determinism-memdebug`).

## `Debug.memCheck()`

From a REPL/Debug build, `Debug.memCheck()` validates every redzone and the
quarantine poison at call time, then returns the violation and leak counters as
JSON:

```json
{"redzone_violations":0,"poison_violations":0,"double_frees":0,
 "live_handles":1,"pinned_cells":0,"heap_lock_violations":0,"alloc_seq":42}
```

Built without `URBI_MEM_DEBUG`, it returns a graceful note instead:

```json
{"note":"built without URBI_MEM_DEBUG"}
```

## GDB inspection

The walkers in `tools/gdb/urbi.py` read the owner sidecar host-side (live target
or core dump), so they work on a halted or wedged board over the usual
SWD/OpenOCD workflow:

```sh
gdb -x tools/gdb/urbi.py ./firmware-memdebug.elf   # built -DURBI_MEM_DEBUG=1 -g
(gdb) urbi-heap vm      # full live-cell walk: per-type cell and byte totals
(gdb) urbi-allocs vm    # top-N allocation sites by bytes (symbolized owner_ret)
(gdb) urbi-leaks vm     # live host handles + creation sites, pinned cells,
                        # heap-lock violations
(gdb) urbi-dump vm      # all of the above plus strands + trace tail
```

On a non-`URBI_MEM_DEBUG` binary the owner-tag walkers degrade gracefully
(`urbi-allocs` reports "no owner data"); `urbi-heap`'s cell walk still works on
any incremental-GC build.

## Limitations

- Incremental-GC-only. The fixed watcher pool and interned atoms bypass the
  single allocation choke point and are therefore not owner-tagged.
- The quarantine holds up to `URBI_MEM_QUARANTINE_DEPTH` freed cells, widening
  but not unbounding the use-after-free detection window; it is flushed and
  fully poison-verified at VM teardown.
- Leading redzones (underflow), root-provider imbalance detection, and a
  host-callable C reporting API are out of scope for this release.
