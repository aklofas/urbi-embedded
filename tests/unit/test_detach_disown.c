/* SPDX-License-Identifier: BSD-3-Clause */
/* test_detach_disown.c — unit tests for detach() and disown() builtins.
 *
 * Tests use urbi_repl_eval (the persistent loader strand path) so that
 * child strands spawned by detach/disown are drained by urbi_repl_eval's
 * internal drain loop (URBI_REPL_DRAIN_BUDGET iterations of urbi_step).
 *
 * Note on tag scopes and OP_POP_TAG: spawning a child with detach() inside
 * a `t: { ... }` scope creates a child linked to the anonymous scope-tag;
 * OP_POP_TAG fires before the child runs and asserts member_strands_head==NULL
 * (design-risk v0.10.9-B: OP_PUSH_TAG doesn't bind the user tag_reg).  These
 * tests deliberately avoid that pattern; T3 verifies disown without a scope.
 */

#include "utest.h"

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "value/uvalue.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Helper: run one REPL line via urbi_repl_eval. Returns URBI_OK on
 * success or the error code.  Call aborts on OOM (no test cleanup). */
static int
do_repl(UVM *vm, URealm *realm, const char *src)
{
    char buf[256] = {0};
    return urbi_repl_eval(vm, realm, src, strlen(src), buf, sizeof(buf));
}

/* Helper: get an integer slot from the realm globals. */
static int64_t
get_int(UVM *vm, URealm *realm, const char *name)
{
    UValue v = {0};
    if (urbi_realm_get_global(vm, realm, name, strlen(name), &v) != URBI_OK)
        return -9999;
    if (v.kind != (uint8_t)UVAL_INT) return -9999;
    return v.v.i;
}

/* ===================================================================
 * T1: detach() spawns a child strand that executes the body
 *
 * Script:
 *   var got = 0
 *   detach(Realm.got = 42)
 *   sleep(10ms)
 *   -- at this point the drain loop in urbi_repl_eval will have run
 *      the child strand, so Realm.got should be 42.
 * =================================================================== */
UTEST(detach_executes_body)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UASSERT_EQ(URBI_OK, do_repl(&vm, r, "var got = 0"));
    UASSERT_EQ(URBI_OK, do_repl(&vm, r, "detach(Realm.got = 42)"));
    UASSERT_EQ(URBI_OK, do_repl(&vm, r, "sleep(10ms)"));

    int64_t val = get_int(&vm, r, "got");
    UASSERT_EQ(val, (int64_t)42);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T2: disown() spawns a child strand that executes the body
 *
 * Same as T1 but uses disown() inside a user tag scope.  The child
 * should still run (tag-stripping does not prevent execution).
 * disown() strips user TAG_SCOPE entries, so the scope tag is cleanly
 * removed from the child's cleanup chain before OP_POP_TAG fires on
 * the parent scope — no utag_destroy member_strands_head assertion.
 * =================================================================== */
UTEST(disown_executes_body)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UASSERT_EQ(URBI_OK, do_repl(&vm, r, "var got = 0"));
    UASSERT_EQ(URBI_OK, do_repl(&vm, r, "var t = Tag.new()"));
    UASSERT_EQ(URBI_OK, do_repl(&vm, r, "t: { disown(Realm.got = 99) }"));
    UASSERT_EQ(URBI_OK, do_repl(&vm, r, "sleep(10ms)"));

    int64_t val = get_int(&vm, r, "got");
    UASSERT_EQ(val, (int64_t)99);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T3: disown() strips user tag scopes; body still executes afterwards
 *
 * Two consecutive disown calls (with no enclosing tag scope, to avoid
 * the OP_POP_TAG/member_strands_head design-risk v0.10.9-B) both
 * complete and their side-effects are observable after sleep.
 * =================================================================== */
UTEST(disown_multiple_bodies_execute)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UASSERT_EQ(URBI_OK, do_repl(&vm, r, "var a = 0; var b = 0"));
    UASSERT_EQ(URBI_OK, do_repl(&vm, r, "disown(Realm.a = 1)"));
    UASSERT_EQ(URBI_OK, do_repl(&vm, r, "disown(Realm.b = 2)"));
    UASSERT_EQ(URBI_OK, do_repl(&vm, r, "sleep(10ms)"));

    UASSERT_EQ(get_int(&vm, r, "a"), (int64_t)1);
    UASSERT_EQ(get_int(&vm, r, "b"), (int64_t)2);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */
void
test_detach_disown_suite(void)
{
    utest_run("detach executes body",              detach_executes_body);
    utest_run("disown executes body",              disown_executes_body);
    utest_run("disown multiple bodies execute",    disown_multiple_bodies_execute);
}
