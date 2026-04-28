/* SPDX-License-Identifier: BSD-3-Clause */
/* uunwind.c — M3 control-transfer walker.
 *
 * T8: bridging stub.  urbi_unwind() handles UEXEC_RETURN by performing
 * the M2-equivalent pop+deliver inline, so all existing tests continue to
 * pass while the OP_RET handler is rerouted through pending_unwind.
 *
 * T9 replaces this body with the real 5-kind walker
 * (RETURN / THROW / TAG_STOP / CANCEL / and the replace-on-raise path). */

#if __STDC_HOSTED__
#  include <assert.h>
#endif

#include "uunwind.h"
#include "ustrand.h"
#include "uframe.h"   /* UCallFrame */
#include "uvm_internal.h"  /* vm_close_upvalues */

void
urbi_unwind(UStrand *s)
{
    if (s->pending_unwind == UEXEC_RETURN) {
        /* M3 bridging stub: walker hasn't landed yet (T9), but OP_RET routes
         * here via pending_unwind.  Perform the M2-equivalent pop+deliver so
         * all existing tests pass.  T9 replaces this with the real walker. */
        UValue val = s->unwind_value;

        /* Pop the call frame (mirrors M2 OP_RET handler, pre-T8 lines 782-800
         * of uvm.c). */
        UCallFrame *done = &s->frames[--s->frame_count];

        /* Close any open upvalues that point into this frame's registers. */
        vm_close_upvalues(s, done->base + done->result_dest_reg + 1,
                          &s->closed_cells);

        /* Restore caller's register window and instruction pointer. */
        s->R       = done->base;
        s->pc      = done->pc + 1;  /* advance past the OP_CALL */
        s->pc_base = s->module->instructions;
        s->cur_consts = (s->frame_count > 0 &&
                         s->frames[s->frame_count - 1].closure != NULL)
                        ? s->frames[s->frame_count - 1].closure->proto->constants
                        : s->module->constants;

        /* Write return value into caller's result slot. */
        s->R[done->result_dest_reg] = val;

        /* Clear the unwind state. */
        s->pending_unwind = UEXEC_OK;
        UValue nil_val = {0};
        s->unwind_value = nil_val;
        return;
    }

    /* THROW / TAG_STOP / CANCEL — T9 owns these. */
#if __STDC_HOSTED__
    assert(0 && "T9 owns THROW/TAG_STOP/CANCEL walker");
#endif
}
