/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_introspect.h — 9 introspection C primitives (v0.9.1).
 *
 * Each primitive walks a chunk of VM state on the MAIN thread and emits a
 * single JSON object into a caller-provided buffer.  The output is wrapped
 * by the dispatcher into a {kind:result,value:<inner>} NDJSON envelope
 * and parsed back into a urbi value by the Debug namespace — both
 * consumers therefore see the same canonical JSON.
 *
 * Thread-safety: MAIN ONLY.  These walk VM linked lists without locks; a
 * concurrent thread mutating realms/strands/watchers would race.  The
 * dispatcher invokes them on the VM thread (the queue-drain hook), so the
 * single-threaded invariant holds in practice.
 *
 * Buffer protocol: on success, returns URBI_OK and writes *out_n with the
 * byte count actually written (no trailing NUL).  If `cap` is too small,
 * returns -1 and writes *out_n with a best-effort estimate of the required
 * size; the caller may retry with a larger buffer.  Output is always valid
 * JSON when URBI_OK is returned. */
#ifndef UREPL_INTROSPECT_H
#define UREPL_INTROSPECT_H

#include <stddef.h>
#include <stdint.h>

struct UVM;
struct URealm;

#ifdef __cplusplus
extern "C" {
#endif

/* Zero-arg primitives.  Each emits a {key:[...]} object scoped to the
 * subsystem name. */
int urbi_introspect_coros   (struct UVM *vm, char *buf, size_t cap, size_t *out_n);
int urbi_introspect_tags    (struct UVM *vm, char *buf, size_t cap, size_t *out_n);
int urbi_introspect_watchers(struct UVM *vm, char *buf, size_t cap, size_t *out_n);
int urbi_introspect_events  (struct UVM *vm, char *buf, size_t cap, size_t *out_n);
int urbi_introspect_profile (const struct UVM *vm, char *buf, size_t cap, size_t *out_n);
int urbi_introspect_gc      (const struct UVM *vm, char *buf, size_t cap, size_t *out_n);
int urbi_introspect_lobbies (struct UVM *vm, char *buf, size_t cap, size_t *out_n);
int urbi_introspect_memcheck(struct UVM *vm, char *buf, size_t cap, size_t *out_n);

/* Single-arg primitives. */
int urbi_introspect_stack(const struct UVM *vm, uint32_t coro_id,
                          char *buf, size_t cap, size_t *out_n);
int urbi_introspect_slots(struct UVM *vm, struct URealm *realm,
                          const char *obj_path, size_t obj_path_len,
                          char *buf, size_t cap, size_t *out_n);

#ifdef __cplusplus
}
#endif

#endif /* UREPL_INTROSPECT_H */
