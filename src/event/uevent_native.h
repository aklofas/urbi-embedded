/* SPDX-License-Identifier: BSD-3-Clause */
/* Event prototype native methods (spec #3 §7.3). */

#ifndef EVENT_NATIVE_H
#define EVENT_NATIVE_H

#include "module/umodule.h"        /* UValue, UVAL_EVENT */
#include "runtime/umacros.h"       /* urbi_zero (used by uvalue_from_event) */
#include "urbi/urbi.h"      /* UHostFn */
#include "vm/uvm.h"         /* UVMError */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct UEvent;
struct UObject;

/* === UValue ↔ UEvent helpers (M5 — UVAL_EVENT kind=9) ===
 *
 * Phase-18 footprint pin (2026-05-09): the three predicates below are
 * `static inline` so the compiler can elide the call-site overhead in
 * the 5 in-tree call sites (4 in uevent_native.c, 1 in uvm.c).  Each
 * is a single field access; out-of-line they cost a function call per
 * dispatch. */

/* Pack a UEvent* into a UVAL_EVENT UValue. */
static inline UValue uvalue_from_event(struct UEvent *e) {
    UValue v;
    urbi_zero(&v, sizeof(v));
    v.kind = (uint8_t)UVAL_EVENT;
    v.v.p = (void *)e;
    return v;
}

/* Extract UEvent* from a UVAL_EVENT UValue.
 * Caller must verify kind == UVAL_EVENT first. */
static inline struct UEvent *uvalue_as_event(UValue v) {
    return (struct UEvent *)v.v.p;
}

/* Predicate: returns non-zero iff v.kind == UVAL_EVENT. */
static inline int uvalue_is_event(UValue v) {
    return v.kind == (uint8_t)UVAL_EVENT;
}

/* === urbi_register_fn ===
 *
 * Install a UVAL_HOST_FN slot on proto under the given name.
 * Returns 0 on success, -1 on OOM or NULL argument. */
int urbi_register_fn(struct UVM *vm, struct UObject *proto,
                     const char *name, UHostFn fn);

/* === event_native_register ===
 *
 * Allocate vm->event_proto and install the four native method slots.
 * Called from urbi_vm_init after the M4 object-model setup completes.
 * Returns UVM_OK on success, UVM_OOM if the proto object allocation fails. */
UVMError event_native_register(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_NATIVE_H */
