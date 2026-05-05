/* SPDX-License-Identifier: BSD-3-Clause */
/* Event prototype native methods (spec #3 §7.3). */

#ifndef EVENT_NATIVE_H
#define EVENT_NATIVE_H

#include "umodule.h"        /* UValue, UVAL_EVENT */
#include "urbi/urbi.h"      /* UHostFn */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct UEvent;
struct UObject;

/* === UValue ↔ UEvent helpers (M5 — UVAL_EVENT kind=9) === */

/* Pack a UEvent* into a UVAL_EVENT UValue. */
UValue  uvalue_from_event(struct UEvent *e);

/* Extract UEvent* from a UVAL_EVENT UValue.
 * Caller must verify kind == UVAL_EVENT first. */
struct UEvent *uvalue_as_event(UValue v);

/* Predicate: returns non-zero iff v.kind == UVAL_EVENT. */
int     uvalue_is_event(UValue v);

/* === urbi_register_fn ===
 *
 * Install a UVAL_HOST_FN slot on proto under the given name.
 * Returns 0 on success, -1 on OOM or NULL argument. */
int urbi_register_fn(struct UVM *vm, struct UObject *proto,
                     const char *name, UHostFn fn);

/* === event_native_register ===
 *
 * Allocate vm->event_proto and install the four native method slots.
 * Called from uvm_init after the M4 object-model setup completes. */
void event_native_register(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_NATIVE_H */
