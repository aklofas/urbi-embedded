/* SPDX-License-Identifier: BSD-3-Clause */
/* tools/stub_stdlib_bytecode.c
 *
 * Stub linked into tools/urbi-compile-stdlib in place of the real
 * src/stdlib/urbi_stdlib_bytecode.gen.o.  Provides 0-length stdlib
 * blob symbols so the bake tool's urbi_vm_init call can run without
 * pulling .gen.o into its link — that would form a build cycle (the
 * bake tool produces the .gen.c that .gen.o is built from).
 *
 * urbi_stdlib_boot gates its work on urbi_stdlib_bytecode_len > 0
 * (src/stdlib/stdlib_boot.c), so the 0-length blob is a clean no-op.
 * The bake tool only invokes urbi_compile_source on its input .u
 * sources; it never needs a populated stdlib at compile time.
 */
#include <stddef.h>

const unsigned char urbi_stdlib_bytecode[1]   = {0};
const size_t        urbi_stdlib_bytecode_len = 0;
