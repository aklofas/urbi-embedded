/* SPDX-License-Identifier: BSD-3-Clause */
/* uchunk_instance.h — UChunkInstance + UProtoInstance: per-VM IC RAM tier.
 *
 * UModule is read-only (flash-resident on freestanding targets); it owns the
 * bytecode + UProto definitions + ic_count / ic_names side tables.  The
 * mutable IC state lives in a per-VM UChunkInstance allocated at module
 * load time.  Two instances of the same UModule (e.g. one per VM) hold
 * independent IC tables — IC fill in one instance does not bleed into the
 * other.
 *
 * Design references: pre-M4 getslot/setslot encoding §4.1/§4.3.
 *
 * Layout:
 *   UChunkInstance (GC cell, type_tag = UTYPE_MODULE_INSTANCE)
 *     -> module: non-owning UModule*
 *     -> vm:     non-owning UVM* (debug + future cross-instance assertions)
 *     -> proto_instances: non-owning UProtoInstanceArr* (separate GC cell)
 *
 *   UProtoInstanceArr (GC cell, type_tag = UTYPE_PROTO_INSTANCE)
 *     Single bulk allocation that holds every per-proto IC table in one
 *     contiguous block.  Layout:
 *       UCell                                     header (2 B + 6 B pad)
 *       uint16_t n                                entry count (= 1 + module->nested_count)
 *       uint16_t _pad[3]                          align entries[] to 8 B
 *       UProtoInstance entries[n]                 one per (root chunk + nested proto)
 *       UIC ics[sum(nested[i]->ic_count)]         contiguous IC tables
 *
 *   UProtoInstance (NOT a GC cell — lives inside the bulk above)
 *     -> proto:    non-owning UProto* (NULL for entries[0], the root chunk)
 *     -> ic_table: pointer into the bulk's trailing IC region; NULL if proto->ic_count == 0
 *
 * Why a single bulk for the IC tables:
 *   - One GC cell to mark/sweep instead of N+1.
 *   - One allocation on the freestanding allocator (saves arena bookkeeping).
 *   - Cache-locality: walking the IC tables for a module instance touches one
 *     contiguous range.
 *
 * Reachability:
 *   The UChunkInstance walker shades the UProtoInstanceArr.  The arr
 *   walker is a deliberate no-op (walk_noop, OBJ-028): every UIC entry's
 *   children (recv_shapes[e], slots[e], uprops[e]) are reachable through
 *   stronger paths — see the type_uproto_instance banner in
 *   src/object/utypes_init.c.  recv_protos[e] (T8b) is exempt outright:
 *   an opaque identity key word, compared but never dereferenced, so it
 *   needs no shading.
 *
 * Lifecycle:
 *   urbi_chunk_instance_create allocates both cells and zero-fills every
 *   IC entry (recv_shapes=NULL, recv_protos=0, topology_gen=0, slots=NULL,
 *   uprops=NULL, flags=0, n=0, replace_cursor=0).  topology_gen=0 is the
 *   unfilled sentinel per pre-M4 topology-generation spec §3.1
 *   (vm->topology_gen init=1 — no live shape ever has gen 0).
 *
 *   urbi_chunk_instance_destroy is a no-op; both cells are GC-managed
 *   and reaped by sweep when no roots reach the UChunkInstance. */

#ifndef UCHUNK_INSTANCE_H
#define UCHUNK_INSTANCE_H

#include <stdint.h>

#include "gc/ugc.h"           /* UCell, UTYPE_MODULE_INSTANCE, UTYPE_PROTO_INSTANCE */
#include "object/uic.h"       /* UIC */
#include "chunk/uchunk.h"          /* UProto (UModule deleted v0.9.2) */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

#ifndef URBI_MODULE_INSTANCE_TYPEDEF_DEFINED
#define URBI_MODULE_INSTANCE_TYPEDEF_DEFINED
typedef struct UChunkInstance UChunkInstance;
#endif

/* === UProtoInstance === */
typedef struct UProtoInstance {
    UProto  *proto;          /* non-owning; NULL for the root-chunk entry */
    UIC     *ic_table;       /* sized to proto->ic_count; NULL if ic_count == 0 */
} UProtoInstance;

/* === UProtoInstanceArr ===
 *
 * Bulk allocation; one GC cell per UChunkInstance.  Field order is
 * load-bearing (UCell first member; explicit pad to 8 B before entries[]). */
typedef struct UProtoInstanceArr {
    UCell           cell;          /* type_tag = UTYPE_PROTO_INSTANCE */
    /* 6 B compiler-inserted padding before n */
    uint16_t        n;             /* entry count = 1 + module->nested_count */
    uint16_t        _pad[3];       /* explicit pad to 8 B align entries[] */
    UProtoInstance  entries[];     /* flexible array; trailing IC bytes follow */
} UProtoInstanceArr;

/* === UChunkInstance ===
 *
 * Public typedef provided in include/urbi/urbi.h via a guarded forward decl;
 * include that header before this one when both are needed. */
struct UChunkInstance {
    UCell                    cell;            /* type_tag = UTYPE_MODULE_INSTANCE */
    /* 6 B compiler-inserted padding before module */
    UProto                  *module;          /* non-owning root UProto (was UModule*; v0.9.2) */
    struct UVM              *vm;              /* non-owning */
    UProtoInstanceArr       *proto_instances; /* non-owning; separate GC cell */
    /* Per-VM list of all live UChunkInstance cells.  Threaded onto
     * vm->module_instances_head at create time so the determinism checksum
     * (and any future cross-instance walker) can iterate every live IC
     * table without an out-of-band registry. */
    struct UChunkInstance  *next_in_vm;
};

/* === API === */

/* Allocate a fresh UChunkInstance bound to (vm, m).  Allocates the
 * UProtoInstanceArr bulk in a second GC cell, sized for one entry per
 * (root chunk + nested proto) plus the contiguous IC tables.  Returns
 * NULL on OOM (either cell allocation may fail). */
UChunkInstance *urbi_chunk_instance_create (struct UVM *vm, UProto *root);

/* No-op: both cells are GC-managed and freed by sweep.  Provided so the
 * public ABI matches the create/destroy pair convention (T22 may grow
 * an explicit teardown for IC entries that pin host resources). */
void             urbi_chunk_instance_destroy(struct UVM *vm, UChunkInstance *mi);

/* Look up a UChunkInstance for (vm, m) on vm->module_instances_head; if
 * absent, create it via urbi_chunk_instance_create and thread on.  Used by
 * the chunk-run path so OP_GETSLOT / OP_SETSLOT find a real IC table on
 * first execution of a module.  Returns NULL only on OOM during create.
 * O(N) in the size of the per-VM instance list — acceptable since N is
 * the number of distinct loaded modules per VM (typically <10).
 *
 * Single-threaded-VM contract: walk-then-prepend is unsynchronised; caller
 * must not invoke from multiple host threads concurrently against the same
 * vm.  Safe today under URBI_SCHED_COOPERATIVE; revisit if parallel realms
 * ship. */
UChunkInstance *urbi_get_or_create_chunk_instance(struct UVM *vm, UProto *root);

#ifdef __cplusplus
}
#endif

#endif /* UCHUNK_INSTANCE_H */
