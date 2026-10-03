/* The stdlib overlay is empty until src/stdlib/stdlib.u is re-baked on the
 * new emitter; uboot_init treats a zero-length blob as "no script overlay". */
#include <stddef.h>

const unsigned char urbi_stdlib_bytecode[1] = { 0 };
const size_t        urbi_stdlib_bytecode_len = 0;
