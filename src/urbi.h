/* SPDX-License-Identifier: BSD-3-Clause */
/* Public C API. */

#ifndef URBI_H
#define URBI_H

#include <stdbool.h>
#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint64_t */

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
 * urbi_load_module: register a module under module_name in the VM's import table.
 * Returns URBI_ERR_INVALID_ARG at M3; real implementation lands at M6. */

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
                   struct UModule *module, UValue *out_result);

int urbi_repl_eval(struct UVM *vm, struct URealm *realm,
                   const char *line, size_t line_len,
                   char *out_buf, size_t out_buf_size);

int urbi_run_script(struct UVM *vm, struct URealm *realm, struct UModule *module);

int urbi_load_module(struct UVM *vm, struct UModule *module, const char *module_name);

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
 * Single-producer / single-consumer: one ISR writer + one thread reader. */
int urbi_inject_event(struct UVM *vm, uint32_t event_id,
                      const void *payload, size_t len);

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

#define URBI_WATCHDOG_WARN   0
#define URBI_WATCHDOG_ASSERT 1

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
#  define URBI_CALLBACK_WARN_US 1000u
#endif

/* URBI_ASSERT_NOT_ISR: in URBI_DEBUG builds, asserts the function is not
 * called from ISR context.  vm must be a pointer to a live UVM. */
#ifdef URBI_DEBUG
#  define URBI_ASSERT_NOT_ISR(vm) \
       do { if ((vm)->isr_check_fn && (vm)->isr_check_fn()) \
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
 * mode: URBI_WATCHDOG_WARN (0) — log warning via host_log_fn.
 *       URBI_WATCHDOG_ASSERT (1) — call urbi_panic on threshold exceeded. */
void urbi_set_callback_watchdog_mode(struct UVM *vm, uint8_t mode);

#ifdef URBI_DEBUG
/* urbi_get_determinism_checksum: FNV-1a hash of observable VM state.
 *
 * Call only at a QUIESCENT point (no strands runnable, no pending events).
 * Returns a stable hash of:
 *   - all UValue bindings across every live Realm's namespace
 *   - watcher pool high-water mark
 *   - gc_total_allocated counter
 *   - intern table entry count
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
