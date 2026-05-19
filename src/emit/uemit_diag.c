/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_diag.c — emit-time diagnostic warnings.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #9). */

#include "uemit_internal.h"
#include "emit/uemit.h"
#include "chunk/umodule.h"
#include "parse/uast.h"
#include "runtime/umacros.h"

#if __STDC_HOSTED__
#  include <stdarg.h>
#  include <stdio.h>   /* vsnprintf */
#endif

/* --- Public API --- */

/* T32: Append a warn-level diagnostic to the emitter's buffer.
 * Uses emit_alloc_for (module allocator) so that the buffer and message
 * strings survive for the lifetime of the emit session.  On OOM the
 * diagnostic is silently dropped — warns are never fatal. */
void emit_diag_warn(UEmitter *e, UAstNode *n, const char *fmt, ...) {
#if __STDC_HOSTED__
    /* Grow the buffer if needed (doubling from 0 → 4 → 8 …). */
    if (e->diag_count >= e->diag_cap) {
        int new_cap = e->diag_cap > 0 ? e->diag_cap * 2 : 4;
        UModuleAllocFn alloc = emit_alloc_for(e->module);
        UEmitDiag *nb = (UEmitDiag *)alloc(e->diag_buf,
                                            (size_t)new_cap * sizeof(UEmitDiag),
                                            e->module->alloc_ud);
        if (nb == NULL) return;  /* OOM — drop silently */
        e->diag_buf = nb;
        e->diag_cap = new_cap;
    }

    /* Format the message into a fixed-size stack buffer then copy. */
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    /* clang-analyzer-valist.Uninitialized is a false positive here — `ap`
     * is initialized by va_start above, then consumed by vsnprintf, then
     * cleared by va_end below.  The analyzer cannot see through the
     * vsnprintf prototype's va_list contract. */
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);  /* NOLINT(clang-analyzer-valist.Uninitialized) — ap initialized by va_start above */
    va_end(ap);

    /* Copy the message string using the module allocator. */
    size_t msg_len = urbi_strlen(buf);
    UModuleAllocFn alloc = emit_alloc_for(e->module);
    char *msg = (char *)alloc(NULL, msg_len + 1U, e->module->alloc_ud);
    if (msg == NULL) return;  /* OOM — drop silently */
    emit_memcpy(msg, buf, msg_len + 1U);

    e->diag_buf[e->diag_count].level   = UEMIT_DIAG_WARN;
    e->diag_buf[e->diag_count].line    = n ? n->line : 0;
    e->diag_buf[e->diag_count].col     = n ? n->col  : 0;
    e->diag_buf[e->diag_count].message = msg;
    e->diag_count++;
#else
    /* Freestanding: no vsnprintf available; diagnostics not supported. */
    (void)e; (void)n; (void)fmt;
#endif
}

void emit_diag_free_all(UEmitter *e) {
#if __STDC_HOSTED__
    if (e->diag_buf == NULL) return;
    UModuleAllocFn alloc = emit_alloc_for(e->module);
    /* Free each message string individually. */
    for (int i = 0; i < e->diag_count; i++) {
        if (e->diag_buf[i].message != NULL) {
            alloc((void *)e->diag_buf[i].message, 0, e->module->alloc_ud);
        }
    }
    /* Free the buffer array itself. */
    alloc(e->diag_buf, 0, e->module->alloc_ud);
    e->diag_buf  = NULL;
    e->diag_count = 0;
    e->diag_cap   = 0;
#else
    (void)e;
#endif
}
