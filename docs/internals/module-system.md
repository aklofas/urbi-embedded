# Module System Internals (post-v0.8.1)

This document describes the post-v0.8.1 shape of the module system: the
UModule thin loader shell, the `UProto` recursive-children layout, module-grain
lifetime via refcount fusion, the v1.7 wire format, strand binding, and the
vm_destroy lifetime ordering invariant.

For full design rationale, see spec §§2–4 of
`docs/superpowers/specs/2026-05-17-v0.8.1-uproto-root-design.md`.
The predecessor model (v0.8.0 persistent loader strand + dual refcount) is
documented in [loader-strand.md](loader-strand.md).

---

## 1. UModule — Thin Loader Shell

After v0.8.1 UModule holds exactly five fields:

```c
typedef struct UModule {
    UProto         *root_proto;   /* THE chunk — owns instructions/constants/IC/nested */
    char           *source_name;  /* human-readable name for diagnostics */
    struct UVM     *origin_vm;    /* debug-only — VM that created this module */
    UModuleAllocFn  alloc_fn;
    void           *alloc_ud;
} UModule;
```

**Ownership:**
- `root_proto` owns all chunk-top data including nested function-literal protos.
  `umodule_destroy` frees `root_proto` (or rescues it to `vm->rescued_protos`
  when `root_proto->refcount > 0`).
- `source_name` is a heap-allocated UTF-8 string freed by `umodule_destroy`.
- `origin_vm` is a weak back-reference (not freed).
- `alloc_fn` / `alloc_ud` are the allocator function + opaque data used for all
  allocations within the module.  Set at deserialize time; consistent across the
  module's lifetime.

**Size:** ~40 B host / ~20 B arm (was ~200 B / ~100 B before v0.8.1).  The
reduction comes from moving all chunk-top data fields to `UProto`.

UModule is opaque to embedders — the layout is not part of the public ABI
surface; only `urbi_*` API functions touch it.

---

## 2. UProto — Recursive Children + Back-pointer

Prior to v0.8.1 `UProto` held per-function-literal bytecode (instructions,
constants, IC metadata, line info) but not the root chunk's data (which lived
on UModule).  After v0.8.1, `UProto` is the canonical per-chunk data container
for both root and nested protos.

```c
typedef struct UProto {
    /* === Serialized fields (wire format v1.7) ==================== */
    uint32_t    *instructions;   size_t instr_count, instr_cap;
    UValue      *constants;      size_t const_count, const_cap;
    int8_t      *line_deltas;
    UAbsLine    *abs_lines;      size_t abs_line_count, abs_line_cap;
    uint8_t      max_reg, nupvals, nparams;
    uint16_t     ic_count;
    USymbol    **ic_names;
    char       **ic_name_strs;

    /* Recursive children — root populates; non-root has nested_count = 0 */
    struct UProto **nested;
    size_t          nested_count, nested_cap;

    /* === Runtime-only fields (NOT serialized) ==================== */
    UModuleAllocFn  alloc_fn;
    void           *alloc_ud;

    /* Back-pointer to the root UProto of the owning module.
     * NULL on the root proto itself.
     * Set to module->root_proto on every nested proto at alloc time. */
    struct UProto  *root;

    /* Module-grain refcount (meaningful on root only; 0 on nested). */
    uint16_t        refcount;
} UProto;
```

### What changed in v0.8.1

- `nested[]` / `nested_count` / `nested_cap` moved from UModule to UProto.
  The root proto owns the flat array of function-literal protos.
- `root` back-pointer added (runtime-only).  All nested protos have
  `root = module->root_proto`; the root proto has `root = NULL`.
- `refcount` moved from UModule to UProto (root only).

### Flat-on-root emitter invariant

The current emitter places all function literals as siblings under
`root_proto.nested[]` regardless of lexical nesting depth.  Non-root protos
have `nested_count = 0`.  `OP_CLOSURE Bx` indexes this flat array.

**Deferred:** a truly-recursive emitter where Bx scopes per-enclosing-proto
(and non-root protos carry their own `nested[]` children) is the long-term
Approach C endgame.  Not scheduled; tracked in design-risks.

---

## 3. Module-grain Lifetime (Variant B Refcount Fusion)

### Semantics

One refcount on `root_proto->refcount` represents the total reference count
for the entire module (shell + root proto + all nested protos).  The module
lives or dies as a unit.

**Counted references:**
- Each strand that references the module (set at strand-bind, cleared at
  strand-destroy).
- Each UClosure whose `proto` field points to any proto in the module
  (bumped at `vm_alloc_closure`, decremented when the closure is freed
  by `pool_free` or swept by `urbi_vm_destroy`).

**Not separately counted:**
- Nested proto slots within `root_proto.nested[]` — their reachability is
  structural, not refcounted.  The slot is valid for the module's lifetime.

### Refcount bump sites

| Site | Action |
|---|---|
| `urbi_strand_create_for_module` | `module->root_proto->refcount++` |
| `op_fork` child copy | `child->root_proto->refcount++` |
| `vm_alloc_closure(proto)` | `uproto_root_of(proto)->refcount++` |

`uproto_root_of(proto)` is an inline that returns `proto->root ?: proto` —
always landing on the root proto.

### Refcount decrement sites

| Site | Action |
|---|---|
| `strand_destroy` | `umodule_strand_refcount_dec(module, root_proto, vm)` |
| `pool_free` OWNS_COND / OWNS_BODY / OWNS_ONLEAVE arms | `uproto_root_of(cl->proto)->refcount--` |
| `urbi_vm_destroy` stdlib_closures sweep | dec via `uproto_root_of(cl->proto)` before freeing each closure |

`strand_closure_unlink` (which NULLed the nested slot in v0.7.3) is now a
near-no-op for refcount purposes — the slot is kept valid for re-use by
subsequent watcher firings (`whenever`, re-fired `at`).

`umodule_alloc_nested_proto` initializes `refcount = 0` (was 1 in v0.7.3).
The slot's reachability is implicit; no slot-implicit ref is needed.

### `umodule_destroy` semantics

```c
void umodule_destroy(UModule *m, UVM *vm) {
    if (!m) return;
    UProto *root = m->root_proto;
    if (root && root->refcount > 0) {
        /* Module is still referenced.
         * Detach root_proto (with nested[] intact) and transfer to
         * vm->rescued_protos so it survives the module-shell free. */
        urbi_uproto_rescue(vm, root);
        m->root_proto = NULL;
    }
    umodule_free_shell(m);   /* frees source_name + the UModule struct itself */
}
```

When `refcount > 0`, the shell is freed immediately but `root_proto` is
transferred to `vm->rescued_protos`.  `urbi_vm_destroy` later frees every
rescued root_proto (and its nested children) in order.

When `refcount == 0`, `root_proto` is freed inline before the shell.

### Behavioral implication for embedders

Any closure that escapes into realm globals pins the entire owning module
structure until VM-destroy.  Long-running REPL sessions that load and escape
many small chunks will accumulate `root_proto` structures.

The forthcoming M8 `urbi_unload(realm, module)` API will be the escape valve
for explicit module eviction.

---

## 4. Wire Format v1.7

### Header

```
[ header (version=0x17, descriptor, magic, canary, zero bytes 16..23) ]
[ source_name (length-prefixed) ]
[ root_proto block: recursive UProto serialization ]
```

`URBI_BYTECODE_VERSION_BYTE = 0x17`.  The loader rejects any buffer with a
different version byte as `ULOAD_UNSUPPORTED_VERSION`.

### Recursive UProto block

Each UProto block serializes as:

```
[ instructions (count + raw uint32_t[]) ]
[ constants (count + typed UValue[]) ]
[ line_deltas (int8_t[]) ]
[ abs_lines (count + UAbsLine[]) ]
[ max_reg, nupvals, nparams (uint8_t each) ]
[ ic_count (uint16_t) ]
[ ic_name_strs (count + length-prefixed strings) ]
[ nested_count (uint32_t) ]
[ nested[0] ... nested[nested_count-1] (each is a recursive UProto block) ]
```

The root block's `nested_count` equals the total number of function literals
in the module.  Non-root proto blocks have `nested_count = 0` under the
current flat-on-root emitter.

### Flat-on-root invariant

`OP_CLOSURE Bx` indexes `root_proto.nested[Bx]`.  The invariant holds: every
function literal is a direct child of the root proto in the wire format, even
if it is lexically deeply nested in source.  The truly-recursive wire format
(where Bx scopes per-enclosing-proto) is deferred; the deserialized tree
shape would differ but the current emitter never produces non-root nested[].

### Reject policy

Loading v1.6 (or any prior) bytecode returns `ULOAD_UNSUPPORTED_VERSION`.
There is no in-band migration and no upgrade tool.  Embedders re-bake from
source (same policy as every prior wire-format bump).

---

## 5. Strand Binding

Each strand holds two module-related fields:

```c
struct UStrand {
    /* ... */
    UModule *module;        /* loader shell — source_name, alloc hooks, origin_vm */
    UProto  *root_proto;    /* chunk data — instructions, constants, IC, nested[] */
    /* ... */
};
```

**Hot path** (opcode dispatch, IC lookup, constant load) uses `s->root_proto`
directly — no extra pointer dereference through `s->module`.

**Cold path** (error diagnostics, source-name reporting) uses `s->module`
for `source_name` and `origin_vm`.

Both fields are set together at bind time:

- `urbi_strand_create_for_module(vm, realm, module)` sets both and calls
  `umodule_strand_refcount_dec` / `umodule_strand_refcount_inc` via the
  bridge helper.
- `op_fork` child copy propagates both fields from parent.

**Approach C endgame:** when UModule is eliminated (every chunk becomes a
bare UProto with header fields), `s->module` will be removed and `s->root_proto`
will carry the diagnostic fields too.  Not scheduled; tracked in design-risks.

---

## 6. Lifetime Ordering Invariant at `urbi_vm_destroy`

This invariant is load-bearing for correctness.

**The invariant:** a closure's `cl->proto` dereference via `uproto_root_of`
must reach a live `root_proto` at the moment the refcount is decremented.

`urbi_vm_destroy` sweeps two vm-owned lists:

1. `vm->stdlib_closures` — surviving UClosure objects (closures that escaped
   into realm globals or watcher install sites and outlived their strand).
2. `vm->rescued_protos` — whole root_protos rescued from destroyed modules
   whose `refcount` was non-zero at destroy time.

**Required order: (1) before (2).**

Rationale:
- Each surviving closure in `stdlib_closures` holds `cl->proto` pointing into
  some `root_proto`.  During the `stdlib_closures` sweep, the free path calls
  `uproto_root_of(cl->proto)->refcount--`.
- If `rescued_protos` were swept first, those root_protos would be freed.
  The subsequent `uproto_root_of(cl->proto)` dereference in the closure sweep
  would UAF.
- With the correct order, root_protos are alive throughout step (1) because
  `rescued_protos` has not been swept yet.  After step (1) completes, all
  rescued root_protos have correctly decremented refcounts.  Step (2) then
  frees them.

Active (non-rescued) root_protos are owned by live UModules.  Those modules
are destroyed via `umodule_destroy(m, vm)` before `urbi_vm_destroy` runs the
sweep, so they do not participate in the ordering — their root_protos are
either freed by `umodule_destroy` (refcount = 0) or rescued to
`vm->rescued_protos` (refcount > 0, covered by the invariant above).

The implementation places the `stdlib_closures` sweep loop immediately before
the `rescued_protos` sweep in `urbi_vm_destroy`, with an inline comment
explaining the ordering requirement.  Both sweeps are O(N).

---

## 7. References

- **Spec:** `docs/superpowers/specs/2026-05-17-v0.8.1-uproto-root-design.md`
  §§2–4 — architecture, refcount fusion, wire format, risk register.
- **Predecessor model:** [loader-strand.md](loader-strand.md) — v0.8.0
  persistent loader strand + UModule.refcount (now fused with root_proto).
- **Realm context:** [realm-and-modules.md](realm-and-modules.md) — per-realm
  globals, module instance cache, ic_name_strs lazy interning.
- **Closure ownership:** [closures.md](closures.md) — OWNS_* watcher flags,
  `strand_closure_unlink`, `vm->stdlib_closures`.
- **Wire format detail:** [bytecode-format.md](bytecode-format.md) — version
  byte policy, header layout, opcode encoding.
- **CHANGELOG:** `v0.8.1-uproto-root` entry — Changed/Removed/Added/Fixed
  with field-level specifics.
