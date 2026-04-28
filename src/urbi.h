/* SPDX-License-Identifier: BSD-3-Clause */
/* Public C API. */

#ifndef URBI_H
#define URBI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *urbi_version(void);

/* === Public error codes (row 7 §8 + T12) ===
 *
 * Functions in the public C API return int: 0 = URBI_OK, negative = error.
 * New codes are appended; never reordered (ABI stability).
 *
 * URBI_ERR_BYTECODE_VERSION_MISMATCH is used by the module loader (T1) and
 * is placed here (rather than umodule.h) so host embedders only include
 * one header for error inspection.  The loader's internal ULOAD_* codes
 * remain in umodule.h for internal use. */
typedef enum {
    URBI_OK                             =  0,
    URBI_ERR_INVALID_ARG                = -1,   /* NULL pointer or out-of-range argument */
    URBI_ERR_STRAND_FATAL               = -2,   /* strand is already DEAD / in fatal state */
    URBI_ERR_OOM                        = -3,   /* allocator returned NULL */
    URBI_ERR_BYTECODE_VERSION_MISMATCH  = -4,   /* module version != runtime (T1) */
    URBI_ERR_COMPILE                    = -5,   /* parse/emit error during eval */
    URBI_ERR_CLEANUP_OVERFLOW           = -6,   /* cleanup stack full (row 7 §4.3) */
    URBI_ERR_EVENT_PAYLOAD_TOO_LARGE    = -7,   /* event ring payload exceeds capacity */
    URBI_ERR_EVENT_RING_FULL            = -8    /* event ring is full (no space) */
} UErrCode;

/* === Row 7 control-transfer C API (M3 / T12) ===
 *
 * These functions allow host C code to inject unwind events into strands
 * and inspect their state.  They operate on struct UStrand / struct UTag /
 * struct UVM — forward-declared here; definitions live in ustrand.h / uvm.h.
 *
 * Thread safety: none at M3 — these are not ISR-safe.  The ISR-safe event
 * ring (urbi_inject_event) is added at T18. */
struct UVM;
struct UStrand;
struct UTag;

#include "ustrand.h"  /* UExecStatus, UValue — needed by return types below */

/* Cross-strand: deposit TAG_STOP on `tag`'s member strands.
 * Synchronous deposit + queue, runs zero bytecode on the caller.
 * T31 wires the real cross-strand walk; T12 provides a validity-check stub. */
int urbi_tag_stop(struct UVM *vm, struct UTag *tag, UValue value);

/* Deposit CANCEL unwind on `strand`. Walks strand to bottom; fatal — no catch. */
int urbi_strand_cancel(struct UStrand *strand, UValue cancel_reason);

/* Strand-level panic: skip walker, mark strand DEAD immediately.
 * For unrecoverable host errors where cleanup must not run. */
int urbi_strand_panic(struct UStrand *strand, const char *msg);

/* Read pending unwind state without modifying it. Returns UEXEC_OK if none. */
UExecStatus urbi_strand_unwind_status(const struct UStrand *strand);

/* Query fatal state.  Returns true if the strand has a fatal status; populates
 * *out_status and *out_value (both may be NULL if caller doesn't need them). */
bool urbi_strand_is_fatal(const struct UStrand *strand,
                          UExecStatus *out_status, UValue *out_value);

/* REPL session restart: clear fatal + unwind state, reset cleanup-stack depth,
 * return strand to DORMANT.  Does not free or reallocate any memory. */
int urbi_strand_reset(struct UStrand *strand);

/* === Host-callback reentrance helpers ===
 *
 * Call these from inside a host C callback (invoked from bytecode via OP_CALL
 * on a native function) to inject control-transfer events.  The dispatch loop
 * detects the non-OK pending_unwind when the callback returns. */

/* Equivalent to executing OP_THROW with `value` from within the same strand. */
void urbi_throw(struct UStrand *strand, UValue value);

/* Equivalent to executing OP_RETURN with `value` from within the same strand. */
void urbi_return_val(struct UStrand *strand, UValue value);

/* Equivalent to executing OP_TAG_STOP for `tag` from within the same strand. */
void urbi_tag_stop_local(struct UStrand *strand, struct UTag *tag, UValue value);

/* === Row 8 chunk-lifecycle C API (M3 / T14) ===
 *
 * Realm lifecycle: create, destroy, global singleton, liveness query.
 * Full struct definition is in urealm.h; include it for direct field access.
 * Forward-declaration here is sufficient for host code using only these funcs.
 *
 * Thread safety: none at M3 — same single-threaded constraint as row 7 API. */
struct URealm;

/* Create a fresh, empty Realm bound to vm.  Returns NULL on OOM. */
struct URealm *urbi_realm_create(struct UVM *vm);

/* Destroy realm: stop its tag (no-op at M3), free namespace, unlink from VM.
 * Safe to call with realm == NULL (no-op). */
void           urbi_realm_destroy(struct UVM *vm, struct URealm *realm);

/* Return (auto-creating if needed) the VM-level global Realm singleton.
 * The global Realm has REALM_GLOBAL set and persists until uvm_destroy().
 * Returns NULL on OOM. */
struct URealm *urbi_realm_global(struct UVM *vm);

/* Liveness query: reads VM-global counters (per-realm partitioning at T15+).
 * Populates out_strands / out_watchers / out_wakes (any may be NULL).
 * Returns non-zero if any liveness counter is positive. */
bool           urbi_realm_has_live_work(struct URealm *realm,
                                        uint32_t *out_strands,
                                        uint32_t *out_watchers,
                                        uint32_t *out_wakes);

#ifdef __cplusplus
}
#endif

#endif
