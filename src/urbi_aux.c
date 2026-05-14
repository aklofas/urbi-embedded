/* SPDX-License-Identifier: BSD-3-Clause */
/* Public convenience-layer implementation. Compiled into liburbi_aux.a
 * (separate archive) per CONTRIBUTING.md "Aux layer governance".
 *
 * All functions here MUST be strictly implementable via <urbi/urbi.h>
 * public API only. No private header access is permitted. */

#include "urbi/aux.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "urbi/version.h"

#include <stdarg.h>   /* va_list, va_start, va_end */
#include <stddef.h>   /* size_t, NULL */
#include <stdint.h>   /* uint8_t */
#include <stdio.h>    /* vsnprintf, snprintf */
#include <string.h>   /* memcpy for string escaping */

/* === urbi_aux_check_version ============================================== */

int urbi_aux_check_version(void) {
    int rt_major = 0, rt_minor = 0, rt_patch = 0;
    urbi_api_version(&rt_major, &rt_minor, &rt_patch);
    if (rt_major != URBI_API_VERSION_MAJOR
     || rt_minor != URBI_API_VERSION_MINOR
     || rt_patch != URBI_API_VERSION_PATCH) {
        return URBI_ERR_API_VERSION_MISMATCH;
    }
    return URBI_OK;
}

/* === urbi_aux_register_event_table ======================================= */

int
urbi_aux_register_event_table(struct UVM *vm, struct URealm *realm,
                               const urbi_aux_event_decl_t *decls,
                               size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        urbi_event_id_t id = urbi_event_register(vm, realm,
                                                  decls[i].name,
                                                  decls[i].destruct_fn,
                                                  decls[i].destruct_ud);
        if (decls[i].out_id != NULL) {
            *decls[i].out_id = id;
        }
        if (id == URBI_EVENT_ID_INVALID) {
            /* Return the error code from the last_error ring.  The caller
             * can inspect urbi_last_error for details. */
            urbi_error_info_t info;
            int code = urbi_last_error(vm, &info);
            return code != URBI_OK ? code : URBI_ERR_INVALID_ARG;
        }
    }
    return URBI_OK;
}

/* === urbi_aux_register_function_table ==================================== */

int
urbi_aux_register_function_table(struct UVM *vm, struct URealm *realm,
                                  const urbi_aux_function_decl_t *decls,
                                  size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        int rc = urbi_register(vm, realm, decls[i].name, decls[i].fn);
        if (rc != URBI_OK) {
            return rc;
        }
    }
    return URBI_OK;
}

/* === urbi_aux_set_error ================================================== */

void
urbi_aux_set_error(struct UVM *vm, int code,
                    const char *source_name, int source_line,
                    const char *fmt, ...)
{
    /* 255 bytes + NUL — matches the internal URBI_ERROR_STRING_BUF size.
     * Stack-allocated; aux is hosted C99 so the stack size is not a concern.
     * The message will be truncated by urbi_set_error if it exceeds the
     * internal ring buffer capacity (same 255-byte cap). */
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt != NULL ? fmt : "", ap);  /* NOLINT(clang-analyzer-valist.Uninitialized) — ap initialized by va_start above */
    va_end(ap);

    urbi_set_error(vm, code, msg, source_name, source_line,
                   "urbi_aux_set_error");
}

/* === urbi_aux_load_and_run =============================================== */

int
urbi_aux_load_and_run(struct UVM *vm,
                       const uint8_t *bytecode, size_t len,
                       UValue *out_result)
{
    if (vm == NULL || bytecode == NULL) {
        return URBI_ERR_INVALID_ARG;
    }

    char errmsg[256] = {0};
    struct UModule *m = urbi_module_from_bytes(bytecode, len,
                                               errmsg, sizeof errmsg);
    if (m == NULL) {
        /* Attempt to identify the error kind from the message.  The public
         * urbi_module_from_bytes sets no error ring entry; we synthesise one
         * here via urbi_aux_set_error for callers using urbi_last_error. */
        urbi_aux_set_error(vm, URBI_ERR_INVALID_ARG,
                           NULL, 0,
                           "urbi_aux_load_and_run: deserialize failed: %s",
                           errmsg[0] ? errmsg : "(no diagnostic)");
        /* Heuristic: the only version-specific error is a version mismatch.
         * umodule.c uses the string "unsupported" in that diagnostic path. */
        for (int i = 0; errmsg[i] != '\0'; i++) {
            if (errmsg[i] == 'u' && errmsg[i+1] == 'n' &&
                errmsg[i+2] == 's' && errmsg[i+3] == 'u') {
                return URBI_ERR_BYTECODE_VERSION_MISMATCH;
            }
        }
        return URBI_ERR_INVALID_ARG;
    }

    struct URealm *realm = urbi_realm_global(vm);
    UValue result;
    int rc = urbi_run_chunk(vm, realm, m, &result);

    urbi_module_free(m);

    if (rc == URBI_OK && out_result != NULL) {
        *out_result = result;
    }
    return rc;
}

/* === urbi_aux_dump_value ================================================= */

/* Helper: write a C-escaped string representation into buf, starting at
 * offset *pos, updating *pos as bytes are appended.  Returns the final
 * total that would be written (snprintf semantics). */
static int
write_escaped_str(const char *s, size_t slen, char *buf, size_t buf_size)
{
    /* Build into a local accumulator that tracks would-be length. */
    int total = 0;

#define EMIT(c) do { \
    if ((size_t)total + 1 < buf_size) { buf[total] = (c); } \
    total++; \
} while (0)

    EMIT('"');
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"')       { EMIT('\\'); EMIT('"'); }
        else if (c == '\\') { EMIT('\\'); EMIT('\\'); }
        else if (c == '\n') { EMIT('\\'); EMIT('n'); }
        else if (c == '\t') { EMIT('\\'); EMIT('t'); }
        else if (c == '\0') { EMIT('\\'); EMIT('0'); }
        else                { EMIT((char)c); }
    }
    EMIT('"');

#undef EMIT

    if (buf_size > 0) {
        size_t nul = (size_t)total < buf_size ? (size_t)total : buf_size - 1;
        buf[nul] = '\0';
    }
    return total;
}

int
urbi_aux_dump_value(struct UVM *vm, UValue v,
                     char *out_buf, size_t buf_size)
{
    (void)vm;  /* reserved for future use */

    if (out_buf == NULL || buf_size == 0) {
        return 0;
    }

    urbi_value_kind_t kind = urbi_value_kind(v);

    switch (kind) {
    case URBI_VALUE_NIL:
        return snprintf(out_buf, buf_size, "nil");

    case URBI_VALUE_BOOL: {
        bool b = urbi_value_as_bool(v);
        return snprintf(out_buf, buf_size, "%s", b ? "true" : "false");
    }

    case URBI_VALUE_INT: {
        long long n = (long long)urbi_value_as_int(v);
        return snprintf(out_buf, buf_size, "%lld", n);
    }

    case URBI_VALUE_FLOAT: {
        double f = urbi_value_as_float(v);
        return snprintf(out_buf, buf_size, "%g", f);
    }

    case URBI_VALUE_STR: {
        size_t slen = 0;
        const char *s = urbi_value_as_str(v, &slen);
        if (s == NULL) {
            return snprintf(out_buf, buf_size, "\"\"");
        }
        return write_escaped_str(s, slen, out_buf, buf_size);
    }

    case URBI_VALUE_OBJECT:
        return snprintf(out_buf, buf_size, "object@%p",
                        (void *)urbi_value_as_object(v));

    case URBI_VALUE_EVENT:
        return snprintf(out_buf, buf_size, "event@%p",
                        (void *)urbi_value_as_event(v));

    case URBI_VALUE_CLOSURE:
        return snprintf(out_buf, buf_size, "closure@%p",
                        (void *)urbi_value_as_closure(v));

    case URBI_VALUE_VOID:
        return snprintf(out_buf, buf_size, "void");

    case URBI_VALUE_PTR:
        return snprintf(out_buf, buf_size, "ptr@%p",
                        urbi_value_as_ptr(v));

    default:
        return snprintf(out_buf, buf_size, "?");
    }
}
