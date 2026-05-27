/* SPDX-License-Identifier: BSD-3-Clause */
/* test_scope_tag.c — v0.10.10-job-introspection / D7-D unit tests for
 * the scopeTag realm-global native (call-style).
 *
 * scopeTag wraps the internal urbi_strand_scope_tag helper and returns the
 * innermost UCLEANUP_TAG_SCOPE.owning_tag on the current strand's cleanup
 * stack as UVAL_TAG.  At chunk-top (no user tag scope) the realm-root tag
 * (realm->tag) sits at cleanup_base[0] and is the result.
 *
 * Surface: `scopeTag()` (call-style native).  The legacy getter-property
 * surface (`scopeTag` bare-name, auto-invoked) requires the OGET dispatch
 * path to bridge native closures, which is a v1.x follow-up.
 *
 * Mirrors the urbi_repl_eval-driven harness used by tests/unit/test_emit_-
 * closure_capture.c — minimal REPL surface, output buffer captures the
 * formatted value, string-compare for true/false / <Tag>.
 */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"

#define UTEST(name) static void name(void)

/* Run one REPL line; copy formatted output to out_buf. */
static int
repl_capture(UVM *vm, const char *src, char *out, size_t out_cap)
{
    return urbi_repl_eval(vm, NULL, src, strlen(src), out, out_cap);
}

/* Test 1: scopeTag() at chunk-top returns the realm-root tag, which equals
 * Lobby.connectionTag (both point at realm->tag).  Outside any user scope
 * the innermost TAG_SCOPE.owning_tag IS the realm-root. */
UTEST(scope_tag_at_chunk_top_equals_connection_tag)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    char buf[64];
    int rc = repl_capture(&vm, "scopeTag() == Lobby.connectionTag",
                          buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_STR_EQ(buf, "true");

    urbi_vm_destroy(&vm);
}

/* Test 2: scopeTag() returns a UVAL_TAG (formats as "<Tag>" — the realm
 * root has no name).  Stable across reads — two calls return the same tag
 * identity (pointer equality, since the realm-root is a singleton). */
UTEST(scope_tag_returns_stable_tag_identity)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    char buf[64];
    UASSERT_EQ(repl_capture(&vm, "var s = scopeTag()", buf, sizeof(buf)),
               URBI_OK);
    UASSERT_STR_EQ(buf, "<Tag>");

    UASSERT_EQ(repl_capture(&vm, "s == scopeTag()", buf, sizeof(buf)),
               URBI_OK);
    UASSERT_STR_EQ(buf, "true");

    urbi_vm_destroy(&vm);
}

/* ===== Suite ===== */
void test_scope_tag_suite(void);

void
test_scope_tag_suite(void)
{
    utest_run("scope_tag_at_chunk_top_equals_connection_tag",
              scope_tag_at_chunk_top_equals_connection_tag);
    utest_run("scope_tag_returns_stable_tag_identity",
              scope_tag_returns_stable_tag_identity);
}
