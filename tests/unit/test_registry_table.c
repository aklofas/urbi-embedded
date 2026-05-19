/* SPDX-License-Identifier: BSD-3-Clause */
/* T69: Static built-in registry — 15 entries. */

#include "utest.h"

#include <stddef.h>

#include "realm/urealm_globals.h"
#include "vm/uvm.h"
#include "chunk/umodule.h"

#define UTEST(name) static void name(void)

UTEST(registry_has_15_entries) {
    UASSERT_EQ(15, (int)urbi_builtin_registry_count);
}

UTEST(registry_first_entry_is_object) {
    UASSERT_STR_EQ("Object", urbi_builtin_registry[0].name);
    UASSERT(urbi_builtin_registry[0].is_const == true);
    UASSERT(urbi_builtin_registry[0].resolver != NULL);
}

UTEST(registry_all_entries_const) {
    size_t i;
    for (i = 0; i < urbi_builtin_registry_count; i++) {
        UASSERT(urbi_builtin_registry[i].is_const == true);
    }
}

UTEST(registry_resolves_object_proto_to_singleton) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue v = urbi_builtin_registry[0].resolver(&vm);
    UASSERT_EQ(UVAL_OBJECT, (int)v.kind);
    UASSERT(v.v.p == vm.atom_object);
    urbi_vm_destroy(&vm);
}

UTEST(registry_nil_entry_is_fourteenth) {
    /* "nil" is the 14th entry (index 13). */
    UASSERT_STR_EQ("nil", urbi_builtin_registry[13].name);
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue v = urbi_builtin_registry[13].resolver(&vm);
    UASSERT_EQ(UVAL_NIL, (int)v.kind);
    urbi_vm_destroy(&vm);
}

UTEST(registry_void_entry_is_fifteenth) {
    /* "void" is the 15th entry (index 14). */
    UASSERT_STR_EQ("void", urbi_builtin_registry[14].name);
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue v = urbi_builtin_registry[14].resolver(&vm);
    UASSERT_EQ(UVAL_VOID, (int)v.kind);
    urbi_vm_destroy(&vm);
}

void
test_registry_table_suite(void)
{
    utest_run("registry has 15 entries",
              registry_has_15_entries);
    utest_run("registry first entry is Object",
              registry_first_entry_is_object);
    utest_run("registry all entries const",
              registry_all_entries_const);
    utest_run("registry resolves Object proto to singleton",
              registry_resolves_object_proto_to_singleton);
    utest_run("registry nil entry is fourteenth",
              registry_nil_entry_is_fourteenth);
    utest_run("registry void entry is fifteenth",
              registry_void_entry_is_fifteenth);
}
