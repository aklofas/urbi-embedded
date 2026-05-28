/* SPDX-License-Identifier: BSD-3-Clause */
/* src/vm/uvm_reactive_install.h — shared dispatch for the seven reactive-install
 * opcodes (v0.10.15-vm-decomp-2, W1 stage 2).
 *
 * OP_AT_INSTALL / OP_AT_SYNC_INSTALL / OP_WHENEVER_INSTALL /
 * OP_WAITUNTIL_INSTALL / OP_AT_EVENT_INSTALL / OP_AT_EVENT_SYNC_INSTALL /
 * OP_WHENEVER_EVENT_INSTALL.  Each arm body moves verbatim into a `switch (op)`
 * inside vm_reactive_install; the operand-check / install / fault helpers move
 * with them (kept static in the .c — they have no caller outside these arms).
 *
 * Audit constraint: NO generic macros that make dispatch harder to audit — a
 * plain switch, one case per opcode, each case a readable copy of its arm.
 *
 * Consumed only by uvm.c and uvm_reactive_install.c.  NOT part of the public
 * API; no versioning obligation. */

#ifndef UVM_REACTIVE_INSTALL_H
#define UVM_REACTIVE_INSTALL_H

#include "vm/uvm.h"
#include "sched/ustrand.h"

/* UVmReactiveInstallResult — return codes from vm_reactive_install.
 *
 * NEXT      — arm continues with NEXT().
 * HALT      — arm does HALT() (helper already set vm->last_error + a diagnostic).
 * PARK_EXIT — OP_WAITUNTIL_INSTALL parked the strand: the helper already did
 *             s->pc++ and decremented vm->strand_runnable_count; the arm does
 *             `steps_consumed++; goto exit_strand;` (steps_consumed is a
 *             dispatch-loop local the helper cannot touch). */
typedef enum {
    UVM_INSTALL_NEXT = 0,
    UVM_INSTALL_HALT,
    UVM_INSTALL_PARK_EXIT
} UVmReactiveInstallResult;

/* vm_reactive_install: execute the install opcode `op` for the instruction at
 * s->pc.  `op` is one of the seven OP_*_INSTALL opcodes listed above. */
UVmReactiveInstallResult vm_reactive_install(UVM *vm, UStrand *s, uint8_t op);

#endif /* UVM_REACTIVE_INSTALL_H */
