/* SPDX-License-Identifier: BSD-3-Clause */
/* Cross-strategy compile smoke: URBI_GC_NONE barrier inlines produce no code.
 *
 * Compiled with -DURBI_GC=URBI_GC_NONE (== -DURBI_GC=2).  Instantiates all
 * three no-op barrier surfaces to confirm they compile clean and (on
 * optimizing compilers) inline to nothing.
 *
 * Assembly inspection (-S + grep for barrier symbol) is a T46 / diagnostic
 * exercise.  At M3 the acceptance criterion is simply: this file compiles.
 *
 * Row 10 acceptance #9.  T28. */

#include "urbi/gc.h"

/* Dummy values that satisfy the type signatures without UVM internals. */
static UCell  g_parent;
static UValue g_child;

/* Call each barrier surface once so the inlines are instantiated. */
static void exercise_barriers(void)
{
    /* Under URBI_GC_NONE these are no-op stubs; the compiler folds them away. */
    urbi_gc_slot_write(NULL, &g_parent, 0U, g_child);
    urbi_gc_register_write(NULL, NULL, 0U, g_child);
    urbi_gc_upvalue_write(NULL, NULL, 0U, g_child);
}

int main(void)
{
    exercise_barriers();
    return 0;
}
