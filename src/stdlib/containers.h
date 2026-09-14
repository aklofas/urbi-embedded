/* SPDX-License-Identifier: BSD-3-Clause */
/* containers.h — the container prototypes' method tables.
 *
 *   List    — mutable, growable; a UList cell
 *   Dict    — mutable, keyed; a UDict cell
 *   Tuple   — a UList cell carrying the Tuple prototype, so it reads but
 *             has no mutators
 *   Pair    — plain object with `first` / `second` slots
 *   Triplet — plain object with `first` / `second` / `third` slots
 *
 * Storage is the runtime's own: rt/ulist.h's UList and UDict are GC
 * cells, traced and swept like everything else.  The v1.0 core kept the
 * backing buffer in a hidden `_storage` slot outside the heap and freed
 * it only at VM teardown, which meant every List a program built leaked
 * until the VM went away; tests/rt/test_leaks.c is the probe that keeps
 * that from coming back. */

#ifndef URBI_STDLIB_CONTAINERS_H
#define URBI_STDLIB_CONTAINERS_H

#include "rt/ustdlib_glue.h"

enum { USTDLIB_PAIR_NMETHODS = 1 };
extern const UMethodDef ustdlib_pair_methods[USTDLIB_PAIR_NMETHODS];

enum { USTDLIB_TRIPLET_NMETHODS = 1 };
extern const UMethodDef ustdlib_triplet_methods[USTDLIB_TRIPLET_NMETHODS];

enum { USTDLIB_TUPLE_NMETHODS = 3 };
extern const UMethodDef ustdlib_tuple_methods[USTDLIB_TUPLE_NMETHODS];

enum { USTDLIB_LIST_NMETHODS = 16 };
extern const UMethodDef ustdlib_list_methods[USTDLIB_LIST_NMETHODS];

enum { USTDLIB_DICT_NMETHODS = 10 };
extern const UMethodDef ustdlib_dict_methods[USTDLIB_DICT_NMETHODS];

#endif
