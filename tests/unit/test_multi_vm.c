/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include <stdlib.h>
#include <string.h>

#include "uarena.h"
#include "uemit.h"
#include "umodule.h"
#include "uvm.h"

#define UTEST(name) static void name(void)

/* --- 2 cases land at this task; 6 more added at T18. --- */

UTEST(uvm_init_zeroes_intern_table_and_topology_gen) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    UASSERT(vm.intern_table == NULL);
    UASSERT_EQ((uint32_t)0, vm.topology_gen);
    uvm_destroy(&vm);
}

UTEST(umodule_origin_vm_initially_null) {
    UModule m = {0};
    UASSERT(m.origin_vm == NULL);
    /* deserialize zeros it; serialize never includes it */
    umodule_destroy(&m);
}

void test_multi_vm_suite(void) {
    utest_run("UVM init zeroes intern_table and topology_gen",
        uvm_init_zeroes_intern_table_and_topology_gen);
    utest_run("UModule origin_vm initially NULL",
        umodule_origin_vm_initially_null);
    /* TODO T18: 6 more cases — allocator isolation, intern isolation,
       last_error isolation, module-binding-stamping, topology_gen-per-VM,
       alternating uvm_run multi-VM. */
}
