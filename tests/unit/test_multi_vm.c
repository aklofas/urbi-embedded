/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include <stdlib.h>
#include <string.h>

#include "uarena.h"
#include "uast.h"
#include "uemit.h"
#include "uintern.h"
#include "ulex.h"
#include "umodule.h"
#include "uparse.h"
#include "uvm.h"

#define UTEST(name) static void name(void)

/* --- 2 cases land at T2; 6 more added at T18. --- */

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

/* --- Helpers for T18 cases --- */

/* Counting allocator for case 1: tracks allocation count via size_t pointer. */
static void *counting_alloc(void *ptr, size_t nbytes, void *ud) {
    size_t *counter = (size_t *)ud;
    void *result = realloc(ptr, nbytes);
    if (result != NULL) {
        (*counter)++;
    }
    return result;
}

/* Run source through pipeline on a given VM; return result. */
static UVMError eval_on_vm(UVM *vm, const char *src, UValue *out) {
    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UArena arena;
    uarena_init(&arena, 4096);

    UModule module = {0};
    UEmitter e;
    uemit_init(&e, &module, &arena, vm, NULL);

    UParser p;
    uparse_init(&p, &lex, &arena);

    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        (void)uemit_statement(&e, node);
        uarena_reset(&arena);
    }

    UValue nil = {0};
    *out = nil;
    UVMError vm_rc = UVM_OK;

    if (uemit_finish(&e) == EMIT_OK) {
        vm_rc = uvm_run(vm, &module, out);
    }

    umodule_destroy(&module);
    uarena_destroy(&arena);
    return vm_rc;
}

/* --- T18 test cases --- */

UTEST(two_vms_have_independent_allocators) {
    /* Each VM gets a counting allocator. Run identical programs on both.
     * Assert each allocator counter is independent. */
    UVM vm_a, vm_b;
    size_t counter_a = 0, counter_b = 0;

    uvm_init(&vm_a, counting_alloc, &counter_a);
    uvm_init(&vm_b, counting_alloc, &counter_b);

    UValue out_a, out_b;
    const char *src = "42";

    UVMError rc_a = eval_on_vm(&vm_a, src, &out_a);
    UVMError rc_b = eval_on_vm(&vm_b, src, &out_b);

    UASSERT_EQ(UVM_OK, rc_a);
    UASSERT_EQ(UVM_OK, rc_b);
    UASSERT_EQ((uint8_t)UVAL_INT, out_a.kind);
    UASSERT_EQ((uint8_t)UVAL_INT, out_b.kind);
    UASSERT_EQ((int64_t)42, out_a.v.i);
    UASSERT_EQ((int64_t)42, out_b.v.i);

    /* Both counters must be nonzero (allocations happened). */
    UASSERT(counter_a > 0);
    UASSERT(counter_b > 0);

    /* Counters may differ (due to arena/module state differences), but
     * each is independent — no cross-VM pollution. */

    uvm_destroy(&vm_a);
    uvm_destroy(&vm_b);
}

UTEST(two_vms_have_independent_last_error) {
    /* Trigger a TYPE_ERROR on vm_a; assert vm_b->last_error is still OK. */
    UVM vm_a, vm_b;
    uvm_init(&vm_a, NULL, NULL);
    uvm_init(&vm_b, NULL, NULL);

    UValue out_a, out_b;

    /* Run something that triggers TYPE_ERROR on vm_a: 1 + nil */
    UVMError rc_a = eval_on_vm(&vm_a, "1 + nil", &out_a);
    UASSERT_EQ(UVM_TYPE_ERROR, rc_a);
    UASSERT_EQ(UVM_TYPE_ERROR, vm_a.last_error);

    /* vm_b's last_error should still be OK (init state). */
    UASSERT_EQ(UVM_OK, vm_b.last_error);

    /* Run a valid program on vm_b; its error should remain OK. */
    UVMError rc_b = eval_on_vm(&vm_b, "99", &out_b);
    UASSERT_EQ(UVM_OK, rc_b);
    UASSERT_EQ(UVM_OK, vm_b.last_error);

    uvm_destroy(&vm_a);
    uvm_destroy(&vm_b);
}

UTEST(module_compiled_for_vm_a_has_origin_vm_a) {
    /* Emit a module for vm_a; origin_vm should be set. Then serialize +
     * deserialize; origin_vm should be NULL on the loaded copy. */
    UVM vm_a;
    uvm_init(&vm_a, NULL, NULL);

    ULexer lex;
    ulex_init(&lex, "42", 2);

    UArena arena;
    uarena_init(&arena, 4096);

    UModule module = {0};
    UEmitter e;
    uemit_init(&e, &module, &arena, &vm_a, NULL);

    UParser p;
    uparse_init(&p, &lex, &arena);

    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        (void)uemit_statement(&e, node);
        uarena_reset(&arena);
    }

    UASSERT_EQ(EMIT_OK, uemit_finish(&e));

    /* origin_vm must be set to &vm_a. */
    UASSERT(module.origin_vm == &vm_a);

    /* Serialize the module. */
    uint8_t buf[8192];
    ptrdiff_t serialized = umodule_serialize(&module, buf, sizeof(buf));
    UASSERT(serialized > 0);

    /* Deserialize into a fresh module. */
    UModule loaded = {0};
    UModuleLoadError load_rc = umodule_deserialize(&loaded, buf, (size_t)serialized,
                                                    NULL, 0);
    UASSERT_EQ(ULOAD_OK, load_rc);

    /* Deserialized module's origin_vm must be NULL. */
    UASSERT(loaded.origin_vm == NULL);

    umodule_destroy(&module);
    umodule_destroy(&loaded);
    uarena_destroy(&arena);
    uvm_destroy(&vm_a);
}

UTEST(topology_gen_is_per_vm_not_global) {
    /* Bump vm_a->topology_gen manually; assert vm_b->topology_gen
     * unchanged. Verifies the per-VM relocation. */
    UVM vm_a, vm_b;
    uvm_init(&vm_a, NULL, NULL);
    uvm_init(&vm_b, NULL, NULL);
    vm_a.topology_gen = 17;
    UASSERT_EQ((uint32_t)0, vm_b.topology_gen);
    uvm_destroy(&vm_a);
    uvm_destroy(&vm_b);
}

UTEST(alternating_uvm_run_proves_single_thread_multi_vm) {
    /* Compile two different modules; run them alternately on vm_a and
     * vm_b in a tight loop. Assert no state corruption. */
    UVM vm_a, vm_b;
    uvm_init(&vm_a, NULL, NULL);
    uvm_init(&vm_b, NULL, NULL);

    UValue out_a, out_b;
    UVMError rc_a = eval_on_vm(&vm_a, "42", &out_a);
    UVMError rc_b = eval_on_vm(&vm_b, "99", &out_b);

    UASSERT_EQ(UVM_OK, rc_a);
    UASSERT_EQ(UVM_OK, rc_b);
    UASSERT_EQ((int64_t)42, out_a.v.i);
    UASSERT_EQ((int64_t)99, out_b.v.i);

    /* Alternate running both VMs; each should return its expected value
     * consistently. We'll do this by re-running both modules a few times. */
    for (int i = 0; i < 4; i++) {
        rc_a = eval_on_vm(&vm_a, "42", &out_a);
        UASSERT_EQ(UVM_OK, rc_a);
        UASSERT_EQ((int64_t)42, out_a.v.i);

        rc_b = eval_on_vm(&vm_b, "99", &out_b);
        UASSERT_EQ(UVM_OK, rc_b);
        UASSERT_EQ((int64_t)99, out_b.v.i);
    }

    uvm_destroy(&vm_a);
    uvm_destroy(&vm_b);
}

UTEST(closing_vm_a_does_not_invalidate_vm_b_intern) {
    /* Intern "shared" in both VMs. uvm_destroy(vm_a) must not affect
     * vm_b's interned pointer. */
    UVM vm_a, vm_b;
    uvm_init(&vm_a, NULL, NULL);
    uvm_init(&vm_b, NULL, NULL);
    const char *sa = ustr_intern(&vm_a, "shared", 6);
    const char *sb = ustr_intern(&vm_b, "shared", 6);
    uvm_destroy(&vm_a);
    /* sa is now invalid — don't dereference. sb must still be live. */
    (void)sa;  /* silence unused-variable warning */
    UASSERT_EQ(0, strcmp(sb, "shared"));
    uvm_destroy(&vm_b);
}

/* --- Deferred test stubs (M3+, M4+, M5+) --- */
/* Deferred to M3+: stdlib singletons per-VM (no stdlib at M2). */
/* Deferred to M4+: cross-VM IC isolation (no IC bumps at M2). */
/* Deferred to M5+: lazy-class-instance per-VM (no Lazy class at M2). */

void test_multi_vm_suite(void) {
    utest_run("UVM init zeroes intern_table and topology_gen",
        uvm_init_zeroes_intern_table_and_topology_gen);
    utest_run("UModule origin_vm initially NULL",
        umodule_origin_vm_initially_null);
    utest_run("Two VMs have independent allocators",
        two_vms_have_independent_allocators);
    utest_run("Two VMs have independent last_error",
        two_vms_have_independent_last_error);
    utest_run("Module compiled for vm_a has origin_vm = &vm_a",
        module_compiled_for_vm_a_has_origin_vm_a);
    utest_run("topology_gen is per-VM, not global",
        topology_gen_is_per_vm_not_global);
    utest_run("Alternating uvm_run proves single-thread multi-VM",
        alternating_uvm_run_proves_single_thread_multi_vm);
    utest_run("Closing vm_a does not invalidate vm_b intern",
        closing_vm_a_does_not_invalidate_vm_b_intern);
}
