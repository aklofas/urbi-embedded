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

#include "module/umodule.h"   /* UValue, UValKind */

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

#define UTAG_FLAG_FROZEN  0x01U  /* RESERVED — Tag.freeze (M5/M6) */
#define UTAG_FLAG_STOPPED 0x02U  /* set by urbi_tag_stop since v0.7.1 (Gap M) */

/* === UTag struct (row 11 §3.2, extended M5 spec #3 §3.4) ===
 *
 * Pure scope-nesting topology: member lists, no parent/child tree.
 * The "hierarchy" emerges from scope nesting via the cleanup-stack.
 *
 * Layout (64-bit host): pinned at exactly 64 B by _Static_assert below.
 *   Cell header  : type_tag(1) + gc_byte(1) + pad0(2) = 4 B
 *   Flags + pad  : flags(1) + pad1[3] = 4 B
 *   Pointers     : member_strands_head(8) + member_watchers_head(8) = 16 B
 *   Event ptrs   : enter_event(8) + leave_event(8) = 16 B    (spec #3 §3.4)
 *   Parent ptr   : parent(8) = 8 B                            (v0.7.1 Gap M)
 *   Name UValue  : 16 B
 *   Total        : 64 B  (TAGCH-006 + parent: pad1[3] + pad0 absorb alignment
 *                         to the first 8 B pointer; UValue ends on an 8 B
 *                         boundary).
 *
 * type_tag = UTYPE_TAG (5); gc_byte = current_white (set by urbi_gc_alloc).
 * enter_event / leave_event are NULL at create; lazy-allocated by getter
 * when the first `at(tag.enter?)` or `tag.leave?` subscriber installs. */

typedef struct UTag {
    /* --- common cell header (row 10 §3.1) --- */
    uint8_t  type_tag;                  /* UTYPE_TAG */
    uint8_t  gc_byte;                   /* GC-managed since M5: tri-color color
                                         * bits + UGC_HAS_SLOT_CHANGE_EVENT.
                                         * Set to vm->current_white by
                                         * urbi_gc_alloc.  TAGCH-007. */
    uint16_t pad0;

    /* --- tag-state flags (declared; runtime use deferred) ---
     *
     * TAGCH-008: the bit positions UTAG_FLAG_FROZEN (0x01) and
     * UTAG_FLAG_STOPPED (0x02) are declared above (alongside this struct)
     * for spec stability, but the runtime does not set or read them in
     * v0.5.x — `Tag.freeze` and `Tag.stop`-state semantics land with the
     * stdlib at M6/M7.  Future flag adds need a header-comment update
     * here and a corresponding macro at file head.  Spec #3 §3.4 + §6 +
     * the stdlib design row in REVIVAL.md §14. */
    uint8_t  flags;                     /* UTAG_FLAG_FROZEN | UTAG_FLAG_STOPPED;
                                         * 0 in v0.5.x */
    uint8_t  pad1[3];

    /* --- membership lists (row 11 §3) ---
     *
     * TAGCH-014: the two _head fields look symmetric but are not — element
     * type and ownership differ:
     *
     *   member_strands_head: chains UCleanupEntry instances (one per strand
     *     that opened a TAG_SCOPE for this tag).  The cleanup entries are
     *     owned by their strand's cleanup stack — the tag holds a back-ref
     *     only.  Threading via UCleanupEntry.next_member.  Strand-driven
     *     mutation: push at scope-enter, splice at scope-leave.
     *
     *   member_watchers_head: chains UWatcher structs directly (no per-link
     *     entry node).  Watchers are owned by their realm (via w->realm),
     *     not by this tag — the tag is one of two intrusive lists they sit
     *     on (the other is vm->active_watchers_head).  Threading via
     *     UWatcher.next_in_tag.  Watcher-driven mutation: head-insert at
     *     install, splice at unregister.
     *
     * The shared "member_*_head" naming reflects "things scoped to this
     * tag" but DO NOT confuse the chains for shared structure: separate
     * link fields, separate element types, separate mutation paths. */
    struct UCleanupEntry *member_strands_head;  /* TAG_SCOPE entries, via next_member */
    struct UWatcher      *member_watchers_head; /* watchers, via UWatcher.next_in_tag */

    /* --- reactive event slots (spec #3 §3.4) ---
     * Lazy-allocated; NULL until the first `at(tag.enter?)` / `tag.leave?`
     * subscriber installs.  Walked by the UTYPE_TAG GC walker. */
    struct UEvent *enter_event;  /* fires when a strand enters this tag scope */
    struct UEvent *leave_event;  /* fires when a strand leaves this tag scope */

    /* --- parent tag pointer (v0.7.1 / Gap M) ---
     * Set by urbi_tag_create to the realm's root tag so urbi_tag_info can
     * report has_parent = true for host-created child tags.  NULL for the
     * realm-root tag itself (created by urbi_realm_create via utag_create).
     * Walked by the UTYPE_TAG GC walker so a host-created tag held only via
     * a child's parent pointer cannot be collected before the child. */
    struct UTag   *parent;               /* NULL for realm-root tags */

    /* --- name (M6 stdlib) --- */
    UValue   name;                      /* UVAL_NIL at M5; populated at M6 */
} UTag;

/* Layout pin: UTag is 64 B on 64-bit hosts after the v0.7.1 parent-pointer
 * addition (+8 B from 56 B).  Guarded on pointer width to avoid a hard
 * failure on 32-bit cross targets (mirrors UEvent / UObject pattern).
 * Update this assert whenever UTag fields change. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(UTag) == 64,
               "UTag size pin on 64-bit (v0.7.1 parent-pointer layout)");
#endif

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
void  utag_destroy(struct UVM *vm, const UTag *tag);

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
