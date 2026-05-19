/* SPDX-License-Identifier: BSD-3-Clause */
/* Multi-realm + cross-session scenarios.  Phase 2 gate for Task 7 of
 * v0.9.0-repl-foundation (OP_CLOSURE rewire).
 *
 * Four scenarios must PASS on the current code (OP_CLOSURE with
 * origin_module_instance fallback) and must continue to PASS after
 * Task 7 replaces that fallback with the new owning_module_instance path.
 *
 * Scenarios:
 *   1. multi_session_class_isolation
 *      Realm A declares `class Foo {}`.  Realm B evaluating `Foo` must fail
 *      (slot not found on B's global_object → URBI_ERR_STRAND_FATAL).
 *
 *   2. cross_session_shared_proto
 *      Realm A sets `Global.foo = 42`.  Global is a VM-level singleton
 *      (vm->global_namespace_proto), shared across realms, so Realm B
 *      reading `Global.foo` must return 42.  (v0.9.1: migrated from
 *      `Object.foo` — Object became readonly per spec §4.2.)
 *
 *   3. cross_session_closure_call
 *      Realm A defines `Global.f = function() { 7 }`.  Realm B calls
 *      `Global.f()` — must return 7.  (v0.9.1: Global vs. Object.)
 *
 *   4. cross_session_closure_survives_realm_destroy
 *      Realm A defines `Global.g = function() { 11 }`.  Realm A is then
 *      destroyed.  Realm B calls `Global.g()` — must still return 11.
 *      The root_proto refcount rescue keeps A's protos alive on
 *      vm->rescued_protos even after the realm is gone.
 *      (v0.9.1: Global vs. Object.)
 */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* ---------------------------------------------------------------------------
 * Helper: compile + run one source line under a specific realm.
 * Returns the urbi_repl_eval return code.
 * ---------------------------------------------------------------------------*/
static int
eval_under(UVM *vm, URealm *r, const char *src, char *out, size_t out_sz)
{
    return urbi_repl_eval(vm, r, src, strlen(src), out, out_sz);
}

/* Helper: eval and extract integer result from the decimal string in out. */
static int64_t
parse_int_result(const char *buf)
{
    int64_t got = 0;
    int neg = 0;
    const char *p = buf;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { got = got * 10 + (*p++ - '0'); }
    return neg ? -got : got;
}

/* ---------------------------------------------------------------------------
 * Scenario 1: class isolation across realms.
 *
 * Realm A declares class Foo {}.  Realm B must not see Foo in its namespace.
 * Evaluating `Foo` from realm B should fail (slot not found → strand fatal).
 * ---------------------------------------------------------------------------*/
UTEST(multi_session_class_isolation)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *a = urbi_realm_create(&vm);
    URealm *b = urbi_realm_create(&vm);
    UASSERT(a != NULL);
    UASSERT(b != NULL);

    char buf[256];

    /* Realm A: declare class Foo. */
    int rc_a = eval_under(&vm, a, "class Foo {}", buf, sizeof(buf));
    UASSERT_EQ(URBI_OK, rc_a);

    /* Realm B: Foo should NOT be visible — must return a non-OK error. */
    int rc_b = eval_under(&vm, b, "Foo", buf, sizeof(buf));
    UASSERT(rc_b != URBI_OK);

    urbi_realm_destroy(&vm, a);
    urbi_realm_destroy(&vm, b);
    urbi_vm_destroy(&vm);
}

/* ---------------------------------------------------------------------------
 * Scenario 2: shared atom proto slot visible across realms.
 *
 * Object is vm->atom_object — a VM-wide singleton.  A slot set on it from
 * realm A is visible when read from realm B.
 * ---------------------------------------------------------------------------*/
UTEST(cross_session_shared_proto)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *a = urbi_realm_create(&vm);
    URealm *b = urbi_realm_create(&vm);
    UASSERT(a != NULL);
    UASSERT(b != NULL);

    char buf[256];

    /* Realm A: set Global.foo = 42.  v0.9.1 migration: Object (and the
     * other 14 builtin atom protos) became readonly at v0.9.1; Global is
     * the new designated mutable shared atom per spec §4.1. */
    int rc_a = eval_under(&vm, a, "Global.foo = 42", buf, sizeof(buf));
    UASSERT_EQ(URBI_OK, rc_a);

    /* Realm B: read Global.foo — must be 42. */
    buf[0] = '\0';
    int rc_b = eval_under(&vm, b, "Global.foo", buf, sizeof(buf));
    UASSERT_EQ(URBI_OK, rc_b);
    UASSERT_EQ((int64_t)42, parse_int_result(buf));

    urbi_realm_destroy(&vm, a);
    urbi_realm_destroy(&vm, b);
    urbi_vm_destroy(&vm);
}

/* ---------------------------------------------------------------------------
 * Scenario 3: closure defined in realm A, called from realm B.
 *
 * Realm A: Object.f = function() { 7 }
 * Realm B: Object.f()  =>  7
 *
 * The closure proto lives in realm A's module; the OP_CLOSURE fallback via
 * origin_module_instance resolves the IC binding so the call succeeds.
 * ---------------------------------------------------------------------------*/
UTEST(cross_session_closure_call)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *a = urbi_realm_create(&vm);
    URealm *b = urbi_realm_create(&vm);
    UASSERT(a != NULL);
    UASSERT(b != NULL);

    char buf[256];

    /* Realm A: define Global.f.  v0.9.1: migrated from Object.f (frozen). */
    int rc_a = eval_under(&vm, a, "Global.f = function() { 7 }", buf, sizeof(buf));
    UASSERT_EQ(URBI_OK, rc_a);

    /* Realm B: call Global.f() — must return 7. */
    buf[0] = '\0';
    int rc_b = eval_under(&vm, b, "Global.f()", buf, sizeof(buf));
    UASSERT_EQ(URBI_OK, rc_b);
    UASSERT_EQ((int64_t)7, parse_int_result(buf));

    urbi_realm_destroy(&vm, a);
    urbi_realm_destroy(&vm, b);
    urbi_vm_destroy(&vm);
}

/* ---------------------------------------------------------------------------
 * Scenario 4: closure survives realm A destruction.
 *
 * Realm A: Object.g = function() { 11 }
 * Destroy realm A.
 * Realm B: Object.g()  =>  11  (root_proto refcount rescue keeps proto alive)
 * ---------------------------------------------------------------------------*/
UTEST(cross_session_closure_survives_realm_destroy)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *a = urbi_realm_create(&vm);
    URealm *b = urbi_realm_create(&vm);
    UASSERT(a != NULL);
    UASSERT(b != NULL);

    char buf[256];

    /* Realm A: define Global.g.  v0.9.1: migrated from Object.g (frozen). */
    int rc_a = eval_under(&vm, a, "Global.g = function() { 11 }", buf, sizeof(buf));
    UASSERT_EQ(URBI_OK, rc_a);

    /* Destroy realm A BEFORE realm B reads Global.g.
     * The root_proto refcount rescue must keep A's protos alive. */
    urbi_realm_destroy(&vm, a);

    /* Realm B: call Global.g() — must still return 11. */
    buf[0] = '\0';
    int rc_b = eval_under(&vm, b, "Global.g()", buf, sizeof(buf));
    UASSERT_EQ(URBI_OK, rc_b);
    UASSERT_EQ((int64_t)11, parse_int_result(buf));

    urbi_realm_destroy(&vm, b);
    urbi_vm_destroy(&vm);
}

/* ---------------------------------------------------------------------------
 * Suite entry
 * ---------------------------------------------------------------------------*/
void
test_multi_realm_suite(void)
{
    utest_run("multi_realm: class isolation across realms",
              multi_session_class_isolation);
    utest_run("multi_realm: shared atom proto slot visible across realms",
              cross_session_shared_proto);
    utest_run("multi_realm: closure defined in realm A callable from realm B",
              cross_session_closure_call);
    utest_run("multi_realm: closure survives realm A destruction",
              cross_session_closure_survives_realm_destroy);
}
