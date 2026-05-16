/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_demo_main — app_main boot sequence.
 *
 * Placeholder TU.  T39/T40 fill this in with:
 *   - destructure_blob (12-byte payload -> 3 UValue ints)
 *   - app_main (urbi_vm_init -> port hooks -> events -> host fns ->
 *     bytecode load -> peripherals -> task spawn)
 *
 * Existing as a stub at T38 only so that CMake configure can succeed
 * — the SRCS list references this file, and ESP-IDF errors out at
 * configure time if a listed source doesn't exist.  The link step will
 * still fail until T40 lands app_main, which is the expected
 * pre-Phase-5-completion state. */
