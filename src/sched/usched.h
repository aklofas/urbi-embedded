/* SPDX-License-Identifier: BSD-3-Clause */
/* Compile-time scheduler selection mechanism.
   Define URBI_SCHED before including this header to pick an alternative
   scheduler; the default is URBI_SCHED_COOPERATIVE. */

#ifndef USCHED_H
#define USCHED_H

/* URBI_SCHED_COOPERATIVE — single-threaded cooperative scheduler; the
 * v1.0 baseline.  Drives every shipped milestone (M1-M5).
 *
 * URBI_SCHED_RT — RESERVED for v1.x.  Preemptive priority-based RT
 * scheduler shape; design pinned in
 * docs/urbi-embedded-design-risks.md row "v1.x — preemptive scheduling
 * readiness" (~25 carry-forward findings).  No header file at v1.0 —
 * `usched_rt.h` is referenced below as a forward-compat hold so that
 * a future build with URBI_SCHED=URBI_SCHED_RT links against the
 * v1.x scheduler implementation.
 *
 * URBI_SCHED_DEADLINE — RESERVED for v1.x / v2.  EDF / deadline-based
 * scheduling shape; design pinned alongside URBI_SCHED_RT in the same
 * design-risks row.  No header file at v1.0. */
#define URBI_SCHED_COOPERATIVE  1
#define URBI_SCHED_RT           2
#define URBI_SCHED_DEADLINE     3

#ifndef URBI_SCHED
#  define URBI_SCHED URBI_SCHED_COOPERATIVE   /* default */
#endif

#if URBI_SCHED == URBI_SCHED_COOPERATIVE
#  include "usched_cooperative.h"
#elif URBI_SCHED == URBI_SCHED_RT
#  include "usched_rt.h"          /* RESERVED v1.x; header does not exist at v1.0 */
#elif URBI_SCHED == URBI_SCHED_DEADLINE
#  include "usched_deadline.h"    /* RESERVED v1.x / v2; header does not exist at v1.0 */
#else
#  error "URBI_SCHED set to unknown value"
#endif

#endif /* USCHED_H */
