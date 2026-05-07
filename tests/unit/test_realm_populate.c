/* SPDX-License-Identifier: BSD-3-Clause */
/* T70: urbi_realm_create populates 15 built-in globals onto global_object. */

#include "utest.h"

#include <stddef.h>
#include <stdlib.h>

#include "vm/uvm.h"
#include "realm/urealm.h"
#include "realm/urealm_globals.h"
#include "value/uintern.h"
#include "object/uobject.h"
#include "object/ushape.h"

#define UTEST(name) static void name(void)

/* Spy allocator for OOM injection. */
typedef struct {
    int alloc_calls;
    int fail_at;        /* fail when alloc_calls > fail_at; -1 = never */
} RealmSpy;

static void *
realm_spy_alloc(void *ptr, size_t n, void *ud)
{
    RealmSpy *spy = (RealmSpy *)ud;
    if (n > 0 && ptr == NULL) {
        spy->alloc_calls++;
        if (spy->fail_at >= 0 && spy->alloc_calls > spy->fail_at)
            return NULL;
    }
    if (n == 0) { free(ptr); return NULL; }
    return realloc(ptr, n);
}

/* Helper: strlen without <string.h>. */
static size_t
rp_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Helper: look up a slot by name on an object (proto-walk). */
static int
local_lookup(UVM *vm, UObject *obj, const char *name, UValue *out)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, name, rp_strlen(name));
    if (sym == NULL) return -1;
    return urbi_object_lookup(vm, obj, sym, out);
}

/* === Test cases === */

UTEST(realm_create_global_object_is_not_null) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT(r->global_object != NULL);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

UTEST(realm_create_populates_object_global) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UValue v;
    int rc = local_lookup(&vm, r->global_object, "Object", &v);
    UASSERT_EQ(0, rc);
    UASSERT_EQ(UVAL_OBJECT, (int)v.kind);
    UASSERT(v.v.p == vm.atom_object);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

UTEST(realm_create_populates_tag_global) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UValue v;
    int rc = local_lookup(&vm, r->global_object, "Tag", &v);
    UASSERT_EQ(0, rc);
    UASSERT_EQ(UVAL_OBJECT, (int)v.kind);
    UASSERT(v.v.p == vm.tag_proto);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

UTEST(realm_create_populates_event_global) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UValue v;
    int rc = local_lookup(&vm, r->global_object, "Event", &v);
    UASSERT_EQ(0, rc);
    UASSERT_EQ(UVAL_OBJECT, (int)v.kind);
    UASSERT(v.v.p == vm.event_proto);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

UTEST(realm_create_populates_nil_global) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UValue v;
    int rc = local_lookup(&vm, r->global_object, "nil", &v);
    UASSERT_EQ(0, rc);
    UASSERT_EQ(UVAL_NIL, (int)v.kind);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

UTEST(realm_self_loop_global) {
    /* The "Realm" slot on global_object should point back to global_object. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UValue v;
    int rc = local_lookup(&vm, r->global_object, "Realm", &v);
    UASSERT_EQ(0, rc);
    UASSERT_EQ(UVAL_OBJECT, (int)v.kind);
    UASSERT(v.v.p == r->global_object);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

UTEST(realm_global_object_slot_is_const) {
    /* The "Object" slot should carry the CONSTANT flag bit. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UObject *go = r->global_object;
    size_t len = 6;  /* "Object" */
    USymbol *sym = (USymbol *)ustr_intern(&vm, "Object", len);
    UASSERT(sym != NULL);
    int32_t idx = urbi_shape_find_slot(go->shape, sym);
    UASSERT(idx >= 0);
    /* Extract 4-bit flag nibble for this slot. */
    uint8_t nibble = (uint8_t)((go->shape->flags >> ((uint32_t)idx * 4U)) & 0x0FU);
    UASSERT(nibble & URBI_SLOT_FLAG_CONSTANT);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

UTEST(realm_has_all_15_globals) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    /* global_object shape count should be at least 15. */
    UASSERT((int)r->global_object->shape->count >= 15);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

UTEST(realm_create_oom_realm_struct_returns_null) {
    /* Use a spy alloc that fails on the very first call.  Pass it to
     * urbi_vm_init; the vm init itself may fail silently (event_ring etc.)
     * but realm_create must return NULL without crashing. */
    RealmSpy spy = { 0, -1 };   /* start with unlimited allocs */
    UVM vm;
    urbi_vm_init(&vm, realm_spy_alloc, &spy);

    /* Reset counter and set fail threshold so only 1 alloc succeeds.
     * realm_create's first alloc is the URealm struct itself. */
    spy.alloc_calls = 0;
    spy.fail_at = 0;  /* fail on call #1 (first alloc after reset) */

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r == NULL);

    /* Restore stdlib alloc for clean teardown. */
    spy.fail_at = -1;
    urbi_vm_destroy(&vm);
}

void
test_realm_populate_suite(void)
{
    utest_run("realm_create global_object is not null",
              realm_create_global_object_is_not_null);
    utest_run("realm_create populates Object global",
              realm_create_populates_object_global);
    utest_run("realm_create populates Tag global",
              realm_create_populates_tag_global);
    utest_run("realm_create populates Event global",
              realm_create_populates_event_global);
    utest_run("realm_create populates nil global",
              realm_create_populates_nil_global);
    utest_run("realm self-loop Realm slot",
              realm_self_loop_global);
    utest_run("realm global object slot is const",
              realm_global_object_slot_is_const);
    utest_run("realm has all 15 globals",
              realm_has_all_15_globals);
    utest_run("realm_create OOM on first alloc returns null",
              realm_create_oom_realm_struct_returns_null);
}
