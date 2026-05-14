/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_error.h — private inter-TU API for per-VM error ring (Gap P, v0.7.1).
 * Only included by src/ translation units that publish last-error on failure.
 * The public surface (urbi_last_error / urbi_clear_error / urbi_error_info_t)
 * lives in <urbi/urbi.h>. */

#ifndef UVM_ERROR_H
#define UVM_ERROR_H

#include <stddef.h>

struct UVM; /* forward; full definition in vm/uvm.h */

/* urbi_set_error_internal: push an error entry onto the per-VM ring.
 *
 * vm          — owning VM; NULL is a no-op.
 * code        — UErrCode value (negative int) describing the error.
 * message     — human-readable description; NULL → empty string.
 * source_name — source file or script name; NULL → empty string.
 * line        — source line number; 0 if unknown.
 * context     — caller-supplied context tag (e.g. "urbi_event_register");
 *               NULL → empty string.
 *
 * Strings are truncated to URBI_ERROR_STRING_BUF - 1 characters and
 * NUL-terminated.  The entry takes ownership of the copies; the caller's
 * pointers need not remain valid after the call. */
void urbi_set_error_internal(struct UVM *vm,
                              int         code,
                              const char *message,
                              const char *source_name,
                              int         line,
                              const char *context);

#endif /* UVM_ERROR_H */
