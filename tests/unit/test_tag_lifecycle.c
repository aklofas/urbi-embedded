/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UTag create/destroy + urbi_strand_scope_tag (T29, row 11). */

#include "utest.h"
#include "uvm.h"
#include "urealm.h"
#include "ustrand.h"
#include "ucleanup.h"
#include "utag.h"
#include "ugc.h"    /* UTYPE_TAG */
#include "urbi.h"

#include <stdlib.h>

#define UTEST(name) static void name(void)

/* === Helpers === */

/* Counting allocator: counts allocation calls (not frees).
   Fails with NULL when alloc_calls > fail_at (fail_at == -1 means never). */
typedef struct {
    int alloc_calls;
    int fail_at;
} AllocSpy;

static void *
spy_alloc(void *ptr, size_t n, void *ud)
{
    AllocSpy *spy = (AllocSpy *)ud;
    if (n > 0 && ptr == NULL) {
        spy->alloc_calls++;
        if (spy->fail_at >= 0 && spy->alloc_calls > spy->fail_at)
            return NULL;
    }
    return realloc(ptr, n);
}

/* === Test cases === */

/* 1. utag_create_basic: realm->tag is non-NULL with correct fields. */
UTEST(utag_create_basic)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    /* T29: realm->tag is now a real UTag, not NULL. */
    UASSERT(r->tag != NULL);
    UASSERT_EQ((unsigned)r->tag->type_tag, (unsigned)UTYPE_TAG);
    UASSERT_EQ((unsigned)r->tag->gc_byte,  0u);
    UASSERT_EQ((unsigned)r->tag->flags,    0u);
    UASSERT(r->tag->member_strands_head  == NULL);
    UASSERT(r->tag->member_watchers_head == NULL);
    UASSERT_EQ((unsigned)r->tag->name.kind, (unsigned)UVAL_NIL);

    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 2. utag_create_oom: allocator fails on the tag alloc → realm_create returns NULL.
 *
 * urbi_realm_create allocation order:
 *   call 1: realm struct
 *   call 2: UTag (utag_create)
 *   call 3: UNamespace (unamespace_create — includes internal alloc(s))
 *
 * We fail at call 2 (fail_at == 1, so call #2 returns NULL) to exercise the
 * OOM rollback path that frees the realm struct and returns NULL. */
UTEST(utag_create_oom)
{
    UVM vm;
    URealm *r;

    /* Calibration run: count how many allocs a successful realm_create uses. */
    AllocSpy spy1 = { 0, -1 };
    uvm_init(&vm, spy_alloc, &spy1);
    r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);

    /* The realm struct is alloc call #1, UTag is #2.
     * Fail at call index 2 (fail_at == 1 means > 1 fails, i.e. call #2 onward). */
    AllocSpy spy2 = { 0, 1 };   /* fail on alloc_calls > 1 = fail on call #2+ */
    uvm_init(&vm, spy_alloc, &spy2);
    r = urbi_realm_create(&vm);
    UASSERT(r == NULL);           /* OOM: UTag alloc failed → whole create returns NULL */
    UASSERT(vm.realms_head == NULL);  /* no partial realm was linked */
    uvm_destroy(&vm);
}

/* 3. utag_destroy_null_safe: utag_destroy(vm, NULL) is a no-op. */
UTEST(utag_destroy_null_safe)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    /* Must not crash. */
    utag_destroy(&vm, NULL);
    uvm_destroy(&vm);
}

/* 4. strand_scope_tag_returns_innermost: scope_tag walks top-down and returns
 *    the owning_tag of the innermost TAG_SCOPE entry. */
UTEST(strand_scope_tag_returns_innermost)
{
    UVM vm;
    UTag inner_tag;
    UCleanupEntry *e;

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT(r->tag != NULL);

    /* Create a strand — its cleanup-stack starts with [realm->tag synthetic]. */
    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);
    UASSERT(s->cleanup_depth == 1);  /* one ambient entry: realm->tag */

    /* The current innermost scope tag is realm->tag. */
    UASSERT(urbi_strand_scope_tag(s) == r->tag);

    /* Push an inner tag scope manually (simulating OP_PUSH_TAG).
     * inner_tag is stack-allocated for simplicity — just need its address. */
    inner_tag.type_tag             = UTYPE_TAG;
    inner_tag.gc_byte              = 0;
    inner_tag.pad0                 = 0;
    inner_tag.flags                = 0;
    inner_tag.pad1[0]              = 0;
    inner_tag.pad1[1]              = 0;
    inner_tag.pad1[2]              = 0;
    inner_tag.member_strands_head  = NULL;
    inner_tag.member_watchers_head = NULL;
    inner_tag.name.kind            = UVAL_NIL;
    inner_tag.name.v.i             = 0;

    e = strand_cleanup_push(s);
    UASSERT(e != NULL);
    e->kind        = (uint8_t)UCLEANUP_TAG_SCOPE;
    e->flags       = 0;
    e->owning_tag  = &inner_tag;
    e->strand_back = s;
    e->next_member = inner_tag.member_strands_head;
    inner_tag.member_strands_head = e;

    /* Now scope_tag must return inner_tag (topmost TAG_SCOPE). */
    UASSERT(urbi_strand_scope_tag(s) == &inner_tag);

    /* Unlink inner_tag entry before destroy (maintain invariant for utag_destroy
     * assertion; inner_tag is stack-allocated so utag_destroy isn't called on it,
     * but ustrand_destroy will unlink all TAG_SCOPE entries automatically). */
    urbi_strand_destroy(s);
    /* inner_tag.member_strands_head should be NULL after strand_unlink_from_tags. */
    UASSERT(inner_tag.member_strands_head == NULL);

    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 5. strand_scope_tag_empty_returns_null: a strand with cleanup_depth == 0
 *    returns NULL from scope_tag. */
UTEST(strand_scope_tag_empty_returns_null)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);

    /* Use ustrand_init directly — bypasses ambient-tag attachment, depth == 0. */
    ustrand_init(&s, &vm);
    UASSERT(s.cleanup_depth == 0);

    UASSERT(urbi_strand_scope_tag(&s) == NULL);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* 6. strand_scope_tag_null_safe: NULL strand returns NULL. */
UTEST(strand_scope_tag_null_safe)
{
    UASSERT(urbi_strand_scope_tag(NULL) == NULL);
}

/* 7. utag_type_tag_constant: UTYPE_TAG has value 5. */
UTEST(utag_type_tag_constant)
{
    UASSERT_EQ((unsigned)UTYPE_TAG, 5u);
}

/* === Suite entry point === */

void
test_tag_lifecycle_suite(void)
{
    printf("test_tag_lifecycle\n");
    utest_run("utag_create_basic",                 utag_create_basic);
    utest_run("utag_create_oom",                   utag_create_oom);
    utest_run("utag_destroy_null_safe",            utag_destroy_null_safe);
    utest_run("strand_scope_tag_returns_innermost",strand_scope_tag_returns_innermost);
    utest_run("strand_scope_tag_empty_returns_null",strand_scope_tag_empty_returns_null);
    utest_run("strand_scope_tag_null_safe",        strand_scope_tag_null_safe);
    utest_run("utag_type_tag_constant",            utag_type_tag_constant);
}
