/* SPDX-License-Identifier: BSD-3-Clause */
/* uobject.c — atom-family singletons + UObject allocator (M4 / T8).
 *
 * Design references: pre-M4 prototype-chain representation §3/§4.1/§8.1.
 *
 * Per-VM lazy-allocated atom prototypes: root Object plus the eight built-in
 * atoms (Integer/Float/String/List/Dict/Tag/Event/Symbol).  T36's root
 * provider (object_roots_walker, registered via urbi_object_register_gc_roots
 * in urbi_vm_init) keeps the singletons alive across GC cycles by shading each
 * non-NULL vm->atom_* field directly during MARK_ROOTS.
 *
 * The single-tag prototype encoding `(root << 1) | 1` used in
 * urbi_object_atom matches the canonical form decoded by UPROTOS_FOREACH
 * (src/object/uobject.h, T9). */

#include <stdint.h>

#include "object/uobject.h"
#include "object/uobject_internal.h"
#include "object/ushape.h"
#include "object/uchunk_instance.h"  /* T36: walk module_instances_head */
#include "vm/uvm.h"
#include "urbi/gc.h"      /* urbi_gc_alloc + urbi_gc_register_root_provider */
#include "gc/ugc_incremental.h"   /* gc_shade_gray */
#include "urbi/object.h"
#include "urbi/urbi.h"    /* urbi_panic */
#include "gc/ugc.h"
#include <stddef.h>

/* === next_id ===
 *
 * Per-VM monotonic UObject identity counter (spec §8.1).
 *
 * vm->next_object_id is initialised to 0 by urbi_vm_init; pre-increment yields
 * 1 on the first call, 2 on the second, etc.  At UINT32_MAX the next bump
 * would overflow — fatal-abort per spec §8.1 rather than silently wrap.
 *
 * Note: deviates from spec §8.1's pseudocode (which shows post-increment +
 * init=0, yielding ids 0..N-1) in favour of the established uvm.h comment
 * + plan test expectation that the first object gets id == 1.  The spec
 * pseudocode and v1.0 ABI agree on the monotonic-counter semantics; only
 * the initial offset disagrees, and 1-based ids leave 0 as a "no id"
 * sentinel for future debug printing. */
uint32_t
next_id(UVM *vm)
{
    if (vm->next_object_id == UINT32_MAX) {
        urbi_panic("URBI_FATAL_OBJECT_ID_EXHAUSTED");
    }
    return ++vm->next_object_id;
}

/* === urbi_object_alloc ===
 *
 * Allocate a fresh UObject in the given atom family.  Wires shape to the
 * root hidden class (no slots), protos to the empty form (0), and stamps
 * a fresh object_id.  Caller is responsible for pinning if the object is
 * a long-lived singleton. */
UObject *
urbi_object_alloc(UVM *vm, URBIAtomFamily family)
{
    /* GC soundness (v0.13.2, refactor-3 TEST-GAP-01 discovery chain):
     * resolve the root shape BEFORE allocating the object cell.  The
     * pre-v0.13.2 order (object cell first, then lazy urbi_shape_root)
     * left the fresh UObject reachable only through the C local `o`
     * while urbi_shape_root performed its own GC allocation — a window
     * in which a collection (URBI_GC_STRESS collects on every alloc)
     * swept the object, and the subsequent field writes corrupted
     * whatever cell recycled the address.  urbi_shape_root stores
     * vm->root_shape before returning, so the shape itself is rooted
     * by object_roots_walker the moment it exists.  This ordering also
     * subsumes OBJ-004 (shape-root OOM after object alloc can no longer
     * leave a half-initialised object in the all-cells list). */
    UShape *root_shape = urbi_shape_root(vm);
    if (root_shape == NULL) {
        return NULL;
    }

    UCell *c = urbi_gc_alloc(vm, sizeof(UObject), UTYPE_OBJECT);
    if (c == NULL) {
        return NULL;
    }
    UObject *o = (UObject *)c;

    o->shape               = root_shape;
    o->slots               = NULL;
    o->protos              = 0U;   /* empty form per spec §4.1 */
    o->lookup_stamp        = 0U;
    o->reserved            = 0U;
    o->changed_events_head = NULL;
    o->object_id           = next_id(vm);
    o->flags               = (uint32_t)((uint32_t)family & URBI_OBJ_ATOM_MASK);
    return o;
}

/* === urbi_atom_family_name ===
 *
 * Stable static string per atom family; used by future error messages
 * (T11 valid_proto failure path and beyond). */
const char *
urbi_atom_family_name(URBIAtomFamily f)
{
    switch (f) {
        case URBI_ATOM_OBJECT:  return "Object";
        case URBI_ATOM_INTEGER: return "Integer";
        case URBI_ATOM_FLOAT:   return "Float";
        case URBI_ATOM_STRING:  return "String";
        case URBI_ATOM_LIST:    return "List";
        case URBI_ATOM_DICT:    return "Dict";
        case URBI_ATOM_TAG:     return "Tag";
        case URBI_ATOM_EVENT:   return "Event";
        case URBI_ATOM_SYMBOL:  return "Symbol";
        case URBI_ATOM_BOOLEAN: return "Boolean";
        case URBI_ATOM_NIL:     return "Nil";
        case URBI_ATOM_VOID:    return "Void";
        default:                return "?";
    }
}

/* === urbi_object_root ===
 *
 * Lazy-allocate the per-VM root Object on first call.  The root has no
 * prototypes (protos field stays at 0 / empty form per spec §4.1).
 * Returns NULL on OOM. */
UObject *
urbi_object_root(struct UVM *vm)
{
    if (vm->atom_object != NULL) {
        return vm->atom_object;
    }

    UObject *o = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
    if (o == NULL) {
        return NULL;
    }
    /* protos already 0 (empty form) from urbi_object_alloc. */
    vm->atom_object = o;

    /* T36: object_roots_walker (registered via urbi_object_register_gc_roots
     * in urbi_vm_init) keeps this singleton alive across collection cycles by
     * shading vm->atom_object directly during MARK_ROOTS.  No explicit pin
     * needed — replaces the synthetic UVAL_CLOSURE wrapper trick used pre-T36. */
    return o;
}

/* === urbi_object_atom ===
 *
 * Lazy-allocate the named atom singleton on first call.  Each non-root
 * atom's protos field carries the single-tag encoding `(uintptr_t)root | 1`
 * pointing at the root Object (the canonical single form decoded by
 * UPROTOS_FOREACH per pre-M4 prototype-chain spec §4.1).
 *
 * Returns NULL on OOM or invalid family tag. */

/* Table entry: per-family vm field offset.  URBIAtomFamily values 0-11
 * (M4 baseline 0-8 plus M6 Phase 4 additions 9-11) are numerically
 * identical to the table indices, so the table is indexed directly by
 * family.  offsetof is used to avoid assuming struct-member ordering
 * beyond what is documented in uvm.h §M4 atom-family singletons. */
static const size_t kAtomFieldOffset[] = {
    [URBI_ATOM_OBJECT]  = offsetof(UVM, atom_object),
    [URBI_ATOM_INTEGER] = offsetof(UVM, atom_integer),
    [URBI_ATOM_FLOAT]   = offsetof(UVM, atom_float),
    [URBI_ATOM_STRING]  = offsetof(UVM, atom_string),
    [URBI_ATOM_LIST]    = offsetof(UVM, atom_list),
    [URBI_ATOM_DICT]    = offsetof(UVM, atom_dict),
    [URBI_ATOM_TAG]     = offsetof(UVM, atom_tag),
    [URBI_ATOM_EVENT]   = offsetof(UVM, atom_event),
    [URBI_ATOM_SYMBOL]  = offsetof(UVM, atom_symbol),
    [URBI_ATOM_BOOLEAN] = offsetof(UVM, atom_boolean),
    [URBI_ATOM_NIL]     = offsetof(UVM, atom_nil),
    [URBI_ATOM_VOID]    = offsetof(UVM, atom_void),
};
#define KATOM_TABLE_COUNT ((int)(sizeof(kAtomFieldOffset) / sizeof(kAtomFieldOffset[0])))

UObject *
urbi_object_atom(struct UVM *vm, URBIAtomFamily family)
{
    if ((int)family < 0 || (int)family >= KATOM_TABLE_COUNT) {
        return NULL;
    }

    /* TIDY-006: avoid the (UObject **)(void *) double-cast by routing the
     * atom-pointer-by-offset access through a single (char *) intermediate.
     * Per C11 §6.5p7, char-pointer access does not violate strict aliasing,
     * and (UObject **)(char *) is a single explicit pointer-to-pointer cast
     * (alignment is guaranteed by the layout of UVM — kAtomFieldOffset
     * indexes into UObject* fields whose alignment matches UObject **). */
    UObject **slot = (UObject **)((char *)vm + kAtomFieldOffset[family]);

    if (*slot != NULL) {
        return *slot;
    }

    /* For URBI_ATOM_OBJECT, route through urbi_object_root so the
     * allocate-and-pin path is identical to a direct urbi_object_root call.
     * urbi_object_root sets vm->atom_object; we return via *slot on next call. */
    if (family == URBI_ATOM_OBJECT) {
        return urbi_object_root(vm);
    }

    /* Ensure the root Object exists first — every non-root atom's protos
     * field references it.  An OOM here propagates as NULL. */
    UObject *root = urbi_object_root(vm);
    if (root == NULL) {
        return NULL;
    }

    /* internal_family == (URBIAtomFamily)family; the two enums are numerically
     * identical per include/urbi/object.h §_F suffix design note. */
    UObject *o = urbi_object_alloc(vm, (URBIAtomFamily)family);
    if (o == NULL) {
        return NULL;
    }
    /* Route through the canonical T10 primitive: empty → single transition,
     * with the forward Dijkstra barrier on the inserted root and a
     * topology_gen bump.  o was just allocated with protos == 0 (empty form)
     * so the "shade existing" branch is a no-op. */
    urbi_object_set_protos_single(vm, o, root);
    *slot = o;

    /* T36: kept alive by object_roots_walker (see urbi_object_root). */
    return o;
}

/* === T39: urbi_object_clone ===
 *
 * Per pre-M2 §4.4 + atom-clone.chk.  Atom-aware clone:
 *   - Allocates a fresh UObject in parent's atom family (low-4 of flags).
 *   - Threads `parent` into the clone's protos as the single-tag form, so
 *     `clone.foo` resolves via the prototype walk to parent.foo (or any
 *     of parent's own prototypes).
 *   - Marks parent as IS_PROTOTYPE (via urbi_object_set_protos_single's
 *     monotonic flag set), so future slot installs on parent bump
 *     topology_gen and invalidate IC entries that walked through it.
 *
 * Returns NULL on NULL parent or OOM. */
UObject *
urbi_object_clone(UVM *vm, UObject *parent)
{
    if (vm == NULL || parent == NULL) {
        return NULL;
    }
    URBIAtomFamily fam =
        (URBIAtomFamily)(parent->flags & URBI_OBJ_ATOM_MASK);
    UObject *clone = urbi_object_alloc(vm, fam);
    if (clone == NULL) {
        return NULL;
    }
    /* set_protos_single fires the forward Dijkstra barrier on parent,
     * sets URBI_OBJ_FLAG_IS_PROTOTYPE on parent, and bumps topology_gen. */
    urbi_object_set_protos_single(vm, clone, parent);
    return clone;
}

/* === T36: GC root provider for atom singletons + UChunkInstance list ===
 *
 * Per pre-M3 GC roots spec §5.3 + pre-M4 amendments.  Three new root sources:
 *   1. Atom-family singletons (vm->atom_object .. vm->atom_symbol).
 *   2. The root shape (vm->root_shape).
 *   3. UChunkInstance chain reachable from vm->module_instances_head.
 *
 * Each is a direct UCell pointer (not a UValue), so we shade via
 * gc_shade_gray rather than calling cb (mark_root_callback acts on UValue
 * slots — UVAL_CLOSURE / UVAL_OBJECT — which doesn't fit the singleton
 * pointers held in UVM fields).  The cb / ctx parameters are unused; their
 * presence keeps the UGcRootProviderFn signature uniform across providers.
 *
 * Children are reached via the registered walkers in src/object/utypes_init.c:
 *   - walk_uobject shades shape, slots, and the proto chain
 *   - walk_ushape shades parent + transitions + props_table contents
 *   - walk_umoduleinstance shades the proto_instances UProtoInstanceArr
 *
 * UProtoInstance entries (UIC caches) intentionally have a no-op walker
 * (walk_noop) — every cell referenced by a cached entry (recv_shape,
 * UProps, USlot pointer's holding object) is already kept alive by a
 * stronger reachability path: receiver-side UObject's walk_ushape walks
 * the shape; UProps cells are reachable through the same shape's
 * props_table walk; USlot pointers point into the holding UObject's
 * slots[], which the holding UObject's walker covers.  See OBJ-028
 * (closed v0.5.7-fixes Phase 13). */
static void
object_roots_walker(UVM *vm, UGcRootCallback cb, void *ctx)
{
    (void)cb; (void)ctx;   /* direct gc_shade_gray; cb only handles UValue slots */

    /* Atom-family singletons — loop over the shared kAtomFieldOffset table.
     * TIDY-006: same (char *) intermediate pattern as urbi_object_atom; see
     * comment there for the strict-aliasing rationale. */
    for (int i = 0; i < KATOM_TABLE_COUNT; i++) {
        UObject *a = *(UObject **)((char *)vm + kAtomFieldOffset[i]);
        if (a != NULL) {
            gc_shade_gray(vm, (UCell *)a);
        }
    }

    /* M5 T53/T54 native proto objects. */
    if (vm->event_proto != NULL) gc_shade_gray(vm, (UCell *)vm->event_proto);
    if (vm->tag_proto   != NULL) gc_shade_gray(vm, (UCell *)vm->tag_proto);

    /* M6 Phase 6: container proto singletons for Pair / Triplet / Tuple. */
    if (vm->container_pair_proto    != NULL) gc_shade_gray(vm, (UCell *)vm->container_pair_proto);
    if (vm->container_triplet_proto != NULL) gc_shade_gray(vm, (UCell *)vm->container_triplet_proto);
    if (vm->container_tuple_proto   != NULL) gc_shade_gray(vm, (UCell *)vm->container_tuple_proto);

    /* M6 Phase 7: Exception primitive proto. */
    if (vm->exception_proto != NULL) gc_shade_gray(vm, (UCell *)vm->exception_proto);
    /* Cached Exception-subclass protos (urbi_exception_subclass_protos_resolve). */
    if (vm->typeerror_proto   != NULL) gc_shade_gray(vm, (UCell *)vm->typeerror_proto);
    if (vm->arityerror_proto  != NULL) gc_shade_gray(vm, (UCell *)vm->arityerror_proto);
    if (vm->lookuperror_proto != NULL) gc_shade_gray(vm, (UCell *)vm->lookuperror_proto);
    if (vm->oomerror_proto    != NULL) gc_shade_gray(vm, (UCell *)vm->oomerror_proto);

    /* M6 Phase 8: namespace proto singletons.  T86 onwards.  platform_-
     * proto is reached transitively via System's "Platform" slot but is
     * shaded directly to keep the walker uniform. */
    if (vm->math_proto             != NULL) gc_shade_gray(vm, (UCell *)vm->math_proto);
    if (vm->system_proto           != NULL) gc_shade_gray(vm, (UCell *)vm->system_proto);
    if (vm->platform_proto         != NULL) gc_shade_gray(vm, (UCell *)vm->platform_proto);
    if (vm->global_namespace_proto != NULL) gc_shade_gray(vm, (UCell *)vm->global_namespace_proto);
    if (vm->callmessage_proto      != NULL) gc_shade_gray(vm, (UCell *)vm->callmessage_proto);

    /* M6 Phase 9: primitive proto singletons (Mutex / Date / Duration). */
    if (vm->mutex_proto    != NULL) gc_shade_gray(vm, (UCell *)vm->mutex_proto);
    if (vm->date_proto     != NULL) gc_shade_gray(vm, (UCell *)vm->date_proto);
    if (vm->duration_proto != NULL) gc_shade_gray(vm, (UCell *)vm->duration_proto);
    /* v1.0 stdlib-completeness: RegExp proto singleton. */
    if (vm->regexp_proto   != NULL) gc_shade_gray(vm, (UCell *)vm->regexp_proto);

    /* v0.9.1 Phase 5: Lobby proto singleton.  Carries the
     * `__builtin_lobby_send` native method + the `lobbies` List slot
     * populated at lobby.u runtime.  Shaded directly so the List held
     * in the `lobbies` slot stays reachable through the proto's slot
     * walk (transitively reached via the proto's UShape + slots[]). */
    if (vm->lobby_proto != NULL) gc_shade_gray(vm, (UCell *)vm->lobby_proto);

    /* v0.10.10 / D7-A: Job proto singleton (R4). */
    if (vm->job_proto != NULL) gc_shade_gray(vm, (UCell *)vm->job_proto);

    /* v0.10.11 / D6: Channel proto singleton. */
    if (vm->channel_proto != NULL) gc_shade_gray(vm, (UCell *)vm->channel_proto);

    /* v0.9.1 Debug namespace proto.  Always present when URBI_ENABLE_REPL=1
     * AND urbi_debug_namespace_register has run; NULL on default builds.
     * The void* in UVM keeps this header REPL-condition-free; cast back
     * to UObject* for the shade. */
    if (vm->debug_proto != NULL) gc_shade_gray(vm, (UCell *)vm->debug_proto);

#ifdef URBI_ENABLE_ROS2
    /* v0.12.0: `ros` namespace proto; NULL when URBI_ENABLE_ROS2=0 (field absent). */
    if (vm->ros_proto != NULL) gc_shade_gray(vm, (UCell *)vm->ros_proto);
#endif

    /* Root shape. */
    if (vm->root_shape != NULL) gc_shade_gray(vm, (UCell *)vm->root_shape);

    /* UChunkInstance chain (each cell's IC tables + proto_instances are
     * traced by walk_umoduleinstance / walk_uprotoinstance). */
    for (UChunkInstance *mi = vm->module_instances_head;
         mi != NULL;
         mi = mi->next_in_vm) {
        gc_shade_gray(vm, (UCell *)mi);
    }
}

void
urbi_object_register_gc_roots(struct UVM *vm)
{
    urbi_gc_register_root_provider(vm, object_roots_walker);
}
