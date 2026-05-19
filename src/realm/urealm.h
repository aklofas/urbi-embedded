/* SPDX-License-Identifier: BSD-3-Clause */
/* URealm: per-execution-context type (namespace + tag-owner + lifetime).
 * UNamespace: name→UValue map owned by a URealm. */

#ifndef UREALM_H
#define UREALM_H

#include <stdbool.h>
#include <stdint.h>

#include "value/uvalue.h"   /* UValue, UValKind */
#include "vm/uvm.h"      /* UVM, UGcRootCallback */
#include "urbi/types.h"  /* UCompileBudget (v0.9.1) */

#ifdef __cplusplus
extern "C" {
#endif

/* === Forward declarations === */

struct UVM;
struct UTag;
struct UNamespace;
struct UModule;

/* === Forward declaration of urbi_writer_fn (v0.9.1) ===
 *
 * Defined in <urbi/urbi.h>.  Mirrored here as a typedef so URealm can store
 * the per-realm writer hook without urealm.h pulling in the whole public
 * API surface (which would create an include cycle: urbi.h includes
 * urbi/types.h, which is consumed by urealm.h's UCompileBudget storage). */
#ifndef URBI_WRITER_FN_TYPEDEF_DEFINED
#define URBI_WRITER_FN_TYPEDEF_DEFINED
typedef void (*urbi_writer_fn)(void *ud,
                               const char *channel, size_t channel_len,
                               const char *msg,     size_t msg_len,
                               uint64_t ts_us);
#endif

/* === Realm flag bits (stored in URealm.flags) === */

#define REALM_GLOBAL   0x1U  /* VM's anonymous global Realm — auto-created on first access */
#define REALM_REPL     0x2U  /* hosts an interactive session */
#define REALM_MODULE   0x4U  /* owns a loaded library module */
/* 0x8-0x80 reserved */

/* === URealm struct ===
 *
 * Total: ~56 bytes on 64-bit (pointer-heavy; aligned naturally).
 *
 * All Realms belonging to a VM are kept on a doubly-linked list rooted at
 * vm->realms_head.  Head-insertion is used; order is unspecified. */

typedef struct URealm {
    /* Identity */
    struct UVM  *vm;            /* owning VM (NULL if destroyed) */
    uint32_t     id;            /* unique per VM, monotonic, never reused (starts at 1) */
    uint8_t      flags;         /* REALM_GLOBAL / REALM_REPL / REALM_MODULE */
    bool         has_compile_budget;  /* v0.9.1: gates compile_budget below */
    uint8_t      _pad[2];

    /* === v0.9.1: per-realm compile-budget + per-realm writer ===
     *
     * compile_budget — parser-depth / AST-node / source-byte limits applied
     *   while compiling source under this realm.  Valid iff
     *   has_compile_budget is true.  See <urbi/types.h> UCompileBudget.
     *
     * writer_fn / writer_ud — per-realm output writer.  When non-NULL, the
     *   writer dispatch in urbi_vm_write_in_realm prefers this over the
     *   VM-wide writer; the REPL service uses this to route per-session
     *   output to the originating client. */
    UCompileBudget compile_budget;
    urbi_writer_fn writer_fn;
    void          *writer_ud;

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

    /* Host-attached data */
    void        *user_data;     /* opaque to runtime */

    /* Doubly-linked-list bookkeeping */
    struct URealm *prev_in_vm;
    struct URealm *next_in_vm;

    /* Strand ownership: singly-linked list of all UStrand objects created
     * under this realm via urbi_strand_create.  Threaded via
     * UStrand.next_in_realm.  Walked by urbi_realm_destroy to free
     * all heap-allocated strands when the realm is torn down (this
     * happens BEFORE utag_destroy so the realm's tag member-list is
     * empty when the tag is freed — see urealm.c step 1).
     *
     * GC-walker contract for UStrand: see ustrand.h (top-of-file comment)
     * "Strand walker contract" for the full mark+sweep interaction. */
    struct UStrand *strands_head;

    /* [runtime-only, NOT serialized] Singly-linked list of UModule shells
     * loaded under this realm via urbi_run_chunk / urbi_repl_eval /
     * urbi_load_module.  Threaded via UModule.next_in_realm; head-insertion.
     * Walked at urbi_realm_destroy time to unload each module (Task 12).
     *
     * Not a GC root chain: UModule is host-allocated (not GC-managed).
     * The UModuleInstance objects referenced by these modules stay
     * GC-rooted via vm->module_instances_head — no shading needed here.
     * v0.9.0-repl. */
    struct UModule *loaded_protos_head;
} URealm;

/* UGcRootCallback is defined in uvm.h (the canonical location).
 * Files that include urealm.h together with uvm.h get the typedef from uvm.h. */

/* === UNamespace public surface ===
 *
 * The struct definition is opaque — defined in urealm_namespace.c.
 * These 5 functions are the only entry points.
 *
 * Production routes for top-level globals at v0.5.x do NOT use this map;
 * they read/write realm->global_object via OP_GETSLOT/OP_SETSLOT and the
 * urbi_realm_{set,get}_global C-API (declared in include/urbi/urbi.h).
 * UNamespace is retained as a side-channel for:
 *   - test harnesses that seed deterministic UValue bindings without
 *     constructing a full UObject (test_realm.c, test_determinism.c)
 *   - the determinism-checksum walk (src/urbi.c::checksum_walk_cb)
 *   - the GC root-provider walker, which folds these UValues into the
 *     reachable set (urealm.c::realm_list_walk_roots step 2)
 * Removing the map would force tests to build full UObjects; the cost
 * vs. complexity trade keeps the side-channel through v1.0. */

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
 * Destroy all Realms still alive at urbi_vm_destroy() time.
 * Called from urbi_vm_destroy(). */
void urealm_teardown_all(struct UVM *vm);

/* === GC root walker for the full realm list ===
 *
 * Called by the GC root-provider registry (row 10 / T26) to enumerate
 * all UValues reachable from every live Realm.
 * Iterates vm->realms_head linked list; for each Realm visits:
 *   1. namespace entries       (via unamespace_walk_roots)
 *   2. realm->global_object    (gc_shade_gray — UTYPE_OBJECT cell)
 *   3. realm->tag              (gc_shade_gray — UTYPE_TAG cell at M5+)
 * The implementation in urealm.c is the source of truth for this list. */
void realm_list_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);

/* === 4 Realm lifecycle C API functions ===
 *
 * Public declarations live in urbi.h.  Declared here too so that urealm.c
 * and internal callers that include urealm.h (but not urbi.h) can reference
 * them without redeclaring.  Both declarations are identical and compatible. */

URealm *urbi_realm_create(struct UVM *vm);
void    urbi_realm_destroy(struct UVM *vm, URealm *realm);
URealm *urbi_realm_global(struct UVM *vm);
URealm *urbi_realm_create_repl(struct UVM *vm);

/* VM-wide liveness inspection.
 * Reads vm->strand_runnable_count / vm->watcher_active_count /
 * vm->wakeup_pending_count.  out_strands, out_watchers, out_wakes may be
 * NULL.  Returns true if any liveness counter is positive.
 *
 * Per-realm partitioning is a v1.x deferral (see urbi-embedded-design-risks.md).
 * The realm-tagged predecessor `urbi_realm_has_live_work` was renamed at
 * v0.6.0 to match the function's actual VM-wide semantic. */
bool    urbi_vm_has_live_work(const struct UVM *vm,
                              uint32_t *out_strands,
                              uint32_t *out_watchers,
                              uint32_t *out_wakes);

#ifdef __cplusplus
}
#endif

#endif /* UREALM_H */
