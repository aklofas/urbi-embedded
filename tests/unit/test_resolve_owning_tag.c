/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: resolve_owning_tag cleanup-stack walk (T35, spec #2 §7.2).
 *
 * resolve_owning_tag is non-static in uwatcher_install.c (exposed for test
 * coverage) — reached here via an extern declaration.
 *
 * T35 cases:
 *   1. resolve_owning_tag_returns_innermost:
 *      Push outer then inner TAG_SCOPE; resolve returns inner.
 *      Pop inner; resolve returns outer.
 *      Pop outer; resolve falls through to realm->tag.
 *   2. resolve_owning_tag_skips_non_tag_scope:
 *      Push TRY_FRAME then TAG_SCOPE; resolve returns the TAG_SCOPE tag,
 *      ignoring the TRY_FRAME above it (top-down scan stops at first TAG_SCOPE).
 *   3. resolve_owning_tag_empty_stack_returns_realm_tag:
 *      Empty cleanup stack → returns s->realm->tag directly. */

#include "utest.h"
#include "vm/uvm.h"
#include "ustrand.h"
#include "runtime/ucleanup.h"          /* strand_cleanup_push/pop, UCLEANUP_TAG_SCOPE, UCLEANUP_TRY_FRAME */
#include "utag.h"              /* UTag, utag_create */
#include "realm/urealm.h"      /* urbi_realm_create/destroy */

#include <string.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* resolve_owning_tag lives in uwatcher_install.c; non-static for test access. */
extern struct UTag *resolve_owning_tag(struct UStrand *s);

/* ===================================================================
 * Helpers
 * =================================================================== */

/* Push a synthetic TAG_SCOPE entry for `tag` onto `s`'s cleanup stack.
 * Returns the entry pointer (non-NULL) or NULL on overflow. */
static UCleanupEntry *
push_tag_scope(UStrand *s, UTag *tag)
{
    UCleanupEntry *e = strand_cleanup_push(s);
    if (e == NULL) return NULL;
    e->kind       = (uint8_t)UCLEANUP_TAG_SCOPE;
    e->owning_tag = tag;
    return e;
}

/* ===================================================================
 * Test cases
 * =================================================================== */

/* 1. resolve_owning_tag_returns_innermost
 *
 * Two TAG_SCOPE entries: outer pushed first, inner pushed second.
 * Top-down scan must return inner (the most recently pushed entry).
 * After popping inner, outer wins.
 * After popping outer, falls through to realm->tag. */
UTEST(resolve_owning_tag_returns_innermost)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    ustrand_init(&s, &vm);
    s.realm = r;   /* wire the realm so fallthrough path works */

    UTag *outer = utag_create(&vm);
    UTag *inner = utag_create(&vm);
    UASSERT(outer != NULL);
    UASSERT(inner != NULL);

    /* Push outer, then inner. */
    UASSERT(push_tag_scope(&s, outer) != NULL);
    UASSERT(push_tag_scope(&s, inner) != NULL);

    /* Innermost TAG_SCOPE (depth-1 = index 1) must win. */
    UASSERT(resolve_owning_tag(&s) == inner);

    /* Pop inner; outer must now win. */
    strand_cleanup_pop(&s, UCLEANUP_TAG_SCOPE);
    UASSERT(resolve_owning_tag(&s) == outer);

    /* Pop outer; no TAG_SCOPE remains — fall through to realm->tag. */
    strand_cleanup_pop(&s, UCLEANUP_TAG_SCOPE);
    UASSERT(resolve_owning_tag(&s) == r->tag);

    ustrand_destroy(&s, &vm);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 2. resolve_owning_tag_skips_non_tag_scope
 *
 * Stack (bottom→top): TAG_SCOPE(outer), TRY_FRAME.
 * Top-down scan: TRY_FRAME skipped (not TAG_SCOPE), TAG_SCOPE found → outer. */
UTEST(resolve_owning_tag_skips_non_tag_scope)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    ustrand_init(&s, &vm);
    s.realm = r;

    UTag *outer = utag_create(&vm);
    UASSERT(outer != NULL);

    /* Push TAG_SCOPE for outer, then a TRY_FRAME on top. */
    UASSERT(push_tag_scope(&s, outer) != NULL);

    UCleanupEntry *tf = strand_cleanup_push(&s);
    UASSERT(tf != NULL);
    tf->kind = (uint8_t)UCLEANUP_TRY_FRAME;

    /* Top-down: TRY_FRAME at top is skipped; TAG_SCOPE below it returns outer. */
    UASSERT(resolve_owning_tag(&s) == outer);

    strand_cleanup_pop(&s, UCLEANUP_TRY_FRAME);
    strand_cleanup_pop(&s, UCLEANUP_TAG_SCOPE);

    ustrand_destroy(&s, &vm);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 3. resolve_owning_tag_empty_stack_returns_realm_tag
 *
 * Empty cleanup stack → immediately returns s->realm->tag. */
UTEST(resolve_owning_tag_empty_stack_returns_realm_tag)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    ustrand_init(&s, &vm);
    s.realm = r;

    /* Stack is empty; must return realm->tag. */
    UASSERT_EQ((void *)resolve_owning_tag(&s), (void *)r->tag);

    ustrand_destroy(&s, &vm);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_resolve_owning_tag_suite(void)
{
    printf("test_resolve_owning_tag\n");
    utest_run("resolve_owning_tag_returns_innermost",
              resolve_owning_tag_returns_innermost);
    utest_run("resolve_owning_tag_skips_non_tag_scope",
              resolve_owning_tag_skips_non_tag_scope);
    utest_run("resolve_owning_tag_empty_stack_returns_realm_tag",
              resolve_owning_tag_empty_stack_returns_realm_tag);
}
