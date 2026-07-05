/* SPDX-License-Identifier: BSD-3-Clause */
/* regexp.h — RegExp atom-backed type (v1.0 stdlib-completeness).
 *
 * A new atom-backed type following the Mutex / Date precedent
 * (src/stdlib/primitives.c): a vanilla URBI_ATOM_OBJECT proto stashed in
 * vm->regexp_proto, GC-reachable via object_roots_walker, with native
 * methods installed by a per-type method table.
 *
 *   RegExp.new(pattern)  — clones vm->regexp_proto and stores the pattern
 *                          in a hidden `_pattern` UVAL_STR slot.
 *   r.test(s)            — Boolean: does the pattern match anywhere in s.
 *   r.match(s)           — alias of test at v1.0 (no capture groups; that
 *                          is a v1.x follow-up).
 *
 * The matcher is a compact freestanding backtracking engine — literal
 * chars, `.`, `*`, `+`, `?`, `^`, `$`, and char classes `[abc]` /
 * `[^abc]` / `[a-z]`.  No <regex.h>, no <stdlib.h>.
 *
 * Boot order: urbi_stdlib_register_regexp(vm) is called from
 * urbi_stdlib_boot AFTER the primitives.  Realm-global binding for the
 * RegExp name is deferred to urbi_stdlib_register_regexp_globals (called
 * by urbi_populate_realm_globals after the registry loop), mirroring the
 * primitive post-loop pattern so the registry's slot 0..7 layout stays
 * intact. */

#ifndef URBI_STDLIB_REGEXP_H
#define URBI_STDLIB_REGEXP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct URealm;

/* Allocates vm->regexp_proto, installs its native methods, and defaults
 * the hidden `_pattern` slot.  Idempotent — guarded by vm->stdlib_booted
 * upstream.  Returns URBI_OK on success or URBI_ERR_OOM on alloc failure. */
int urbi_stdlib_register_regexp(struct UVM *vm);

/* Post-registry hook: binds RegExp as a realm global on `realm`.  Lands
 * at slots 15+, past the v1.0 packed-flag CONSTANT enforcement range.
 * Returns URBI_OK / URBI_ERR_OOM / URBI_ERR_INVALID_ARG. */
int urbi_stdlib_register_regexp_globals(struct UVM *vm, struct URealm *realm);

/* Exposed for direct unit testing of the matcher.
 * Returns:
 *   1   pattern matches somewhere in [s, s_end)
 *   0   no match
 *  -1   budget exhausted (step limit or depth cap hit); the caller
 *       (regexp_do_test) converts this into a catchable RangeError. */
int urbi_regexp_search(const char *re, size_t relen,
                       const char *s, const char *s_end);

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_REGEXP_H */
