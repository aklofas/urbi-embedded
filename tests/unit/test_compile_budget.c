/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_compile_budget.c — v0.9.1 per-realm UCompileBudget.
 *
 * Verifies the three limits are enforced at the boundary surfaces:
 *   max_source_bytes  — checked in urbi_repl_eval before any alloc.
 *   max_ast_nodes     — checked in make_node().
 *   max_parser_depth  — checked at parse_expression entry.
 *
 * Test approach: build pathological inputs sized just over each limit
 * with a small custom budget, then call urbi_repl_eval and pin the rc.
 *
 * The global Realm has no budget by default; urbi_realm_create_repl
 * auto-applies URBI_DEFAULT_REPL_BUDGET; explicit overrides are exercised
 * here by urbi_realm_set_compile_budget on a non-REPL realm.
 */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "realm/urealm.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ---- T1: source-bytes limit is honoured ------------------------------- */
UTEST(budget_source_bytes_triggers)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UCompileBudget tight = { 0U, 0U, 8U };  /* 8 bytes max source */
    urbi_realm_set_compile_budget(r, &tight);

    /* "12345678" fits exactly; submit 9 bytes -> trip. */
    char buf[256];
    int rc = urbi_repl_eval(&vm, r, "123456789", 9, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_ERR_COMPILE_BUDGET_SOURCE);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- T2: source-bytes at the boundary compiles cleanly ---------------- */
UTEST(budget_source_bytes_at_boundary_compiles)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_create(&vm);
    UCompileBudget tight = { 0U, 0U, 5U };
    urbi_realm_set_compile_budget(r, &tight);

    char buf[256];
    /* "1+2+3" = 5 bytes, equals the limit (-> allowed). */
    int rc = urbi_repl_eval(&vm, r, "1+2+3", 5, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(strcmp(buf, "6") == 0);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- T3: parser depth limit triggers on nested parens ---------------- */
UTEST(budget_depth_limit_triggers)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_create(&vm);
    UCompileBudget tight = { 5U, 0U, 1024U };
    urbi_realm_set_compile_budget(r, &tight);

    /* Build "((((((1))))))" — 6 nested parens (depth 6 > limit 5). */
    char src[64];
    size_t n = 0;
    for (int i = 0; i < 6; i++) src[n++] = '(';
    src[n++] = '1';
    for (int i = 0; i < 6; i++) src[n++] = ')';
    src[n] = '\0';

    char buf[256];
    buf[0] = '\0';
    int rc = urbi_repl_eval(&vm, r, src, n, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_ERR_COMPILE_BUDGET_DEPTH);
    UASSERT(strstr(buf, "depth") != NULL);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- T4: shallow expression below depth limit compiles --------------- */
UTEST(budget_depth_below_limit_compiles)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_create(&vm);
    UCompileBudget loose = { 64U, 0U, 1024U };
    urbi_realm_set_compile_budget(r, &loose);

    char buf[256];
    int rc = urbi_repl_eval(&vm, r, "((1+2))", 7, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(strcmp(buf, "3") == 0);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- T5: AST-node-count limit triggers on long expression ------------- */
UTEST(budget_node_count_triggers)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_create(&vm);
    UCompileBudget tight = { 1024U, 5U, 1024U };  /* 5-node cap */
    urbi_realm_set_compile_budget(r, &tight);

    /* "1+2+3+4+5+6" produces > 5 nodes (each integer + binary). */
    char buf[256];
    buf[0] = '\0';
    int rc = urbi_repl_eval(&vm, r, "1+2+3+4+5+6", 11, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_ERR_COMPILE_BUDGET_NODES);
    UASSERT(strstr(buf, "nodes") != NULL);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- T6: global realm (no budget) compiles deeply ---------------------- */
UTEST(budget_off_for_global_realm)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_global(&vm);
    UASSERT(urbi_realm_get_compile_budget(r) == NULL);

    /* Build "((((((((((1))))))))))" — 10 nested parens; would trip a
     * REPL-default budget (256) somewhere far above this, but the global
     * realm should accept arbitrary depth. */
    char src[128];
    size_t n = 0;
    for (int i = 0; i < 10; i++) src[n++] = '(';
    src[n++] = '1';
    for (int i = 0; i < 10; i++) src[n++] = ')';
    src[n] = '\0';

    char buf[256];
    int rc = urbi_repl_eval(&vm, r, src, n, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(strcmp(buf, "1") == 0);

    urbi_vm_destroy(&vm);
}

/* ---- T7: REPL realm auto-applies the default budget ------------------- */
UTEST(budget_repl_realm_default_compiles_normal_source)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_create_repl(&vm);
    UASSERT(urbi_realm_get_compile_budget(r) != NULL);

    /* A normal expression compiles cleanly under the default budget. */
    char buf[256];
    int rc = urbi_repl_eval(&vm, r, "1+2", 3, buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(strcmp(buf, "3") == 0);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- suite entry ------------------------------------------------------ */
void
test_compile_budget_suite(void)
{
    printf("test_compile_budget\n");
    utest_run("budget_source_bytes_triggers",            budget_source_bytes_triggers);
    utest_run("budget_source_bytes_at_boundary_compiles", budget_source_bytes_at_boundary_compiles);
    utest_run("budget_depth_limit_triggers",             budget_depth_limit_triggers);
    utest_run("budget_depth_below_limit_compiles",       budget_depth_below_limit_compiles);
    utest_run("budget_node_count_triggers",              budget_node_count_triggers);
    utest_run("budget_off_for_global_realm",             budget_off_for_global_realm);
    utest_run("budget_repl_realm_default_compiles_normal_source",
              budget_repl_realm_default_compiles_normal_source);
}
