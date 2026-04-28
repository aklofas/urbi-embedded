/* SPDX-License-Identifier: BSD-3-Clause */
/* uunwind.h — M3 control-transfer walker.
 *
 * urbi_unwind() is called from dispatch_loop_until_yield whenever
 * s->pending_unwind != UEXEC_OK.  T8 ships a bridging stub that handles
 * UEXEC_RETURN (the M2-equivalent pop+deliver).  T9 replaces the body with
 * the real 5-kind walker (RETURN / THROW / TAG_STOP / CANCEL). */

#ifndef UUNWIND_H
#define UUNWIND_H

#include "ustrand.h"  /* UStrand */

/* Walk the unwind stack for the pending control-transfer on strand s.
 * On return, s->pending_unwind == UEXEC_OK (transfer absorbed)
 * or s->state == USTRAND_STATE_DEAD (unhandled). */
void urbi_unwind(UStrand *s);

#endif /* UUNWIND_H */
