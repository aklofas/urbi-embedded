/* SPDX-License-Identifier: BSD-3-Clause */
/* UProto — nested function prototype and per-proto helpers.  Freestanding.
 *
 * Slot sites: every GETSLOT / SETSLOT / SETSLOT_UPDATE / SELF /
 * GETSLOT_CHANGE_EVENT instruction names its slot through a per-proto
 * site index.  UProto.site_count sizes the site table; site_name_strs
 * carries the names on the wire and site_names holds them interned once
 * the runtime binds the chunk (rt/uexec.c).  The runtime's per-site slot
 * cache (site_cache) is sized from the same count.
 *
 * Mirror discipline: any change to the site fields must be applied to all
 * readers and to the wire-format writer/decoder in uemit_serialize.c /
 * uchunk_io.c. */

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
 * Numeric values for UValKind are pinned by the bytecode wire format;
 * the kind-byte field comments below document the runtime semantics
 * still managed at this layer.
 *
 * Runtime-semantics notes for each UValKind discriminator:
 *   UVAL_NIL/INT/FLOAT/BOOL/STR — bytecode-pool kinds (constants)
 *   UVAL_CLOSURE — function closure; runtime-only
 *   UVAL_VOID    — result of `&` separator; runtime-only
 *   UVAL_STRAND  — strand handle (joining OP_FORK -> OP_JOIN_WAIT).
 *                  Stores a UStrand* in v.p.  GC root walker skips
 *                  (strands are sched-managed, not GC cells).
 *   UVAL_OBJECT  — UObject pointer; runtime-only.  Receivers for
 *                  OP_GETSLOT/OP_SETSLOT live in registers tagged
 *                  UVAL_OBJECT.  Heap-bearing — UObject embeds UCell.
 *   UVAL_EVENT   — UEvent pointer; runtime-only.  Heap-bearing.
 *                  Used by tag.enter / tag.leave getters.
 *   UVAL_HOST_FN — native host function slot; UHostFn cast to void*.
 *                  Used by uevent_native_register / utag_native_register.
 *                  NOT heap-bearing — function pointers are not GC cells.
 *   The loader rejects any bytecode constant-pool kind greater than
 *   UVAL_STR; the runtime-only kinds above never appear on disk. */
#include "urbi/types.h"

/* (This header used to re-export UCallFrame, UUpvalCell, UVM_MAX_FRAMES and
   UVM_STACK_CAP from the old runtime's frame header.  Call frames and the
   register stack are the strand's business now: src/rt/ustrand.h owns both,
   sizes the stack per strand instead of per VM, and grows it on demand.) */

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

struct USymbol;
typedef struct USymbol USymbol;

struct UChunkInstance;

/* Forward declaration — URealm is referenced by the absorbed root-only fields
 * below.  Defined as opaque here to avoid a circular dependency with urealm.h. */
struct URealm;

typedef struct UProto {
    uint32_t  *instructions;
    size_t     instr_count;
    size_t     instr_cap;

    UValue    *constants;
    size_t     const_count;
    size_t     const_cap;
    /* [runtime-only, NOT serialized] true iff constants[] was populated by
     * the chunk deserializer (decode_constants_into), whose UVAL_STR arm
     * mallocs a fresh buffer per string constant — uproto_destroy_buffers
     * must free those.  false for emit-time protos, whose UVAL_STR
     * constants hold intern-table pointers (VM-owned, must NOT be freed
     * here).  A single UProto's constants[] is populated exclusively by
     * one path or the other, never a mix.  Zero-init (false) elsewhere via
     * urbi_zero at proto alloc. */
    bool       constants_owned;

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

    /* Number of slot sites in this function; site_names[] and
     * site_name_strs[] are sized to it.  An instruction's C operand holds
     * the low byte of a site index and a preceding OP_EXTARG the high
     * bits, so the cap is 65,535. */
    uint16_t       site_count;
    /* Parallel array, length == site_count: the interned name of each
     * site.  Set by the emitter, or by the runtime's chunk bind for a
     * deserialized chunk.  Owned by the proto's allocator; freed in
     * uproto_destroy_buffers. */
    USymbol      **site_names;
    /* Parallel string array; one entry per site; UTF-8, NUL-terminated.
     * Populated by the emitter and by the deserializer (in lieu of
     * site_names, which stays NULL until the runtime interns the
     * strings).  Owned by the proto's allocator; each entry and the array
     * itself are freed in uproto_destroy_buffers. */
    char         **site_name_strs;

    /* Allocator hook inherited from the owning module. */
    UChunkAllocFn alloc_fn;
    void          *alloc_ud;

    struct UProto **nested;
    size_t          nested_count;
    size_t          nested_cap;

    /* The runtime's per-site slot cache: site_count entries, allocated by
     * src/rt on this proto's first slot operation and freed with the
     * chunk.  Opaque here; the chunk layer only ever writes NULL to it. */
    void          *site_cache;

    struct UProto *root;

    /* [runtime-only, NOT serialized] Per-root-proto reference count for
     * the module-grain closure lifetime.  Bumped at every strand bind
     * (uproto_root_of(proto)->refcount); decremented when the strand or
     * closure is released.  uchunk_destroy checks this counter:
     * if 0, the root_proto is freed normally; if non-zero, it is rescued onto
     * vm->rescued_protos so surviving closures keep a valid backing proto.
     *
     * uint16_t with saturation at UINT16_MAX (logs URBI_LOG_WARN; proto leaks
     * — acceptable for the v1.0 timeframe). */
    uint16_t       refcount;

    /* [runtime-only, NOT serialized] DFS pre-order serial assigned at
     * UProto construction.  Root proto gets proto_index = 0; subsequent
     * UProto allocations get module->next_proto_serial++ via either the
     * emit path (uproto_alloc_nested) or the deserialize path
     * (decode_nested_protos_into).  Both paths walk the tree in DFS pre-order
     * so serial assignment is identical regardless of load source. */
    uint16_t       proto_index;

    /* [runtime-only, NOT serialized] Back-pointer to the UChunkInstance
     * this UProto was first instantiated under.  Populated once at
     * urbi_chunk_instance_create time (tree walk over every proto).  Used
     * by OP_CLOSURE to bind cl->proto_inst without a fallback chain:
     * cl->proto_inst = &owning_module_instance->proto_instances->entries[proto_index].
     *
     * Lifetime contract: owning_module_instance is GC-managed and remains
     * valid as long as this UProto exists (the instance is kept reachable
     * via vm->module_instances_head; the module-destroy path unlinks the
     * instance from that list before the proto's refcount can hit 0).
     */
    struct UChunkInstance *owning_module_instance;

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

typedef struct UClosure UClosure;

/* --- Proto helpers --- */

/* uproto_root_of: returns the canonical-refcount target for proto.
 * For root protos: returns proto itself (proto->root == NULL).
 * For nested protos: returns the owning module's root_proto via back-pointer.
 * NULL-safe (returns NULL if proto is NULL).
 */
static inline UProto *
uproto_root_of(UProto *proto)
{
    if (!proto) return NULL;
    return proto->root ? proto->root : proto;
}

/* All external callers MUST use these functions rather than touching
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
 *   silent on freestanding.  Does NOT increment further (proto leaks —
 *   a v1.0 deferral).
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

#ifdef __cplusplus
}
#endif

#endif /* UPROTO_H */
