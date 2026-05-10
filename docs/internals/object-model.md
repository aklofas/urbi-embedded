# Object model

The object model is the runtime's representation of every user-visible value
that has identity: prototypes, instances, atom singletons, and the slots they
carry. This document covers the layout in memory, the hidden-class machinery,
the prototype chain encoding, and the inline-cache tier that fast-paths slot
access. Read [architecture.md](architecture.md) first; this is the depth doc
for the object subsystem.

Source: `src/object/uobject.{c,h}`, `src/object/uobject_slot.c`,
`src/object/uobject_proto.c`, `src/object/uobject_lookup.c`,
`src/object/ushape.{c,h}`, `src/object/uic.{c,h}`,
`src/object/utypes_init.c`. Public surface in `include/urbi/object.h`.

---

## UObject

Every object — root prototype, atom singleton, user clone, internal helper —
is a `UObject` cell. The header lives in `src/object/uobject.h`:

```c
struct UObject {
    UCell                 cell;                 /* 2 B  GC color + type tag */
    /* 6 B compiler-inserted padding before shape* */
    UShape               *shape;                /* 8 B  hidden class */
    USlot                *slots;                /* 8 B  local storage,
                                                        length == shape->count */
    uintptr_t             protos;               /* 8 B  tagged single-or-heap
                                                        proto encoding */
    uint32_t              object_id;            /* 4 B  stable identity */
    uint32_t              lookup_stamp;         /* 4 B  visited marker for the
                                                        prototype walk */
    uint32_t              flags;                /* 4 B  atom family + frozen
                                                        + readonly + spare */
    uint32_t              reserved;             /* 4 B  zero at v1.0 */
    struct UChangedNode  *changed_events_head;  /* 8 B  slot-change subscriber
                                                        chain */
};
```

Total on a 64-bit host: 56 bytes. The literal byte total is pinned by

```c
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(struct UObject) == 56, ...);
#endif
```

The pin is gated on pointer width because 32-bit cross targets (Cortex-M7,
rv32imc) shrink the pointer fields and reshuffle the natural alignment, so the
literal 56 B no longer holds. On 32-bit targets the host-only runtime offset
checks in `tests/unit/test_uobject.c` supply the second signal — they pin
field offsets rather than the total. Architecturally the same nine fields
appear in the same order on every supported target.

Field-order is load-bearing. `cell` must be the first member so a `UCell *`
points at the same byte as the enclosing `UObject *` (every walker recovers
the object via `(UObject *)((UCell *)payload - 1)`). The `slots` pointer is
not the slot-storage cell itself; it points at the `entries[]` flexible array
of a `USlotArray` wrapper that the GC walker recovers via `offsetof`.

`flags` packs the atom family in the low 4 bits, plus
`URBI_OBJ_FLAG_FROZEN` (bit 4), `URBI_OBJ_FLAG_SANDBOX_RO` (bit 5), and
`URBI_OBJ_FLAG_IS_PROTOTYPE` (bit 6). The IS_PROTOTYPE bit is monotonic —
set when an object is first referenced as another's prototype, never
cleared. It drives the conditional `topology_gen` bump in
`urbi_object_set_local_slot` (described under [Inline cache](#inline-cache)
below).

---

## UShape: hidden classes and transitions

Each `UObject` points at a `UShape` describing its slot layout. Two objects
that have evolved through the same sequence of slot-adds share the same
`UShape` (transition interning), which is what makes shape-keyed inline-cache
lookup viable. The struct lives in `src/object/ushape.h`:

```c
struct UShape {
    UCell        cell;
    /* 6 B compiler-inserted padding */
    USymbol     *name;        /* 8 B  last-added slot name (NULL for root) */
    uint32_t     index;       /* 4 B  slot offset in UObject.slots[] */
    uint32_t     count;       /* 4 B  slot count at this shape */
    uint32_t     flags;       /* 4 B  per-slot flag nibbles (4 bits/slot) */
    uint32_t     _pad;
    UShape      *parent;      /* 8 B  shape without this slot */
    UShapeMap   *transitions; /* 8 B  name -> child shape cache */
    UProps     **props_table; /* 8 B  dense per-slot UProps* array */
};
```

56 bytes on 64-bit hosts, pinned by `_Static_assert` (also gated on
pointer width). A `UShape` lineage is a singly-linked chain via `parent`:
the root shape has `name == NULL`; each child adds exactly one slot at the
recorded `index`.

### UShapeMap (transition cache)

The forward direction — "given parent shape, what's the child that adds
name N?" — is a per-shape `UShapeMap`: an open-addressing hash table keyed
on interned `USymbol *` pointer identity. Allocated lazily on the first
transition out of a parent shape, capacity is a power of two, resizes 2x
at >= 75% load. Probe scheme is linear probing on `(key >> 4)` low bits;
the shift strips alignment-zero bits.

The cache makes shape sharing deterministic: any two slot-add sequences
that visit the same names in the same order land on the same `UShape *`.
This is what makes the IC's `recv_shape == cached_shape` check meaningful.

### UProps and the per-shape props_table

Slot-level metadata — getter, setter, constant — does not live in the
`USlot` itself. `USlot` is exactly `UValue` (16 B). Property metadata lives
in a parallel `UProps` cell, allocated lazily and addressed through the
shape's `props_table`:

```c
struct UProps {
    UCell        cell;        /* 2 B */
    /* 6 B padding */
    UValue       oget;        /* getter (UVAL_VOID when unset) */
    UValue       oset;        /* setter (UVAL_VOID when unset) */
    uint32_t     constant : 1;
    uint32_t     _spare   : 31;
};
```

`UProps` is 48 B on 64-bit hosts (also pinned). `UShape.props_table` is
`NULL` until any slot in the lineage has at least one property installed.
When non-NULL it points at the `entries[]` flexible array of a `UPropsTable`
wrapper cell; the wrapper is recovered via `offsetof` for GC walking, the
same `container_of`-style pattern used for `USlotArray`.

Per-slot flag bits are also packed into `UShape.flags` as 4-bit nibbles
(`URBI_SLOT_FLAG_OGET`, `_OSET`, `_CONSTANT`, `_LOCAL`). The packed form
covers slot indices 0..7 only; v1.0 takes the simplification that the
4-bit nibble is unavailable for indices >= 8. Indices >= 8 carry no
packed flags at the cost of an inevitable IC slow-path miss for getter
or setter dispatch — acceptable for the shape-stable common case.

---

## Prototype chain

`UObject.protos` is a tagged `uintptr_t` with three storage forms, encoded
in the low bit:

| `obj->protos` value          | Form     | Meaning                  |
| ---------------------------- | -------- | ------------------------ |
| `0`                          | empty    | no parent prototypes     |
| `(p << 1) \| 1U`             | single   | one proto, address `p`   |
| raw `UProtos *` (bit 0 = 0)  | heap     | n >= 2 protos in a block |

Heap form points at a `UProtos` block with a flexible `items[]` array. The
encoding is a load-bearing design pin: the bit-0 distinction is what lets a
zero-or-one-proto object skip the heap allocation entirely (the common case
for atom singletons, where every non-root atom has the root Object as its
single proto).

Iteration uses `UPROTOS_FOREACH(obj, p)` (defined in `uobject.h`), which
captures `obj->protos` once at iteration start and dispatches across all
three forms. Capturing once means iteration sees a stable snapshot even if
the chain is mutated underneath — legacy semantics carried forward.

Three primitives mutate the chain:
`urbi_object_set_protos_empty`, `_single`, and `_heap`. Each fires the GC's
forward Dijkstra barrier on the existing value before overwriting, shades
the new child(ren), and bumps `vm->topology_gen`. The higher-level
`urbi_object_add_proto` / `_remove_proto` / `_set_protos` wrappers in
`include/urbi/object.h` add cycle detection, dedup, and `valid_proto`
checks before routing through one of the three primitives.

---

## Atom-family singletons

Nine atom families are defined in `include/urbi/object.h`:

| Family           | Value | C type-side analogue                  |
| ---------------- | ----- | ------------------------------------- |
| `URBI_ATOM_OBJECT`  | 0  | root Object — atom of all atoms       |
| `URBI_ATOM_INTEGER` | 1  | Integer                               |
| `URBI_ATOM_FLOAT`   | 2  | Float                                 |
| `URBI_ATOM_STRING`  | 3  | String                                |
| `URBI_ATOM_LIST`    | 4  | List                                  |
| `URBI_ATOM_DICT`    | 5  | Dict                                  |
| `URBI_ATOM_TAG`     | 6  | Tag                                   |
| `URBI_ATOM_EVENT`   | 7  | Event                                 |
| `URBI_ATOM_SYMBOL`  | 8  | Symbol                                |

Values 9..15 are reserved for v1.x. Each VM holds a singleton pointer per
family (`vm->atom_object`, `vm->atom_integer`, etc.); they're lazy-allocated
on first access via `urbi_object_atom(vm, family)`. The root Object's
`protos` is empty; every other atom's `protos` is the single-tag form
pointing at the root.

The same nine families are reflected as `UTYPE_*` GC type tags for cell
walkers, alongside the wider object-model cell-type vocabulary. The full
inventory is laid out in [gc.md](gc.md); the object-model subset is:

| Tag                     | Value | Cell                       |
| ----------------------- | ----- | -------------------------- |
| `UTYPE_OBJECT`          | 1     | `UObject`                  |
| `UTYPE_PROTOS`          | 9     | `UProtos` (heap form)      |
| `UTYPE_SHAPE`           | 10    | `UShape`                   |
| `UTYPE_PROPS`           | 11    | `UProps`                   |
| `UTYPE_SLOTHANDLE`      | 12    | `USlotHandle` (handle API) |
| `UTYPE_MODULE_INSTANCE` | 13    | `UModuleInstance`          |
| `UTYPE_PROTO_INSTANCE`  | 14    | `UProtoInstance`           |
| `UTYPE_SHAPE_MAP`       | 15    | `UShapeMap`                |
| `UTYPE_PROPS_TABLE`     | 16    | `UPropsTable` (wrapper)    |
| `UTYPE_SLOT_ARRAY`      | 17    | `USlotArray` (wrapper)     |

Nine cell types are introduced for the object-model proper (`UObject`
plus the eight new tags 9..17). Walker functions for each are registered
into `vm->type_table[]` by `urbi_object_builtin_types_init`.

---

## Slot operations: OP_GETSLOT / OP_SETSLOT

Bytecode encodes slot access with two ABC-form opcodes:

- `OP_GETSLOT A B C`: `R[A] := R[B].slot[ic_index=C]`
- `OP_SETSLOT A B C`: `R[B].slot[ic_index=C] := R[A]`

C is not a register — it's an inline-cache site index into the function's
per-call-site IC table. The emitter assigns one IC slot per `GETSLOT` /
`SETSLOT` site at compile time; the runtime allocates the IC table when the
module is bound to a VM. The IC table itself lives in the per-VM
`UProtoInstance` (see [realm-and-modules.md](realm-and-modules.md) for the
module-instance binding protocol and the per-VM IC RAM tier).

Dispatch flow in `src/vm/uvm.c`:

1. Resolve the per-VM `UProtoInstance` via `ic_resolve_pi(s)`. Each call
   frame carries the proto-instance for its owning closure
   (see [closures.md](closures.md) — `UClosure.proto_inst`).
2. Look up the `UIC` at `pi->ic_table[ic_index]`.
3. Type-check the receiver: must be `UVAL_OBJECT`.
4. Linear-scan `ic->n` entries for a `(recv->shape, vm->topology_gen)`
   match. On hit, take the fast path.
5. On miss, call `urbi_slot_get_slow` / `urbi_slot_set_slow`.

For `OP_GETSLOT` the fast path copies `*ic->slots[k]` into `R[A]`, modulo
a getter check (OGET flag in `ic->flags[k]` triggers a getter dispatch
that's currently a clean diagnostic — getter dispatch wires through with
the stdlib bring-up).

For `OP_SETSLOT` the fast path is more layered. On shape+topology hit:

- OSET flag set → setter dispatch (also diagnose-only at v1.0).
- CONSTANT flag set → reject the write with a TypeError.
- LOCAL flag set → in-place write through the GC's
  `urbi_gc_slot_write` barrier.
- LOCAL flag clear → fall to the slow path for **copy-on-write**.

Copy-on-write is the legacy urbiscript semantics for an inherited slot:
writing through a receiver that resolves the slot on a prototype installs
a fresh local slot on the receiver, leaving the prototype's slot intact.
`urbi_slot_set_slow` handles this by calling `urbi_object_set_local_slot`,
which leaf-shape-adds the receiver and grows its `USlotArray`.

---

## Inline cache

Each `OP_GETSLOT` / `OP_SETSLOT` site has one `UIC` record:

```c
typedef struct UIC {
    USymbol  *name;
    UShape   *recv_shapes  [URBI_IC_ENTRIES_PER_SITE];
    uint64_t  topology_gen [URBI_IC_ENTRIES_PER_SITE];
    USlot    *slots        [URBI_IC_ENTRIES_PER_SITE];
    UProps   *uprops       [URBI_IC_ENTRIES_PER_SITE];
    uint8_t   flags        [URBI_IC_ENTRIES_PER_SITE];
    uint8_t   n;
    uint8_t   replace_cursor;
} UIC;
```

`URBI_IC_ENTRIES_PER_SITE` is a compile-time tunable bound to {1, 2, 4}.
Default is 4 (host build; pinned `sizeof(UIC) == 144` on 64-bit hosts);
the embedded-footprint preset binds 2 (built and tested in CI); 1 is
the monomorphic-only configuration (also in CI). The `_Static_assert`
in `uic.h` rejects any other value.

### Linear scan with round-robin eviction

Lookup is a linear scan over the first `ic->n` entries. On hit, the IC
fast path runs. On miss with cache not yet full (`ic->n <
URBI_IC_ENTRIES_PER_SITE`), the slow path resolves and grows the cache
into the next free slot. On miss with cache full, the slow path overwrites
the slot at `ic->replace_cursor`, then advances the cursor mod cap. This
is round-robin, not strict LRU — there is no per-entry LRU bookkeeping
field. Round-robin gives a fair eviction order at lower runtime cost; for
the small cap (4 default, 2 footprint, 1 monomorphic) the difference from
true LRU is negligible.

`replace_cursor` advancement happens in `ic_fill_at_cursor` (`uic.c`) —
that's the only writer.

### topology_gen invalidation

`vm->topology_gen` is a `uint64_t` bumped on shape-tree mutations that
could invalidate any IC entry's cached resolution. It pairs with
`recv_shape` as the IC's two-part cache key. Initial value is 1; 0 is
reserved as the "unfilled" sentinel for fresh IC entries.

**Sites that bump** (per the topology-generation contract):

- `urbi_object_remove_slot` — unconditional bump. Slot removal can move
  surviving slots to lower indices, so any IC entry past the removed
  slot is potentially stale.
- `urbi_object_install_property` / `_remove_property` /
  `_set_property_value` — bumped because cached `uprops[]` pointers
  reference the old `UProps` cell, which is now stale after a fresh
  `UProps` was published.
- `urbi_object_set_protos_empty` / `_single` / `_heap` — bumped because
  prototype-chain shape determines lookup resolution.
- `urbi_object_set_local_slot` — bumped **only** if the receiver has
  `URBI_OBJ_FLAG_IS_PROTOTYPE` set. See below.

**The IS_PROTOTYPE conditional bump in `urbi_object_set_local_slot`:**

For the leaf-shape-add case on a non-prototype receiver, the bump is
deliberately skipped. Reasoning: the IC's per-site
`recv_shape == cached_shape` check is sufficient to invalidate stale
entries, because the shape transitioned. Without the bump, however, IC
entries that walked **through** the receiver looking for a name and
cached a miss-then-fall-through resolution past it become stale only
when the receiver is itself a prototype of some other object. For a
non-prototype receiver, no IC entry ever cached a path through it, so
no bump is needed. For a prototype receiver, the bump is necessary —
hence the IS_PROTOTYPE conditional, which the parent-object's
`set_protos_*` site sets monotonically when the relationship is first
established.

**Subtle point on the bump-skip and `uprops[e]` caching:** the same
bump-skip is also load-bearing in the other direction. IC entries cache
`uprops[e]` as a pointer into the holding shape's `props_table`; for a
leaf-shape-add on a non-prototype receiver, no `UProps` mutation has
happened, so the cached `uprops[e]` pointer is still correct. Bumping
unconditionally would invalidate IC entries that are still semantically
valid, with no correctness gain. A future-release expansion (a separate
props-content generation counter) would let `topology_gen` tighten its
contract; until that lands, the conditional skip is correctness-necessary
for the IC `uprops[e]` pointer to remain a valid cache.

### Slow-path fill

Both `urbi_slot_get_slow` and `urbi_slot_set_slow` route through
`urbi_object_resolve_slot` for the prototype-chain DFS. On hit, they call
`ic_fill_at_cursor` to write exactly one IC entry at `ic->replace_cursor`
with `(recv_shape, vm->topology_gen, slot_ptr, uprops, flags)`, then
advance the cursor.

Resolve uses a fixed 64-deep DFS stack (`URBI_RESOLVE_STACK_CAP`) and a
per-VM `lookup_id` for cycle detection — every node visited stamps its
`lookup_stamp` to the current id, so re-visits short-circuit. The id wraps
explicitly: when the next bump would produce 0, a force-wrap pass clears
every object's `lookup_stamp` and resets `lookup_id` to 1.

---

## get/set parse sugar (T41 — M6 Wave 2)

Parse-only desugar; no new opcodes, no runtime change beyond wiring the
deferred `oget`/`oset` dispatch arms.

```
get x() { body }
```

becomes (at emit time)

```
recv.setProperty("x", "oget", function() { body })
```

Symmetric for `set y(v) { body }` → `setProperty("y", "oset", ...)`. The
runtime `oget` / `oset` slot-property dispatch (M4 baseline machinery,
`URBI_SLOT_FLAG_OGET` / `OSET` on `UProps`) was wired live in the same
ship — `OP_GETSLOT` / `OP_SETSLOT` route through
`urbi_run_closure_on_scratch[_with_payload]` when an IC entry's flags
indicate a property closure.

Parse context: `get` and `set` are recognized as method-decl prefixes
**only** when followed by `IDENT (`. Outside that strict shape they
remain plain identifiers — no keyword reservation breakage. Detection
uses `peek2()` (parser 2-token lookahead) at two sites:

1. `parse_member_access` — `Foo.get value(...)` form (member-access form).
2. `parse_assign_or_expr` — `get value(...)` at statement start (used in
   class body and at top level, both with implicit receiver).

The implicit-receiver class-body form has its own emit arm
(`emit_class_body_property_decl` in `src/emit/uemit_class.c`) that wires
`foo_reg` (the class object's register) as the explicit `setProperty`
receiver during desugar — the synthetic AST scaffold used by
`emit_property_decl_arm` can't address a raw register, so this is a
parallel call-sequence builder.

The backing C-native `Object.setProperty(name, prop, value)` lives in
`src/stdlib/object_root.c`. It materializes a nil placeholder slot when
the slot is absent (legacy semantics: `get`/`set` implicitly creates the
slot) and then calls `urbi_object_install_property` with the appropriate
flag bit.

---

## Cross-references

- [GC](gc.md) — cell types, gc_byte bit layout, the slot-write barrier
  (`urbi_gc_slot_write`), and the realm-hierarchy walker that keeps
  `UObject` / `UShape` / `UProps` / wrapper cells reachable.
- [Realm and modules](realm-and-modules.md) — how the per-VM
  `UModuleInstance` carries the IC RAM tier and lazy-interns IC name
  strings on first use.
- [Closures](closures.md) — `UClosure.proto_inst` is the binding the
  `OP_GETSLOT` / `OP_SETSLOT` dispatch arms read to resolve the IC table.
- [Bytecode format](bytecode-format.md) and [opcodes](opcodes.md) — the
  ABC encoding for `OP_GETSLOT` / `OP_SETSLOT` and the wire-format
  treatment of the IC name table.
