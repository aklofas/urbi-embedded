/* SPDX-License-Identifier: BSD-3-Clause */
/* namespaces.h — the System and Global method tables, and the init hook
 * that installs the constant slots (Math.pi, System.Platform.kind) the
 * boot table has no column for. */

#ifndef URBI_STDLIB_NAMESPACES_H
#define URBI_STDLIB_NAMESPACES_H

#include "rt/ustdlib_glue.h"

enum { USTDLIB_SYSTEM_NMETHODS = 5 };
extern const UMethodDef ustdlib_system_methods[USTDLIB_SYSTEM_NMETHODS];
enum { USTDLIB_GLOBAL_NMETHODS = 1 };
extern const UMethodDef ustdlib_global_methods[USTDLIB_GLOBAL_NMETHODS];

int urbi_namespaces_init(UVM *vm);

#endif
