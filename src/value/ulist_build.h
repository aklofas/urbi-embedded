/* SPDX-License-Identifier: BSD-3-Clause */
/* ulist_build.h — internal List C-builder for the ROS2 bridge.
 *
 * These three thin wrappers let C code construct urbiscript List objects
 * from incoming ROS2 sequence fields without touching the file-static
 * internals of stdlib/containers.c directly.
 *
 * All three are INTERNAL only — declared here (src/value/), NOT in the
 * public include/urbi/ API surface.  The ABI therefore stays PATCH.
 *
 * Gated by URBI_ENABLE_ROS2: the bodies live in stdlib/containers.c
 * inside a matching #ifdef block so the base build is byte-identical. */

#ifndef URBI_VALUE_ULIST_BUILD_H
#define URBI_VALUE_ULIST_BUILD_H

#ifdef URBI_ENABLE_ROS2

#include "urbi/types.h"   /* UValue */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

/* Create a new empty List object (clone of the List atom proto, fresh
 * backing storage).  Returns a UVAL_OBJECT UValue on success; returns
 * urbi_make_nil() on allocation failure (check .kind != UVAL_NIL). */
UValue urbi_list_create(struct UVM *vm);

/* Append value `v` to List object `lst`.
 * Returns 0 on success, -1 on allocation failure or if `lst` is not
 * a valid List object. */
int urbi_list_append(struct UVM *vm, UValue lst, UValue v);

/* Return the current number of elements in List object `lst`.
 * Returns -1 if `lst` is not a valid List object. */
int urbi_list_len(struct UVM *vm, UValue lst);

/* Return the element at index `i` (0-based) from List object `lst`.
 * Returns urbi_make_nil() if `i` is out of range or `lst` is invalid. */
UValue urbi_list_get(struct UVM *vm, UValue lst, int i);

#ifdef __cplusplus
}
#endif

#endif /* URBI_ENABLE_ROS2 */
#endif /* URBI_VALUE_ULIST_BUILD_H */
