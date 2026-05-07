/* SPDX-License-Identifier: BSD-3-Clause */
/* Per-scheduler API umbrella.  Consumers include this header (not usched.h
   directly) to get both the chosen scheduler's interface and the
   URBI_SCHED_HAS_* flags with correct defaults.
   Include order: urbi/sched.h → usched.h → usched_cooperative.h (for M3).
   usched_cooperative.h hard-defines URBI_SCHED_HAS_* to 0; the #ifndef
   guards below are no-ops for cooperative and provide defaults for future
   RT/deadline schedulers that omit some flags. */

#ifndef URBI_SCHED_H
#define URBI_SCHED_H

#include "sched/usched.h"

/* Per-scheduler feature flags — cooperative defaults (all 0).
   RT/deadline schedulers hard-define these before this point via their own
   header, which usched.h includes first; the #ifndef guards preserve them. */
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
   Cooperative never defines URBI_SCHED_HAS_PRIORITY != 0, so these
   declarations are absent in M3 builds.

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

void        urbi_strand_set_priority(struct UStrand *s, uint8_t priority);
uint8_t     urbi_strand_get_priority(struct UStrand *s);
USchedClass urbi_strand_get_sched_class(struct UStrand *s);
#endif /* URBI_SCHED_HAS_PRIORITY */

#endif /* URBI_SCHED_H */
