/* SPDX-License-Identifier: BSD-3-Clause */
/* uchunk.h — internal loader-strand driver API (v0.8.0).
 *
 * Loader-private declarations for the park-or-die state machine that powers
 * urbi_run_chunk's persistent loader strand path.  Not for general embedder
 * use — embedders use the public urbi_step + urbi_strand_create API from
 * <urbi/urbi.h> instead. */

#ifndef UCHUNK_STRAND_H
#define UCHUNK_STRAND_H

#ifdef __cplusplus
extern "C" {
#endif

#include "urbi/types.h"   /* UValue, UErrCode (URBI_OK etc.) */

struct UVM;
struct UStrand;

/* uchunk_loader_drive: internal driver loop for the loader strand.
 *
 * Calls urbi_step iteratively until the strand parks (any WAITING sub-state:
 * WAITING_SLEEP, WAIT_WATCHER, WAIT_EVENT, WAITING_JOIN, WAITING_HOST) or
 * dies (DEAD).  Returns:
 *
 *   URBI_OK                — clean death (OP_RET) or parked.  *out_result
 *                             populated with OP_RET value on DEAD; nil on
 *                             parked.  Strand persists in realm->strands_head
 *                             on parked.
 *   URBI_ERR_STRAND_FATAL  — strand died with fatal error.  *out_result nil.
 *   URBI_ERR_LOADER_BUDGET — outer cap exhausted; strand still runnable
 *                             (likely infinite loop at chunk top).
 *
 * Loader-private API; not for general use.  Task 8's urbi_run_chunk
 * restructure consumes this driver. */
int uchunk_loader_drive(struct UVM *vm, struct UStrand *loader,
                        UValue *out_result);

#ifdef __cplusplus
}
#endif

#endif /* UCHUNK_STRAND_H */
