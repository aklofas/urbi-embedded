/* SPDX-License-Identifier: BSD-3-Clause */
/* include/urbi/urbi.h — the public C API.
 *
 * One header, one archive, no optional layers.  Every function here is
 * implemented in src/rt/uapi.c against the runtime core in src/rt/.
 *
 * Error convention: functions return int, 0 (URBI_OK) on success and a
 * negative UErrCode otherwise.  Functions that return a pointer return
 * NULL on failure.  A script-level failure — anything that would be a
 * throw inside urbiscript — surfaces as URBI_ERR_UNCAUGHT_THROW with the
 * rendered exception available from urbi_last_error.
 *
 * Subsystems still under construction (events, watchers, tags, and the
 * scheduler behind urbi_step) are declared here with their final
 * signatures and return URBI_ERR_INVALID_STATE until their task lands;
 * each one says so in its own comment.  Nothing here changes shape when
 * they do.
 *
 * Threading: a UVM is single-threaded.  urbi_inject_event is the one
 * exception and is safe to call from an interrupt handler. */

#ifndef URBI_H
#define URBI_H

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility push(default)
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "urbi/types.h"
#include "urbi/version.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * Lifecycle
 * =================================================================== */

/* Optional knobs for urbi_open.  Pass NULL for the defaults. */
typedef struct UVMConfig {
    uint32_t step_budget;   /* instructions per urbi_step slice; 0 = built-in default */
    uint8_t  boot_stdlib;   /* 1 = install the standard library at open (default 1) */
} UVMConfig;

/* Create a VM.  `alloc` is realloc-shaped (see UVMAllocFn) and is the
 * only way the VM ever reaches the host's memory; `ud` is passed back
 * to it unchanged.  Returns NULL on OOM or a NULL allocator. */
UVM *urbi_open(UVMAllocFn alloc, void *ud, const UVMConfig *config);

/* Destroy a VM and everything it owns: realms, strands, objects, loaded
 * chunks.  Every pointer obtained from this VM is invalid afterwards. */
void urbi_close(UVM *vm);

/* What one urbi_step slice ended in.  Non-negative, so an error (always
 * negative, see URBI_OK and the URBI_ERR_* codes) is still distinguished
 * by sign; URBI_STEP_RAN is URBI_OK, which is what a host that only
 * checks for failure sees. */
typedef enum {
    URBI_STEP_RAN        = 0,   /* the budget ran out with strands still runnable */
    URBI_STEP_IDLE_UNTIL = 1,   /* nothing runnable, a timer pending: see next_wake_us */
    URBI_STEP_QUIESCENT  = 2    /* no runnable strand and no timer */
} UStepResult;

/* Run the scheduler for up to `budget` instructions (0 = until nothing
 * is runnable).  One slice reaps dead strands, delivers anything an
 * interrupt handler injected, fires every timer due at the clock reading
 * taken on entry, then runs the run queue.
 *
 * Returns a URBI_STEP_* value, or a negative URBI_ERR_* code.  On
 * URBI_STEP_IDLE_UNTIL the next timer deadline is written through
 * `next_wake_us` when that pointer is non-NULL; it is left untouched
 * otherwise, so an event loop can sleep until then. */
int urbi_step(UVM *vm, uint32_t budget, uint64_t *next_wake_us);

/* True when the VM has a runnable strand or a pending timer.  Strands
 * parked on an event nobody will emit do not count: a program with
 * nothing left to drive it is quiescent, not live. */
bool urbi_has_live_work(UVM *vm);

/* Host hooks.  All are optional; each may be set at any time.
 *   clock   — monotonic microseconds, used by timers and durations.
 *   diag    — receives runtime diagnostics (level follows syslog order).
 *   writer  — receives script output, one call per (channel, message).
 *   wake    — called when the VM becomes runnable from an ISR-side
 *             injection, so an event loop can break out of its sleep. */
void urbi_set_clock(UVM *vm, uint64_t (*fn)(void *ud), void *ud);
void urbi_set_diag(UVM *vm, void (*fn)(UVM *vm, void *ud, int level, const char *msg, size_t len), void *ud);
void urbi_set_writer(UVM *vm, void (*fn)(void *ud, const char *chan, size_t chan_len, const char *msg, size_t msg_len), void *ud);
void urbi_set_wake(UVM *vm, void (*fn)(void *ud), void *ud);

/* ===================================================================
 * Realms
 * =================================================================== */

/* A realm is one isolated global namespace: its globals object inherits
 * from the VM's built-in globals, so realms share the standard library
 * but not each other's variables. */
URealm *urbi_realm_new(UVM *vm);
/* Detach a realm.  Its objects are reclaimed by the collector once
 * nothing else refers to them.  Freeing the main realm is a no-op. */
void    urbi_realm_free(UVM *vm, URealm *realm);
/* The realm created at urbi_open; never NULL for a live VM. */
URealm *urbi_realm_main(UVM *vm);

/* ===================================================================
 * Code
 * =================================================================== */

/* Compile source to a serialized bytecode chunk.  On success writes a
 * freshly allocated buffer through `out_bytes`/`out_len`; release it
 * with urbi_chunk_free.  On failure writes a positioned diagnostic into
 * `err` (pass NULL/0 to suppress) and returns URBI_ERR_COMPILE or
 * URBI_ERR_OOM. */
int  urbi_compile(UVM *vm, const char *src, size_t n, const char *name,
                  uint8_t **out_bytes, size_t *out_len, char *err, size_t errcap);

/* Load a serialized chunk and run it on `realm`, exactly as urbi_run
 * does for source.  Returns URBI_ERR_BYTECODE_VERSION_MISMATCH for a
 * chunk from an incompatible build. */
int  urbi_load(UVM *vm, URealm *realm, const uint8_t *bytes, size_t n, UValue *out);

/* Compile and run source on `realm`.  *out receives the value of the
 * last statement (void for a statement that produces none).  Returns
 * URBI_ERR_COMPILE with `err` filled in, or URBI_ERR_UNCAUGHT_THROW
 * with urbi_last_error filled in. */
int  urbi_run(UVM *vm, URealm *realm, const char *src, size_t n, const char *name,
              UValue *out, char *err, size_t errcap);

/* Call a closure value synchronously and collect its result. */
int  urbi_call(UVM *vm, URealm *realm, UValue callee, UValue recv,
               const UValue *argv, uint8_t argc, UValue *out);

/* Release a buffer returned by urbi_compile. */
void urbi_chunk_free(UVM *vm, uint8_t *bytes, size_t n);

/* ===================================================================
 * Values
 * ===================================================================
 *
 * Constructors, predicates and accessors are the inlines in
 * <urbi/types.h> (urbi_make_*, urbi_value_is_*, urbi_value_as_*).  The
 * two functions here are the ones that need a live VM. */

/* Intern bytes as a script string value.  The result is a UValue of
 * kind URBI_VALUE_STR. */
UValue urbi_make_string(UVM *vm, const char *bytes, size_t n);

/* Render any value the way the REPL prints it.  Returns the number of
 * bytes written (always NUL-terminated when cap > 0).
 *
 * Hosted builds only: the Float rendering the .chk corpus pins needs
 * snprintf, so this lives outside the freestanding core
 * (src/host/uformat.c) and is absent from a freestanding build, the same
 * way urbi_compile is absent without the compiler frontend. */
size_t urbi_value_to_string(UVM *vm, UValue v, char *buf, size_t cap);

/* Keep a heap value alive across calls that may collect, and release it
 * again.  A value held only in host C memory is invisible to the
 * collector; urbi_ref pins it and urbi_unref releases it.  Both are
 * no-ops for immediate values (numbers, booleans, nil, void).
 *
 * This is ONE PIN BIT PER CELL, not a reference count: a second urbi_ref
 * on an already-pinned value changes nothing, and the first urbi_unref
 * releases it regardless of how many times it was pinned.  Nesting is
 * not supported — a host that needs it must count on its own side.  The
 * runtime never sets or clears this bit for its own purposes, so a pin
 * is only ever released by the host that took it. */
void urbi_ref(UVM *vm, UValue v);
void urbi_unref(UVM *vm, UValue v);

/* ===================================================================
 * Globals and slots
 * =================================================================== */

int urbi_global_get(UVM *vm, URealm *realm, const char *name, UValue *out);
int urbi_global_set(UVM *vm, URealm *realm, const char *name, UValue v);
int urbi_slot_get(UVM *vm, UValue obj, const char *name, UValue *out);
int urbi_slot_set(UVM *vm, UValue obj, const char *name, UValue v);

/* ===================================================================
 * Host functions
 * =================================================================== */

/* A host function.  `self` is the receiver (nil for a plain call),
 * `args` points at `nargs` arguments, and the result goes in `*out`.
 * Return UEXEC_OK, or UEXEC_THROW after calling urbi_throw. */
typedef int (*urbi_native_fn)(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);

/* Install a host function at a dotted path, e.g. "Robot.move".  The
 * path's leading components must already resolve to objects reachable
 * from the main realm's globals; the last component names the slot. */
int urbi_register(UVM *vm, const char *path, urbi_native_fn fn, uint8_t min_args, uint8_t max_args);

/* Raise a script exception from inside a host function.  `proto` names
 * one of the built-in exception prototypes ("TypeError", "RangeError",
 * ...); an unknown name raises a plain Exception.  Returns UEXEC_THROW
 * so a native can `return urbi_throw(...)`. */
int urbi_throw(UVM *vm, const char *proto, const char *msg);

/* ===================================================================
 * Events and watchers
 * ===================================================================
 *
 * urbi_watch is NOT YET AVAILABLE: it returns URBI_ERR_INVALID_STATE
 * until the reactive-runtime task lands.  The rest of the section is
 * live. */

/* Create a named event object on `realm`. */
int urbi_event_new(UVM *vm, URealm *realm, const char *name, UValue *out);
/* Emit an event with an optional payload value. */
int urbi_event_emit(UVM *vm, UValue event, UValue payload);
/* Create-or-find a named event on `realm` and hand back the id an
 * interrupt handler routes through.  This is the other half of
 * urbi_inject_event: an id has no other source, and a registered event
 * is held for the life of the VM so an ISR can never name a collected
 * one.  URBI_ERR_OOM once the id table is full. */
int urbi_event_register(UVM *vm, URealm *realm, const char *name, urbi_event_id_t *out_id);
/* Deposit an event from an interrupt handler.  ISR-safe and
 * allocation-free: the payload is copied into a lock-free ring and
 * delivered at the next urbi_step, where it reaches script as one
 * integer -- the first eight bytes of the payload, zero-extended.
 * URBI_ERR_INVALID_ARG for an unregistered id or an oversized payload,
 * URBI_ERR_OOM when the ring is full (the injection is dropped, which is
 * the only thing available without blocking an interrupt). */
int urbi_inject_event(UVM *vm, urbi_event_id_t id, const urbi_event_payload_t *payload, size_t n);
/* Watch a condition expression; `cb` fires on each rising edge. */
int urbi_watch(UVM *vm, URealm *realm, const char *expr,
               int (*cb)(UVM *vm, void *ud, UValue value), void *ud);

/* ===================================================================
 * Tags
 * ===================================================================
 *
 * A tag names a cancellable scope.  Stopping one unwinds every strand
 * inside it through its cleanup -- from wherever the caller is, inside
 * the scope or out.  block and freeze are independent gates. */

int urbi_tag_new(UVM *vm, URealm *realm, const char *name, UValue *out);
int urbi_tag_stop(UVM *vm, UValue tag);
int urbi_tag_block(UVM *vm, UValue tag);
int urbi_tag_unblock(UVM *vm, UValue tag);
int urbi_tag_freeze(UVM *vm, UValue tag);
int urbi_tag_unfreeze(UVM *vm, UValue tag);

/* ===================================================================
 * Errors
 * =================================================================== */

typedef struct UErrorInfo {
    int         code;      /* the UErrCode of the last failure, or URBI_OK */
    const char *message;   /* NUL-terminated; owned by the VM, valid until the next call */
} UErrorInfo;

/* Read the last error recorded on this VM.  Returns that error's code
 * (URBI_OK when there is none) and fills `info` when it is non-NULL. */
int  urbi_last_error(UVM *vm, UErrorInfo *info);
void urbi_clear_error(UVM *vm);

/* ===================================================================
 * Garbage collector
 * =================================================================== */

typedef struct UGcStats {
    size_t   bytes_live;    /* live bytes as of the last cycle, plus tracked arrays */
    size_t   bytes_since;   /* bytes allocated since the last cycle */
    uint32_t cells_live;    /* live cells as of the last cycle */
    uint32_t cycles;        /* completed collections */
} UGcStats;

/* Run one full collection now. */
void urbi_gc_collect(UVM *vm);
int  urbi_gc_stats(UVM *vm, UGcStats *out);

/* ===================================================================
 * Version
 * =================================================================== */

/* Library version as "MAJOR.MINOR.PATCH". */
const char *urbi_version(void);

#ifdef __cplusplus
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif
#endif /* URBI_H */
