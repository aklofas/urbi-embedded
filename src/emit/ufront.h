/* SPDX-License-Identifier: BSD-3-Clause */
/* src/emit/ufront.h — the single entry point the runtime core calls into
 * the compiler frontend through.
 *
 * The new core (src/rt/) never touches ULexer, UParser or UEmitter
 * directly: ufront_compile is the whole lex -> parse -> emit pipeline
 * behind one function, so the frontend can be swapped or stripped
 * (URBI_BYTECODE_ONLY) without the core noticing.
 *
 * Intern seam.  The emitter interns every identifier and string literal
 * through ustr_intern(vm, bytes, n), declared by src/value/uintern.h.
 * That function is implemented HERE, over the new core's USym table
 * (rt/ustr.h), returning the interned symbol's NUL-terminated bytes.  So
 * there is exactly one string table in the process: a `const char *` an
 * emit-time constant holds is a USym's payload, and uproto_bind's
 * re-intern of those bytes returns that same USym.  The old
 * open-addressing table in src/value/uintern.c is no longer built. */

#ifndef UFRONT_H
#define UFRONT_H

#include <stddef.h>
#include <stdint.h>

struct UVM;
struct UProto;

/* Compile `src` into a fresh heap-allocated root UProto.
 *
 * On success returns URBI_OK and writes the root through *out; the caller
 * owns it and must hand it to uproto_bind (which takes ownership) or
 * release it with uchunk_destroy.
 *
 * `name` is the source name used in diagnostics and in runtime error
 * prefixes; NULL leaves it unset, which renders as "<stdin>" — the
 * spelling the .chk corpus pins for REPL input.
 *
 * On failure returns URBI_ERR_COMPILE or URBI_ERR_OOM, writes a
 * positioned diagnostic into `err` (when errcap > 0), and leaves *out
 * NULL with nothing for the caller to free. */
int ufront_compile(struct UVM *vm, const char *src, size_t n, const char *name,
                   struct UProto **out, char *err, size_t errcap);

/* Serialize a compiled chunk to the on-disk bytecode format.  Returns
 * the byte count, or a negative value on failure; pass (NULL, 0) to
 * query the required size.  Thin pass-through to the kept encoder. */
ptrdiff_t ufront_serialize(struct UProto *root, unsigned char *buf, size_t cap);

/* Disassemble one bound-or-unbound chunk to stdout (hosted only).  Thin
 * pass-through to the kept disassembler so tools/ does not need the
 * emitter's internal headers. */
void ufront_disassemble(struct UProto *root, const char *name);

#endif
