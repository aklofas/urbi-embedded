/* SPDX-License-Identifier: BSD-3-Clause */
/* Public C API.
 *
 * Stability: core. PR-review rule: changes here need an ABI-bump
 * justification per <urbi/version.h> policy.
 *
 * Holding a pointer to an opaque type (UVM*, UStrand*, etc.) is part of
 * the ABI; reading through it is not. */

#ifndef URBI_H
#define URBI_H

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility push(default)   /* v1.0: export only public-header symbols */
#endif

#include <stdbool.h>
#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint64_t */

/* UValue, UErrCode, UCallbackSignal, UExecStatus (deprecated — see UStrandUnwind),
 * UStrandUnwind, UStrandState, UVMAllocFn, opaque struct fwd-decls.
 * Replaces the pre-v0.5.5 `#include "sched/ustrand.h"` that pulled an
 * internal header into the public surface; closes API-012 / INC-003.
 * v0.9.2: UModule removed (struct deleted; a module IS its root UProto).
 * v0.10.3: UVMError retired; urbi_vm_run now returns int.
 *   UCallbackSignal added for host-callback returns.
 * v0.10.3: UExecStatus deprecated; replaced by UStrandUnwind.
 *   17 functions gain (struct UVM *vm, ...) first arg; 3 void→int. */
#include "urbi/version.h"  /* URBI_ADVANCED (URBI_EXPERIMENTAL / URBI_DEPRECATED reserved) */
#include "urbi/types.h"
#include "urbi/require.h"  /* URBI_REQUIRE — invariant macro that fires in all build modes */

#ifdef __cplusplus
extern "C" {
#endif

const char *urbi_version(void);

/* === Row 7 control-transfer C API ===
 *
 * These functions allow host C code to inject unwind events into strands
 * and inspect their state.  They operate on struct UStrand / struct UTag /
 * struct UVM — forward-declared in <urbi/types.h>; definitions live in
 * sched/ustrand.h and vm/uvm.h.
 *
 * Thread safety: none at v1.0 — these are not ISR-safe.  The ISR-safe event
 * ring is available via urbi_inject_event. */

/* Cross-strand: deposit TAG_STOP on `tag`'s member strands.
 * Synchronous deposit + queue, runs zero bytecode on the caller.
 *
 * WARNING: not ISR-safe.  Walks tag->member_strands_head
 * and is reentrant via host callbacks invoked by waker logic (e.g.
 * sched_strand_unblock through urbi_event_drain_handler).  Embedders
 * calling from ISR contexts must use the urbi_inject_event ring instead
 * and let the safepoint drain (run on the consumer thread) translate the
 * event id into a tag stop.  The urbi_in_isr(vm) check fires as
 * URBI_ASSERT_NOT_ISR in debug builds when called from an ISR context
 * detected by the caller-installed ISR check function (see
 * urbi_set_isr_check_fn / urbi_in_isr below).
 *
 * The cross-strand walk is fully wired; validity checks run on every call. */
int urbi_tag_stop(struct UVM *vm, struct UTag *tag, UValue value);

/* urbi_tag_block / urbi_tag_unblock — cross-strand suspend (v0.10.9).
 *
 * urbi_tag_block walks tag->member_strands_head and arms each member's
 * BLOCK suspension gate: READY/RUNNING members suspend in place; a member
 * parked in a wait (sleep / event / join / waituntil) stays parked with
 * the gate armed, and its eventual wake lands in SUSPENDED instead of
 * READY (v0.13.3 / SCHED-08).  Each strand's unblock_value is set to
 * resume_value so a future resume can deliver it.  Sets UTAG_FLAG_BLOCKED
 * on the tag.
 *
 * urbi_tag_unblock clears UTAG_FLAG_BLOCKED and clears each member's
 * BLOCK gate; a member resumes only once its FREEZE gate is also clear.
 * Block and freeze are independent gates per workspace ledger §S6 —
 * block -> freeze -> unblock leaves the member suspended until unfreeze.
 *
 * Not ISR-safe.  Returns URBI_OK on success or URBI_ERR_INVALID_ARG
 * on NULL vm/tag. */
int urbi_tag_block(struct UVM *vm, struct UTag *tag, UValue resume_value);
int urbi_tag_unblock(struct UVM *vm, struct UTag *tag);

/* urbi_tag_freeze / urbi_tag_unfreeze — cross-strand suspend (v0.10.9).
 *
 * Same shape as urbi_tag_block / _unblock but arming the FREEZE gate (no
 * resume-value semantic).  Sets and clears UTAG_FLAG_FROZEN.  A parked
 * (sleeping/waiting) member of a frozen tag keeps its park; its wake is
 * gated into SUSPENDED until unfreeze (v0.13.3 / SCHED-08).  unfreeze
 * clears the FREEZE gate; BLOCK-gated members are independent and stay
 * suspended until unblock.
 *
 * Not ISR-safe.  Returns URBI_OK on success or URBI_ERR_INVALID_ARG
 * on NULL vm/tag. */
int urbi_tag_freeze(struct UVM *vm, struct UTag *tag);
int urbi_tag_unfreeze(struct UVM *vm, struct UTag *tag);

/* === vm-first-arg convention — control-transfer family (v0.10.3) ===
 *
 * 9 functions previously took strand as their first argument.  They now
 * follow the dominant (struct UVM *vm, ...) convention used by every other
 * public API function.  Internally, vm is already available via strand->vm;
 * the explicit vm arg adds routing clarity for the v1.x multi-VM direction
 * and closes api-ergonomics F3.
 *
 * 2 functions (urbi_throw, urbi_return_val) change void→int so they can
 * return URBI_ERR_INVALID_ARG on NULL vm/strand (api-ergonomics F8).
 *
 * urbi_strand_unwind_status now returns UStrandUnwind (public mirror of
 * internal UExecStatus) instead of UExecStatus directly; numeric values
 * are identical.  UExecStatus is deprecated but retained for one release. */

/* Deposit CANCEL unwind on `strand`. Walks strand to bottom; fatal — no catch. */
int urbi_strand_cancel(struct UVM *vm, struct UStrand *strand, UValue cancel_reason);

/* Strand-level panic: skip walker, mark strand DEAD immediately.
 * For unrecoverable host errors where cleanup must not run. */
int urbi_strand_panic(struct UVM *vm, struct UStrand *strand, const char *msg);

/* Read pending unwind state without modifying it.
 * Returns URBI_UNWIND_OK (0) if no pending unwind.  The internal
 * UExecStatus and UStrandUnwind numeric values are identical. */
UStrandUnwind urbi_strand_unwind_status(struct UVM *vm, const struct UStrand *strand);

/* Query fatal state.  Returns true if the strand has a fatal status; populates
 * *out_status and *out_value (both may be NULL if caller doesn't need them).
 * out_status receives a UStrandUnwind value; cast to UExecStatus if needed. */
bool urbi_strand_is_fatal(struct UVM *vm, const struct UStrand *strand,
                          UStrandUnwind *out_status, UValue *out_value);

/* REPL session restart: clear fatal + unwind state, reset cleanup-stack depth,
 * return strand to DORMANT.  Does not free or reallocate any memory. */
int urbi_strand_reset(struct UVM *vm, struct UStrand *strand);

/* === Host-callback reentrance helpers ===
 *
 * Call these from inside a host C callback (invoked from bytecode via OP_CALL
 * on a native function) to inject control-transfer events.  The dispatch loop
 * detects the non-OK pending_unwind when the callback returns.
 *
 * v0.10.3: all three gain vm as first arg.  urbi_throw and
 * urbi_return_val change void → int so NULL vm/strand can return
 * URBI_ERR_INVALID_ARG (api-ergonomics F8). */

/* Equivalent to executing OP_THROW with `value` from within the same strand.
 * Returns URBI_OK on success, URBI_ERR_INVALID_ARG if vm or strand is NULL. */
int urbi_throw(struct UVM *vm, struct UStrand *strand, UValue value);

/* Equivalent to executing OP_RETURN with `value` from within the same strand.
 * Returns URBI_OK on success, URBI_ERR_INVALID_ARG if vm or strand is NULL. */
int urbi_return_val(struct UVM *vm, struct UStrand *strand, UValue value);

/* Equivalent to executing OP_TAG_STOP for `tag` from within the same strand. */
void urbi_tag_stop_local(struct UVM *vm, struct UStrand *strand,
                         struct UTag *tag, UValue value);

/* === Row 8 chunk-lifecycle C API ===
 *
 * Realm lifecycle: create, destroy, global singleton, liveness query.
 * Full struct definition is in realm/urealm.h; include it for direct field access.
 * Forward-declaration here is sufficient for host code using only these funcs.
 *
 * Thread safety: none at v1.0 — same single-threaded constraint as row 7 API. */
struct URealm;

/* Create a fresh, empty Realm bound to vm.  Returns NULL on OOM.
 * On failure returns NULL; the per-VM error ring (queryable via
 * urbi_last_error) is populated with the failure code. */
struct URealm *urbi_realm_create(struct UVM *vm);

/* Destroy realm: stop its tag, free namespace, unlink from VM.
 * Safe to call with realm == NULL (no-op). */
void           urbi_realm_destroy(struct UVM *vm, struct URealm *realm);

/* Return (auto-creating if needed) the VM-level global Realm singleton.
 * The global Realm has REALM_GLOBAL set and persists until urbi_vm_destroy().
 * Returns NULL on OOM.
 * On failure returns NULL; the per-VM error ring (queryable via
 * urbi_last_error) is populated with the failure code. */
struct URealm *urbi_realm_global(struct UVM *vm);

/* Convenience wrapper: creates a URealm and sets REALM_REPL on it.  Equivalent
 * to urbi_realm_create followed by an internal flag set.  Use this for
 * per-session REPL lobbies.  The flag marks the realm for REPL-specific
 * behaviors that may land in v0.9.1-repl-service (e.g. disconnect-cleanup,
 * introspection visibility).
 *
 * Returns NULL on OOM.
 * On failure returns NULL; the per-VM error ring (queryable via
 * urbi_last_error) is populated with the failure code.
 * Thread safety: MAIN. */
struct URealm *urbi_realm_create_repl(struct UVM *vm);

/* Liveness query — INCLUSIVE "is anything at all alive?".  Returns true
 * when ANY live work exists: runnable strands, pending internal work
 * (injected events, host calls, reactive drains), timers (sleepers /
 * periodics), or ARMED work (armed watchers of any mode plus SUSPENDED /
 * WAITING strands).  Armed work counts here even though it does not block
 * urbi_step's QUIESCENT verdict — use this query to distinguish a
 * fully-dead VM from an armed-but-idle one that host input would re-arm.
 *
 * Populates out_strands (runnable strands) / out_watchers (armed watchers)
 * / out_wakes (sleeping strands); any may be NULL.  The out-params do not
 * cover every truth source the return value integrates: the function may
 * return true with all three zero (pending internal work such as injected
 * events or host calls, or SUSPENDED/WAITING strands).
 *
 * The function is VM-wide despite the per-realm spec wording: the counters
 * themselves are not partitioned per realm.  Per-realm partitioning is a
 * v1.x deferral (see docs/urbi-embedded-design-risks.md).  The realm-tagged
 * predecessor `urbi_realm_has_live_work` was renamed at v0.6.0 to match
 * actual semantic. */
bool           urbi_vm_has_live_work(const struct UVM *vm,
                                     uint32_t *out_strands,
                                     uint32_t *out_watchers,
                                     uint32_t *out_wakes);

/* === Realm globals C API ===
 *
 * Install or retrieve slots on realm->global_object directly from host C.
 * Use urbi_realm_set_global_const to create a write-protected binding that
 * urbiscript cannot overwrite at runtime.
 *
 * name / name_len — raw byte string (need not be NUL-terminated).
 * value           — the UValue to store (any kind, including UVAL_NIL).
 * out_value       — caller-allocated; written on URBI_OK return from get.
 *
 * Returns URBI_OK on success.
 * Returns URBI_ERR_INVALID_ARG if vm, realm, or name is NULL.
 * Returns URBI_ERR_OOM if intern or slot allocation fails.
 * urbi_realm_get_global additionally returns URBI_ERR_SLOT_NOT_FOUND when
 * the name is absent from the global object's prototype chain. */
int urbi_realm_set_global(struct UVM *vm, struct URealm *realm,
                          const char *name, size_t name_len, UValue value);

int urbi_realm_set_global_const(struct UVM *vm, struct URealm *realm,
                                const char *name, size_t name_len, UValue value);

int urbi_realm_get_global(struct UVM *vm, struct URealm *realm,
                          const char *name, size_t name_len, UValue *out_value);

/* === Row 8 step driver + chunk-execution C API ===
 *
 * urbi_step: drive the VM for up to budget_instructions opcodes, returning
 * a 4-state result describing what the caller should do next.
 *
 * QUIESCENT means no internal work remains: no runnable strand, no pending
 * event/host-call/reactive drain, no timer.  Armed watchers and SUSPENDED
 * (blocked/frozen) strands do NOT prevent QUIESCENT — they are re-armed by
 * host slot writes, injected events, or tag unblock/unfreeze; call
 * urbi_step again after providing input.  Use urbi_vm_has_live_work to
 * distinguish a fully-dead VM from an armed-but-idle one.
 *
 * urbi_run_chunk: run a module's root chunk under the given Realm.  realm == NULL
 * uses the VM's global Realm (auto-created on first call).  Returns URBI_OK on
 * clean completion, URBI_ERR_UNCAUGHT_THROW (-18) if the root chunk dies with
 * an uncaught script throw (*out_result is set to nil; vm->last_errmsg carries a
 * diagnostic for Exception-typed throws and is empty for scalar throws), or
 * URBI_ERR_STRAND_FATAL for non-throw fatal halts (type errors, etc.).
 *
 * urbi_repl_eval: compile a source line and run it; format the result into
 * out_buf.  Suitable for a read-eval-print loop.
 *
 * urbi_run_script: thin wrapper around urbi_run_chunk that discards the result.
 *
 * urbi_load_chunk: bind a pre-compiled module into the VM and run its root
 * chunk under the global Realm so top-level bindings install into realm
 * globals.  module_name is currently advisory (no import-table lookup yet —
 * v1.x backlog).  Returns URBI_OK on success, URBI_ERR_INVALID_ARG if any
 * argument is NULL, URBI_ERR_OOM on UChunkInstance allocation failure, or
 * an int error code (URBI_ERR_*) if root-chunk execution fails. */

struct UProto;        /* forward decl — v0.9.2: UModule deleted; a module IS its root UProto */

typedef enum {
    URBI_STEP_RUNNING   = 0,  /* budget exhausted or yield; call again */
    URBI_STEP_QUIESCENT = 1,  /* no internal work; the reactive surface may
                                 stay armed — see QUIESCENT paragraph above */
    URBI_STEP_FATAL     = 2,  /* a strand entered fatal state; inspect via urbi_strand_is_fatal */
    URBI_STEP_WAKE_AT   = 3   /* no runnable strand now; *out_next_wake_us set */
} UStepResult;

UStepResult urbi_step(struct UVM *vm,
                      uint64_t budget_instructions,
                      uint64_t *out_next_wake_us);

int urbi_run_chunk(struct UVM *vm, struct URealm *realm,
                   struct UProto *root, UValue *out_result);

/* Compile-error gated when URBI_BYTECODE_ONLY=1: the
 * compiler frontend (src/lex, src/parse, src/emit) is not linked in
 * bytecode-only builds, so urbi_repl_eval cannot function — embedders
 * trying to call it get a compile error at the call site rather than
 * an unresolved link symbol. */
#if !defined(URBI_BYTECODE_ONLY)
int urbi_repl_eval(struct UVM *vm, struct URealm *realm,
                   const char *line, size_t line_len,
                   char *out_buf, size_t out_buf_size);
#endif

int urbi_run_script(struct UVM *vm, struct URealm *realm, struct UProto *root);

int urbi_load_chunk(struct UVM *vm, struct UProto *root, const char *module_name);

/* === v0.9.0-repl: urbi_unload ===
 *
 * Unload root from its owning realm's loaded_protos_head list.  If the
 * root's refcount is > 0 (a strand is parked on the loader, or closures hold
 * UProtos), the rescue mechanism transfers the root to vm->rescued_protos and
 * final cleanup completes when the last refcount-holder releases.  Otherwise
 * the root is destroyed immediately.
 *
 * Returns URBI_OK on success (whether immediate or deferred).
 * Returns URBI_ERR_INVALID_ARG if vm or root is NULL, or if root is not
 *   bound to any realm (already unloaded).
 *
 * Thread safety: MAIN.  Not ISR-safe. */
int urbi_unload(struct UVM *vm, struct UProto *root);

/* urbi_chunk_translate_load_err: map an internal UChunkLoadError (passed
 * as int) to the corresponding public UErrCode.  Currently routes
 * UCHUNK_LOAD_UNSUPPORTED_VERSION → URBI_ERR_BYTECODE_VERSION_MISMATCH and
 * collapses every other internal code to URBI_ERR_INVALID_ARG.  Closes
 * API-005: URBI_ERR_BYTECODE_VERSION_MISMATCH is now reachable from a
 * public-API call site, even though the deserialize-bytes entry point
 * itself is available via urbi_chunk_from_bytes. */
URBI_ADVANCED int urbi_chunk_translate_load_err(int load_err);

/* === Stdlib bake (build-time tool) ===
 *
 * urbi_compile_source: compile a urbiscript source buffer to serialized
 * v1.5 wire-format bytecode.  Used by tools/urbi-compile-stdlib at build
 * time to bake `.u` overlays into the stdlib bytecode blob; usable by any
 * embedder that wants to ship pre-compiled modules.
 *
 * vm        — used during compile for string interning + emit-time identifier
 *             tables.  Must be initialized via urbi_vm_init.  The compiled
 *             bytecode is portable across VMs (deserialize re-interns).
 * src       — source bytes; need not be NUL-terminated.
 * src_len   — length of src in bytes.
 * src_name  — diagnostic-only identifier for error messages.  May be NULL.
 * out_buf   — receives a pointer to a newly-allocated buffer holding the
 *             serialized bytecode; the CALLER must free() it (hosted only —
 *             freestanding builds use the configured allocator's free path).
 * out_len   — receives the byte count.
 * err_buf   — caller-allocated diagnostic buffer (may be NULL).
 * err_cap   — capacity of err_buf in bytes; ignored if err_buf is NULL.
 *
 * Returns URBI_OK on success.  On failure, *out_buf is NULL and a
 * human-readable message is written into err_buf (NUL-terminated).
 * Failure codes:
 *   URBI_ERR_INVALID_ARG — NULL vm/src/out_buf/out_len.
 *   URBI_ERR_OOM         — allocation or serialize failure.
 *   URBI_ERR_INVALID_ARG — parse or emit error (see err_buf for details). */
/* Compile-error gated when URBI_BYTECODE_ONLY=1: the
 * compiler frontend (src/lex, src/parse, src/emit) is not linked in
 * bytecode-only builds.  Pre-baked bytecode is loaded through
 * urbi_load_chunk / urbi_run_chunk instead. */
#if !defined(URBI_BYTECODE_ONLY)
int urbi_compile_source(struct UVM *vm,
                        const char *src, size_t src_len,
                        const char *src_name,
                        unsigned char **out_buf, size_t *out_len,
                        char *err_buf, size_t err_cap);
#endif

/* === Row 9 strand lifecycle C API ===
 *
 * Separate _create (DORMANT alloc) from _start (DORMANT → READY enqueue) so
 * callers can pre-attach tags, set scheduler attrs (v1.x), or pool/recycle
 * strands before making them runnable.  _spawn is the convenience composite.
 *
 * urbi_strand_create: allocate a strand in DORMANT state and bind it to realm.
 *   entry is the closure to invoke at first activation (frame-0 setup deferred
 *   to urbi_step or a future urbi_strand_arm).  Returns NULL on OOM.
 *
 * urbi_strand_start: transition strand from DORMANT → READY (enqueue to run).
 *   Precondition (debug): strand must be in DORMANT state.
 *
 * urbi_strand_spawn: convenience composite — create + start.  Returns NULL on OOM.
 *
 * urbi_strand_destroy: dequeue from scheduler, free cleanup stack, free strand.
 *   Safe to call with s == NULL (no-op).
 *
 * Thread safety: none at v1.0 — not ISR-safe.  Must be called from the same
 * thread that drives the VM.
 *
 * urbi_strand_cancel / urbi_strand_panic / urbi_strand_reset are declared
 * in the row 7 control-transfer section above. */

struct UClosure;   /* forward decl — definition in src/chunk/uproto.h */

/* urbi_native_method_fn: signature for host C functions that back a
 * UClosure slot.  Called by OP_CALL when the closure's native_fn field is
 * set (v0.6.0+).
 *
 * Parameters:
 *   vm    — the VM executing the call.
 *   self  — receiver value (the object the slot was loaded from).
 *   args  — argument array (NULL when nargs == 0).
 *   nargs — argument count.
 *   out   — write the return value here; initialised to NIL before the call.
 *
 * Return value (v0.10.3 unified convention):
 *   URBI_CB_OK   (0) — no exception; *out holds the result.
 *   URBI_CB_THROW    — host raised an exception (set throw value via urbi_throw;
 *                      *out is ignored).
 *   negative URBI_ERR_* — host-side error (treated as fatal at v1.0).
 * Note: UEXEC_OK (0) and UEXEC_THROW (1) are still accepted for source
 * compatibility (UEXEC_OK == URBI_CB_OK == 0; UEXEC_THROW == URBI_CB_THROW
 * after the UCallbackSignal update in v0.10.3).
 *
 * Promoted to the public API at v0.7.1 (was internal-only in
 * src/runtime/uclosure.h).  urbi_make_native_closure (Gap L) takes this
 * type; so does the Gap A urbi_register helper.
 *
 * Guard prevents double-typedef when internal src/runtime/uclosure.h is
 * also included (identical definition — C99 §6.7 allows re-typedef only
 * when both are the same type). */
#ifndef URBI_NATIVE_METHOD_FN_DEFINED
#define URBI_NATIVE_METHOD_FN_DEFINED
typedef int (*urbi_native_method_fn)(struct UVM *vm,
                                     UValue self,
                                     UValue *args,
                                     uint8_t nargs,
                                     UValue *out);
#endif /* URBI_NATIVE_METHOD_FN_DEFINED */

/* urbi_make_native_closure: allocate a GC-managed UClosure backed by a
 * host C function (Gap L — foundation for urbi_register, Gap A).
 *
 * The returned closure has native_fn = fn and no bytecode body.  It becomes
 * script-callable when stored in a realm global, an object slot, or wrapped
 * as a UValue (kind UVAL_CLOSURE).  Until reachable from a GC root it may
 * be collected — embedders should either install it immediately (urbi_register
 * does this atomically) or hold a urbi_ref to it (Gap Q).
 *
 * Returns NULL on OOM or if vm == NULL or fn == NULL.
 * On failure returns NULL; the per-VM error ring (queryable via
 * urbi_last_error) is populated with the failure code.
 *
 * Thread safety: MAIN. */
struct UClosure *urbi_make_native_closure(struct UVM *vm,
                                          urbi_native_method_fn fn);

/* === Gap A — host-function registration (v0.7.1) ===
 *
 * urbi_register: install a native C function as a script-visible global.
 * Composite of urbi_make_native_closure (Gap L) + urbi_realm_set_global_const.
 * The binding is const by default — re-registering the same name returns
 * URBI_ERR_CONST_SLOT_WRITE.
 *
 * vm     — the VM owning the closure allocation.
 * realm  — target realm; NULL uses the VM's global realm.
 * name   — NUL-terminated symbol name (e.g., "myFn").
 * fn     — the C function to back the closure; must be non-NULL.
 *
 * Returns URBI_OK on success.
 * Returns URBI_ERR_INVALID_ARG if vm, name, or fn is NULL.
 * Returns URBI_ERR_OOM if the closure allocation or slot intern fails.
 * Returns URBI_ERR_CONST_SLOT_WRITE if a binding with this name already exists.
 *
 * Thread safety: MAIN. */
int urbi_register(struct UVM *vm, struct URealm *realm,
                  const char *name, urbi_native_method_fn fn);

/* === Gap M — tag state types (v0.7.1) ===
 *
 * urbi_tag_state_t: observable state of a UTag derived from its flags byte.
 *
 *   URBI_TAG_RUNNING — default state; no flags set.
 *   URBI_TAG_STOPPED — UTAG_FLAG_STOPPED (0x02) set via urbi_tag_stop.
 *   URBI_TAG_FROZEN  — UTAG_FLAG_FROZEN  (0x01) set (reserved; v1.x stdlib).
 *   URBI_TAG_BLOCKED — reserved for future scheduler-integration state.
 *
 * urbi_tag_info_t: aggregate tag state snapshot returned by urbi_tag_info.
 *   state        — decoded from UTag.flags.
 *   member_count — number of UCleanupEntry nodes on UTag.member_strands_head.
 *   has_parent   — true if UTag.parent != NULL (set by urbi_tag_create). */
typedef enum {
    URBI_TAG_RUNNING = 0,
    URBI_TAG_STOPPED = 1,
    URBI_TAG_FROZEN  = 2,
    URBI_TAG_BLOCKED = 3
} urbi_tag_state_t;

typedef struct {
    urbi_tag_state_t state;
    size_t           member_count;
    bool             has_parent;
} urbi_tag_info_t;

/* Drift-detection: the urbi_tag_state_t encoding in urbi_tag_info is derived
 * from UTAG_FLAG_* bits.  The _Static_asserts that verify UTAG_FLAG_FROZEN ==
 * 0x01 and UTAG_FLAG_STOPPED == 0x02 live in src/tag/utag_api.c (which includes
 * the internal utag.h header); they cannot live here because urbi.h is a public
 * header that must not include internal src/ headers. */

/* === Gap M — tag lifecycle + query C API (v0.7.1) ===
 *
 * urbi_tag_create: allocate a GC-managed UTag, intern its name, and parent
 *   it under realm->tag so urbi_tag_info reports has_parent = true.
 *   Returns NULL on OOM or if vm/realm is NULL.
 *   On failure returns NULL; the per-VM error ring (queryable via
 *   urbi_last_error) is populated with the failure code.
 *   The returned UTag is GC-managed: it lives until it becomes unreachable
 *   from the GC root set.  The caller is responsible for keeping it reachable
 *   (e.g. store it in an object slot or hold a urbi_ref — Gap Q).
 *   Thread safety: MAIN.
 *
 * urbi_tag_info: populate *out with an observable snapshot of `tag`:
 *   state        — derived from tag->flags (RUNNING/STOPPED/FROZEN).
 *   member_count — number of strands currently scoped to this tag.
 *   has_parent   — true if the tag has a parent (set by urbi_tag_create).
 *   Returns URBI_OK on success, URBI_ERR_INVALID_ARG if tag or out is NULL.
 *   Thread safety: MAIN (reads tag->flags without synchronisation). */
struct UTag *urbi_tag_create(struct UVM *vm, struct URealm *realm,
                             const char *name, size_t name_len);

/* v0.10.3: vm added as first arg (api-ergonomics F6 partial). */
int urbi_tag_info(struct UVM *vm, const struct UTag *tag, urbi_tag_info_t *out);

/* === Gap K — slot read/write from host C (v0.7.1) ===
 *
 * urbi_slot_get: read slot `name[0..name_len)` from receiver `obj`.
 *   Dispatches on obj's kind:
 *     UVAL_OBJECT → walk prototype chain (left-first DFS, cycle-safe).
 *     Atom kinds (INT/FLOAT/STR/BOOL/NIL/VOID) → route through the
 *       per-kind atom proto (v0.6.0 baseline; mirrors OP_GETSLOT).
 *   Returns URBI_OK + *out_value on success.
 *   Returns URBI_ERR_INVALID_ARG if vm, name, or out_value is NULL.
 *   Returns URBI_ERR_SLOT_NOT_FOUND if the name is absent.
 *   Returns URBI_ERR_OOM if name interning fails.
 *
 * urbi_slot_set: write `value` to local slot `name[0..name_len)` on `obj`.
 *   Only UVAL_OBJECT receivers are supported; atoms are immutable.
 *   Respects the CONSTANT flag on locally-owned slots: rejects writes
 *   with URBI_ERR_CONST_SLOT_WRITE.  COW-inherited slots (slot on a
 *   prototype) receive a mutable local copy per spec §6.1.
 *   Returns URBI_OK on success.
 *   Returns URBI_ERR_INVALID_ARG if vm or name is NULL.
 *   Returns URBI_ERR_TYPE if obj is not UVAL_OBJECT (type mismatch).
 *   Returns URBI_ERR_CONST_SLOT_WRITE if the slot is locally const-flagged.
 *   Returns URBI_ERR_OOM on allocation failure.
 *
 * Thread safety: MAIN (not ISR-safe). */
int urbi_slot_get(struct UVM *vm, UValue obj,
                  const char *name, size_t name_len,
                  UValue *out_value);

int urbi_slot_set(struct UVM *vm, UValue obj,
                  const char *name, size_t name_len,
                  UValue value);

/* urbi_make_str_interned: intern s[0..len) and return a UVAL_STR UValue.
 *
 * Two calls with byte-equal inputs always return a UValue whose v.p points
 * to the same canonical address (pointer-equality implies content-equality).
 * Suitable for use as dict keys, slot names, and any comparison-heavy string
 * usage.
 *
 * s need not be NUL-terminated; len bytes are interned.  Passing s == NULL
 * with len == 0 interns the empty string.  Passing s == NULL with len > 0
 * returns urbi_make_nil() (invalid argument).
 *
 * On OOM: returns urbi_make_nil().  Caller must check kind == UVAL_NIL.
 *
 * Thread safety: MAIN. */
UValue urbi_make_str_interned(struct UVM *vm, const char *s, size_t len);

/* === vm-first-arg convention — strand lifecycle (v0.10.3) ===
 *
 * 4 functions previously took realm/strand as their first argument.  They
 * now follow the (struct UVM *vm, ...) convention.
 *
 * urbi_strand_destroy changes void → int (api-ergonomics F8): returns
 *   URBI_OK           — success (or NULL strand no-op).
 *   URBI_ERR_INVALID_STATE — strand is not DORMANT or DEAD (debug only;
 *                            release behaves as unchecked teardown).
 *
 * urbi_strand_state: new function — query strand lifecycle state before
 * destroying.  NULL strand returns URBI_STRAND_DEAD (safe to destroy). */
struct UStrand *urbi_strand_create(struct UVM *vm, struct URealm *realm,
                                   struct UClosure *entry);
void            urbi_strand_start(struct UVM *vm, struct UStrand *s);
struct UStrand *urbi_strand_spawn(struct UVM *vm, struct URealm *realm,
                                  struct UClosure *entry);
/* Returns URBI_OK on success.  URBI_ERR_INVALID_STATE in debug builds if the
 * strand is not in DORMANT or DEAD state (call urbi_strand_state first). */
int             urbi_strand_destroy(struct UVM *vm, struct UStrand *s);
/* Query the current lifecycle state of a strand.  NULL strand returns DEAD. */
UStrandState    urbi_strand_state(struct UVM *vm, const struct UStrand *s);

/* === Row 9 ISR-safe event ring ===
 *
 * urbi_inject_event: single-producer ISR-safe primitive.
 * May be called from interrupt context; no locks, no heap allocation.
 * Returns URBI_OK on success.
 * Returns URBI_ERR_EVENT_PAYLOAD_TOO_LARGE if len > URBI_EVENT_PAYLOAD_MAX.
 * Returns URBI_ERR_EVENT_RING_FULL if the ring is full.
 *
 * The VM drains injected events at the start of each urbi_step() call.
 * Single-producer / single-consumer: one ISR writer + one thread reader.
 *
 * Payload alignment: 8 bytes.  Embedders pushing typed
 * payloads (uint64_t, double, struct fields) from ISR contexts may rely
 * on natural alignment for atomic-load semantics on aligned-only
 * architectures.  The underlying ring entry's payload field is
 * _Alignas(8); copy-in via the `payload` argument preserves this
 * alignment in the stored entry. */
int urbi_inject_event(struct UVM *vm, uint32_t event_id,
                      const void *payload, size_t len);

/* === ISR ring drain handler ===
 *
 * urbi_register_event_drain: install a drain callback invoked at each
 * safepoint (urbi_step entry) for every entry in the ISR event ring.
 *
 * Handler signature:
 *   void handler(UVM *vm, uint32_t event_id, UValue payload);
 *
 *   event_id — the ID passed to urbi_inject_event.
 *   payload  — NIL at baseline (raw-bytes ring does not carry UValues;
 *              host implements event_id → UEvent* mapping in the handler).
 *
 * The handler runs in main-thread context (at safepoint, not in ISR).
 * Typical usage: map event_id → UEvent* and call c_event_emit_async(vm, e, p).
 * Host owns the event_id namespace (spec §9.3).
 *
 * Pass NULL to remove a previously registered handler.
 * Not ISR-safe (must be called from the same thread that drives urbi_step). */
/* v0.10.3: urbi_event_drain_handler gains a void *ud parameter
 * (api-ergonomics F7) so the drain callback can carry per-VM state. */
typedef void (*urbi_event_drain_handler)(struct UVM *vm,
                                         void *ud,
                                         uint32_t event_id,
                                         UValue payload);
/* urbi_register_event_drain: install the ISR-ring drain callback.
 * h is the handler (NULL to remove).  ud is forwarded to every call.
 * Not ISR-safe (must be called from the same thread that drives urbi_step). */
URBI_ADVANCED void urbi_register_event_drain(struct UVM *vm,
                                             urbi_event_drain_handler h,
                                             void *ud);

/* === Gap B — Named-event payload destructure fn (v0.7.1) ===
 *
 * urbi_event_payload_destructure_fn: convert raw ISR payload bytes into
 * UValues for `at(name ?(args))` watcher body.
 *
 * Runs on MAIN thread at safepoint drain; may allocate; may call urbi_make_*.
 * Returns argc (>= 0) on success, -1 on error (drain logs the failure and
 * drops the event body arguments for this occurrence).
 *
 * out_args   — caller-allocated array of UValues, capacity max_args.
 * max_args   — maximum number of UValue arguments to write.
 * payload    — raw ISR-injected bytes (may be NULL when len == 0).
 * payload_len — byte count of payload buffer.
 * ud         — user-data pointer registered with urbi_event_register.
 *
 * Thread safety: MAIN. */
typedef int (*urbi_event_payload_destructure_fn)(
    struct UVM *vm,
    const urbi_event_payload_t *payload, size_t payload_len,
    UValue *out_args, int max_args, void *ud);

/* urbi_event_register: allocate a UEvent, install it as a const realm-global
 * under `name`, and record the (id, event, destruct_fn) triple in the VM's
 * event registry.
 *
 * Subsequent urbi_inject_event with the returned id routes through this UEvent
 * at drain time.  destruct_fn may be NULL (no-args event); when non-NULL it
 * runs on MAIN thread at drain to convert raw ISR payload bytes into UValues
 * for the `at(name ?(args))` body.
 *
 * Returns URBI_EVENT_ID_INVALID on error; consult urbi_last_error (Phase 8)
 * for the specific code:
 *   URBI_ERR_INVALID_ARG      — NULL vm, realm, or name
 *   URBI_ERR_EVENT_NAME_TAKEN — name already registered in this VM
 *   URBI_ERR_OOM              — UEvent alloc or registry grow failed
 *
 * Thread safety: MAIN. */
urbi_event_id_t urbi_event_register(struct UVM *vm, struct URealm *realm,
                                    const char *name,
                                    urbi_event_payload_destructure_fn destruct_fn,
                                    void *destruct_ud);

/* urbi_event_unregister: reverse a previous urbi_event_register call.
 *
 * Fires a final sentinel async-emit (NIL payload) through the UEvent so any
 * `at(name)` watchers still bound to it execute one last body invocation.
 * Unbinds the realm-global slot (sets it to nil) for the event's name.
 * Tombstones the registry entry so id is no longer routed at drain time.
 * The UEvent cell itself remains live until the next GC sweep (GC-managed).
 *
 * Returns:
 *   URBI_OK                — success
 *   URBI_ERR_INVALID_ARG   — vm NULL or id not found in registry
 *   URBI_ERR_HEAP_LOCKED   — heap-locked VM (post-urbi_lock_heap)
 *
 * Thread safety: MAIN. */
int urbi_event_unregister(struct UVM *vm, struct URealm *realm,
                          urbi_event_id_t id);

/* === Gap E — Pluggable I/O writer (v0.7.1) ===
 *
 * urbi_writer_fn: callback invoked by urbi_vm_write for every channel write.
 *   ud         — user-data pointer registered with urbi_set_writer.
 *   channel    — NUL-terminated channel name (e.g., "cout", "cerr", "clog").
 *   channel_len — length of channel name in bytes (not including NUL).
 *   msg        — message bytes (not NUL-terminated; may be empty).
 *   msg_len    — length of message in bytes.
 *   ts_us      — timestamp in monotonic microseconds from vm->host_time_us,
 *                or 0 if the time hook is not installed.
 *
 * Default writer (hosted builds): "cout"/"clog" → stdout (with newline),
 *   "cerr" → stderr (with newline), all other channels silently discarded.
 *   Freestanding builds default to a silent sink; embedders MUST install a
 *   writer or all output goes nowhere.
 *
 * Pass NULL writer to urbi_set_writer to restore the default.
 *
 * Thread safety: MAIN. */
#ifndef URBI_WRITER_FN_TYPEDEF_DEFINED
#define URBI_WRITER_FN_TYPEDEF_DEFINED
typedef void (*urbi_writer_fn)(void *ud,
                               const char *channel, size_t channel_len,
                               const char *msg,     size_t msg_len,
                               uint64_t ts_us);
#endif

void urbi_set_writer(struct UVM *vm, urbi_writer_fn writer, void *ud);

/* === Per-realm writer (v0.9.1) ===
 *
 * Each URealm may install its own writer.  The runtime dispatch
 * (urbi_vm_write_in_realm) consults realm->writer_fn first; if NULL,
 * it falls back to vm->writer_fn (the VM-wide writer installed via
 * urbi_set_writer).
 *
 * The REPL service uses this to route per-session output back to the
 * originating client.  Same callback signature as the VM-wide writer.
 *
 * Pass fn=NULL to clear (revert to fallback chain).
 * NULL vm or NULL realm is a no-op.
 * Thread safety: MAIN. */
void urbi_realm_set_writer(struct UVM *vm, struct URealm *realm,
                           urbi_writer_fn fn, void *ud);

/* === Per-realm compile-budget guard (v0.9.1) ===
 *
 * Install (or clear) the compile-budget that the parser will honour for
 * any source compiled under `realm` via urbi_repl_eval / urbi_compile_source.
 *
 * urbi_realm_set_compile_budget(realm, &budget) — apply a copy of *budget
 *   (zero in any field = unlimited for that limit).
 * urbi_realm_set_compile_budget(realm, NULL)    — clear (unlimited).
 *
 * urbi_realm_get_compile_budget returns NULL if no budget is set, else a
 * pointer to the realm's stored budget (valid until the realm is destroyed
 * or the budget is cleared).  Read-only — callers must NOT mutate.
 *
 * urbi_realm_create_repl auto-applies URBI_DEFAULT_REPL_BUDGET; the global
 * realm (urbi_realm_global) has no budget by default (trusted host code).
 *
 * Thread safety: MAIN. */
/* v0.10.3: vm added as first arg (api-ergonomics F6 partial). */
void urbi_realm_set_compile_budget(struct UVM *vm, struct URealm *realm,
                                   const UCompileBudget *budget);
const UCompileBudget *urbi_realm_get_compile_budget(struct UVM *vm,
                                                    const struct URealm *realm);

/* Default budget applied by urbi_realm_create_repl.  Defined in urealm.c;
 * exported so embedders + tests can read the canonical values
 * (256 / 100000 / 1 MiB). */
extern const UCompileBudget URBI_DEFAULT_REPL_BUDGET;

/* === Runtime diagnostic channel (v0.7.3 / S41) ===
 *
 * urbi_diag_fn: callback invoked by the runtime itself for internal
 *   diagnostic events — body throw, watcher-spawn OOM, ambient-attach
 *   overflow, callback watchdog warnings, etc.  Distinct from
 *   urbi_writer_fn above (the script-side I/O sink for `print` /
 *   `Stream.write` and any embedder-registered `log` host fn): this
 *   channel is the runtime reporting ON ITSELF.  Naming carries the
 *   `diag` prefix specifically to avoid confusion with the script-side
 *   `log` host fns embedders typically register via urbi_register.
 *
 *   vm    — the originating VM.
 *   level — URBI_LOG_DEBUG / INFO / WARN / ERROR (see ULogLevel below;
 *           levels are shared with any future script-side log channel
 *           since the levels are universal, only the SINK differs).
 *   fmt   — printf-style format string.  Today's callers pass fixed
 *           strings only, but the variadic surface reserves room for
 *           future formatted reports; embedders should be ready to
 *           vsnprintf this in their shim.
 *
 * Default is NULL — runtime diagnostics are silently dropped unless
 * the embedder installs a callback.  Pass NULL to urbi_set_diag_fn to
 * uninstall.  An aux helper `urbi_aux_diag_to_stderr` is available in
 * <urbi/aux.h> for hosted builds with no platform log system.
 *
 * Thread safety: MAIN.  The runtime never invokes this from ISR context
 * — ring-deposited events surface via the drain on the main thread,
 * where this callback fires.
 *
 * Naming: the `_fn` suffix matches the v0.7.1 setter pattern for
 * verb-/concept-callbacks (urbi_set_wake_fn, urbi_set_isr_check_fn).
 * Lua precedent: lua_setwarnf (Lua 5.4).  SQLite precedent:
 * sqlite3_config(SQLITE_CONFIG_LOG, ...). */
/* v0.10.3: urbi_diag_fn gains a void *ud parameter (api-ergonomics F7).
 * The ud is passed through by urbi_set_diag_fn and forwarded on every callback
 * invocation.  Embedders that want per-VM log state no longer need globals. */
typedef void (*urbi_diag_fn)(struct UVM *vm, void *ud, int level,
                             const char *fmt, ...);

/* urbi_set_diag_fn: install the runtime diagnostic channel callback.
 * fn is the callback (NULL to uninstall).  ud is forwarded to every call.
 * NULL vm is a no-op. */
void urbi_set_diag_fn(struct UVM *vm, urbi_diag_fn fn, void *ud);

/* urbi_vm_write: write `msg[0..msg_len)` to `channel[0..channel_len)` on `vm`.
 *
 * Routes through the installed urbi_writer_fn (or the default writer if none
 * has been set).  `ts_us` is set to vm->host_time_us() when available.
 * NULL vm is a no-op.  Embedders call this to emit host-generated output
 * through the same channel as urbiscript's cout / cerr.
 *
 * Thread safety: MAIN. */
/* === urbi_vm_write_in_realm (v0.9.1) ===
 *
 * Emit msg to channel through the writer chain, consulting `realm`'s
 * per-realm writer first.  If realm is non-NULL and realm->writer_fn is
 * set, that writer receives the call; otherwise falls back to the VM-wide
 * writer (vm->writer_fn) or the default writer.  Passing realm == NULL is
 * equivalent to calling urbi_vm_write (VM-wide writer / default only).
 *
 * Both ts_us and writer/ud resolution are identical to urbi_vm_write apart
 * from the realm-first lookup.
 *
 * NULL vm is a no-op.  Thread safety: MAIN. */
void urbi_vm_write_in_realm(struct UVM *vm, struct URealm *realm,
                            const char *channel, size_t channel_len,
                            const char *msg,     size_t msg_len);

void urbi_vm_write(struct UVM *vm,
                   const char *channel, size_t channel_len,
                   const char *msg,     size_t msg_len);

/* === Gap F — Pluggable time source (v0.7.1) ===
 *
 * urbi_time_us_fn: callback returning monotonic microseconds.  urbi uses
 *   this for every/sleep precision; 1 kHz control loops need µs granularity.
 *
 * Default: clock_gettime(CLOCK_MONOTONIC) on hosted builds.
 *   Freestanding: default returns 0 (sleep/every are effectively disabled).
 *
 * Pass NULL to urbi_set_clock_fn to restore the default.
 *
 * Thread safety: MAIN. */
/* v0.10.3: urbi_time_us_fn gains a void *ud parameter (api-ergonomics F7).
 * Embedders that maintain per-VM time state no longer need globals. */
typedef uint64_t (*urbi_time_us_fn)(void *ud);

/* urbi_set_clock_fn: install the monotonic time source.
 * fn is the callback (NULL restores the default).  ud is forwarded to every call.
 * NULL vm is a no-op. */
void urbi_set_clock_fn(struct UVM *vm, urbi_time_us_fn fn, void *ud);

/* === Gap S — Wake notification hook (v0.7.1) ===
 *
 * urbi_wake_fn: callback fired after each successful urbi_inject_event ring
 *   deposit.  May run from ISR context.  The callback MUST be O(1),
 *   non-blocking, and MUST NOT allocate memory.  Typical use: post a
 *   FreeRTOS task notification (xTaskNotifyGiveFromISR) or POSIX sem_post so
 *   the urbi task wakes from its blocking wait.
 *
 *   ud — user-data pointer registered with urbi_set_wake_fn.
 *
 * Default: NULL (no wake signal — embedder polls urbi_step directly).
 * Pass NULL fn to urbi_set_wake_fn to restore the default (silent).
 *
 * Thread safety: ISR or MAIN. */
typedef void (*urbi_wake_fn)(void *ud);

void urbi_set_wake_fn(struct UVM *vm, urbi_wake_fn fn, void *ud);

/* === Gap R — atomic event sections (v0.7.1) ===
 *
 * urbi_atomic_begin / urbi_atomic_end: bracket a group of ISR-deposited
 * events that must be observed together.  While atomic_active is true,
 * uevent_ring_drain is a no-op; all ring entries stay queued until
 * urbi_atomic_end clears the flag and triggers a drain pass.
 *
 * Typical use (IMU pattern — accelerometer + gyroscope simultaneously):
 *   urbi_atomic_begin(vm);
 *   urbi_inject_event(vm, ACCEL_ID);
 *   urbi_inject_event(vm, GYRO_ID);
 *   urbi_atomic_end(vm);
 *   // Both watcher bodies see the same tick; no partial observation.
 *
 * Nesting: NOT supported.  In URBI_DEBUG builds, calling urbi_atomic_begin
 * while already active triggers urbi_panic ("atomic section nested").
 * Release builds have undefined behaviour for double-begin.
 *
 * Watchdog: in URBI_DEBUG builds, urbi_step checks whether the section
 * has been held for more than URBI_ATOMIC_MAX_US microseconds and calls
 * urbi_panic if so.  Requires vm->host_time_us to be installed.
 *
 * Thread safety: MAIN (urbi_atomic_begin and urbi_atomic_end must be
 * called from the same thread that drives urbi_step). */
#ifndef URBI_ATOMIC_MAX_US
#  define URBI_ATOMIC_MAX_US 100U
#endif

void urbi_atomic_begin(struct UVM *vm);
void urbi_atomic_end(struct UVM *vm);

/* === Reactive: watcher-body-completion callback ===
 *
 * urbi_watcher_handle_t is an opaque int — non-zero identifies a live
 * host-installed watcher (urbi_register_watcher); zero (URBI_WATCHER_HANDLE_INVALID)
 * is used by script-side watcher body-completion notifications.  Embedders
 * can observe watcher body completion for telemetry, profiling, or
 * hot-reload diagnostics.
 *
 * Callback is invoked from urbi_watcher_body_completed after internal
 * cleanup (back-pointers cleared) and before any re-spawn triggered by
 * the pending_refire_count queue.  For script-side watchers the handle is
 * always 0 (URBI_WATCHER_HANDLE_INVALID); for host-side watchers (Gap J)
 * it matches the handle returned by urbi_register_watcher.
 * completion_status mirrors the strand's fatal_status (UEXEC_OK / THROW /
 * TAG_STOP / CANCEL) cast to int for script-side; for host-side it is
 * URBI_CB_OK (0) or URBI_CB_UNREGISTER (1) / URBI_ERR_WATCHER_UNREGISTER.
 *
 * Default is NULL after urbi_vm_init; pass NULL to uninstall. */
typedef int urbi_watcher_handle_t;

/* URBI_WATCHER_HANDLE_INVALID: sentinel — not a live handle.
 * Returned when urbi_register_watcher fails; used as the handle value
 * for script-side watcher-body-done notifications. */
#define URBI_WATCHER_HANDLE_INVALID  ((urbi_watcher_handle_t)0)

/* v0.10.3: urbi_watcher_body_done_fn gains a void *ud parameter
 * (api-ergonomics F7 / reactive-runtime F7). */
typedef void (*urbi_watcher_body_done_fn)(struct UVM *vm,
                                          void *ud,
                                          urbi_watcher_handle_t handle,
                                          int completion_status);

/* urbi_set_watcher_body_done_fn: install the watcher-body-completion hook.
 * fn is the callback (NULL to uninstall).  ud is forwarded to every call.
 * NULL vm is a no-op. */
void urbi_set_watcher_body_done_fn(struct UVM *vm,
                                   urbi_watcher_body_done_fn fn, void *ud);

/* === Gap J — host-side reactive watchers (v0.7.1) ===
 *
 * urbi_register_watcher installs a C callback that fires at safepoint drain
 * whenever a named event is dispatched.  Coexists with script-side
 * `at(name?)` watchers — both fire on the same dispatch.
 *
 * urbi_watcher_fn: host watcher callback signature.
 *   vm       — VM that owns the event.
 *   event_id — the event that fired.
 *   args     — destructured UValue arguments; argc is their count.
 *              NULL when argc == 0.
 *   argc     — number of valid entries in args[].
 *   ud       — user-data pointer registered with urbi_register_watcher.
 *
 * Return value contract (v0.10.3 unified convention):
 *   URBI_CB_OK         (0) — remain registered; fire again on next event.
 *   URBI_CB_UNREGISTER (1) — auto-unregister after this firing.
 *   URBI_ERR_WATCHER_UNREGISTER   — legacy alias for URBI_CB_UNREGISTER.
 *   negative URBI_ERR_*           — host-side error (treated as fatal at v1.0).
 *
 * Thread safety: MAIN — invoked from the safepoint drain on the main thread. */
typedef int (*urbi_watcher_fn)(struct UVM *vm,
                               urbi_event_id_t event_id,
                               const UValue *args, int argc,
                               void *ud);

/* urbi_register_watcher: install a host-side reactive watcher for event_id.
 *
 * event_id must be a valid id returned by urbi_event_register for this vm.
 * cb must be non-NULL.  ud is forwarded to each cb invocation.
 *
 * Returns a non-zero handle on success; URBI_WATCHER_HANDLE_INVALID on
 * failure (NULL vm, NULL cb, unknown event_id, or OOM growing the table).
 *
 * Multiple watchers may be registered for the same event_id; each fires
 * independently in registration order.
 *
 * Thread safety: MAIN. */
urbi_watcher_handle_t urbi_register_watcher(struct UVM *vm,
                                            struct URealm *realm,
                                            urbi_event_id_t event_id,
                                            urbi_watcher_fn cb, void *ud);

/* urbi_unregister_watcher: deferred-removal of a host-side watcher.
 *
 * In-flight firings at the moment of this call still complete; subsequent
 * firings are suppressed.  Actual removal (compaction of the table slot)
 * occurs at the end of the current drain pass.
 *
 * Returns URBI_OK on success.
 * Returns URBI_ERR_INVALID_ARG if handle == URBI_WATCHER_HANDLE_INVALID
 *   or handle is not found in the watcher table.
 *
 * Thread safety: MAIN. */
int urbi_unregister_watcher(struct UVM *vm, urbi_watcher_handle_t handle);

/* === ISR-safety assertions + URBI_DEBUG callback watchdog ===
 *
 * URBI_LOG_* — log level constants for the urbi_diag_fn callback
 *   (installed via urbi_set_diag_fn).  Named LOG_* because levels are
 *   universal — same values would apply to any future script-side log
 *   channel — even though today they're only consumed by the runtime
 *   diagnostic channel.
 * URBI_WATCHDOG_* — watchdog mode: warn or assert on slow callbacks.
 * UHostFn — typedef for host-callable C functions invoked by OP_CALL.
 * urbi_panic — fatal runtime error; aborts on hosted builds.
 * URBI_ASSERT_NOT_ISR — asserts that the current call is NOT in ISR context.
 * urbi_call_host_with_watchdog — debug-build wrapper for host callbacks.
 * urbi_set_isr_check_fn — register ISR-context predicate.
 * urbi_set_callback_watchdog_mode — set watchdog mode (WARN or ASSERT). */

typedef enum {
    URBI_LOG_DEBUG = 0,
    URBI_LOG_INFO  = 1,
    URBI_LOG_WARN  = 2,
    URBI_LOG_ERROR = 3
} ULogLevel;

/* UWatchdogMode: response to slow host-callback timing in URBI_DEBUG builds.
 * Promoted from #defines to a typedef enum at v0.5.5 to match the
 * sibling ULogLevel idiom; numeric values pinned (0 = WARN, 1 = ASSERT). */
typedef enum {
    URBI_WATCHDOG_WARN   = 0,
    URBI_WATCHDOG_ASSERT = 1
} UWatchdogMode;

/* UHostFn: signature for host-implemented native functions called by OP_CALL.
 * Wired into the call-dispatch path; introduced alongside the watchdog
 * wrapper infrastructure. */
typedef UValue (*UHostFn)(struct UStrand *s, int argc, UValue *argv);

/* URBI_NORETURN: marks functions that never return.
 * Used to suppress compiler "missing return" warnings when a function's only
 * exit path is urbi_panic or similar fatal control flow. */
#if defined(__GNUC__) || defined(__clang__)
#  define URBI_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#  define URBI_NORETURN __declspec(noreturn)
#else
#  define URBI_NORETURN
#endif

/* urbi_panic: fatal runtime error.
 * On hosted builds: prints msg to stderr and calls abort().
 * On freestanding builds: spins forever (no OS, no abort).
 * Declared here; defined in urbi.c. */
URBI_ADVANCED URBI_NORETURN void urbi_panic(const char *msg);

/* URBI_CALLBACK_WARN_US: default watchdog threshold (microseconds).
 * Overridable at compile time: -DURBI_CALLBACK_WARN_US=2000 */
#ifndef URBI_CALLBACK_WARN_US
#  define URBI_CALLBACK_WARN_US 1000U
#endif

/* urbi_in_isr: returns true if currently in ISR context, false otherwise.
 *
 * Reads vm->isr_check_fn (registered via urbi_set_isr_check_fn); returns
 * false if no check function has been registered, or if vm is NULL.
 * URBI_DEBUG-only.
 *
 * Hides the internal isr_check_fn field, allowing URBI_ASSERT_NOT_ISR to
 * be written without requiring a complete struct UVM definition in the
 * embedder's TU.  Closes the structural half of API-018 / GC-012. */
#ifdef URBI_DEBUG
URBI_ADVANCED bool urbi_in_isr(const struct UVM *vm);
#endif

/* URBI_ASSERT_NOT_ISR: in URBI_DEBUG builds, asserts the function is not
 * called from ISR context.  vm must be a pointer to a live UVM.
 *
 * Lives in this public header (rather than src/runtime/umacros.h) because
 * the macro is part of the embedder-facing assertion surface — host C
 * code can sprinkle it across its own bridges if it wants debug-build
 * catches for ISR-unsafe entry points. */
#ifdef URBI_DEBUG
#  define URBI_ASSERT_NOT_ISR(vm) \
       do { if (urbi_in_isr(vm)) \
                urbi_panic("called non-ISR-safe function from ISR context"); \
          } while (0)
#else
#  define URBI_ASSERT_NOT_ISR(vm) ((void)0)
#endif

/* urbi_call_host_with_watchdog: invoke a UHostFn and check elapsed time.
 * In URBI_DEBUG builds: times the call; if elapsed > vm->callback_warn_us,
 *   logs a warning (URBI_WATCHDOG_WARN) or panics (URBI_WATCHDOG_ASSERT).
 * In non-debug builds: collapses to a direct call with no overhead.
 * vm  — the VM owning the call.
 * s   — the strand executing the call (passed as first arg to fn).
 * fn  — the host function to invoke.
 * argc, argv — arguments forwarded to fn. */
#ifdef URBI_DEBUG
/* URBI_DEBUG builds: real function defined in urbi.c (needs full UVM struct). */
URBI_ADVANCED UValue urbi_call_host_with_watchdog(struct UVM *vm, struct UStrand *s,
                                                  UHostFn fn, int argc, UValue *argv);
#else
/* Non-debug builds: zero-overhead macro — collapsed to a bare call. */
#  define urbi_call_host_with_watchdog(vm, s, fn, argc, argv) \
       ((fn)((s), (argc), (argv)))
#endif

/* urbi_set_isr_check_fn: register a predicate that returns true when called
 * from ISR context.  Pass NULL to disable ISR checking (default).
 * v0.10.3: gains a trailing void *ud; the callback receives ud on each
 * invocation so ISR-detection state can be per-VM without globals. */
void urbi_set_isr_check_fn(struct UVM *vm, bool (*fn)(void *ud), void *ud);

/* urbi_set_callback_watchdog_mode: set the watchdog response mode.
 * URBI_WATCHDOG_WARN — log warning via host_log_fn.
 * URBI_WATCHDOG_ASSERT — call urbi_panic on threshold exceeded. */
void urbi_set_callback_watchdog_mode(struct UVM *vm, UWatchdogMode mode);

/* === Module-instance C API ===
 *
 * The root UProto is read-only (flash-resident on freestanding targets).
 * The mutable IC state lives in a per-VM UChunkInstance.  Two instances of
 * the same root UProto (one per VM, or two per VM for redundant chunks) hold
 * independent IC tables — IC fill in one instance does not bleed into the
 * other.
 *
 * urbi_chunk_instance_create allocates the UChunkInstance + its
 * UProtoInstanceArr bulk in two GC cells.  Returns NULL on OOM.
 *
 * urbi_chunk_instance_destroy is a no-op at v1.0 — both cells are
 * GC-managed and reaped by sweep when no roots reach the instance.
 * (AUDIT: OBJ-027 — function body is dead at v1.0; symbol kept for
 * public-API stability.  Future module-instance lifecycle work may give
 * the call host-visible side-effects (e.g. detaching from a host-owned
 * registry); until then, callers should still pair create/destroy so
 * the symbol can grow semantics without source churn.)
 *
 * Thread safety: none at v1.0; same single-threaded constraint as the rest
 * of the v1.0 API. */
#ifndef URBI_MODULE_INSTANCE_TYPEDEF_DEFINED
#define URBI_MODULE_INSTANCE_TYPEDEF_DEFINED
typedef struct UChunkInstance UChunkInstance;
#endif

URBI_ADVANCED UChunkInstance *urbi_chunk_instance_create (struct UVM *vm, struct UProto *root);
URBI_ADVANCED void             urbi_chunk_instance_destroy(struct UVM *vm, UChunkInstance *mi);

/* === Opaque VM allocation API (v0.10.3) ===
 *
 * urbi_vm_create  — allocate + initialise a UVM via the supplied allocator.
 *                   Preferred entry point for new embedders.  Returns NULL on
 *                   alloc failure or when alloc_fn == NULL.
 *
 * urbi_vm_free    — tear down + free a UVM created via urbi_vm_create.
 *                   Safe to call with vm == NULL (no-op).
 *
 * urbi_vm_sizeof  — size in bytes of struct UVM, for embedders who want to
 *                   declare static/BSS storage without including the internal
 *                   src/vm/uvm.h header.
 *
 * urbi_vm_alignof — alignment requirement for struct UVM (always a power of two).
 *
 * For static/BSS allocation (advanced; see urbi_vm_init below):
 *   _Alignas(8) static char vm_storage[131072];  // sized for current build
 *   if (sizeof(vm_storage) < urbi_vm_sizeof()) abort();  // runtime guard
 *   struct UVM *vm = (struct UVM *)vm_storage;
 *   urbi_vm_init(vm, my_alloc, NULL);
 *   ... use vm ...
 *   urbi_vm_destroy(vm);
 *
 * For heap allocation (recommended for new code):
 *   struct UVM *vm = urbi_vm_create(my_alloc, NULL);
 *   ... use vm ...
 *   urbi_vm_free(vm);
 */
struct UVM *urbi_vm_create (UVMAllocFn alloc_fn, void *alloc_ud);
void        urbi_vm_free   (struct UVM *vm);
size_t      urbi_vm_sizeof(void);
size_t      urbi_vm_alignof(void);

/* === API-013: VM lifecycle (promoted to public at v0.5.5) ===
 *
 * Hosts allocate a UVM struct themselves, initialize it with urbi_vm_init
 * (passing a host allocator), drive it via urbi_step / urbi_run_chunk /
 * urbi_repl_eval, and tear it down with urbi_vm_destroy.  urbi_vm_run is
 * a convenience wrapper that runs a module's root chunk to completion.
 *
 * Pre-v0.5.5 these were `uvm_*` and lived in src/vm/uvm.h; tests had to
 * include the internal header to call them.  v0.5.5 promotes the names
 * to `urbi_vm_*` and publishes the supporting types via urbi/types.h.
 * Closes API-013 + API-027.
 *
 * Conservative scope: pure rename.  Signatures, semantics, and error
 * codes are byte-identical to the pre-v0.5.5 internal forms. */
/* v0.7.0 — returns URBI_OK on success, URBI_ERR_OOM if any
 * sub-system allocation (event_ring, deferred_slot_changes, watcher pool,
 * op_overload IC, ...) fails.  Pre-v0.7.0 this returned void; the change
 * is permitted under the pre-v1.0 ABI escape clause documented in
 * <urbi/version.h>.  urbi_vm_destroy remains safe to call regardless of
 * the return value (partial-init state is reaped on the destroy path). */
URBI_ADVANCED int      urbi_vm_init   (struct UVM *vm, UVMAllocFn alloc_fn, void *alloc_ud);
URBI_ADVANCED void     urbi_vm_destroy(struct UVM *vm);
/* urbi_vm_run: run root proto to completion.
 * v0.10.3: return type changed from UVMError to int.
 *   URBI_OK (0) on success; *out receives the final value.
 *   URBI_ERR_OOM if allocation fails during execution.
 *   URBI_ERR_UNCAUGHT_THROW (-18) if the root strand died with an uncaught
 *     script throw; the thrown value is delivered via *out when non-NULL,
 *     and vm->last_errmsg is set (value-formatted for scalars, message slot
 *     for Exception-typed throws).
 *   URBI_ERR_STRAND_FATAL for non-throw fatal halts (type errors, etc.).
 * Callers that stored the result in `UVMError rc` still compile (UVMError
 * is now a typedef for int) but should migrate to plain `int rc`. */
int      urbi_vm_run    (struct UVM *vm, struct URealm *realm,
                         const struct UProto *root, UValue *out);

/* === urbi_lock_heap ===
 *
 * Lock the allocator post-init.  After this call, urbi_gc_alloc declines
 * to allocate new GC-managed cells and returns NULL; the caller observes
 * the failure as an OOM-shaped failure mode (URBI_ERR_OOM at native
 * boundaries, or a TypeError raise via urbi_raise_oom on the script
 * surface).  Existing GC-tracked objects continue to operate; collection
 * still runs (sweep / mark slices do not allocate).
 *
 * Scope (v1.0, GC cells ONLY): the latch gates urbi_gc_alloc.  Non-GC
 * allocations through the embedder's alloc_fn are NOT gated and continue
 * after lock: strand register stacks, container backing buffers
 * (List/Dict/Tuple growth), string-intern table entries and rehash, REPL
 * buffers.  A hard-RT embedder needing a total allocation freeze must
 * also size those pools up front; see docs/embedding-guide.md.
 *
 * Intended use: v2.0 hard-RT mode where post-init allocation is
 * forbidden by policy.  The API surface lands at v1.0; the policy
 * enforcement is opt-in — embedders that want allocation throughout
 * the program lifetime simply never call this.
 *
 * Idempotent: calling on an already-locked VM is a no-op.  No unlock
 * primitive at v1.0 (one-way latch matches the hard-RT contract).
 *
 * vm == NULL is a no-op. */
void urbi_lock_heap(struct UVM *vm);

#ifdef URBI_DEBUG
/* urbi_get_determinism_checksum: FNV-1a hash of observable VM state.
 *
 * Call only at a QUIESCENT point (no strands runnable, no pending events).
 * Returns a stable hash of:
 *   1. all UValue bindings across every live Realm's namespace
 *   2. watcher pool high-water mark
 *   3. gc_total_allocated counter
 *   4. intern table entry count
 *   5. topology_gen, lookup_id, next_object_id (object-model counters)
 *   6. per-IC observable state across every live UChunkInstance:
 *      ic->n, ic->replace_cursor, and ic->topology_gen[0..n) for each
 *      UIC site in each UProtoInstance's IC table.  Heap pointers
 *      (recv_shapes, slots, uprops) are deliberately NOT folded —
 *      they are not stable across process invocations.
 *
 * String values (UVAL_STR) are hashed by their interned pointer, which is
 * stable within a single VM lifetime but NOT guaranteed cross-run-stable
 * (intern pointer addresses depend on allocation order).  The checksum is
 * deterministic for two VMs that process identical input within one process
 * (as used by test_determinism_two_runs).
 *
 * URBI_DEBUG only: zero overhead in non-debug builds (function absent). */
URBI_ADVANCED uint64_t urbi_get_determinism_checksum(struct UVM *vm);
#endif /* URBI_DEBUG */

/* === Gap P — error inspection (v0.7.1) ===
 *
 * urbi_error_info_t: structured error detail for the most-recent API failure.
 *
 *   code        — UErrCode value (negative; URBI_OK == 0 means no error).
 *   message     — human-readable description (may be empty string).
 *   source_name — source file or script name (may be empty string).
 *   source_line — source line number (0 if unknown).
 *   context     — caller-supplied context tag, e.g. "urbi_event_register"
 *                 (may be empty string).
 *
 * Lifetime: all const char* fields point into UVM-owned storage.  They are
 * valid until the next API call that mutates error state (any call that
 * internally invokes urbi_set_error_internal, including itself if it fails).
 * Copy the strings if you need them across subsequent API calls. */
typedef struct {
    int         code;
    const char *message;
    const char *source_name;
    int         source_line;
    const char *context;
} urbi_error_info_t;

/* urbi_last_error: read the most-recent error from the per-VM ring.
 *
 * Populates *out_info with the most-recent error entry and returns its code.
 * If the ring is empty (no error since the last urbi_clear_error, or since
 * VM init), zeroes *out_info and returns URBI_OK (0).
 *
 * vm == NULL or out_info == NULL: returns URBI_OK and zeroes *out_info if
 * out_info is non-NULL.
 *
 * Thread safety: MAIN. */
int  urbi_last_error (struct UVM *vm, urbi_error_info_t *out_info);

/* urbi_clear_error: empty the per-VM error ring.
 *
 * After this call, urbi_last_error returns URBI_OK + zeroed info until
 * the next API call that sets an error.  vm == NULL is a no-op.
 *
 * Thread safety: MAIN. */
void urbi_clear_error(struct UVM *vm);

/* === Gap Q — reference management (v0.7.1) ===
 *
 * urbi_ref_t: opaque GC-root handle.  Encodes a 24-bit slot index and an
 * 8-bit generation counter.  URBI_REF_INVALID (== 0) is the sentinel.
 *
 * The ref table is a per-VM growable array.  Slot 0 is permanently reserved
 * so that URBI_REF_INVALID can never encode a valid slot+generation pair.
 * Valid handles always have (index >> 8) >= 1. */
typedef uint32_t urbi_ref_t;
#define URBI_REF_INVALID ((urbi_ref_t)0)

/* urbi_ref: pin a UValue as a GC root and return an opaque handle.
 *
 * The returned handle keeps the value alive across GC cycles until
 * urbi_unref is called.  Returns URBI_REF_INVALID on OOM (table can't
 * grow) or if vm == NULL.
 *
 * Thread safety: MAIN. */
urbi_ref_t urbi_ref    (struct UVM *vm, UValue value);

/* urbi_ref_get: retrieve the pinned UValue for a live handle.
 *
 * Returns the originally pinned UValue if the handle is valid and live.
 * Returns urbi_make_nil() if the handle is URBI_REF_INVALID, stale
 * (generation mismatch after urbi_unref), or vm == NULL.
 *
 * Thread safety: MAIN. */
UValue     urbi_ref_get(struct UVM *vm, urbi_ref_t ref);

/* urbi_unref: release a pinned handle, freeing the slot for reuse.
 *
 * Increments the slot's generation counter so any copy of the old handle
 * becomes stale.  No-op on URBI_REF_INVALID, stale handles, or vm == NULL.
 * After urbi_unref the value is no longer pinned — the GC may collect it
 * if no other root keeps it alive.
 *
 * Thread safety: MAIN. */
void       urbi_unref  (struct UVM *vm, urbi_ref_t ref);

/* === Public error publishing (v0.7.1 spec amendment) ===
 *
 * urbi_set_error: publish an error entry to the per-VM error ring.
 *
 * Thin public wrapper around the internal urbi_set_error_internal; exposes
 * the entry point needed by the aux layer (urbi_aux_set_error) without
 * violating the aux governance rule that aux functions may not include
 * internal headers.
 *
 * vm          — owning VM; NULL is a no-op.
 * code        — UErrCode value (negative int).
 * message     — human-readable description; NULL → empty string.
 * source_name — source file or script name; NULL → empty string.
 * source_line — source line number; 0 if unknown.
 * context     — caller-supplied context tag; NULL → empty string.
 *
 * Thread safety: MAIN. */
void urbi_set_error(struct UVM *vm, int code,
                    const char *message,
                    const char *source_name, int source_line,
                    const char *context);

/* === Public bytecode deserialization (v0.7.1 spec amendment) ===
 *
 * v0.10.3: both functions gain (struct UVM *vm, ...) as first arg.
 * urbi_chunk_from_bytes now routes allocation through vm->alloc_fn instead
 * of libc malloc, matching the rest of the VM allocator domain and closing
 * the cross-allocator hazard documented in api-ergonomics F3.
 *
 * urbi_chunk_from_bytes: deserialize a wire-format bytecode buffer into a
 * heap-allocated root UProto.  (v0.9.2: was UModule*; UModule deleted.)
 *
 * On success: returns a non-NULL pointer that must be freed with
 * urbi_chunk_free when no longer needed.  The caller must ensure no live VM
 * is executing inside the module when urbi_chunk_free is called.
 *
 * On error: returns NULL; if errmsg is non-NULL, writes a diagnostic into
 * errmsg[0..errcap) (NUL-terminated).
 *
 * Error conditions:
 *   NULL vm or buf                      → returns NULL (URBI_ERR_INVALID_ARG)
 *   bytecode version mismatch           → returns NULL
 *   any other deserialize or OOM error  → returns NULL
 *
 * Thread safety: MAIN (calls the heap allocator). */
struct UProto *urbi_chunk_from_bytes(struct UVM *vm, const uint8_t *buf,
                                     size_t len,
                                     char *errmsg, size_t errcap);

/* urbi_chunk_free: free a root UProto returned by urbi_chunk_from_bytes.
 *
 * Frees via the same allocator domain as urbi_chunk_from_bytes (vm->alloc_fn
 * on hosted builds; no-op on freestanding).  NULL root is a no-op.
 *
 * Thread safety: MAIN. */
void urbi_chunk_free(struct UVM *vm, struct UProto *root);

#ifdef __cplusplus
}
#endif

/* === REPL service (v0.9.1) ===
 *
 * Opt-in surface — only included when URBI_ENABLE_REPL is defined at
 * configuration time.  The REPL service requires the compiler frontend
 * (it accepts source text over the wire), so it is mutually exclusive
 * with URBI_BYTECODE_ONLY (enforced both at build time in the Makefile
 * and at #include time inside <urbi/repl.h>). */
#if defined(URBI_ENABLE_REPL) && !defined(URBI_BYTECODE_ONLY)
#  include <urbi/repl.h>
#endif

/* === URBI_BYTECODE_ONLY link-time guard (audit-1 F2, roadmap F7) ===
 *
 * Bytecode-only builds strip src/lex/, src/parse/, src/emit/ from the
 * archive.  An embedder that builds its application TUs without
 * URBI_BYTECODE_ONLY=1 but links against a bytecode-only liburbi.a
 * will see an undefined-symbol link error naming the mismatch:
 *   urbi_abi_requires_full_parser (undefined)
 * rather than late runtime failures from missing parser symbols.
 *
 * Suppressed when URBI_INTERNAL_GUARD_REF=1 (uabi_guards.c defines
 * the symbols and must not also reference them). */
#ifndef URBI_INTERNAL_GUARD_REF
#  ifdef URBI_BYTECODE_ONLY
extern const int urbi_abi_requires_bytecode_only;
static const int *urbi_abi_bytecode_guard_ref __attribute__((unused)) =
    &urbi_abi_requires_bytecode_only;
#  else
extern const int urbi_abi_requires_full_parser;
static const int *urbi_abi_full_parser_guard_ref __attribute__((unused)) =
    &urbi_abi_requires_full_parser;
#  endif
#endif /* !URBI_INTERNAL_GUARD_REF */


#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif
#endif
