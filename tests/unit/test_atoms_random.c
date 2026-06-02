/* SPDX-License-Identifier: BSD-3-Clause */
/* test_atoms_random — Float.random() returns a Float in [0, 1).
 *
 * random() is non-deterministic in value (xorshift64 PRNG advancing a
 * file-static seed), so it is verified by range invariant over many draws
 * rather than by a fixed expected value in a .chk fixture.  100 draws cover
 * the full mantissa range without flaking. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "value/uvalue.h"
#include "vm/uvm.h"

#include <stdint.h>

#define UTEST(name) static void name(void)

UTEST(random_in_unit_interval) {
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    int i;
    for (i = 0; i < 100; i++) {
        UValue out = utest_e2e_make_nil();
        int rc = utest_e2e_compile_and_run(&vm, "Float.random()", &out);
        UASSERT_EQ(0, rc);
        UASSERT_EQ((int)UVAL_FLOAT, (int)out.kind);
        UASSERT((double)out.v.f >= 0.0);
        UASSERT((double)out.v.f < 1.0);
    }

    urbi_vm_destroy(&vm);
}

void test_atoms_random_suite(void);
void test_atoms_random_suite(void)
{
    printf("test_atoms_random\n");
    utest_run("random_in_unit_interval", random_in_unit_interval);
}
