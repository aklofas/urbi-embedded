/* SPDX-License-Identifier: BSD-3-Clause */
/* object_root.h — the Object root's method table.
 *
 * The file used to carry the shared native-method installer and the
 * raise helpers as well; both moved to rt/ustdlib_glue.h when the boot
 * table took over registration.  What is left is the one thing a stdlib
 * file now exports: its table, for uboot_table to point at. */

#ifndef URBI_STDLIB_OBJECT_ROOT_H
#define URBI_STDLIB_OBJECT_ROOT_H

#include "rt/ustdlib_glue.h"

enum { USTDLIB_OBJECT_NMETHODS = 19 };
extern const UMethodDef ustdlib_object_methods[USTDLIB_OBJECT_NMETHODS];

#endif
