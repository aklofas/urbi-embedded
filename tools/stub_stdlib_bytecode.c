/* SPDX-License-Identifier: BSD-3-Clause */
/* tools/stub_stdlib_bytecode.c
 *
 * Linked into tools/urbi-compile-stdlib in place of
 * src/stdlib/urbi_stdlib_bytecode.gen.o.  Without it the bake tool would
 * depend on the file it produces:
 *
 *     liburbi.a -> .gen.o -> .gen.c -> bake tool -> liburbi.a
 *
 * uboot_init treats a zero-length blob as "no script overlay" and boots
 * the C half normally, which is everything the tool needs -- it only
 * ever compiles.
 */
#include <stddef.h>

const unsigned char urbi_stdlib_bytecode[1] = { 0 };
const size_t        urbi_stdlib_bytecode_len = 0;
