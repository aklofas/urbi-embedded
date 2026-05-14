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

#include <stdbool.h>
#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint64_t */

/* UValue, UExecStatus, UErrCode, UVMError, UVMAllocFn, opaque struct
 * fwd-decls (UVM, UStrand, UTag, URealm, UModule, UClosure).  Replaces
 * the pre-v0.5.5 `#include "sched/ustrand.h"` that pulled an internal
 * header into the public surface; closes API-012 / INC-003 structurally. */
#include "urbi/types.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *urbi_version(void);

/* === Row 7 control-transfer C API (M3 / T12) ===
 *
 * These functions allow host C code to inject unwind events into strands
 * and inspect their state.  They operate on struct UStrand / struct UTag /
 * struct UVM — forward-declared in <urbi/types.h>; definitions live in
 * sched/ustrand.h and vm/uvm.h.
 *
 * Thread safety: none at M3 — these are not ISR-safe.  The ISR-safe event
 * ring (urbi_inject_event) is added at T18. */

/* Cross-strand: deposit TAG_STOP on `tag`'s member strands.
 * Synchronous deposit + queue, runs zero bytecode on the caller.
 *
 * WARNING (T28 / FOUND-013): not ISR-safe.  Walks tag->member_strands_head
 * and is reentrant via host callbacks invoked by waker logic (e.g.
 * sched_strand_unblock through urbi_event_drain_handler).  Embedders
 * calling from ISR contexts must use the urbi_inject_event ring instead
 * and let the safepoint drain (run on the consumer thread) translate the
 * event id into a tag stop.  The urbi_in_isr(vm) check fires as
 * URBI_ASSERT_NOT_ISR in debug builds when called from an ISR context
 * detected by the caller-installed ISR check function (see
 * urbi_set_isr_check_fn / urbi_in_isr below).
 *
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
 * Full struct definition is in realm/urealm.h; include it for direct field access.
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
 * The global Realm has REALM_GLOBAL set and persists until urbi_vm_destroy().
 * Returns NULL on OOM. */
struct URealm *urbi_realm_global(struct UVM *vm);

/* Liveness query: reads VM-wide runnable / active-watcher / pending-wakeup
 * counters.  Populates out_strands / out_watchers / out_wakes (any may be
 * NULL).  Returns true if any counter is positive.
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

/* === M5 realm globals C API (spec #5 §7) ===
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

/* === Row 8 step driver + chunk-execution C API (M3 / T16) ===
 *
 * urbi_step: drive the VM for up to budget_instructions opcodes, returning
 * a 4-state result describing what the caller should do next.
 *
 * urbi_run_chunk: run a module's root chunk under the given Realm.  realm == NULL
 * uses the VM's global Realm (auto-created on first call).
 *
 * urbi_repl_eval: compile a source line and run it; format the result into
 * out_buf.  Suitable for a read-eval-print loop.
 *
 * urbi_run_script: thin wrapper around urbi_run_chunk that discards the result.
 *
 * urbi_load_module: bind a pre-compiled module into the VM and run its root
 * chunk under the global Realm so top-level bindings install into realm
 * globals.  module_name is currently advisory (no import-table lookup yet —
 * v1.x backlog).  Returns URBI_OK on success, URBI_ERR_INVALID_ARG if any
 * argument is NULL, URBI_ERR_OOM on UModuleInstance allocation failure, or
 * a UVMError-derived code if root-chunk execution fails. */

struct UModule;       /* forward decl — definition in umodule.h */

typedef enum {
    URBI_STEP_RUNNING   = 0,  /* budget exhausted or yield; call again */
    URBI_STEP_QUIESCENT = 1,  /* no live work; host may sleep or exit */
    URBI_STEP_FATAL     = 2,  /* a strand entered fatal state; inspect via urbi_strand_is_fatal */
    URBI_STEP_WAKE_AT   = 3   /* no runnable strand now; *out_next_wake_us set */
} UStepResult;

UStepResult urbi_step(struct UVM *vm,
                      uint64_t budget_instructions,
                      uint64_t *out_next_wake_us);

int urbi_run_chunk(struct UVM *vm, struct URealm *realm,
                   const struct UModule *module, UValue *out_result);

/* Compile-error gated when URBI_BYTECODE_ONLY=1 (M7 Wave 1 T16): the
 * compiler frontend (src/lex, src/parse, src/emit) is not linked in
 * bytecode-only builds, so urbi_repl_eval cannot function — embedders
 * trying to call it get a compile error at the call site rather than
 * an unresolved link symbol. */
#if !defined(URBI_BYTECODE_ONLY)
int urbi_repl_eval(struct UVM *vm, struct URealm *realm,
                   const char *line, size_t line_len,
                   char *out_buf, size_t out_buf_size);
#endif

int urbi_run_script(struct UVM *vm, struct URealm *realm, const struct UModule *module);

int urbi_load_module(struct UVM *vm, struct UModule *module, const char *module_name);

/* urbi_load_translate_load_err: map an internal UModuleLoadError (passed
 * as int) to the corresponding public UErrCode.  Currently routes
 * ULOAD_UNSUPPORTED_VERSION → URBI_ERR_BYTECODE_VERSION_MISMATCH and
 * collapses every other internal code to URBI_ERR_INVALID_ARG.  Closes
 * API-005: URBI_ERR_BYTECODE_VERSION_MISMATCH is now reachable from a
 * public-API call site, even though the deserialize-bytes entry point
 * itself remains M6 work in progress. */
int urbi_load_translate_load_err(int load_err);

/* === Phase 10 stdlib bake (M6 Wave 2) ===
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
/* Compile-error gated when URBI_BYTECODE_ONLY=1 (M7 Wave 1 T16): the
 * compiler frontend (src/lex, src/parse, src/emit) is not linked in
 * bytecode-only builds.  Pre-baked bytecode is loaded through
 * urbi_load_module / urbi_run_chunk instead. */
#if !defined(URBI_BYTECODE_ONLY)
int urbi_compile_source(struct UVM *vm,
                        const char *src, size_t src_len,
                        const char *src_name,
                        unsigned char **out_buf, size_t *out_len,
                        char *err_buf, size_t err_cap);
#endif

/* === Row 9 strand lifecycle C API (M3 / T20) ===
 *
 * Separate _create (DORMANT alloc) from _start (DORMANT → READY enqueue) so
 * callers can pre-attach tags (T29), set scheduler attrs (v1.x), or pool/recycle
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
 * Thread safety: none at M3 — not ISR-safe.  Must be called from the same
 * thread that drives the VM.
 *
 * urbi_strand_cancel / urbi_strand_panic / urbi_strand_reset are declared
 * in the row 7 control-transfer section above (T12). */

struct UClosure;   /* forward decl — definition in umodule.h */

/* urbi_native_method_fn: signature for host C functions that back a
 * UClosure slot.  Called by OP_CALL when the closure's native_fn field is
 * set (M6 Phase 3 dispatch arm, v0.6.0+).
 *
 * Parameters:
 *   vm    — the VM executing the call.
 *   self  — receiver value (the object the slot was loaded from).
 *   args  — argument array (NULL when nargs == 0).
 *   nargs — argument count.
 *   out   — write the return value here; initialised to NIL before the call.
 *
 * Return UEXEC_OK (0) on success, UEXEC_THROW (1) to signal an exception
 * (see urbi_raise_* helpers in <urbi/urbi.h>).
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
 *   URBI_TAG_FROZEN  — UTAG_FLAG_FROZEN  (0x01) set (reserved; M7+ stdlib).
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

int urbi_tag_info(const struct UTag *tag, urbi_tag_info_t *out);

/* === Gap K — slot read/write from host C (v0.7.1) ===
 *
 * urbi_slot_get: read slot `name[0..name_len)` from receiver `obj`.
 *   Dispatches on obj's kind:
 *     UVAL_OBJECT → walk prototype chain (left-first DFS, cycle-safe).
 *     Atom kinds (INT/FLOAT/STR/BOOL/NIL/VOID) → route through the
 *       per-kind atom proto (M6 Wave 1 baseline; mirrors OP_GETSLOT).
 *   Returns URBI_OK + *out_value on success.
 *   Returns URBI_ERR_INVALID_ARG if vm, name, or out_value is NULL.
 *   Returns URBI_ERR_SLOT_NOT_FOUND if the name is absent.
 *   Returns URBI_ERR_OOM if name interning fails.
 *
 * urbi_slot_set: write `value` to local slot `name[0..name_len)` on `obj`.
 *   Only UVAL_OBJECT receivers are supported; atoms are immutable.
 *   Respects the CONSTANT flag on locally-owned slots: rejects writes
 *   with URBI_ERR_CONST_SLOT_WRITE.  COW-inherited slots (slot on a
 *   prototype) receive a mutable local copy per pre-M2 §6.1.
 *   Returns URBI_OK on success.
 *   Returns URBI_ERR_INVALID_ARG if vm or name is NULL, or obj is not UVAL_OBJECT.
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

struct UStrand *urbi_strand_create(struct URealm *realm, struct UClosure *entry);
void            urbi_strand_start(struct UStrand *s);
struct UStrand *urbi_strand_spawn(struct URealm *realm, struct UClosure *entry);
void            urbi_strand_destroy(struct UStrand *s);

/* === Row 9 ISR-safe event ring (M3 / T18) ===
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
 * Payload alignment: 8 bytes (T25 / EVENT-003).  Embedders pushing typed
 * payloads (uint64_t, double, struct fields) from ISR contexts may rely
 * on natural alignment for atomic-load semantics on aligned-only
 * architectures.  The underlying ring entry's payload field is
 * _Alignas(8); copy-in via the `payload` argument preserves this
 * alignment in the stored entry. */
int urbi_inject_event(struct UVM *vm, uint32_t event_id,
                      const void *payload, size_t len);

/* === T57 ISR ring drain handler (M5 / spec #3 §9) ===
 *
 * urbi_register_event_drain: install a drain callback invoked at each
 * safepoint (urbi_step entry) for every entry in the ISR event ring.
 *
 * Handler signature:
 *   void handler(UVM *vm, uint32_t event_id, UValue payload);
 *
 *   event_id — the ID passed to urbi_inject_event.
 *   payload  — NIL at M5 baseline (raw-bytes ring does not carry UValues;
 *              host implements event_id → UEvent* mapping in the handler).
 *
 * The handler runs in main-thread context (at safepoint, not in ISR).
 * Typical usage: map event_id → UEvent* and call c_event_emit_async(vm, e, p).
 * Host owns the event_id namespace (spec §9.3).
 *
 * Pass NULL to remove a previously registered handler.
 * Not ISR-safe (must be called from the same thread that drives urbi_step). */
typedef void (*urbi_event_drain_handler)(struct UVM *vm,
                                         uint32_t event_id,
                                         UValue payload);
void urbi_register_event_drain(struct UVM *vm, urbi_event_drain_handler h);

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
typedef void (*urbi_writer_fn)(void *ud,
                               const char *channel, size_t channel_len,
                               const char *msg,     size_t msg_len,
                               uint64_t ts_us);

void urbi_set_writer(struct UVM *vm, urbi_writer_fn writer, void *ud);

/* urbi_vm_write: write `msg[0..msg_len)` to `channel[0..channel_len)` on `vm`.
 *
 * Routes through the installed urbi_writer_fn (or the default writer if none
 * has been set).  `ts_us` is set to vm->host_time_us() when available.
 * NULL vm is a no-op.  Embedders call this to emit host-generated output
 * through the same channel as urbiscript's cout / cerr.
 *
 * Thread safety: MAIN. */
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
 * Pass NULL to urbi_set_time_us to restore the default.
 *
 * Thread safety: MAIN. */
typedef uint64_t (*urbi_time_us_fn)(void);

void urbi_set_time_us(struct UVM *vm, urbi_time_us_fn fn);

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

/* === Reactive: watcher-body-completion callback (Wave 1 T33) ===
 *
 * urbi_watcher_handle_t is an opaque int — Wave 2 (ESP-IDF port) defines
 * the real watcher-identity story; Wave 1 provides the seam.  Embedders
 * can observe watcher body completion for telemetry, profiling, or
 * hot-reload diagnostics.
 *
 * Callback is invoked from urbi_watcher_body_completed after internal
 * cleanup (back-pointers cleared) and before any re-spawn triggered by
 * URBI_WATCHER_PENDING_REFIRE.  At Wave 1 the handle is a placeholder
 * (always 0); the completion_status mirrors the strand's fatal_status
 * (UEXEC_OK / THROW / TAG_STOP / CANCEL) cast to int.
 *
 * Default is NULL after urbi_vm_init; pass NULL to uninstall. */
typedef int urbi_watcher_handle_t;

typedef void (*urbi_watcher_body_done_fn)(struct UVM *vm,
                                          urbi_watcher_handle_t handle,
                                          int completion_status);

void urbi_set_watcher_body_done_fn(struct UVM *vm,
                                   urbi_watcher_body_done_fn fn);

/* === T19: ISR-safety assertions + URBI_DEBUG callback watchdog ===
 *
 * URBI_LOG_* — log level constants for host_log_fn callback.
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
 * Promoted from #defines to a typedef enum at v0.5.5 (T10) to match the
 * sibling ULogLevel idiom; numeric values pinned (0 = WARN, 1 = ASSERT). */
typedef enum {
    URBI_WATCHDOG_WARN   = 0,
    URBI_WATCHDOG_ASSERT = 1
} UWatchdogMode;

/* UHostFn: signature for host-implemented native functions called by OP_CALL.
 * M5 wires this into the call-dispatch path; M3 defines the typedef for the
 * watchdog wrapper infrastructure introduced here at T19. */
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
URBI_NORETURN void urbi_panic(const char *msg);

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
bool urbi_in_isr(const struct UVM *vm);
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
UValue urbi_call_host_with_watchdog(struct UVM *vm, struct UStrand *s,
                                    UHostFn fn, int argc, UValue *argv);
#else
/* Non-debug builds: zero-overhead macro — collapsed to a bare call. */
#  define urbi_call_host_with_watchdog(vm, s, fn, argc, argv) \
       ((fn)((s), (argc), (argv)))
#endif

/* urbi_set_isr_check_fn: register a predicate that returns true when called
 * from ISR context.  Pass NULL to disable ISR checking (default). */
void urbi_set_isr_check_fn(struct UVM *vm, bool (*fn)(void));

/* urbi_set_callback_watchdog_mode: set the watchdog response mode.
 * URBI_WATCHDOG_WARN — log warning via host_log_fn.
 * URBI_WATCHDOG_ASSERT — call urbi_panic on threshold exceeded. */
void urbi_set_callback_watchdog_mode(struct UVM *vm, UWatchdogMode mode);

/* === M4 module-instance C API (T16) ===
 *
 * UModule is read-only (flash-resident on freestanding targets).  The
 * mutable IC state lives in a per-VM UModuleInstance.  Two instances of
 * the same UModule (one per VM, or two per VM for redundant chunks) hold
 * independent IC tables — IC fill in one instance does not bleed into the
 * other.
 *
 * urbi_module_instance_create allocates the UModuleInstance + its
 * UProtoInstanceArr bulk in two GC cells.  Returns NULL on OOM.
 *
 * urbi_module_instance_destroy is a no-op at v1.0 — both cells are
 * GC-managed and reaped by sweep when no roots reach the instance.
 * (AUDIT: OBJ-027 — function body is dead at v1.0; symbol kept for
 * public-API stability.  M7 module-instance lifecycle work may give
 * the call host-visible side-effects (e.g. detaching from a host-owned
 * registry); until then, callers should still pair create/destroy so
 * the symbol can grow semantics without source churn.)
 *
 * Thread safety: none at M4; same single-threaded constraint as the rest
 * of the v1.0 API. */
#ifndef URBI_MODULE_INSTANCE_TYPEDEF_DEFINED
#define URBI_MODULE_INSTANCE_TYPEDEF_DEFINED
typedef struct UModuleInstance UModuleInstance;
#endif

UModuleInstance *urbi_module_instance_create (struct UVM *vm, struct UModule *m);
void             urbi_module_instance_destroy(struct UVM *vm, UModuleInstance *mi);

/* === API-013: VM lifecycle (promoted to public at v0.5.5) ===
 *
 * Hosts allocate a UVM struct themselves, initialize it with urbi_vm_init
 * (passing a host allocator), drive it via urbi_step / urbi_run_chunk /
 * urbi_repl_eval, and tear it down with urbi_vm_destroy.  urbi_vm_run is
 * a convenience wrapper that runs a module's root chunk to completion.
 *
 * Pre-v0.5.5 these were `uvm_*` and lived in src/vm/uvm.h; tests had to
 * include the internal header to call them.  Wave 3 promotes the names
 * to `urbi_vm_*` and publishes the supporting types via urbi/types.h.
 * Closes API-013 + API-027.
 *
 * Conservative scope: pure rename.  Signatures, semantics, and error
 * codes are byte-identical to the pre-v0.5.5 internal forms. */
/* T23 (v0.7.0 Wave 1) — returns URBI_OK on success, URBI_ERR_OOM if any
 * sub-system allocation (event_ring, deferred_slot_changes, watcher pool,
 * op_overload IC, ...) fails.  Pre-v0.7.0 this returned void; the change
 * is permitted under the pre-v1.0 ABI escape clause documented in
 * <urbi/version.h>.  urbi_vm_destroy remains safe to call regardless of
 * the return value (partial-init state is reaped on the destroy path). */
int      urbi_vm_init   (struct UVM *vm, UVMAllocFn alloc_fn, void *alloc_ud);
void     urbi_vm_destroy(struct UVM *vm);
UVMError urbi_vm_run    (struct UVM *vm, struct URealm *realm,
                         const struct UModule *module, UValue *out);

/* === urbi_lock_heap (Phase 13 / T145) ===
 *
 * Lock the allocator post-init.  After this call, urbi_gc_alloc declines
 * to allocate new GC-managed cells and returns NULL; the caller observes
 * the failure as an OOM-shaped failure mode (URBI_ERR_OOM at native
 * boundaries, or a TypeError raise via urbi_raise_oom on the script
 * surface).  Existing GC-tracked objects continue to operate; collection
 * still runs (sweep / mark slices do not allocate).
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
 *   5. topology_gen, lookup_id, next_object_id (M4 object-model counters)
 *   6. per-IC observable state across every live UModuleInstance (M4 T30):
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
uint64_t urbi_get_determinism_checksum(struct UVM *vm);
#endif /* URBI_DEBUG */

#ifdef __cplusplus
}
#endif

#endif
