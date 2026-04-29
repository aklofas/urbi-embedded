/* SPDX-License-Identifier: BSD-3-Clause */
/* Compile-time scheduler selection mechanism.
   Define URBI_SCHED before including this header to pick an alternative
   scheduler; the default is URBI_SCHED_COOPERATIVE. */

#ifndef USCHED_H
#define USCHED_H

#define URBI_SCHED_COOPERATIVE  1
#define URBI_SCHED_RT           2
#define URBI_SCHED_DEADLINE     3

#ifndef URBI_SCHED
#  define URBI_SCHED URBI_SCHED_COOPERATIVE   /* default */
#endif

#if URBI_SCHED == URBI_SCHED_COOPERATIVE
#  include "usched_cooperative.h"
#elif URBI_SCHED == URBI_SCHED_RT
#  include "usched_rt.h"          /* spec-only at M3; header may not exist */
#elif URBI_SCHED == URBI_SCHED_DEADLINE
#  include "usched_deadline.h"    /* v2 */
#else
#  error "URBI_SCHED set to unknown value"
#endif

#endif /* USCHED_H */
