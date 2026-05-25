/* SPDX-License-Identifier: BSD-3-Clause */
/* src/runtime/uabi_guards.c — link-time symbols for build-flag mismatch detection.
 *
 * Each (flag, value) pair gets one extern const int symbol DEFINED here and
 * REFERENCED in the relevant public header under the active #if.  If the
 * embedder builds its application under one flag value and links against a
 * library built under another, the extern reference in the header resolves to
 * an undefined symbol — link fails with a self-documenting name.
 *
 * Guards provided:
 *   URBI_FLOAT_TYPE            — 4 (float/f32) or 8 (double/f64)
 *   URBI_REPL_COOPERATIVE_ONLY — defined or undefined
 *   URBI_BYTECODE_ONLY         — defined or undefined
 *
 * See docs/release/release-readiness.md §"Build-flag mismatch link-time guards"
 * and docs/internals/assertion-discipline.md §"Adoption Plan" for context.
 * Tracked under audit-1 F2, roadmap F7, repl-portability spec.
 *
 * URBI_INTERNAL_GUARD_REF suppresses the extern references that the public
 * headers inject into every including TU.  This TU defines the symbols, so it
 * must not consume them — that would make each reference self-referential and
 * defeat the mismatch detection. */

#define URBI_INTERNAL_GUARD_REF 1

/* urbi/types.h supplies the URBI_FLOAT_TYPE default (8 = double) when the
 * flag is not supplied via -D on the command line. */
#include "urbi/types.h"

/* === URBI_FLOAT_TYPE ===
 *
 * Default: 8 (double/f64) for hosted builds.  Cross targets opt in to
 * URBI_FLOAT_TYPE=4 (float/f32) via -DURBI_FLOAT_TYPE=4 in their CFLAGS.
 * Motivating incident: v0.8.2 STM32F4 bring-up — embedder built with
 * URBI_FLOAT_TYPE=8 (header default) while liburbi.a was built with
 * URBI_FLOAT_TYPE=4, silently zeroing every UVAL_FLOAT crossing the boundary. */
#if URBI_FLOAT_TYPE == 4
const int urbi_abi_requires_float_type_4 = 1;
#elif URBI_FLOAT_TYPE == 8
const int urbi_abi_requires_float_type_8 = 1;
#else
#  error "URBI_FLOAT_TYPE must be 4 (float/f32) or 8 (double/f64)"
#endif

/* === URBI_REPL_COOPERATIVE_ONLY ===
 *
 * Cooperative-only builds (bare-metal Pico, STM32, etc.) omit the pthread
 * listener and change struct layouts in urepl_threading.h.  Embedders MUST
 * link with the same setting the library was compiled with. */
#ifdef URBI_REPL_COOPERATIVE_ONLY
const int urbi_abi_requires_repl_cooperative_only = 1;
#else
const int urbi_abi_requires_repl_pthread = 1;
#endif

/* === URBI_BYTECODE_ONLY ===
 *
 * Bytecode-only builds strip src/lex/, src/parse/, src/emit/ from the
 * archive.  Embedders that include <urbi/urbi.h> expecting urbi_compile_source
 * or urbi_repl_eval and link against a bytecode-only library get a link error
 * naming the flag mismatch. */
#ifdef URBI_BYTECODE_ONLY
const int urbi_abi_requires_bytecode_only = 1;
#else
const int urbi_abi_requires_full_parser = 1;
#endif
