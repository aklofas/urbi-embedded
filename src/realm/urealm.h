/* SPDX-License-Identifier: BSD-3-Clause */
/* URealm: per-execution-context type (namespace + tag-owner + lifetime).
 * UNamespace: name→UValue map owned by a URealm.
 * Row 8 / T14. */

#ifndef UREALM_H
#define UREALM_H

#include <stdbool.h>
#include <stdint.h>

#include "value/uvalue.h"   /* UValue, UValKind */
#include "uvm.h"      /* UVM, UGcRootCallback */

#ifdef __cplusplus
extern "C" {
#endif

/* === Forward declarations === */

struct UVM;
struct UTag;   /* T29 */
struct UNamespace;

/* === Realm flag bits (stored in URealm.flags) === */

#define REALM_GLOBAL   0x1u  /* VM's anonymous global Realm — auto-created on first access */
#define REALM_REPL     0x2u  /* hosts an interactive session */
#define REALM_MODULE   0x4u  /* owns a loaded library module */
/* 0x8-0x80 reserved */

/* === URealm struct ===
 *
 * Total: ~72 bytes on 64-bit (pointer-heavy; aligned naturally).
 * Row 8 §4.2 layout.
 *
 * All Realms belonging to a VM are kept on a doubly-linked list rooted at
 * vm->realms_head.  Head-insertion is used; order is unspecified. */

typedef struct URealm {
    /* Identity */
    struct UVM  *vm;            /* owning VM (NULL if destroyed) */
    uint32_t     id;            /* unique per VM, monotonic, never reused (starts at 1) */
    uint8_t      flags;         /* REALM_GLOBAL / REALM_REPL / REALM_MODULE */
    uint8_t      _pad[3];

    /* Tag-ownership: implicit watcher/coroutine cleanup boundary.
     * UTag is created at realm-creation time and host-managed via vm->alloc_fn.
     * GC migration (M5/M6) moves UTag to urbi_gc_alloc when needed. */
    struct UTag *tag;           /* allocated by urbi_realm_create via utag_create */

    /* Namespace: top-level bindings */
    struct UNamespace *bindings; /* name → UValue map; owned */

    /* Global object: M4 UObject that holds the realm's named slot table.
     * Populated at urbi_realm_create with the 15 v1.0 built-in globals
     * (spec #5 §4.1).  NULL until realm_create completes the alloc step.
     * GC-managed via realm_list_walk_roots shading this cell. */
    struct UObject *global_object; /* UTYPE_OBJECT; owned by GC */

    /* Reflective handle (urbiscript-visible Realm.this / Realm.tag).
     * NIL at M3; populated at M4+. */
    UValue       reflective;    /* UVAL_OBJECT at M4+; UVAL_NIL at M3 */

    /* Host-attached data */
    void        *user_data;     /* opaque to runtime */

    /* Doubly-linked-list bookkeeping */
    struct URealm *prev_in_vm;
    struct URealm *next_in_vm;

    /* Strand ownership (T38): singly-linked list of all UStrand objects
     * created under this realm via urbi_strand_create.  Threaded via
     * UStrand.next_in_realm.  Walked by urbi_realm_destroy to free
     * all heap-allocated strands when the realm is torn down.
     *
     * GC walker contract (pre-M4 GC strand-walker spec §6.1):
     *   strands_head MUST contain every live strand whose register window
     *   may hold GC-managed UValues.  Scheduler implementations are
     *   responsible for maintaining this invariant — the GC walker visits
     *   every strand on this list (with the DEAD-state filter applied
     *   inside strand_walk_roots).  This decouples GC correctness from any
     *   single scheduler's internal queues (cooperative ready/sleep,
     *   future priority bands, mutex/event wait queues, ...).
     *   See docs/internals/scheduler-design.md for the full contract. */
    struct UStrand *strands_head;
} URealm;

/* UGcRootCallback is defined in uvm.h (the canonical location).
 * Files that include urealm.h together with uvm.h get the typedef from uvm.h. */

/* === UNamespace public surface ===
 *
 * The struct definition is opaque — defined in urealm_namespace.c.
 * These 5 functions are the only entry points. */

struct UNamespace *unamespace_create(struct UVM *vm);
void               unamespace_destroy(struct UVM *vm, struct UNamespace *ns);
int                unamespace_set(struct UVM *vm, struct UNamespace *ns,
                                  const char *name, UValue value);
/* Returns pointer to stored UValue on hit, NULL on miss.
 * name must be an interned pointer (pointer-equality is used for lookup). */
UValue            *unamespace_get(struct UNamespace *ns, const char *name);
void               unamespace_walk_roots(struct UNamespace *ns,
                                         UGcRootCallback cb,
                                         struct UVM *vm, void *ctx);

/* === VM teardown helper ===
 *
 * Destroy all Realms still alive at uvm_destroy() time.
 * Called from uvm_destroy() — T14 wires this up. */
void urealm_teardown_all(struct UVM *vm);

/* === GC root walker for the full realm list ===
 *
 * Called by the GC root-provider registry (row 10 / T26) to enumerate
 * all UValues reachable from every live Realm.
 * Iterates vm->realms_head linked list; for each Realm visits:
 *   1. realm->reflective
 *   2. namespace entries (via unamespace_walk_roots)
 *   3. realm->tag — host-managed at M3; GC enrollment deferred to M5/M6 */
void realm_list_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);

/* === 4 Realm lifecycle C API functions ===
 *
 * Public declarations live in urbi.h.  Declared here too so that urealm.c
 * and internal callers that include urealm.h (but not urbi.h) can reference
 * them without redeclaring.  Both declarations are identical and compatible. */

URealm *urbi_realm_create(struct UVM *vm);
void    urbi_realm_destroy(struct UVM *vm, URealm *realm);
URealm *urbi_realm_global(struct UVM *vm);

/* Per-Realm liveness inspection.
 * Reads VM-global counters at M3 (per-realm partitioning lands at T15+).
 * out_strands, out_watchers, out_wakes may be NULL.
 * Returns non-zero if any liveness counter is positive. */
bool    urbi_realm_has_live_work(URealm *realm,
                                 uint32_t *out_strands,
                                 uint32_t *out_watchers,
                                 uint32_t *out_wakes);

#ifdef __cplusplus
}
#endif

#endif /* UREALM_H */
