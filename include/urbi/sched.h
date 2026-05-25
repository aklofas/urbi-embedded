/* SPDX-License-Identifier: BSD-3-Clause */
/* Public per-scheduler API.
 *
 * Stability: core (feature flags); experimental (priority API — v1.x only).
 *
 * Declares the scheduler strategy enum, per-scheduler feature flags, and
 * priority-accessor prototypes (compiled only when the selected scheduler
 * exposes URBI_SCHED_HAS_PRIORITY).
 *
 * === W2: public-header de-leak ===
 * Before v0.10.3, this header included "sched/usched.h" — a src/-prefixed
 * path that caused header-not-found errors for embedders using -Iinclude
 * alone.  The public-facing content (feature flags, USchedClass enum, and 3
 * priority-accessor prototypes) is now declared directly in this header.
 * Internal src/ callers that need the cooperative-scheduler implementation
 * details continue to #include "sched/usched.h" directly with -Isrc.
 * Closes audit-1 F1 (completion).
 * === end W2 === */

#ifndef URBI_SCHED_H
#define URBI_SCHED_H

#include <stdint.h>

#include "urbi/version.h"  /* URBI_EXPERIMENTAL */

#ifdef __cplusplus
extern "C" {
#endif

/* === W2: public-header de-leak === */

/* Per-scheduler feature flags — cooperative defaults (all 0).
   RT/deadline schedulers hard-define these before including this header via
   their own implementation header; the #ifndef guards preserve them.
   Include order for internal callers: "sched/usched.h" → "sched/usched_cooperative.h"
   (cooperative) sets URBI_SCHED_HAS_PRIORITY to 0, confirming the default. */
#ifndef URBI_SCHED_HAS_PRIORITY
#  define URBI_SCHED_HAS_PRIORITY  0
#endif
#ifndef URBI_SCHED_HAS_DEADLINE
#  define URBI_SCHED_HAS_DEADLINE  0
#endif
#ifndef URBI_SCHED_HAS_AGING
#  define URBI_SCHED_HAS_AGING     0
#endif

/* Priority API — only compiled when the selected scheduler supports it.
   Cooperative (v1.0 baseline) never defines URBI_SCHED_HAS_PRIORITY != 0,
   so these declarations are absent in shipped builds.

   v0.5.5 (T11) made the `_CLASS_` infix uniform across all three
   enumerators (closes API-019).  The original asymmetric form had only
   the third member CLASS-prefixed; dropping CLASS to match the others
   would have collided with USCHED_DEADLINE (the scheduler-strategy
   selector at src/sched/usched.h:11), so the infix was pushed onto the
   first two enumerators instead. */
#if URBI_SCHED_HAS_PRIORITY
typedef enum {
    URBI_SCHED_CLASS_DEFAULT  = 0,
    URBI_SCHED_CLASS_PRIORITY = 1,
    URBI_SCHED_CLASS_DEADLINE = 2
} USchedClass;

/* Strand priority + scheduler-class accessors.
 *
 * Compiled only when the selected scheduler defines URBI_SCHED_HAS_PRIORITY.
 * The cooperative scheduler (v1.0 baseline) defines this to 0 so these
 * symbols are absent in shipped builds; the declarations are RESERVED for
 * v1.x preemptive / RT scheduler shapes.  No implementation exists at v1.0.
 *
 * urbi_strand_set_priority — assign a priority byte (0=lowest, 255=highest;
 *   exact semantics defined by the chosen scheduler).  Caller owns `s`.
 * urbi_strand_get_priority — read the strand's current priority byte.
 * urbi_strand_get_sched_class — return the strand's scheduling class
 *   (DEFAULT / PRIORITY / DEADLINE).
 *
 * All three operate on caller-owned UStrand; no ownership transfer. */
URBI_EXPERIMENTAL void        urbi_strand_set_priority(struct UStrand *s, uint8_t priority);
URBI_EXPERIMENTAL uint8_t     urbi_strand_get_priority(struct UStrand *s);
URBI_EXPERIMENTAL USchedClass urbi_strand_get_sched_class(struct UStrand *s);
#endif /* URBI_SCHED_HAS_PRIORITY */

/* === end W2: public-header de-leak === */

#ifdef __cplusplus
}
#endif

#endif /* URBI_SCHED_H */
