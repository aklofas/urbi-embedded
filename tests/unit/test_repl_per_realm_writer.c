/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_per_realm_writer.c — v0.9.1 per-realm writer routing.
 *
 * Verifies the writer fallback chain installed in src/vm/uvm_writer.c:
 *   realm->writer_fn (if non-NULL)
 *   -> vm->writer_fn (if non-NULL)
 *   -> default_writer
 *
 * Drives the dispatch via urbi_vm_write_in_realm directly (no urbiscript
 * `echo`/`print` builtins exist at v0.9.1 baseline; the REPL service in
 * Phase 5 lands the lobby-flavored stdlib that exposes them).
 *
 * Also exercises:
 *  - urbi_realm_set_writer accepts a NULL realm without crashing.
 *  - Clearing the realm writer (fn=NULL) falls back to the VM writer.
 *  - URBI_DEFAULT_REPL_BUDGET is auto-applied by urbi_realm_create_repl.
 *  - urbi_realm_set_compile_budget / get_compile_budget round-trip.
 */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ---- two independent capture sinks ------------------------------------ */

typedef struct {
    char     msg[256];
    size_t   msg_len;
    int      calls;
    void    *ud_received;
} Sink;

static Sink vm_sink, realm_sink;

static void
vm_capture(void *ud,
           const char *channel, size_t channel_len,
           const char *msg,     size_t msg_len,
           uint64_t ts_us)
{
    (void)channel; (void)channel_len; (void)ts_us;
    vm_sink.ud_received = ud;
    if (msg_len < sizeof(vm_sink.msg)) {
        memcpy(vm_sink.msg + vm_sink.msg_len, msg, msg_len);
        vm_sink.msg_len += msg_len;
        vm_sink.msg[vm_sink.msg_len] = '\0';
    }
    vm_sink.calls++;
}

static void
realm_capture(void *ud,
              const char *channel, size_t channel_len,
              const char *msg,     size_t msg_len,
              uint64_t ts_us)
{
    (void)channel; (void)channel_len; (void)ts_us;
    realm_sink.ud_received = ud;
    if (msg_len < sizeof(realm_sink.msg)) {
        memcpy(realm_sink.msg + realm_sink.msg_len, msg, msg_len);
        realm_sink.msg_len += msg_len;
        realm_sink.msg[realm_sink.msg_len] = '\0';
    }
    realm_sink.calls++;
}

static void reset_sinks(void) {
    memset(&vm_sink,    0, sizeof(vm_sink));
    memset(&realm_sink, 0, sizeof(realm_sink));
}

/* ---- T1: per-realm writer wins over VM writer ------------------------- */
UTEST(per_realm_writer_routes_to_realm)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    reset_sinks();

    urbi_set_writer(&vm, vm_capture, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    urbi_realm_set_writer(&vm, r, realm_capture, NULL);

    urbi_vm_write_in_realm(&vm, r, "cout", 4, "hi", 2);

    UASSERT_EQ(realm_sink.calls, 1);
    UASSERT(strcmp(realm_sink.msg, "hi") == 0);
    UASSERT_EQ(vm_sink.calls, 0);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- T2: realm writer unset (NULL fn) falls back to VM writer --------- */
UTEST(per_realm_writer_unset_falls_back_to_vm)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    reset_sinks();

    urbi_set_writer(&vm, vm_capture, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    /* No urbi_realm_set_writer call: realm->writer_fn stays NULL. */

    urbi_vm_write_in_realm(&vm, r, "cout", 4, "fallback", 8);

    UASSERT_EQ(vm_sink.calls, 1);
    UASSERT(strcmp(vm_sink.msg, "fallback") == 0);
    UASSERT_EQ(realm_sink.calls, 0);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- T3: NULL realm passes through to VM writer ----------------------- */
UTEST(per_realm_writer_null_realm_uses_vm_writer)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    reset_sinks();

    urbi_set_writer(&vm, vm_capture, NULL);

    urbi_vm_write_in_realm(&vm, NULL, "cout", 4, "x", 1);

    UASSERT_EQ(vm_sink.calls, 1);
    UASSERT_EQ(realm_sink.calls, 0);

    urbi_vm_destroy(&vm);
}

/* ---- T4: clearing realm writer (fn=NULL) reverts to VM writer --------- */
UTEST(per_realm_writer_cleared_falls_back)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    reset_sinks();

    urbi_set_writer(&vm, vm_capture, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    urbi_realm_set_writer(&vm, r, realm_capture, NULL);

    /* First message routes to realm. */
    urbi_vm_write_in_realm(&vm, r, "cout", 4, "A", 1);
    UASSERT_EQ(realm_sink.calls, 1);
    UASSERT_EQ(vm_sink.calls, 0);

    /* Clear realm writer; subsequent calls route to VM writer. */
    urbi_realm_set_writer(&vm, r, NULL, NULL);
    urbi_vm_write_in_realm(&vm, r, "cout", 4, "B", 1);
    UASSERT_EQ(realm_sink.calls, 1);  /* unchanged */
    UASSERT_EQ(vm_sink.calls, 1);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- T5: ud pointer is threaded through unchanged --------------------- */
UTEST(per_realm_writer_ud_threaded)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    reset_sinks();

    int realm_sentinel = 7;
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    urbi_realm_set_writer(&vm, r, realm_capture, &realm_sentinel);

    urbi_vm_write_in_realm(&vm, r, "cout", 4, "ud", 2);
    UASSERT(realm_sink.ud_received == (void *)&realm_sentinel);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- T6: urbi_realm_create_repl auto-applies the default budget ------- */
UTEST(repl_realm_has_default_compile_budget)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_create_repl(&vm);
    UASSERT(r != NULL);

    const UCompileBudget *b = urbi_realm_get_compile_budget(&vm, r);
    UASSERT(b != NULL);
    UASSERT_EQ((long long)b->max_parser_depth,
               (long long)URBI_DEFAULT_REPL_BUDGET.max_parser_depth);
    UASSERT_EQ((long long)b->max_ast_nodes,
               (long long)URBI_DEFAULT_REPL_BUDGET.max_ast_nodes);
    UASSERT_EQ((long long)b->max_source_bytes,
               (long long)URBI_DEFAULT_REPL_BUDGET.max_source_bytes);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- T7: plain urbi_realm_create has no budget by default ------------- */
UTEST(global_realm_has_no_compile_budget)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UASSERT(urbi_realm_get_compile_budget(&vm, r) == NULL);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- T8: set + get round-trip; NULL clears ---------------------------- */
UTEST(compile_budget_set_get_roundtrip)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UCompileBudget custom = { 32U, 500U, 1024U };
    urbi_realm_set_compile_budget(&vm, r, &custom);

    const UCompileBudget *b = urbi_realm_get_compile_budget(&vm, r);
    UASSERT(b != NULL);
    UASSERT_EQ((long long)b->max_parser_depth, 32LL);
    UASSERT_EQ((long long)b->max_ast_nodes,    500LL);
    UASSERT_EQ((long long)b->max_source_bytes, 1024LL);

    urbi_realm_set_compile_budget(&vm, r, NULL);
    UASSERT(urbi_realm_get_compile_budget(&vm, r) == NULL);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ---- T9: NULL realm is a no-op for setters ---------------------------- */
UTEST(setters_null_realm_noop)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    /* These calls must not crash. */
    urbi_realm_set_writer(&vm, NULL, realm_capture, NULL);
    urbi_realm_set_compile_budget(&vm, NULL, &URBI_DEFAULT_REPL_BUDGET);
    UASSERT(urbi_realm_get_compile_budget(&vm, NULL) == NULL);

    urbi_vm_destroy(&vm);
}

/* ---- suite entry point ------------------------------------------------ */
void
test_repl_per_realm_writer_suite(void)
{
    printf("test_repl_per_realm_writer\n");
    utest_run("per_realm_writer_routes_to_realm",          per_realm_writer_routes_to_realm);
    utest_run("per_realm_writer_unset_falls_back_to_vm",   per_realm_writer_unset_falls_back_to_vm);
    utest_run("per_realm_writer_null_realm_uses_vm_writer", per_realm_writer_null_realm_uses_vm_writer);
    utest_run("per_realm_writer_cleared_falls_back",       per_realm_writer_cleared_falls_back);
    utest_run("per_realm_writer_ud_threaded",              per_realm_writer_ud_threaded);
    utest_run("repl_realm_has_default_compile_budget",     repl_realm_has_default_compile_budget);
    utest_run("global_realm_has_no_compile_budget",        global_realm_has_no_compile_budget);
    utest_run("compile_budget_set_get_roundtrip",          compile_budget_set_get_roundtrip);
    utest_run("setters_null_realm_noop",                   setters_null_realm_noop);
}
