/* SPDX-License-Identifier: BSD-3-Clause */
/* Row 11 / row 12 §3 — , and & separator runtime.
 *
 * Declares the three fork-opcode handler bodies and the wake-on-DEAD helper.
 * Implementations in src/uop_fork.c; dispatch wiring in src/uvm.c.
 *
 * M3 closure-spawn semantics:
 *   Each forked child runs a closure compiled from the fork expression.
 *   This defers spec §7.1 (shared-frame , semantics) to M5+; see
 *   TODO(M5+/design-risks-7) in src/uop_fork.c for the full rationale.
 */

#ifndef UOP_FORK_H
#define UOP_FORK_H

#include <stdbool.h>
#include <stdint.h>

#include "sched/ustrand.h"   /* UStrand */
#include "vm/uvm.h"       /* UVM, urbi_vm_dispatch_loop_until_yield */

#ifdef __cplusplus
extern "C" {
#endif

/* UValue UVAL_STRAND constructor / accessor macros.
 * Matches the UVAL_CLOSURE pattern — truthy by default.
 * uv.v.p holds the raw UStrand pointer. */
#define UVAL_STRAND_MAKE(sp) \
    ((UValue){ .kind = (uint8_t)UVAL_STRAND, .v = { .p = (void *)(sp) } })
#define UVAL_AS_STRAND(uv)   ((UStrand *)((uv).v.p))

/* OP_FORK_DETACH handler.
 * ABC: A = closure_reg.  B and C are reserved (encoded as 0; not decoded).
 * Spawns child from R[A] as a detached strand.
 * Parent inherits ambient tag chain; child also inherits it.
 * Returns 0 on success; sets s->fatal_status and returns -1 on OOM. */
int urbi_vm_op_fork_detach(UStrand *s, UVM *vm, uint32_t instr);

/* OP_FORK_JOIN handler.
 * ABC: A = closure_reg, B = child_handle_reg.  C is reserved (encoded as 0).
 * Spawns child from R[A]; stores child handle in R[B].
 * Returns 0 on success; -1 on OOM (strand fatal). */
int urbi_vm_op_fork_join(UStrand *s, UVM *vm, uint32_t instr);

/* OP_JOIN_WAIT handler.
 * ABC: A = child_handle_reg.  B and C are reserved (encoded as 0).
 * If child is already DEAD: returns 0 (parent continues).
 * Otherwise: threads parent onto child->joiners_head, blocks parent with
 * urbi_sched_strand_block(REASON_JOIN), and returns 1 (caller must goto exit_strand). */
int urbi_vm_op_join_wait(UStrand *s, UVM *vm, uint32_t instr);

/* Called at every strand-DEAD transition.
 * Walks s->joiners_head chain via wait_next and calls
 * urbi_sched_strand_make_runnable() for each blocked joiner.
 * Idempotent (clears joiners_head after walking).
 * Not ISR-safe; must be called from the dispatch loop or step driver. */
void urbi_vm_fork_wake_joiners(UStrand *s, UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* UOP_FORK_H */
