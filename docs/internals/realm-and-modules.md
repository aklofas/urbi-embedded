# Realm and Modules

## Overview

A `URealm` is the per-execution-context type. It owns the top-level globals
visible to bytecode (via `realm->global_object`), an implicit cleanup `UTag`,
and a list of strands created under it. Modules are read-only artifacts
(`UModule`); their per-VM mutable IC state lives in `UModuleInstance` cells.
Each VM keeps a single linked list of every live `UModuleInstance` rooted at
`vm->module_instances_head`; `urbi_get_or_create_module_instance` walks that
list and prepends a fresh entry on miss.

Source: `src/realm/urealm.{h,c}`, `src/realm/urealm_globals.{h,c}`,
`src/realm/urealm_namespace.c`, `src/object/umodule_instance.{h,c}`,
`src/module/umodule.{h,c}`, `src/module/uchunk.c`, `src/vm/uvm_run.c`,
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

## Module instance cache

`UModuleInstance` (`src/object/umodule_instance.h`) is the per-VM IC RAM tier
that mirrors the read-only `UModule`. Two GC cells:

- `UModuleInstance` (`UTYPE_MODULE_INSTANCE`) — header, `module`, `vm`,
  `proto_instances`, and `next_in_vm`.
- `UProtoInstanceArr` (`UTYPE_PROTO_INSTANCE`) — single bulk allocation
  holding `entries[1 + module->nested_count]` plus the contiguous IC
  tables. `entries[0]` is the root chunk; `entries[1..n-1]` mirror
  `module->nested[]`.

### `urbi_get_or_create_module_instance`

```c
UModuleInstance *
urbi_get_or_create_module_instance(struct UVM *vm, UModule *m);
```

Walks `vm->module_instances_head` looking for a matching `module ==
m`; on hit, returns the cached entry. On miss, calls
`urbi_module_instance_create`, which prepends the new instance at the head
of the list. The cost is O(N) in distinct loaded modules per VM (typically
< 10).

The walk-then-prepend is **unsynchronised**. Single-threaded-VM contract:
the caller must not invoke this from multiple host threads concurrently
against the same `vm`. The current `URBI_SCHED_COOPERATIVE` baseline makes
this safe; parallel-realms support is a post-`v1.0` expansion.

### Auto-binding from chunk-run paths

- `urbi_run_chunk` (`src/module/uchunk.c`) resolves the realm (NULL →
  global) and calls `urbi_vm_run`. No separate pre-create — the strand's
  cache resolution happens inside `urbi_vm_run` itself.
- `urbi_vm_run` (`src/vm/uvm_run.c`) unconditionally calls
  `urbi_module_instance_create(vm, module)` for its transient strand.
  Forcing a fresh create defends against the REPL pattern of
  stack-allocating `UModule` and reusing the same address — a cache hit on
  a stale stack-allocated module would hand out an instance with freed
  `ic_names`.

---

## `ic_name_strs` lazy interning

`UProto.ic_names` and the root-chunk `UModule.ic_names` are arrays of
interned `USymbol *` parallel to each chunk's IC sites. The deserializer
cannot intern (interning needs a VM in scope, and the loader does not have
one), so it instead populates the companion `char **ic_name_strs` with
freshly-allocated UTF-8 copies and leaves `ic_names == NULL`.

`intern_ic_names_from_strs` (in `src/object/umodule_instance.c`) closes the
gap on first `urbi_module_instance_create`. For each chunk:

1. If `ic_count == 0`, return success.
2. If `ic_names != NULL`, return success — already interned.
3. If `ic_name_strs == NULL`, return failure (deserializer never ran).
4. Allocate a `USymbol *` array of length `ic_count`.
5. Walk `ic_name_strs[k]`, call `ustr_intern`, populate the new array.
6. Publish into `ic_names`.

The helper is **idempotent** — second and subsequent
`urbi_module_instance_create` calls on the same `UModule` are no-ops.

---

## Module-load contract

`umodule_deserialize(module, buf, size, errmsg, errcap)` populates a
zero-initialized `UModule` from a `.urb` byte buffer. Refer to
[bytecode-format.md](bytecode-format.md) for the wire format itself.

### Strict zero on header bytes 16–23

`v1.0` defines no flag bits in the 8-byte reserved region at header offsets
16–23. The loader rejects any non-zero byte in that range with
`ULOAD_CORRUPT` and a diagnostic `"non-zero reserved byte 0x%02x at offset
%zu"`. Earlier forward-compat tolerance silently dropped flags older
builds did not recognize, which is the wrong policy when bytecode stability
is not yet promised.

### Partial-buffer-on-error policy

On any non-OK return other than `ULOAD_INVALID_ARG` (NULL module or buffer),
`module` may hold partial buffers from whichever decode section completed
before the failure. `umodule_destroy(module)` is safe in **either** case
— success or failed-partial — and is the correct cleanup path either way.
Internally, every per-section decoder zero-initialises its target slots
before populating, so the destroy walk sees well-defined NULLs even after
mid-section failure.

`ic_names` interning is deferred to the first
`urbi_module_instance_create` (above); deserialize itself does not require
a VM. See also [object-model.md](object-model.md) for the per-VM IC RAM
tier and how interned `ic_names` feed `UIC.name`, and
[closures.md](closures.md) for how `UClosure` carries its enclosing
`proto_inst` for IC dispatch.

---

## Single-threaded VM assumptions

Two structures in this area assume a single-threaded VM and are pinned to
the `v1.0` `URBI_SCHED_COOPERATIVE` baseline:

- **`urbi_get_or_create_module_instance`** — walk-then-prepend on
  `vm->module_instances_head` is unsynchronised.
- **The deferred slot-change ring** (`vm->deferred_slot_changes`, capacity
  `URBI_DEFERRED_SLOT_CHANGE_RING_SIZE`) — entries are weakly referenced
  and survive only on the safepoint-ordered invariant `defer-site → next
  safepoint → drain → watcher_eval_dirty`. The cooperative scheduler steps
  GC only at the dispatch-loop safepoint, and the drain runs before any
  potential GC trigger at that same safepoint. A preemptive scheduler
  would have to upgrade the ring entries to strong refs via a GC root
  provider that walks `vm->deferred_slot_changes[head..tail]`.

Multi-threaded VM contracts (parallel realms, `URBI_SCHED_PREEMPTIVE`,
cross-thread instance-cache lookup) are a post-`v1.0` expansion.
