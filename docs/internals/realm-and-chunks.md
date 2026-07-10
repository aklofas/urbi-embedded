# Realm and Chunks

## Overview

A `URealm` is the per-execution-context type. It owns the top-level globals
visible to bytecode (via `realm->global_object`), an implicit cleanup `UTag`,
and a list of strands created under it. Chunks are compiled artifacts
represented by a root `UProto`; their per-VM mutable IC state lives in
`UChunkInstance` cells. Each VM keeps a single linked list of every live
`UChunkInstance` rooted at `vm->module_instances_head`;
`urbi_get_or_create_chunk_instance` walks that list and prepends a fresh
entry on miss.

Source: `src/realm/urealm.{h,c}`, `src/realm/urealm_globals.{h,c}`,
`src/realm/urealm_namespace.c`, `src/object/uchunk_instance.{h,c}`,
`src/chunk/uproto.{h,c}`, `src/chunk/uchunk.{h,c}`,
`src/chunk/uchunk_strand.c`, `src/vm/uvm_run.c`,
`include/urbi/urbi.h`.

---

## Realm structure

`URealm` (defined in `src/realm/urealm.h`) holds:

- `vm`, `id` (per-VM monotonic, never reused; first assigned `1`), `flags`
  (`REALM_GLOBAL` / `REALM_REPL` / `REALM_MODULE`).
- `tag` — implicit cleanup boundary; created at realm-create, freed by
  `urbi_realm_destroy`.
- `bindings` — `UNamespace`, a flat name-to-`UValue` array used as a
  side-channel by the determinism checksum, the GC root walker, and tests.
  Production global reads/writes do **not** route through this map.
- `global_object` — a fresh `UObject` (atom family `URBI_ATOM_OBJECT`)
  populated at create time with the 15 built-in entries from
  `urbi_builtin_registry[]`. This is what `OP_GETSLOT` / `OP_SETSLOT` and
  the `urbi_realm_*_global` C API target.
- `strands_head` — singly-linked list of `UStrand` objects allocated under
  this realm. Walked by `urbi_realm_destroy` before `utag_destroy` so the
  tag's member-strand list is empty by the time the tag is freed.
- `loaded_protos_head` — singly-linked list (via `UProto.next_in_realm`) of
  every root `UProto` loaded into this realm. Populated by `urbi_load_chunk`
  and `urbi_run_chunk` via head-insertion; unlinked and freed by
  `urbi_chunk_free` or `urbi_realm_destroy`.
- `prev_in_vm` / `next_in_vm` — doubly-linked list rooted at
  `vm->realms_head`; head-insertion at create.

`urbi_realm_global(vm)` lazily allocates the VM's anonymous global Realm on
first call and sets `REALM_GLOBAL`.

---

## Per-realm globals API

Public surface in `include/urbi/urbi.h`; implementation in
`src/realm/urealm_globals.c`. All three accept a raw byte string (`name`,
`name_len`) which is interned via `ustr_intern`.

```c
int urbi_realm_set_global       (UVM *vm, URealm *realm,
                                 const char *name, size_t name_len, UValue value);
int urbi_realm_set_global_const (UVM *vm, URealm *realm,
                                 const char *name, size_t name_len, UValue value);
int urbi_realm_get_global       (UVM *vm, URealm *realm,
                                 const char *name, size_t name_len, UValue *out_value);
```

- `set_global` rejects writes to a slot whose `URBI_SLOT_FLAG_CONSTANT` bit
  is already set, returning `URBI_ERR_CONST_SLOT_WRITE` — otherwise host
  code could silently bypass `CONSTANT` via the non-const variant.
- `set_global_const` routes through a shared `realm_install_const` helper
  also used by `urbi_populate_realm_globals`, so the registry-driven boot
  path and the public API stay in lockstep. Same
  `URBI_ERR_CONST_SLOT_WRITE` on overwrite of an existing constant.
- `get_global` resolves through the prototype chain. Returns
  `URBI_ERR_SLOT_NOT_FOUND` on miss; `URBI_ERR_PROTO_DEPTH` if the 64-deep
  resolve stack is exhausted (distinct from OOM).

`urbi_register_event_drain(vm, h)` (declared in `urbi.h`, defined in
`src/vm/uvm_init.c`) installs an optional host callback fired at every
safepoint for each entry in the ISR SPSC event ring. Pass `NULL` to remove.
Step-quiescence is enforced: the install asserts `vm->cur_strand == NULL`
(must not be called from inside an `urbi_step` / `urbi_vm_run` slice). The
write uses `__ATOMIC_RELEASE`, paired with `__ATOMIC_ACQUIRE` in
`uevent_ring_drain`.

---

## Chunk instance cache

`UChunkInstance` (`src/object/uchunk_instance.h`) is the per-VM IC RAM tier
that mirrors the read-only root `UProto`. Two GC cells:

- `UChunkInstance` (`UTYPE_MODULE_INSTANCE`) — header, root `UProto*`, `vm`,
  `proto_instances`, and `next_in_vm`.
- `UProtoInstanceArr` (`UTYPE_PROTO_INSTANCE`) — single bulk allocation
  holding `entries[1 + root->nested_count]` plus the contiguous IC
  tables. `entries[0]` is the root proto-instance; `entries[1..n-1]` mirror
  `root->nested[]`.

### `urbi_get_or_create_chunk_instance`

```c
UChunkInstance *
urbi_get_or_create_chunk_instance(struct UVM *vm, UProto *root);
```

Walks `vm->module_instances_head` looking for a matching root proto;
on hit, returns the cached entry. On miss, calls
`urbi_chunk_instance_create`, which prepends the new instance at the head
of the list. The cost is O(N) in distinct loaded chunks per VM (typically
< 10).

The walk-then-prepend is **unsynchronised**. Single-threaded-VM contract:
the caller must not invoke this from multiple host threads concurrently
against the same `vm`. The current `URBI_SCHED_COOPERATIVE` baseline makes
this safe; parallel-realms support is a post-`v1.0` expansion.

### Auto-binding from chunk-run paths

- `urbi_run_chunk` (`src/chunk/uchunk_strand.c`) resolves the realm (NULL →
  global) and calls `urbi_vm_run`. No separate pre-create — the strand's
  cache resolution happens inside `urbi_vm_run` itself.
- `urbi_vm_run` (`src/vm/uvm_run.c`) unconditionally calls
  `urbi_chunk_instance_create(vm, root)` for its transient strand.
  Forcing a fresh create defends against the REPL pattern of
  stack-allocating a root `UProto` and reusing the same address — a cache
  hit on a stale stack-allocated proto would hand out an instance with freed
  `ic_names`.

---

## `ic_name_strs` lazy interning

`UProto.ic_names` and `UProto.ic_name_strs` are parallel arrays per proto
(both root and nested), sized to `UProto.ic_count`. The deserializer cannot
intern (interning needs a VM in scope, and the loader does not have one), so
it instead populates `ic_name_strs` with freshly-allocated UTF-8 copies and
leaves `ic_names == NULL`.

`intern_ic_names_from_strs` (in `src/object/uchunk_instance.c`) closes the
gap on first `urbi_chunk_instance_create`. For each proto in the tree:

1. If `ic_count == 0`, return success.
2. If `ic_names != NULL`, return success — already interned.
3. If `ic_name_strs == NULL`, return failure (deserializer never ran).
4. Allocate a `USymbol *` array of length `ic_count`.
5. Walk `ic_name_strs[k]`, call `ustr_intern`, populate the new array.
6. Publish into `ic_names`.

The helper is **idempotent** — second and subsequent
`urbi_chunk_instance_create` calls on the same root proto are no-ops.

---

## Chunk-load contract

`uchunk_deserialize(root, buf, size, alloc_fn, alloc_ctx, errmsg, errcap)`
allocates a root `UProto` from a `.urb` byte buffer. Refer to
[bytecode-format.md](bytecode-format.md) for the wire format itself.

The public API is `urbi_chunk_from_bytes(buf, len, errmsg, errcap)`, which
calls `uchunk_deserialize` with the default allocator, then registers the
root proto onto the target realm's `loaded_protos_head` list.

### Strict zero on header bytes 16–23

`v1.0` defines no flag bits in the 8-byte reserved region at header offsets
16–23. The loader rejects any non-zero byte in that range with
`UCHUNK_LOAD_CORRUPT` and a diagnostic `"non-zero reserved byte 0x%02x at
offset %zu"`. Earlier forward-compat tolerance silently dropped flags older
builds did not recognize, which is the wrong policy when bytecode stability
is not yet promised.

### Partial-buffer-on-error policy

On any non-OK return other than `UCHUNK_LOAD_INVALID_ARG` (NULL root or
buffer), the root `UProto` may hold partial allocations from whichever
decode section completed before the failure. `uchunk_destroy(root, vm)` is
safe in **either** case — success or failed-partial — and is the correct
cleanup path either way. Internally, every per-section decoder
zero-initialises its target slots before populating, so the destroy walk
sees well-defined NULLs even after mid-section failure.

`ic_names` interning is deferred to the first
`urbi_chunk_instance_create` (above); deserialize itself does not require
a VM. See also [object-model.md](object-model.md) for the per-VM IC RAM
tier and how interned `ic_names` feed `UIC.name`, and
[closures.md](closures.md) for how `UClosure` carries its enclosing
`proto_inst` for IC dispatch.

---

## Single-threaded VM assumptions

Two structures in this area assume a single-threaded VM and are pinned to
the `v1.0` `URBI_SCHED_COOPERATIVE` baseline:

- **`urbi_get_or_create_chunk_instance`** — walk-then-prepend on
  `vm->module_instances_head` is unsynchronised.
- **The deferred slot-change ring** (`vm->deferred_slot_changes`, capacity
  `URBI_DEFERRED_SLOT_CHANGE_RING_SIZE`) — entries are weakly referenced
  and survive only on the safepoint-ordered invariant `defer-site → next
  safepoint → drain → urbi_vm_watcher_eval_dirty`. The cooperative scheduler steps
  GC only at the dispatch-loop safepoint, and the drain runs before any
  potential GC trigger at that same safepoint. A preemptive scheduler
  would have to upgrade the ring entries to strong refs via a GC root
  provider that walks `vm->deferred_slot_changes[head..tail]`.

Multi-threaded VM contracts (parallel realms, `URBI_SCHED_PREEMPTIVE`,
cross-thread instance-cache lookup) are a post-`v1.0` expansion.

---

## Stdlib boot integration (M6 Wave 2)

`urbi_stdlib_boot(vm)` runs from inside `urbi_populate_realm_globals`
on the first realm-create per VM (the `vm->event_proto == NULL`
guard).  Wave 2 (Phase 4) extends Wave 1's two-step C-native boot
into a three-step boot:

1. **C-native Object root methods** — `urbi_object_root_register`
   installs `setSlot` / `getSlot` / `clone` / etc. on `vm->atom_object`.
2. **C-native atom proto stubs** — `urbi_atom_protos_register`
   allocates `Boolean` / `Nil` / `Void` singletons and installs the
   minimum Wave-1 method set (`Boolean.toString`, `String.length`).
3. **Baked-bytecode load** — when `urbi_stdlib_bytecode_len > 0`,
   `uchunk_deserialize` parses the blob into a heap-allocated root `UProto`
   stored on `vm->stdlib_module`, then
   `urbi_get_or_create_chunk_instance` binds a per-VM
   `UChunkInstance` so the IC machinery sees the chunk's
   `ic_name_strs` lazy-interned to live `USymbol *`.

The baked blob comes from `tools/urbi-compile-stdlib`, which walks
`src/stdlib/STDLIB_ORDER.txt` at build time, compiles each `.u` to
v1.5 bytecode, concatenates the buffers, and emits
`src/stdlib/urbi_stdlib_bytecode.gen.c` exposing
`urbi_stdlib_bytecode[]` and `urbi_stdlib_bytecode_len`.  The two-pass
build is described in [build-system.md](build-system.md).

**Parser-independent.** Step 3 deserializes pre-compiled bytecode; it
never invokes the lexer / parser / emitter pipeline.  Phase 13's
`URBI_BYTECODE_ONLY` smoke build verifies the runtime can satisfy the
boot path with the front-end stripped out — a prerequisite for the M7
embedding contract where freestanding targets ship liburbi.a without
the source-compile path linked in.

**Phase 4 baseline.** `STDLIB_ORDER.txt` is empty, so the blob is
0 bytes and step 3 is dead code.  Phase 10 populates the order file
and the deserialize+bind branch becomes live.

**Run-end deferral.** `urbi_stdlib_boot` does **not** run the stdlib
chunk's root proto — it is reachable from the realm-create path, and
`urbi_run_chunk` would re-enter realm population.  Phase 10 wires a
deferred-run hook that fires once the global Realm is fully
populated.  Errors during deserialize or bind surface as
`URBI_ERR_STDLIB_BOOT_FAILED` (slot −15 in `UErrCode`); allocation
failures still surface as `URBI_ERR_OOM`.

Chunk ownership: `vm->stdlib_module` is a root `UProto*` freed via
`uchunk_destroy` plus `vm->alloc_fn(_, 0, _)` from inside
`urbi_vm_destroy`, sequenced **after** `urbi_gc_destroy` so any
`UChunkInstance` referencing the chunk has already been reaped.
