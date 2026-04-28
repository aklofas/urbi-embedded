/* SPDX-License-Identifier: BSD-3-Clause */
/* uunwind.h — M3 control-transfer walker.
 *
 * urbi_unwind() is called from dispatch_loop_until_yield whenever
 * s->pending_unwind != UEXEC_OK.  T9 implements the real 3-kind walker:
 * CALL_FRAME absorption (RETURN), TRY_FRAME absorption (THROW/FINALLY),
 * TAG_SCOPE stub (T29 owns absorption), and fatal escalation on
 * unhandled unwinds. */

#ifndef UUNWIND_H
#define UUNWIND_H

#include "ustrand.h"  /* UStrand */

#ifdef __cplusplus
extern "C" {
#endif

/* Walk the cleanup stack for the pending control-transfer on strand s.
 * On return, s->pending_unwind == UEXEC_OK (transfer absorbed)
 * or s->state == USTRAND_STATE_DEAD (unhandled / fatal escalation). */
void urbi_unwind(UStrand *s);

#ifdef __cplusplus
}
#endif

#endif /* UUNWIND_H */
