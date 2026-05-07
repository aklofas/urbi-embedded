/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_internal.h — private inter-TU API for the emit subsystem.
 *
 * Consumed only by src/emit/*.c.  Public emit API is in src/emit/uemit.h.
 * Created v0.5.4-decompose; do NOT include from outside src/emit/. */

#ifndef UEMIT_INTERNAL_H
#define UEMIT_INTERNAL_H

#include "uemit.h"

#include <stddef.h>
#include <stdint.h>

/* --- Byte-copy / string helpers (freestanding-safe) --- */

/* Local byte-copy.  Replaces memcpy so the serializer compiles without
   a hosted <string.h>.  Same pattern as module_memcpy in umodule.c. */
static inline void emit_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *pd = (unsigned char *)dst;
    const unsigned char *ps = (const unsigned char *)src;
    size_t i;
    for (i = 0u; i < n; i++) pd[i] = ps[i];
}

/* Local byte-move (overlapping-safe right shift).  Used by the prologue
   prepend helper to shift instruction / line-delta arrays rightward. */
static inline void emit_memmove_right(void *dst, const void *src, size_t n) {
    unsigned char *pd = (unsigned char *)dst;
    const unsigned char *ps = (const unsigned char *)src;
    size_t i = n;
    while (i > 0u) { i--; pd[i] = ps[i]; }
}

/* Local strlen replacement (byte-loop).  Freestanding-safe. */
static inline size_t emit_strlen(const char *s) {
    size_t n = 0u;
    while (s[n] != '\0') n++;
    return n;
}

/* --- Module allocator helper --- */

#if __STDC_HOSTED__
#  include <stdlib.h>

static inline void *emit_stdlib_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;
    if (nbytes == 0u) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

#endif  /* __STDC_HOSTED__ */

/* Return the allocator to use for module c.  Available in both hosted and
   freestanding builds so that emit_grow can call it unconditionally.
   In freestanding builds the stdlib fallback is absent; the caller must have
   supplied alloc_fn, and emit_grow will return false if it is NULL. */
static inline UModuleAllocFn emit_alloc_for(const UModule *c) {
#if __STDC_HOSTED__
    return c->alloc_fn != NULL ? c->alloc_fn : emit_stdlib_alloc;
#else
    return c->alloc_fn;   /* freestanding: caller must supply */
#endif
}

/* --- Forward decls for cross-TU functions (extract-driven; added T6-T13) --- */

/* Diag-emit funnel (defined in uemit_diag.c). */
void emit_diag_warn(UEmitter *e, UAstNode *n, const char *fmt, ...);
void emit_diag_free_all(UEmitter *e);

/* Funcstate ops (defined in uemit_funcstate.c — T8+). */
UFuncState *uemit_open_function(UEmitter *e, UFuncState *parent);
UFuncState *uemit_close_function(UEmitter *e);
int uemit_assign_ic_index(UEmitter *e, USymbol *name);
int uemit_declare_local(UEmitter *e, const char *name, int name_len);
bool uemit_open_block(UEmitter *e, bool is_loop);
bool uemit_close_block(UEmitter *e);
void uemit_emit_loop_back_close(UEmitter *e);
int find_or_install_upvalue(UEmitter *e, UFuncState *fs,
                            const char *name, int name_len);

/* --- Sentinels and biases shared across emit TUs --- */

#define UEMIT_NO_OPERAND      ((uint8_t)0xFFu)   /* "no body / no onleave" — replaces inline 0xFFu (EMIT-023) */
#define UEMIT_JMP_BIAS        32768              /* used by emit_stmt + emit_expr (EMIT-024) */
#define UEMIT_REG_LIMIT       UFS_MAX_REGS       /* alias for clarity at exhaustion-guard sites (EMIT-025) */

#endif /* UEMIT_INTERNAL_H */
