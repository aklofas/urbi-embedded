/* SPDX-License-Identifier: BSD-3-Clause */
/* Event emit primitives (internal header, src/ only).
 * Spec #3 §5.2, §5.3, §5.4, §7.1.
 * Public C API binding lands in T53. */

#ifndef UEVENT_EMIT_H
#define UEVENT_EMIT_H

#include "umodule.h"   /* UValue */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct UEvent;

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

#ifdef __cplusplus
}
#endif

#endif /* UEVENT_EMIT_H */
