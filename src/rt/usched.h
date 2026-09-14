/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/usched.h — scheduler queues for the refound/core runtime.
 *
 * STUB.  This header currently carries only the run-queue container the
 * VM struct embeds and the forward declarations the exec layer needs;
 * the scheduler itself (park/wake, the timer heap, step) lands with the
 * scheduler task.  It sits between ustrand and uexec in the include
 * order so USched can be a by-value member of UVM. */

#ifndef URT_SCHED_H
#define URT_SCHED_H
#include "rt/ustrand.h"

/* Opaque to every layer below the ones that define them. */
struct UTag;
struct UEvent;
struct URealm;

typedef struct USched {
    struct UStrand *run_head, *run_tail;   /* FIFO run queue, linked via UStrand.link */
    struct UStrand *current;               /* the one strand in dispatch, or NULL */
} USched;

/* Accessor into the owning VM, defined by the layer above (uexec.c) —
 * same pattern as uvm_gc/uvm_strings/uvm_objstats. */
USched *uvm_sched(struct UVM *vm);

#endif
