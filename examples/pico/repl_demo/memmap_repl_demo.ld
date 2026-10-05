/* SPDX-License-Identifier: BSD-3-Clause */
/* The pico-sdk default memory map, with the core-0 stack moved from the
 * 4 KB SCRATCH_Y bank to the top of main SRAM and sized at 64 KB.
 *
 * The eval service compiles whatever a client types, and the parser is
 * recursive.  Measured on a Cortex-M qemu target through the service
 * with this firmware's configuration: `1+1` alone touches 16,880 B of
 * stack, and the worst static path per expression-nesting level on
 * Cortex-M0+ is 2,976 B, so the 12 levels the session's compile budget
 * allows take 52,592 B and leave about 12.9 KB.  Statement and brace
 * nesting are not counted by that budget; the session's 1 KiB source cap
 * bounds them for ordinary lines.  The SDK installs a 32-byte no-access
 * guard at __StackBottom (PICO_USE_STACK_GUARDS, CMakeLists.txt), which
 * turns most overruns into a hard fault and the LED error pattern; a
 * frame larger than 32 B can step over it, so a deep enough line can
 * still reach the heap until the parser counts statement nesting.
 *
 * The stack sits above the heap and newlib's _sbrk stops at
 * __StackLimit, so the two cannot meet by allocation either.
 *
 * memmap_default.ld and pico_flash_region.ld are found through the -L
 * paths CMakeLists.txt adds (the SDK's rp2040 crt0 directory and the
 * build directory, where the SDK generates the flash fragment). */
INCLUDE memmap_default.ld

__StackTop    = ORIGIN(RAM) + LENGTH(RAM);
__StackBottom = __StackTop - 0x10000;
__StackLimit  = __StackBottom;
__HeapLimit   = __StackLimit;

/* The SDK only checks __StackLimit >= __HeapLimit, which the lines above
 * make true by construction; this checks that the statics and the
 * minimum heap the SDK reserves in .heap end below the stack. */
ASSERT(ADDR(.heap) + SIZEOF(.heap) <= __StackLimit,
       "statics and the minimum heap overlap the 64 KB stack")
