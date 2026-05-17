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

#include "sched/ustrand.h"        /* UStrand */
#include "runtime/uclosure.h"     /* UClosure */
#include "module/umodule.h"       /* UModule, UValue */

#ifdef __cplusplus
extern "C" {
#endif

/* Walk the cleanup stack for the pending control-transfer on strand s.
 * On return, s->pending_unwind == UEXEC_OK (transfer absorbed)
 * or s->state == USTRAND_STATE_DEAD (unhandled / fatal escalation). */
void urbi_unwind(UStrand *s);

/* ustrand_consts_for_closure — single source of truth for the
 * "constants pool from closure (or module fall-back)" rule.  Used by both
 * OP_CALL (entering callee) and pop_call_frame (returning to caller) so
 * the two sites cannot drift.  cl may be NULL when the calling frame is
 * the module's top-level.  Closes FOUND-032 (Wave 2 carry). */
static inline const UValue *
ustrand_consts_for_closure(const UStrand *s, const UClosure *cl)
{
    if (cl != NULL && cl->proto != NULL && cl->proto->constants != NULL) {
        return cl->proto->constants;
    }
    /* Chunk-top strands: constants live on s->root_proto.  Body strands (no
     * UModule — task #23 fix) fall through to entry_closure->proto. */
    if (s->root_proto != NULL) {
        return s->root_proto->constants;
    }
    if (s->entry_closure != NULL && s->entry_closure->proto != NULL) {
        return s->entry_closure->proto->constants;
    }
    return NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* UUNWIND_H */
