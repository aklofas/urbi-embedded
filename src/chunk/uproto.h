/* SPDX-License-Identifier: BSD-3-Clause */
/* UProto — nested function prototype and per-proto helpers.  Freestanding.
 *
 * --- Inline-cache (IC) layout (v0.8.1-uproto-root) ---
 * The pair (ic_count + ic_names) appears in two places, each owned by a
 * different layer.  UModule no longer holds a copy — the root chunk is now
 * modeled as root_proto, a full UProto:
 *
 *   1. UProto.ic_count / UProto.ic_names — per-proto (both root and nested).
 *      Populated by uemit at compile time, persisted in bytecode v1.3+,
 *      freed by uproto_destroy_buffers.
 *   2. UProtoInstance.ic_count + UIC entries[] — runtime IC table per
 *      (vm, proto) pair (object/uchunk_instance.h).  Sized from #1 at
 *      module-instance creation; UIC.name is copied from ic_names.
 *
 * Mirror discipline: any change to UProto IC field naming or layout must
 * be applied to all readers and to the wire-format encoder/decoder in
 * uemit.c / uchunk_io.c. */

#ifndef UPROTO_H
#define UPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- tagged value shape shared between pool and runtime registers ---
 *
 * UValKind and UValue moved to <urbi/types.h> at v0.5.5 to break
 * the cycle where include/urbi/urbi.h pulled in this internal header
 * for UValue's definition.  Numeric values for UValKind are pinned by
 * the bytecode wire format; the kind-byte field comments below document
 * the runtime semantics still managed at this layer.
 *
 * Runtime-semantics notes for each UValKind discriminator:
 *   UVAL_NIL/INT/FLOAT/BOOL/STR — bytecode-pool kinds (constants)
 *   UVAL_CLOSURE — v0.2.0: function closure; runtime-only
 *   UVAL_VOID    — v0.2.0: result of `&` separator; runtime-only
 *   UVAL_STRAND  — v0.3.0: strand handle (OP_FORK_JOIN → OP_JOIN_WAIT).
 *                  Stores a UStrand* in v.p.  GC root walker skips
 *                  (strands are sched-managed, not GC cells).
 *                  TODO(v1.x): revisit if strand handles become user-visible.
 *   UVAL_OBJECT  — v0.4.0: UObject pointer; runtime-only.  Receivers for
 *                  OP_GETSLOT/OP_SETSLOT live in registers tagged
 *                  UVAL_OBJECT.  Heap-bearing — UObject embeds UCell.
 *   UVAL_EVENT   — v0.5.0: UEvent pointer; runtime-only.  Heap-bearing.
 *                  Used by tag.enter / tag.leave getters.
 *   UVAL_HOST_FN — v0.5.0: native host function slot; UHostFn cast to void*.
 *                  Used by uevent_native_register / utag_native_register.
 *                  NOT heap-bearing — function pointers are not GC cells.
 *   Kinds 0-10 in use at v0.5.5; kinds 11-15 reserved for future extension.
 *   In v0.5.5 bytecode constant pools, the loader rejects any kind >
 *   UVAL_STR (kinds 5-10 are runtime-only and never appear on disk). */
#include "urbi/types.h"

/* UUpvalCell, UCallFrame, UVM_MAX_FRAMES, UVM_STACK_CAP — placed here so
   UValue is in scope when uframe.h is processed (uframe.h uses UValue but
   cannot include uproto.h/uvalue.h to avoid a circular dependency). */
#include "runtime/uframe.h"

/* --- absolute-line checkpoint record --- */

typedef struct {
    uint32_t pc;
    uint32_t line;
} UAbsLine;

/* --- pluggable allocator (matches uarena pattern) --- */

typedef void *(*UChunkAllocFn)(void *ptr, size_t nbytes, void *ud);
/* Standard realloc semantics:
 *   ptr == NULL && nbytes > 0  : allocate fresh buffer; return non-NULL or NULL on OOM.
 *   ptr != NULL && nbytes == 0 : free ptr; return NULL.
 *   ptr != NULL && nbytes > 0  : reallocate ptr to nbytes (may move); return non-NULL or NULL on OOM.
 *   ptr == NULL && nbytes == 0 : no-op; return NULL.
 * ud is an opaque caller-supplied cookie passed through unchanged (same pattern as uarena). */

/* Forward declaration — USymbol is introduced in v0.4.0 (see uintern.h / object
 * model tasks).  UProto.ic_names below holds a parallel array of USymbol
 * pointers populated at emit time; populated by emit, consumed by IC fill at
 * module-instance load.  Defined as opaque here to keep uproto.h
 * dependency-free from the object/intern layer. */
struct USymbol;
typedef struct USymbol USymbol;

/* Forward declaration — UChunkInstance is introduced in v0.4.0 (see
 * object/uchunk_instance.h).  UProto.owning_module_instance (added v0.9.0)
 * holds a back-pointer to the runtime instance this proto was first
 * instantiated under.  Defined as opaque here to avoid a circular dependency
 * on object/ layer types. */
struct UChunkInstance;

/* Forward declaration — URealm is referenced by the absorbed root-only fields
 * below.  Defined as opaque here to avoid a circular dependency with urealm.h. */
struct URealm;

/* --- UProto: nested function prototype (used for function definitions). ---
 * A UProto holds the bytecode, constants, and line info for one nested
 * function body.  After v0.9.2 UModule is gone; a module IS its
 * root UProto.  Nested functions get heap-allocated UProtos stored in
 * root_proto->nested[]. */

typedef struct UProto {
    uint32_t  *instructions;
    size_t     instr_count;
    size_t     instr_cap;

    UValue    *constants;
    size_t     const_count;
    size_t     const_cap;

    int8_t    *line_deltas;

    UAbsLine  *abs_lines;
    size_t     abs_line_count;
    size_t     abs_line_cap;

    uint8_t    max_reg;
    uint8_t    nupvals;          /* count of upvalues captured by this proto */
    uint8_t    nparams;          /* count of formal parameters */
    uint8_t    arity_prologue;   /* [wire: module-header flag bit 0, NOT a
                                    proto-record byte] v0.13.5 arity self-check
                                    discipline.  1 = every >=1-param proto in
                                    this module carries a bytecode prologue
                                    that throws a catchable error below its
                                    min arity and fills omitted defaulted
                                    params, reading the passed-arg count from
                                    R[nparams] (seeded by OP_CALL / the
                                    strand-arm paths).  OP_CALL's arity check
                                    relaxes to `nargs <= nparams` for flagged
                                    protos; unflagged (pre-v0.13.5) blobs keep
                                    the exact-match check.  Set by the emitter
                                    on every proto it produces; propagated by
                                    the deserializer from the header flag byte
                                    to every proto in the chunk. */

    /* === v1.3 additions (encoding spec §5.1) === */
    /* Number of GETSLOT/SETSLOT IC sites in this function.  Populated by the
     * emitter; the parallel ic_names[] array is sized to this count.  Capped
     * at 256 by the encoding spec §3.4 (an IC site index lives in a uint8). */
    uint16_t       ic_count;
    /* Parallel array, length == ic_count; set at emit time and consumed at
     * module-instance load to populate UIC.name for each IC site.  Owned by
     * the proto's allocator; freed in uproto_destroy_buffers. */
    USymbol      **ic_names;
    /* Parallel string array; one entry per IC site; UTF-8, NUL-terminated.
     * Populated by the emitter (mirroring ic_names) and by the deserializer
     * (in lieu of ic_names, which stays NULL until module-instance create
     * interns the strings).  Owned by the proto's allocator; each entry and
     * the array itself are freed in uproto_destroy_buffers. */
    char         **ic_name_strs;

    /* Allocator hook inherited from the owning module. */
    UChunkAllocFn alloc_fn;
    void          *alloc_ud;

    /* NEW (Phase 1 v0.8.1-uproto-root): recursive child protos.
     * For v0.8.1 tag: populated only on the root_proto (flat-on-root emitter
     * per spec §4.2).  Non-root UProtos: nested_count = 0, nested = NULL.
     * For root_proto, this holds the module's nested functions.
     * Truly-recursive emitter where Bx scopes per-enclosing-proto is
     * deferred per spec §11.3. */
    struct UProto **nested;
    size_t          nested_count;
    size_t          nested_cap;

    /* [runtime-only, NOT serialized] Intrusive list link with dual Variant B
     * semantics (spec §3.7 lifetime ordering invariant):
     *
     * (a) List link — when this proto is the root_proto of a rescued module,
     *     next_alloc threads it onto vm->rescued_protos.
     *     NULL while the proto is still owned by its originating UModule.
     *
     * (b) Self-link sentinel — set by uchunk_destroy(m, NULL) (the vm=NULL
     *     defensive path) when root_proto->refcount > 0 but no vm is available
     *     to rescue immediately.  next_alloc == root_proto itself signals
     *     "destroy pending — promote to vm->rescued_protos when refcount hits 0
     *     during vm_destroy's stdlib_closures sweep".  Unambiguous because an
     *     in-module or in-list proto never points to itself.
     *
     * Zero-initialized alongside the rest of UProto at alloc time
     * (uproto_alloc_nested). */
    struct UProto *next_alloc;

    /* [runtime-only, NOT serialized] Back-pointer to the root UProto of the
     * owning module.  NULL on the root proto itself; set to module->root_proto
     * on every nested proto at allocation time.  Used by Phase 2 refcount
     * bumpers to find the canonical refcount via (proto->root ?: proto).
     * Zero-initialized at alloc time; populated by uemit_finish and
     * uchunk_deserialize post-pass. */
    struct UProto *root;

    /* [runtime-only, NOT serialized] Per-root-proto reference count for the
     * module-grain closure lifetime fix (v0.7.3 + v0.8.1).  Bumped at every
     * strand bind (uproto_root_of(proto)->refcount); decremented when the
     * strand or closure is released.  uchunk_destroy checks this counter:
     * if 0, the root_proto is freed normally; if non-zero, it is rescued onto
     * vm->rescued_protos so surviving closures keep a valid backing proto.
     *
     * uint16_t with saturation at UINT16_MAX (logs URBI_LOG_WARN; proto leaks
     * — acceptable for the v1.0 timeframe). */
    uint16_t       refcount;

    /* [runtime-only, NOT serialized] DFS pre-order serial assigned at
     * UProto construction.  Root proto gets ic_index = 0; subsequent
     * UProto allocations get module->next_proto_serial++ via either the
     * emit path (uproto_alloc_nested) or the deserialize path
     * (decode_nested_protos_into).  Both paths walk the tree in DFS pre-order
     * so serial assignment is identical regardless of load source.  Root
     * proto's ic_index = 0 is set explicitly at root-proto allocation; the
     * first nested allocation produces ic_index = 1.  v0.8.5-recursive-emit. */
    uint16_t       ic_index;

    /* [runtime-only, NOT serialized] Back-pointer to the UChunkInstance
     * this UProto was first instantiated under.  Populated once at
     * urbi_chunk_instance_create time (tree walk over every proto).  Used
     * by OP_CLOSURE to bind cl->proto_inst without a fallback chain:
     * cl->proto_inst = &owning_module_instance->proto_instances->entries[ic_index].
     *
     * Lifetime contract: owning_module_instance is GC-managed and remains
     * valid as long as this UProto exists (the instance is kept reachable
     * via vm->module_instances_head; the module-destroy path unlinks the
     * instance from that list before the proto's refcount can hit 0).
     *
     * Zero-initialised at alloc time; populated lazily on first instance
     * creation.  NULL is the "not yet instantiated" state and is detected
     * by the OP_CLOSURE assert when read.  v0.9.0-repl. */
    struct UChunkInstance *owning_module_instance;

    /* === NEW v0.9.2: absorbed from UModule (root-only meaningful) ===
     * On non-root protos (proto->root != NULL) these are zero-initialized
     * and never written.  uproto_alloc_nested asserts proto->root != NULL
     * at allocation time. */
    char           *source_name;            /* error messages — root only */
    struct UVM     *origin_vm;              /* debug — root only */
    uint16_t        next_proto_serial;      /* emit+deserialize bookkeeping — root only */
    uint16_t        total_proto_count;      /* root only */
    struct UProto  *next_in_realm;          /* realm-lifecycle linkage — root only */
    struct URealm  *owning_realm;           /* root only */
    bool            heap_allocated;         /* renamed from shell_heap_allocated — root only */
    bool            vm_owned;               /* GC-18: lifetime owned by
                                               urbi_vm_destroy, NEVER by realm teardown.
                                               Set by the stdlib boot and by every overlay
                                               register path (urobotics, future overlays).
                                               Replaces the pointer-compare + #ifdef
                                               exclusion stack in urbi_realm_destroy.
                                               Root only; zero-init (false) everywhere
                                               else. */
} UProto;

/* --- UClosure: runtime function value (proto + captured upvalues).
 * Forward declaration only — full struct definition lives in uclosure.h
 * (v0.4.0 split: UClosure embeds UCell as first member, which can't be done
 * here without a circular include via gc/ugc.h).  Files that only need
 * `UClosure *` use the typedef below; files that touch UClosure fields
 * include "uclosure.h" explicitly. */
typedef struct UClosure UClosure;

/* --- Proto helpers --- */

/* uproto_root_of: returns the canonical-refcount target for proto.
 * For root protos: returns proto itself (proto->root == NULL).
 * For nested protos: returns the owning module's root_proto via back-pointer.
 * NULL-safe (returns NULL if proto is NULL).
 *
 * v0.8.1 Variant B Phase 2: all closure-related refcount inc/dec sites
 * route through this helper to ensure bumps land on root_proto.refcount
 * (the single canonical counter for the whole module grain). */
static inline UProto *
uproto_root_of(UProto *proto)
{
    if (!proto) return NULL;
    return proto->root ? proto->root : proto;
}

/* --- UProto refcount typed-handle API (v0.10.1) ---
 *
 * All external callers MUST use these functions rather than touching
 * p->refcount directly.  The typed owner tag enables debug-build accounting
 * (urbi_proto_ref_assert_balanced) and surfaces diagnostics that the prior
 * silent inline helpers omitted.
 *
 * Two logical owner families:
 *   Closure-bind  — acquired in urbi_vm_alloc_closure; released in uclosure_destroy.
 *   Strand-bind   — acquired at strand creation (urbi_strand_create_for_module,
 *                   uop_fork, uvm_run transient); released in ustrand_destroy /
 *                   uchunk_strand early-discharge path.
 *
 * Behaviour:
 *   Saturation (refcount == UINT16_MAX): logs to stderr on hosted builds,
 *   silent on freestanding.  Does NOT increment further (proto leaks — v1.0
 *   deferral documented in design-risks).
 *   Underflow (dec when refcount == 0): URBI_REQUIRE failure (all build modes).
 */

/* Owner tag — one value per logical site identified in runtime-invariants F3.
 * Index range 0-15 maps to g_per_owner_count[] in uproto_ref.c. */
typedef int urbi_proto_ref_owner_t;
#define URBI_PROTO_REF_OWNER_CLOSURE  0   /* urbi_vm_alloc_closure / uclosure_destroy */
#define URBI_PROTO_REF_OWNER_STRAND   1   /* urbi_strand_create_for_module */
#define URBI_PROTO_REF_OWNER_FORK     2   /* uop_fork child spawn */
#define URBI_PROTO_REF_OWNER_TRANSIENT 3  /* uvm_run transient strand */
/* 4-15 reserved for future owner sites */

/* Closure-bind: long-lived ref held for the lifetime of a UClosure cell.
 * acquire: call before publishing the closure (e.g. in urbi_vm_alloc_closure).
 * release: call in the GC finalizer uclosure_destroy. */
void urbi_proto_ref_acquire(UProto *p, urbi_proto_ref_owner_t owner);
void urbi_proto_ref_release(UProto *p, urbi_proto_ref_owner_t owner);

/* Strand-bind: ref held for the lifetime of a UStrand execution.
 * acquire: call after s->root_proto is set.
 * release: call via uproto_strand_refcount_dec (which handles deferred-destroy). */
void urbi_proto_strand_ref_acquire(UProto *p, urbi_proto_ref_owner_t owner);
void urbi_proto_strand_ref_release(UProto *p, urbi_proto_ref_owner_t owner);

/* Debug-build VM lifecycle hooks.
 * Call urbi_proto_ref_vm_born() from urbi_vm_init and
 * urbi_proto_ref_vm_gone() from urbi_vm_destroy (both gated #ifdef URBI_DEBUG).
 * The balanced check fires only when the last active VM is destroyed so that
 * multi-VM tests do not produce false positives from closures still alive
 * in peer VMs. */
#ifdef URBI_DEBUG
void urbi_proto_ref_vm_born(void);
void urbi_proto_ref_vm_gone(void);   /* calls assert_balanced when last vm */
void urbi_proto_ref_assert_balanced(void);  /* can also be called manually */
#endif

/* --- Internal refcount primitives (src/chunk/ internal use only) ---
 *
 * These remain available for the uproto_strand_refcount_dec deferred-destroy
 * helper in uchunk_io.c, which already has the release accounting logic.
 * Do NOT call these directly from outside src/chunk/ — use the typed-handle
 * API above instead.
 */
static inline void
uproto_refcount_inc(UProto *p)
{
    if (p == NULL) return;
    if (p->refcount == UINT16_MAX) {
        /* Saturated: caller must use urbi_proto_ref_acquire which logs.
         * This path is hit only from within src/chunk/ via the deferred-destroy
         * helper; saturation logging is handled at the acquire layer. */
        return;
    }
    p->refcount = (uint16_t)(p->refcount + 1U);
}

static inline void
uproto_refcount_dec(UProto *p)
{
    if (p == NULL) return;
    if (p->refcount == UINT16_MAX) {
        /* Saturated — "leak forever" contract preserved; dec is a no-op. */
        return;
    }
    /* Note: underflow (refcount == 0) is caught at the urbi_proto_ref_release
     * layer before this helper is reached.  The uproto_strand_refcount_dec
     * helper in uchunk_io.c calls this directly after the external acquire
     * layer has already validated; no second guard needed here. */
    p->refcount = (uint16_t)(p->refcount - 1U);
}

/* Returns the source_name for any proto (root or nested) by routing
 * through the root-of resolver.  NULL-safe. */
static inline const char *
uproto_source_name(const UProto *p)
{
    if (!p) return NULL;
    return p->root ? p->root->source_name : p->source_name;
}

/* Layout pin — update deliberately when UProto fields change.
 * Guarded on pointer width to avoid a hard failure on 32-bit cross
 * targets, matching the UStrand / UEvent pattern.  Post-v0.9.2:
 * +40 B on 64-bit from absorbed root metadata (source_name, origin_vm,
 * next_proto_serial, total_proto_count, next_in_realm, owning_realm,
 * heap_allocated) relative to the v0.9.1 layout. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
URBI_STATIC_ASSERT(sizeof(UProto) == 224,
               "UProto size pin on 64-bit — update deliberately when fields change"
               /* v0.9.2: 224 B post-absorption of 7 root-only UModule fields */);
#endif

#ifdef __cplusplus
}
#endif

#endif /* UPROTO_H */
