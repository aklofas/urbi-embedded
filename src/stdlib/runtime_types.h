/* SPDX-License-Identifier: BSD-3-Clause */
/* runtime_types.h — the Exception prototype's method table and its one
 * init hook.  The subclass protos are table rows in rt/uboot.c. */

#ifndef URBI_STDLIB_RUNTIME_TYPES_H
#define URBI_STDLIB_RUNTIME_TYPES_H

#include "rt/ustdlib_glue.h"

enum { USTDLIB_EXCEPTION_NMETHODS = 2 };
extern const UMethodDef ustdlib_exception_methods[USTDLIB_EXCEPTION_NMETHODS];

int urbi_exception_init(UVM *vm, UObject *proto);

#endif
