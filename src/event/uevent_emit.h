/* SPDX-License-Identifier: BSD-3-Clause */
/* Event emit primitives (internal header, src/ only).
 * Spec #3 §5.2, §5.3, §5.4, §7.1.
 * Public C API binding lands in T53. */

#ifndef UEVENT_EMIT_H
#define UEVENT_EMIT_H

#include "module/umodule.h"   /* UValue */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct UEvent;
struct UStrand;

/* c_event_emit_async: fan out payload to all subscribers asynchronously.
 *   AT_EVENT / AT_EVENT_SYNC watcher bodies → spawn strand via
 *     do_spawn_body_coroutine (fire_context NULL at M5 baseline).
 *   Waiters (USTRAND_WAIT_EVENT) → deposit payload + wake.
 * FIFO registration order (D-event-1 determinism).
 * Not ISR-safe. */
void c_event_emit_async(struct UVM *vm, struct UEvent *e, UValue payload);

/* c_event_emit_sync: as async but AT_EVENT_SYNC bodies run inline on the
 * watcher scratch frame before returning.  Degrades to async with a
 * one-shot URBI_LOG_WARN if any scratch-context flag is set (§5.4 / S23).
 * Not ISR-safe. Added by T50. */
void c_event_emit_sync(struct UVM *vm, struct UEvent *e, UValue payload);

/* c_event_waituntil: block the calling strand until e is emitted.
 *   Tail-appends caller to e->waiters_head; transitions to USTRAND_WAIT_EVENT.
 *   Opcode caller (T53) MUST goto exit_strand after this returns.
 *   Returns last_event_payload deposited by c_event_emit_* on wake.
 * Scratch-context guard: returns NIL + URBI_LOG_WARN if in_watcher_scratch
 *   or in_watcher_eval is set.
 * Requires vm->cur_strand to point at the currently-dispatching strand.
 * Not ISR-safe. Added by T51. */
UValue c_event_waituntil(struct UVM *vm, struct UEvent *e);

/* uevent_waiter_unregister: splice s out of e->waiters_head (spec #3 §6.4).
 *
 * Called when a strand on USTRAND_WAIT_EVENT transitions out for any
 * non-emit reason (tag-stop, cancel, panic).  After this call:
 *   - s is no longer linked on e->waiters_head.
 *   - s->wait_event_target == NULL.
 *   - s->next_event_waiter == NULL.
 *   - s->last_event_payload is left NIL — caller resumes with NIL,
 *     interpreted by stdlib as cancellation.
 *
 * Idempotent: if s->wait_event_target is already NULL, returns immediately.
 * Not ISR-safe. Added by T52. */
void uevent_waiter_unregister(struct UStrand *s);

#ifdef __cplusplus
}
#endif

#endif /* UEVENT_EMIT_H */
