/* SPDX-License-Identifier: BSD-3-Clause */
/* include/urbi/urobotics.h — optional Standard Robotics API facet overlay
 * (URBI_ENABLE_UROBOTICS).  Pure-urbiscript facets baked into a separate
 * bytecode blob; the core VM has no reference to it. */
#ifndef URBI_UROBOTICS_H
#define URBI_UROBOTICS_H

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility push(default)   /* v1.0: export only public-header symbols */
#endif

#ifdef URBI_ENABLE_UROBOTICS

#include <stddef.h>

struct UVM;
struct URealm;

/* The baked overlay blob (defined by src/urobotics/urobotics_bytecode.gen.c). */
extern const unsigned char urbi_urobotics_bytecode[];
extern const size_t        urbi_urobotics_bytecode_len;

/* Deserialize the baked urobotics overlay blob and cache the module on the VM.
 * Idempotent. Called from urbi_stdlib_boot after the main stdlib blob loads. */
int urbi_urobotics_register(struct UVM *vm);

/* Run the overlay's root chunk under `realm` so its top-level statements
 * install the `Robotics` global. Called from urbi_populate_realm_globals after
 * the main stdlib chunk has run. No-op if the overlay was never registered. */
int urbi_urobotics_run(struct UVM *vm, struct URealm *realm);

#endif /* URBI_ENABLE_UROBOTICS */

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif
#endif /* URBI_UROBOTICS_H */
