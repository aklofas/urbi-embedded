/* SPDX-License-Identifier: BSD-3-Clause */
/* atoms.h — the per-atom-family method tables.
 *
 * Boolean, Integer, Float and String each get one table; uboot_table
 * points its matching row at them.  A fifth name would mean a fifth atom
 * family, which the UP_* enum would have to grow first. */

#ifndef URBI_STDLIB_ATOMS_H
#define URBI_STDLIB_ATOMS_H

#include "rt/ustdlib_glue.h"

enum { USTDLIB_BOOL_NMETHODS = 2 };
extern const UMethodDef ustdlib_bool_methods[USTDLIB_BOOL_NMETHODS];
enum { USTDLIB_INT_NMETHODS = 12 };
extern const UMethodDef ustdlib_int_methods[USTDLIB_INT_NMETHODS];
enum { USTDLIB_FLOAT_NMETHODS = 23 };
extern const UMethodDef ustdlib_float_methods[USTDLIB_FLOAT_NMETHODS];
enum { USTDLIB_STRING_NMETHODS = 19 };
extern const UMethodDef ustdlib_string_methods[USTDLIB_STRING_NMETHODS];

#endif
