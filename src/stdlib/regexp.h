/* SPDX-License-Identifier: BSD-3-Clause */
/* regexp.h — the RegExp method table and its init hook.
 *
 * The matcher itself (a compact backtracking engine with a step budget)
 * is private to regexp.c; only the table and the default-pattern seed
 * cross the file boundary. */

#ifndef URBI_STDLIB_REGEXP_H
#define URBI_STDLIB_REGEXP_H

#include "rt/ustdlib_glue.h"

enum { K_REGEXP_NMETHODS = 3 };
extern const UMethodDef k_regexp_methods[K_REGEXP_NMETHODS];

int urbi_regexp_init(UVM *vm, UObject *proto);

#endif
