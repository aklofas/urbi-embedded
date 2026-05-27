/* SPDX-License-Identifier: BSD-3-Clause */
/* v0.10.10 / D7-A: Job proto — script-side strand introspection.
 *
 * Job is a thin Object wrapper around a UStrand identified by its
 * pointer cast to a uint64_t stored in the `__strand` slot.  Using the
 * pointer (not a raw pointer dereference) lets Job instances survive
 * past strand-DEAD without dangling: the resolve function walks
 * realm->strands_head looking for a live match; a miss (strand already
 * reaped) returns nil / empty-list / "dead" instead of UAF.
 *
 * Per-VM singleton at vm->job_proto.  Allocated by
 * urbi_job_proto_register from urbi_stdlib_boot.  Bound as the realm
 * global "Job" by urbi_job_proto_register_globals in the post-loop
 * hook inside urbi_populate_realm_globals. */

#ifndef URBI_JOB_PROTO_H
#define URBI_JOB_PROTO_H

#include "urbi/types.h"   /* UValue */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct UStrand;
struct UObject;

/* Allocate vm->job_proto if not yet present and install its four
 * C-native methods (current, tags, uid, status).  Idempotent.
 * Returns URBI_OK on success, URBI_ERR_OOM on allocation failure.
 * Called from urbi_stdlib_boot. */
int urbi_job_proto_register(struct UVM *vm);

/* Bind "Job" as a realm-global constant on `realm`.  Called from
 * urbi_populate_realm_globals in the post-loop hook section (after
 * urbi_job_proto_register has been called from urbi_stdlib_boot). */
int urbi_job_proto_register_globals(struct UVM *vm, struct URealm *realm);

/* Create a fresh Job instance wrapping `strand` (pointer stored as
 * UVAL_INT internally).  Returns a UVAL_NIL on OOM; caller must root
 * the returned value before triggering GC.  `strand` must be non-NULL. */
UValue urbi_job_make(struct UVM *vm, struct UStrand *strand);

#ifdef __cplusplus
}
#endif

#endif /* URBI_JOB_PROTO_H */
