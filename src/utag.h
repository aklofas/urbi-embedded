/* SPDX-License-Identifier: BSD-3-Clause */
/* UTag: scope-nesting topology node for tag-based concurrency control.
 * Row 11 / T29.
 *
 * M5 (T18): UTag is now GC-managed (urbi_gc_alloc, UTYPE_TAG).
 * gc_byte is set by urbi_gc_alloc (current_white); utag_destroy retains
 * the §3.5 member-list invariant assertion but no longer frees (GC owns). */

#ifndef UTAG_H
#define UTAG_H

#include <stdint.h>

#include "umodule.h"   /* UValue, UValKind */

#ifdef __cplusplus
extern "C" {
#endif

/* === Forward declarations === */

struct UVM;
struct UStrand;
struct UCleanupEntry;
struct UWatcher;
struct UEvent;

/* === UTag flag bits (stored in UTag.flags) === */

#define UTAG_FLAG_FROZEN  0x01u  /* RESERVED — Tag.freeze (M5/M6) */
#define UTAG_FLAG_STOPPED 0x02u  /* RESERVED — Tag.stop state (M5/M6) */

/* === UTag struct (row 11 §3.2, extended M5 spec #3 §3.4) ===
 *
 * Pure scope-nesting topology: member lists, no parent/child tree.
 * The "hierarchy" emerges from scope nesting via the cleanup-stack.
 *
 * Layout at M5: ~64 bytes on 64-bit host.
 *   Cell header  : type_tag(1) + gc_byte(1) + pad0(2) = 4 B
 *   Flags + pad  : flags(1) + pad1[3] = 4 B
 *   Pointers     : member_strands_head(8) + member_watchers_head(8) = 16 B
 *   Event ptrs   : enter_event(8) + leave_event(8) = 16 B    (spec #3 §3.4)
 *   Name UValue  : 16 B
 *   Total        : 56 B + natural padding = ~64 B
 *
 * type_tag = UTYPE_TAG (5); gc_byte = current_white (set by urbi_gc_alloc).
 * enter_event / leave_event are NULL at create; lazy-allocated by getter
 * when the first `at(tag.enter?)` or `tag.leave?` subscriber installs. */

typedef struct UTag {
    /* --- common cell header (row 10 §3.1) --- */
    uint8_t  type_tag;                  /* UTYPE_TAG */
    uint8_t  gc_byte;                   /* GC color bits; 0 at M3 (host-managed) */
    uint16_t pad0;

    /* --- watcher-related state (RESERVED v1.x; placeholders at M3) --- */
    uint8_t  flags;                     /* UTAG_FLAG_FROZEN / UTAG_FLAG_STOPPED */
    uint8_t  pad1[3];

    /* --- membership lists (row 11 §3) --- */
    struct UCleanupEntry *member_strands_head;  /* TAG_SCOPE entries, via next_member */
    struct UWatcher      *member_watchers_head; /* watchers, via UWatcher.next_in_tag */

    /* --- reactive event slots (spec #3 §3.4) ---
     * Lazy-allocated; NULL until the first `at(tag.enter?)` / `tag.leave?`
     * subscriber installs.  Walked by the UTYPE_TAG GC walker. */
    struct UEvent *enter_event;  /* fires when a strand enters this tag scope */
    struct UEvent *leave_event;  /* fires when a strand leaves this tag scope */

    /* --- name (M6 stdlib) --- */
    UValue   name;                      /* UVAL_NIL at M5; populated at M6 */
} UTag;

/* === UTag lifecycle API ===
 *
 * utag_create: allocate and zero-initialize a UTag via urbi_gc_alloc.
 *   Sets type_tag = UTYPE_TAG; gc_byte is set by urbi_gc_alloc (current_white).
 *   enter_event, leave_event, member lists, and name are zeroed/NIL.
 *   Returns NULL on OOM.
 *   Not ISR-safe.
 *
 * utag_destroy: assert §3.5 invariant (member lists empty).
 *   Does NOT free the UTag memory — the GC sweep handles deallocation.
 *   NULL-safe.  Not ISR-safe. */

UTag *utag_create(struct UVM *vm);
void  utag_destroy(struct UVM *vm, UTag *tag);

/* === Ambient-tag lookup (C-internal) ===
 *
 * urbi_strand_scope_tag: walk the strand's cleanup-stack top-down and return
 *   the owning_tag of the innermost TAG_SCOPE entry, or NULL if none.
 *   Read-only; ISR-safe (no mutation).
 *
 * Used by OP_FORK_DETACH / OP_FORK_JOIN to find the innermost ambient tag
 * and by watchers to determine their ambient scope. */

UTag *urbi_strand_scope_tag(struct UStrand *s);

#ifdef __cplusplus
}
#endif

#endif /* UTAG_H */
