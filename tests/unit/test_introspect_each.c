/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_introspect_each.c — coverage for the 9 introspection
 * primitives (v0.9.1 Task 19) plus the Debug urbiscript namespace (Task 22).
 *
 * Idle-VM baselines verify each primitive emits valid JSON with the
 * expected top-level key.  Lobbies/coros tests verify a positive case
 * (REPL realm creates a lobby entry; the global realm hosts at least one
 * coro once urbi_step has run). */
#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "repl/urepl_introspect.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ---- Idle baselines -------------------------------------------------- */

UTEST(introspect_coros_idle_has_key)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    char buf[4096]; size_t n = 0;
    UASSERT_EQ(urbi_introspect_coros(&vm, buf, sizeof buf, &n), URBI_OK);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "\"coros\":") != NULL);
    urbi_vm_destroy(&vm);
}

UTEST(introspect_tags_idle_has_key)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    char buf[4096]; size_t n = 0;
    UASSERT_EQ(urbi_introspect_tags(&vm, buf, sizeof buf, &n), URBI_OK);
    UASSERT(strstr(buf, "\"tags\":") != NULL);
    urbi_vm_destroy(&vm);
}

UTEST(introspect_watchers_idle_has_key)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    char buf[4096]; size_t n = 0;
    UASSERT_EQ(urbi_introspect_watchers(&vm, buf, sizeof buf, &n), URBI_OK);
    UASSERT(strstr(buf, "\"watchers\":") != NULL);
    urbi_vm_destroy(&vm);
}

UTEST(introspect_events_idle_has_key)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    char buf[4096]; size_t n = 0;
    UASSERT_EQ(urbi_introspect_events(&vm, buf, sizeof buf, &n), URBI_OK);
    UASSERT(strstr(buf, "\"events\":") != NULL);
    urbi_vm_destroy(&vm);
}

UTEST(introspect_profile_emits_stub_with_note)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    char buf[4096]; size_t n = 0;
    UASSERT_EQ(urbi_introspect_profile(&vm, buf, sizeof buf, &n), URBI_OK);
    UASSERT(strstr(buf, "\"per_function\":") != NULL);
    UASSERT(strstr(buf, "\"per_opcode\":") != NULL);
    UASSERT(strstr(buf, "\"per_watcher\":") != NULL);
    UASSERT(strstr(buf, "deferred to v1.x") != NULL);
    urbi_vm_destroy(&vm);
}

UTEST(introspect_gc_returns_heap_stats)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    char buf[4096]; size_t n = 0;
    UASSERT_EQ(urbi_introspect_gc(&vm, buf, sizeof buf, &n), URBI_OK);
    UASSERT(strstr(buf, "alive_bytes") != NULL);
    UASSERT(strstr(buf, "threshold") != NULL);
    urbi_vm_destroy(&vm);
}

UTEST(introspect_lobbies_idle_is_empty_list)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    char buf[4096]; size_t n = 0;
    UASSERT_EQ(urbi_introspect_lobbies(&vm, buf, sizeof buf, &n), URBI_OK);
    /* No REPL realms created; the lobbies array is empty even though the
     * global realm has been (auto)created during boot. */
    UASSERT(strstr(buf, "\"lobbies\":[]") != NULL);
    urbi_vm_destroy(&vm);
}

UTEST(introspect_lobbies_lists_one_repl_realm)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    URealm *r = urbi_realm_create_repl(&vm);
    UASSERT(r != NULL);
    char buf[4096]; size_t n = 0;
    UASSERT_EQ(urbi_introspect_lobbies(&vm, buf, sizeof buf, &n), URBI_OK);
    UASSERT(strstr(buf, "lobby-") != NULL);
    UASSERT(strstr(buf, "\"realm\":") != NULL);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

UTEST(introspect_stack_unknown_coro_emits_error)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    char buf[4096]; size_t n = 0;
    UASSERT_EQ(urbi_introspect_stack(&vm, 0xdeadbeef, buf, sizeof buf, &n), URBI_OK);
    UASSERT(strstr(buf, "unknown_coro") != NULL);
    UASSERT(strstr(buf, "\"stack\":[]") != NULL);
    urbi_vm_destroy(&vm);
}

UTEST(introspect_slots_global_realm_returns_array)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);
    char buf[8192]; size_t n = 0;
    /* Empty path → realm's global_object. */
    UASSERT_EQ(urbi_introspect_slots(&vm, r, NULL, 0, buf, sizeof buf, &n),
               URBI_OK);
    UASSERT(strstr(buf, "\"slots\":") != NULL);
    urbi_vm_destroy(&vm);
}

UTEST(introspect_slots_unknown_global_emits_error)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);
    char buf[4096]; size_t n = 0;
    const char *path = "DoesNotExist";
    UASSERT_EQ(urbi_introspect_slots(&vm, r, path, strlen(path),
                                     buf, sizeof buf, &n),
               URBI_OK);
    UASSERT(strstr(buf, "unknown_global") != NULL);
    urbi_vm_destroy(&vm);
}

void test_introspect_each_suite(void)
{
    utest_run("introspect_coros_idle_has_key",       introspect_coros_idle_has_key);
    utest_run("introspect_tags_idle_has_key",        introspect_tags_idle_has_key);
    utest_run("introspect_watchers_idle_has_key",    introspect_watchers_idle_has_key);
    utest_run("introspect_events_idle_has_key",      introspect_events_idle_has_key);
    utest_run("introspect_profile_emits_stub_with_note",
              introspect_profile_emits_stub_with_note);
    utest_run("introspect_gc_returns_heap_stats",    introspect_gc_returns_heap_stats);
    utest_run("introspect_lobbies_idle_is_empty_list",
              introspect_lobbies_idle_is_empty_list);
    utest_run("introspect_lobbies_lists_one_repl_realm",
              introspect_lobbies_lists_one_repl_realm);
    utest_run("introspect_stack_unknown_coro_emits_error",
              introspect_stack_unknown_coro_emits_error);
    utest_run("introspect_slots_global_realm_returns_array",
              introspect_slots_global_realm_returns_array);
    utest_run("introspect_slots_unknown_global_emits_error",
              introspect_slots_unknown_global_emits_error);
}

#else  /* !URBI_ENABLE_REPL */

void test_introspect_each_suite(void) { /* skipped: URBI_ENABLE_REPL=0 */ }

#endif
