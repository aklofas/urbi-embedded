/* SPDX-License-Identifier: BSD-3-Clause */
/* The pico-sdk default memory map, with the core-0 stack moved from the
 * 4 KB SCRATCH_Y bank to the top of main SRAM and sized at 32 KB.
 *
 * The eval service compiles whatever a client types, and the parser is
 * recursive: on this core one nesting level costs about 1 KB of stack.
 * The default 2 KB stack in SCRATCH_Y has nothing but the next bank
 * below it, so a deep line would walk into SCRATCH_X and then into the
 * heap with no check.  Here the stack sits above the heap and newlib's
 * _sbrk stops at __StackLimit, so the two cannot meet.
 *
 * memmap_default.ld and pico_flash_region.ld are found through the -L
 * paths CMakeLists.txt adds (the SDK's rp2040 crt0 directory and the
 * build directory, where the SDK generates the flash fragment). */
INCLUDE memmap_default.ld

__StackTop    = ORIGIN(RAM) + LENGTH(RAM);
__StackBottom = __StackTop - 0x8000;
__StackLimit  = __StackBottom;
__HeapLimit   = __StackLimit;
