/* SPDX-License-Identifier: BSD-3-Clause */
/* src/vm/uvm_atomic.c — Gap R: urbi_atomic_begin / urbi_atomic_end.
 *
 * An atomic section brackets a group of ISR-deposited events that must be
 * observed together (e.g. accelerometer + gyroscope from the same sensor tick).
 * While atomic_active is set, uevent_ring_drain (src/event/uevent_ring.c) is a
 * no-op; all ring entries stay queued.  urbi_atomic_end clears the flag and
 * calls uevent_ring_drain to flush the accumulated entries in one pass.
 *
 * URBI_DEBUG nesting guard: calling urbi_atomic_begin while already active
 * calls urbi_panic("atomic section nested").  Release builds have undefined
 * behaviour for double-begin (documented in the public header). */

#include "urbi/urbi.h"          /* URBI_ATOMIC_MAX_US, urbi_panic */
#include "vm/uvm.h"             /* UVM, atomic_active, atomic_begin_us */
#include "event/uevent_ring.h"  /* uevent_ring_drain */

void
urbi_atomic_begin(struct UVM *vm)
{
    if (!vm) return;

#ifdef URBI_DEBUG
    if (vm->atomic_active) {
        urbi_panic("atomic section nested");
    }
#endif

    vm->atomic_active = 1;

    /* Record timestamp for the URBI_DEBUG watchdog in urbi_step. */
    if (vm->host_time_us != NULL) {
        vm->atomic_begin_us = vm->host_time_us(vm->host_time_ud);
    } else {
        vm->atomic_begin_us = 0;
    }
}

void
urbi_atomic_end(struct UVM *vm)
{
    if (!vm) return;
    vm->atomic_active = 0;
    vm->atomic_begin_us = 0;
    /* Flush any ring entries accumulated while the section was open. */
    uevent_ring_drain(vm);
}
