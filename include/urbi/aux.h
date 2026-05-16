/* SPDX-License-Identifier: BSD-3-Clause */
/* Public convenience layer (aux).
 *
 * Stability: aux. Free to evolve within a MAJOR per <urbi/version.h>.
 *
 * Compiled into separate liburbi_aux.a archive — linker-omittable for
 * minimal-footprint or certification builds. Optional at link time.
 *
 * Governance rule: every aux function must be strictly implementable via
 * <urbi/urbi.h> public API. No private header access, no internal state
 * peeking, no performance shortcuts. Enforced at PR review (see
 * CONTRIBUTING.md "Aux layer governance"). If a proposed aux function
 * can't meet the rule, either refactor until it can, or propose the
 * addition to core (paying the cost against the < 80-fn urbi.h budget
 * per REVIVAL §6).
 *
 * v0.7.0 ships with one helper: urbi_aux_check_version. Wave 2 adds
 * batch registration helpers, error formatter, load-and-run composite,
 * and value dump utility.
 */

#ifndef URBI_AUX_H
#define URBI_AUX_H

#include "urbi/urbi.h"    /* full public API — types, vm, realm, events, fns */

#include <stddef.h>       /* size_t */
#include <stdint.h>       /* uint8_t */

#ifdef __cplusplus
extern "C" {
#endif

/* === Compile-vs-runtime ABI version mismatch check ===
 *
 * Returns URBI_OK if the runtime library matches the header version the
 * embedder compiled against, or URBI_ERR_API_VERSION_MISMATCH otherwise.
 *
 * Mechanically derivable from URBI_API_VERSION_* macros — the embedder's
 * TU bakes the macro values at compile time; the library implementation
 * compares against the runtime URBI_API_VERSION_* values. Mismatch means
 * the embedder linked against a different library version than the
 * headers they #included.
 *
 * Lua precedent: lua_aux_checkversion. */
int urbi_aux_check_version(void);

/* === Batch registration helpers ===
 *
 * urbi_aux_event_decl_t: one row of an event-registration table.
 *
 *   name        — NUL-terminated event name to install as a realm global.
 *   destruct_fn — optional payload destructure callback (may be NULL).
 *   destruct_ud — user-data passed to destruct_fn (ignored if NULL fn).
 *   out_id      — receives the registered urbi_event_id_t on success, or
 *                 URBI_EVENT_ID_INVALID on failure.  Must not be NULL.
 *
 * urbi_aux_register_event_table: register multiple named events in one call.
 *
 * Iterates the decls[] table of `count` entries, calling urbi_event_register
 * for each.  Stops at the first failure; entries after the failed index are
 * not attempted.  There is no rollback — successfully-registered events
 * remain installed.
 *
 * Returns URBI_OK if all entries succeeded; otherwise the error code from
 * the first failure (see urbi_event_register for possible codes). */
typedef struct {
    const char                         *name;
    urbi_event_payload_destructure_fn   destruct_fn;
    void                               *destruct_ud;
    urbi_event_id_t                    *out_id;
} urbi_aux_event_decl_t;

int urbi_aux_register_event_table(struct UVM *vm, struct URealm *realm,
                                   const urbi_aux_event_decl_t *decls,
                                   size_t count);

/* urbi_aux_function_decl_t: one row of a host-function registration table.
 *
 *   name — NUL-terminated script-visible name for the function.
 *   fn   — host C function to install (urbi_native_method_fn signature).
 *
 * urbi_aux_register_function_table: register multiple host functions in one
 * call.  Iterates decls[], calling urbi_register for each.  Stops at first
 * failure (no rollback).
 *
 * Returns URBI_OK if all entries succeeded; error code from first failure
 * otherwise (see urbi_register for possible codes). */
typedef struct {
    const char             *name;
    urbi_native_method_fn   fn;
} urbi_aux_function_decl_t;

int urbi_aux_register_function_table(struct UVM *vm, struct URealm *realm,
                                      const urbi_aux_function_decl_t *decls,
                                      size_t count);

/* === printf-style error setter ===
 *
 * urbi_aux_set_error: format an error message and publish it to the per-VM
 * error ring (via urbi_set_error).
 *
 * vm          — target VM; NULL is a no-op.
 * code        — UErrCode value (negative int) for the error entry.
 * source_name — source file/script name; NULL → empty string.
 * source_line — source line number; 0 if unknown.
 * fmt, ...    — printf-style format string + arguments.  The formatted
 *               message is truncated to URBI_ERROR_STRING_BUF - 1 characters.
 *
 * Implementation: vsnprintf into a stack buffer, then urbi_set_error.
 * Uses <stdio.h>/<stdarg.h> (aux is hosted C99; not available in core). */
#ifdef __GNUC__
__attribute__((format(printf, 5, 6)))
#endif
void urbi_aux_set_error(struct UVM *vm, int code,
                         const char *source_name, int source_line,
                         const char *fmt, ...);

/* === Composite bytecode load + run ===
 *
 * urbi_aux_load_and_run: deserialize a wire-format bytecode buffer into a
 * UModule, run its root chunk under the VM's global realm, and return the
 * chunk result.
 *
 * vm         — initialized VM; must not be NULL.
 * bytecode   — wire-format bytes (as produced by urbi_compile_source or the
 *              bake tool); must not be NULL.
 * len        — length of bytecode in bytes.
 * out_result — receives the result UValue of the root chunk; may be NULL
 *              if the caller does not need the result.
 *
 * Internally calls urbi_module_from_bytes + urbi_run_chunk + urbi_module_free.
 * The temporary UModule is freed before returning.
 *
 * Returns:
 *   URBI_OK                            — success
 *   URBI_ERR_INVALID_ARG               — NULL vm or bytecode
 *   URBI_ERR_BYTECODE_VERSION_MISMATCH — bytecode version does not match
 *   URBI_ERR_OOM                       — deserialize or run OOM */
int urbi_aux_load_and_run(struct UVM *vm,
                           const uint8_t *bytecode, size_t len,
                           UValue *out_result);

/* === Human-readable value formatter ===
 *
 * urbi_aux_dump_value: format a UValue into out_buf as a human-readable
 * string for logging and debugging.
 *
 * vm       — reserved for future use; may be NULL.
 * v        — the value to format.
 * out_buf  — destination buffer; NUL-terminated on return.
 * buf_size — capacity of out_buf in bytes (must be >= 1).
 *
 * Format per kind:
 *   URBI_VALUE_NIL     → "nil"
 *   URBI_VALUE_BOOL    → "true" or "false"
 *   URBI_VALUE_INT     → decimal integer (%lld)
 *   URBI_VALUE_FLOAT   → shortest decimal (%g)
 *   URBI_VALUE_STR     → double-quoted with basic C-style escapes
 *   URBI_VALUE_OBJECT  → "object@0x<addr>"
 *   URBI_VALUE_EVENT   → "event@0x<addr>"
 *   URBI_VALUE_CLOSURE → "closure@0x<addr>"
 *   URBI_VALUE_VOID    → "void"
 *   URBI_VALUE_PTR     → "ptr@0x<addr>"
 *   unknown kind       → "?"
 *
 * Returns the number of bytes written excluding the NUL terminator, or the
 * number of bytes that WOULD have been written if buf_size had been large
 * enough (snprintf semantics). */
int urbi_aux_dump_value(struct UVM *vm, UValue v,
                         char *out_buf, size_t buf_size);

/* === urbi_aux_diag_to_stderr ===========================================
 *
 * Default-shaped urbi_diag_fn callback for hosted builds.  Embedders who
 * don't have a platform log system can pass this directly to
 * urbi_set_diag_fn:
 *
 *     urbi_set_diag_fn(vm, urbi_aux_diag_to_stderr);
 *
 * Formats the message with a "[urbi level=N] " prefix and a trailing
 * newline, then writes to stderr via fputs.  No allocation, no
 * dependencies beyond <stdio.h>.  Truncates at 256 bytes — long
 * runtime messages are rare and not the place for unbounded buffers.
 *
 * Freestanding builds: not available (uses stderr).  Embedders without
 * a libc should wire urbi_set_diag_fn to a platform-specific shim
 * (e.g., port_diag_to_esp on ESP-IDF).
 *
 * Signature matches urbi_diag_fn — see <urbi/urbi.h> for the
 * level convention (URBI_LOG_DEBUG/INFO/WARN/ERROR). */
void urbi_aux_diag_to_stderr(struct UVM *vm, int level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* URBI_AUX_H */
