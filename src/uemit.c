/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode emitter. */

#include "uemit.h"

#include <stddef.h>

/* Public-API stubs — filled in by later tasks. */

void uemit_init(Emitter *e, Chunk *chunk, Arena *arena, const char *source_name) {
    (void)e;
    (void)chunk;
    (void)arena;
    (void)source_name;
}

EmitError uemit_statement(Emitter *e, AstNode *stmt) {
    (void)e;
    (void)stmt;
    return EMIT_OK;
}

EmitError uemit_finish(Emitter *e) {
    (void)e;
    return EMIT_OK;
}

const char *uemit_error_name(EmitError code) {
    (void)code;
    return "EMIT_UNKNOWN";
}

size_t uemit_disassemble(const Chunk *chunk, char *buf, size_t cap) {
    (void)chunk;
    if (cap > 0 && buf != NULL) buf[0] = '\0';
    return 0;
}

ptrdiff_t uchunk_serialize(const Chunk *chunk, uint8_t *buf, size_t cap) {
    (void)chunk;
    (void)buf;
    (void)cap;
    return 0;
}
