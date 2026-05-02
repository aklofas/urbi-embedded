/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for src/object/uic.h — UIC inline-cache record + tunable.
 *
 * The header carries a gated _Static_assert pinning sizeof(UIC) == 144 at
 * the default 4-entry / 64-bit-pointer build; if it trips this file won't
 * compile.  These runtime tests give a second, test-runner-visible signal
 * and additionally re-pin the URBI_SLOT_FLAG_* attribute bits for the
 * IC.flags summary defined alongside in uobject.h.
 *
 * T16 also covers UModuleInstance / UProtoInstance (the per-VM IC RAM
 * tier) — multi-instance independence is the load-bearing invariant. */

#include "utest.h"

#include "object/uic.h"
#include "object/umoduleinstance.h"
#include "object/uobject.h"
#include "uintern.h"
#include "umodule.h"
#include "uvm.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

UTEST(uic_layout_at_default_4_entries) {
    UASSERT_EQ(URBI_IC_ENTRIES_PER_SITE, 4);
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
    UASSERT_EQ((int)sizeof(UIC), 144);
#endif
}

UTEST(uic_flag_bits_distinct) {
    UASSERT_EQ((int)(URBI_SLOT_FLAG_OGET
                   | URBI_SLOT_FLAG_OSET
                   | URBI_SLOT_FLAG_CONSTANT
                   | URBI_SLOT_FLAG_LOCAL),
               0x0F);
}

/* === T16: UModuleInstance + UProtoInstance ===
 *
 * Two instances of the same UModule must hold independent IC tables.
 * Build a minimal module with one nested proto whose ic_count = 3 and
 * ic_names = [foo, bar, baz]; spin up two instances and verify
 *   1. both creates succeed and return distinct pointers
 *   2. each instance has its own UProtoInstanceArr (distinct addresses)
 *   3. each instance's IC tables are distinct allocations
 *   4. the IC name pointers match across both instances (same UModule)
 *   5. every IC entry is zero-initialised (topology_gen[e] == 0 sentinel) */

UTEST(module_instance_basic_create) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UModule m = {0};
    UProto *p = umodule_alloc_nested_proto(&m);
    UASSERT(p != NULL);

    /* Pretend the emitter populated three IC sites. */
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    USymbol *bar = (USymbol *)ustr_intern(&vm, "bar", 3);
    USymbol *baz = (USymbol *)ustr_intern(&vm, "baz", 3);
    UASSERT(foo != NULL); UASSERT(bar != NULL); UASSERT(baz != NULL);

    p->ic_count = 3;
    p->ic_names = (USymbol **)malloc(3 * sizeof(USymbol *));
    UASSERT(p->ic_names != NULL);
    p->ic_names[0] = foo;
    p->ic_names[1] = bar;
    p->ic_names[2] = baz;

    UModuleInstance *mi = urbi_module_instance_create(&vm, &m);
    UASSERT(mi != NULL);
    UASSERT(mi->module == &m);
    UASSERT(mi->vm == &vm);
    UASSERT(mi->proto_instances != NULL);

    /* n = 1 (root chunk) + 1 (nested proto) = 2 */
    UASSERT_EQ((int)mi->proto_instances->n, 2);

    /* entries[0]: root chunk — sentinel (proto NULL, ic_table NULL). */
    UASSERT(mi->proto_instances->entries[0].proto    == NULL);
    UASSERT(mi->proto_instances->entries[0].ic_table == NULL);

    /* entries[1]: the nested proto with ic_count = 3 and a real ic_table. */
    UProtoInstance *pi = &mi->proto_instances->entries[1];
    UASSERT(pi->proto    == p);
    UASSERT(pi->ic_table != NULL);

    /* Each IC site has its name copied from the proto, n == 0 (no fills),
     * and every entry zero-initialised — topology_gen[e] == 0 is the
     * pre-M4 §3.1 "unfilled" sentinel. */
    UASSERT(pi->ic_table[0].name == foo);
    UASSERT(pi->ic_table[1].name == bar);
    UASSERT(pi->ic_table[2].name == baz);
    for (int k = 0; k < 3; k++) {
        UASSERT_EQ((int)pi->ic_table[k].n,              0);
        UASSERT_EQ((int)pi->ic_table[k].replace_cursor, 0);
        for (int e = 0; e < URBI_IC_ENTRIES_PER_SITE; e++) {
            UASSERT(pi->ic_table[k].recv_shapes[e] == NULL);
            UASSERT(pi->ic_table[k].slots[e]       == NULL);
            UASSERT(pi->ic_table[k].uprops[e]      == NULL);
            UASSERT_EQ((int)pi->ic_table[k].topology_gen[e], 0);
            UASSERT_EQ((int)pi->ic_table[k].flags[e],        0);
        }
    }

    urbi_module_instance_destroy(&vm, mi);
    umodule_destroy(&m);
    uvm_destroy(&vm);
}

UTEST(module_instance_two_instances_independent) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UModule m = {0};
    UProto *p = umodule_alloc_nested_proto(&m);
    UASSERT(p != NULL);

    USymbol *alpha = (USymbol *)ustr_intern(&vm, "alpha", 5);
    USymbol *beta  = (USymbol *)ustr_intern(&vm, "beta",  4);
    p->ic_count = 2;
    p->ic_names = (USymbol **)malloc(2 * sizeof(USymbol *));
    UASSERT(p->ic_names != NULL);
    p->ic_names[0] = alpha;
    p->ic_names[1] = beta;

    UModuleInstance *mi_a = urbi_module_instance_create(&vm, &m);
    UModuleInstance *mi_b = urbi_module_instance_create(&vm, &m);
    UASSERT(mi_a != NULL);
    UASSERT(mi_b != NULL);

    /* Distinct UModuleInstance cells. */
    UASSERT(mi_a != mi_b);

    /* Distinct UProtoInstanceArr bulk allocations. */
    UASSERT(mi_a->proto_instances != mi_b->proto_instances);

    /* Distinct ic_table allocations (different memory regions). */
    UProtoInstance *pi_a = &mi_a->proto_instances->entries[1];
    UProtoInstance *pi_b = &mi_b->proto_instances->entries[1];
    UASSERT(pi_a->ic_table != pi_b->ic_table);

    /* Same UModule means matching IC names by pointer identity (interned). */
    UASSERT(pi_a->ic_table[0].name == pi_b->ic_table[0].name);
    UASSERT(pi_a->ic_table[0].name == alpha);
    UASSERT(pi_a->ic_table[1].name == pi_b->ic_table[1].name);
    UASSERT(pi_a->ic_table[1].name == beta);

    /* Mutate one IC entry and confirm the other instance is untouched —
     * the load-bearing per-VM independence invariant.  Use n + replace_cursor
     * (scalar fields) so we don't need to allocate UShape / USlot here. */
    pi_a->ic_table[0].n              = 1;
    pi_a->ic_table[0].replace_cursor = 1;
    UASSERT_EQ((int)pi_b->ic_table[0].n,              0);
    UASSERT_EQ((int)pi_b->ic_table[0].replace_cursor, 0);

    urbi_module_instance_destroy(&vm, mi_b);
    urbi_module_instance_destroy(&vm, mi_a);
    umodule_destroy(&m);
    uvm_destroy(&vm);
}

UTEST(module_instance_zero_nested_protos) {
    /* Edge case: module with no nested protos.  proto_instances should
     * still be allocated with n = 1 (just the root-chunk sentinel) and
     * zero IC bytes trailing the entries[] array. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UModule m = {0};

    UModuleInstance *mi = urbi_module_instance_create(&vm, &m);
    UASSERT(mi != NULL);
    UASSERT(mi->proto_instances != NULL);
    UASSERT_EQ((int)mi->proto_instances->n, 1);
    UASSERT(mi->proto_instances->entries[0].proto    == NULL);
    UASSERT(mi->proto_instances->entries[0].ic_table == NULL);

    urbi_module_instance_destroy(&vm, mi);
    umodule_destroy(&m);
    uvm_destroy(&vm);
}

UTEST(module_instance_proto_with_zero_ic_count) {
    /* Edge case: nested proto whose ic_count == 0.  ic_table should be
     * NULL for that entry; bulk allocation accounts for zero IC bytes
     * from this proto. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UModule m = {0};
    UProto *p = umodule_alloc_nested_proto(&m);
    UASSERT(p != NULL);
    /* Leave p->ic_count = 0, p->ic_names = NULL (zero-init from
     * umodule_alloc_nested_proto). */

    UModuleInstance *mi = urbi_module_instance_create(&vm, &m);
    UASSERT(mi != NULL);
    UASSERT_EQ((int)mi->proto_instances->n, 2);
    UASSERT(mi->proto_instances->entries[1].proto    == p);
    UASSERT(mi->proto_instances->entries[1].ic_table == NULL);

    urbi_module_instance_destroy(&vm, mi);
    umodule_destroy(&m);
    uvm_destroy(&vm);
}

UTEST(module_instance_invalid_args_return_null) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    UModule m = {0};
    UASSERT(urbi_module_instance_create(NULL, &m) == NULL);
    UASSERT(urbi_module_instance_create(&vm, NULL) == NULL);
    uvm_destroy(&vm);
}

void test_uic_suite(void) {
    utest_run("uic: layout at default 4 entries",
              uic_layout_at_default_4_entries);
    utest_run("uic: flag bits distinct",
              uic_flag_bits_distinct);
    utest_run("uic: module instance basic create",
              module_instance_basic_create);
    utest_run("uic: module instance two instances are independent",
              module_instance_two_instances_independent);
    utest_run("uic: module instance with zero nested protos",
              module_instance_zero_nested_protos);
    utest_run("uic: module instance with proto.ic_count == 0",
              module_instance_proto_with_zero_ic_count);
    utest_run("uic: module instance invalid args return NULL",
              module_instance_invalid_args_return_null);
}
