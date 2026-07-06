/* SPDX-License-Identifier: BSD-3-Clause */
/* UCleanupEntry: per-strand cleanup-stack record for unwind/tag/try handling. */

#ifndef UCLEANUP_H
#define UCLEANUP_H

#include <stdint.h>
#include "urbi/types.h"   /* URBI_STATIC_ASSERT */

#ifdef __cplusplus
extern "C" {
#endif

/* === UCleanupKind — what kind of boundary this entry guards === */

typedef enum {
    UCLEANUP_TRY_FRAME  = 1,  /* try-finally / try-catch boundary */
    UCLEANUP_TAG_SCOPE  = 2,  /* tag-scope boundary, may carry onleave */
    UCLEANUP_CALL_FRAME = 3   /* function call boundary */
} UCleanupKind;

/* === Flag bits stored in UCleanupEntry.flags === */

#define FLAG_HAS_CATCH    0x1U
#define FLAG_HAS_FINALLY  0x2U
#define FLAG_HAS_ONLEAVE  0x4U
/* TAG_SCOPE only (v0.10.15-vm-decomp-2, v0.10.9-B): the scope is bound to a
 * user tag (R[tag_reg] held a UVAL_TAG at OP_PUSH_TAG), so OP_POP_TAG must NOT
 * utag_destroy it — the tag outlives the scope (still reachable via the user's
 * variable, and may have other open member scopes).  Anonymous scopes (bit
 * clear) own their tag and destroy it at pop. */
#define FLAG_TAG_USER_OWNED 0x8U
/* TAG_SCOPE only (v0.13.3 / v0.13.1-M): synthetic
 * ambient-inheritance entry pushed by urbi_strand_attach_ambient_tags (realm
 * tag at strand create; parent chain at fork).  These entries reference a
 * SHARED tag (the realm's, or an outer scope's) and were never opened by an
 * OP_PUSH_TAG: the unwind walker's pass-through must NOT run the scope
 * teardown on them (no leave event, no watcher cascade, no utag_destroy —
 * other strands may still be members).  Member unlink happens at
 * ustrand_destroy via strand_unlink_from_tags, as before.  0x10 sits above
 * OP_PUSH_TAG's 4-bit flags nibble A[7:4], so bytecode can never set it. */
#define FLAG_TAG_AMBIENT    0x10U

/* === Forward declarations for types that land in later tasks. === */

struct UTag;
struct UPattern;
struct UStrand;  /* ustrand.h */
struct UVM;      /* uvm.h */

/* === UCleanupEntry struct (row 7 §4.2 + row 11 §3.3 amendment) ===

   Row 7 layout: 24 B on 64-bit host.
     Fixed header: kind(1) + flags(1) + register_base(2) + register_count(2)
                   + handler_pc(2) = 8 B.
     Pointers (64-bit): owning_tag(8) + catch_pattern(8) = 16 B.
     Total row 7: 24 B.
   Row 11 amendment adds next_member(8) + strand_back(8) = 16 B → 40 B on 64-bit.
   v0.13.1 adds frame_depth(2) → fixed header 10 B
   + 6 B padding before the 8-byte-aligned pointers → 48 B on 64-bit.

   On 32-bit targets (Cortex-M, RISC-V rv32), pointers shrink to 4 B each:
     10 B fixed + 2 B padding + 4 × 4 B = 28 B.  Unit tests run host-side
     only (64-bit), so the 48 B assertion in test_cleanup.c is correct there.

   kind is stored as uint8_t (not int-sized enum) to keep the fixed header
   compact and hit the 48 B target on 64-bit. */

typedef struct UCleanupEntry {
    uint8_t       kind;            /* 1 byte — UCleanupKind value */
    uint8_t       flags;           /* 1 byte — FLAG_HAS_CATCH/FINALLY/ONLEAVE */
    uint16_t      register_base;   /* 2 bytes — first reg to clear on teardown */
    uint16_t      register_count;  /* 2 bytes — number of regs in scope */
    uint16_t      handler_pc;      /* 2 bytes — finally/catch/onleave entry */
    uint16_t      frame_depth;     /* 2 bytes — s->frame_count at push time;
                                      the walker pops call frames back to this
                                      depth before running or absorbing the
                                      entry */
    struct UTag         *owning_tag;    /* ptr — TAG_SCOPE only */
    struct UPattern     *catch_pattern; /* ptr — TRY_FRAME with catch */
    /* Row 11 §3.3 additions for tag membership tracking */
    struct UCleanupEntry *next_member;  /* ptr — TAG_SCOPE: threads tag.member_strands_head */
    struct UStrand       *strand_back;  /* ptr — TAG_SCOPE: back-link for tag.stop() walk */
} UCleanupEntry;

/* v0.5.5: pin the row 11 §3.3 layout target on 64-bit targets
 * (amended: +frame_depth, 40 → 48).
 * 32-bit targets fall through (10 B fixed + 2 B pad + 4 × 4 B = 28 B); the
 * pointer-width guard mirrors the UObject / UIC pattern in src/object/. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
URBI_STATIC_ASSERT(sizeof(UCleanupEntry) == 48,
               "UCleanupEntry must be 48 bytes on 64-bit per row 7 §4.2 + row 11 §3.3 + VM-01");
#endif

/* === URBI_CLEANUP_MAX: pre-allocated slots per strand (row 7 §4.3) ===

   Default 64; footprint preset (cross-arm Makefile) overrides to 16.
   No dynamic growth — caller must set strand fatal on overflow. */

#ifndef URBI_CLEANUP_MAX
#  define URBI_CLEANUP_MAX 64
#endif

/* === Cleanup-stack operations (operate on UStrand) ===

   These functions are internal to the runtime; public only so that
   ustrand.c can call urbi_sched_strand_cleanup_stack_init/destroy. */

/* Push a new cleanup entry onto s's stack. Returns pointer to the new top
   entry (caller fills in its fields), or NULL if the stack is full.
   Caller must set strand fatal status on NULL return. */
UCleanupEntry *urbi_sched_strand_cleanup_push(struct UStrand *s);

/* Pop the top entry and assert that its kind matches expected_kind.
   Precondition: s->cleanup_depth > 0. */
void urbi_sched_strand_cleanup_pop(struct UStrand *s, UCleanupKind expected_kind);

/* Allocate and zero-initialize the cleanup-stack array for s.
   Uses vm->alloc_fn for allocation; compatible with freestanding targets.
   Returns 0 on success, -1 on allocation failure.
   On failure, cleanup_base = NULL, cleanup_cap = 0, cleanup_depth = 0. */
int urbi_sched_strand_cleanup_stack_init(struct UStrand *s, struct UVM *vm, uint16_t cap);

/* Free the cleanup-stack array for s using vm->alloc_fn and zero all fields. */
void urbi_sched_strand_cleanup_stack_destroy(struct UStrand *s, struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* UCLEANUP_H */
