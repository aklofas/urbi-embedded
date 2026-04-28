/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: ambient-tag inheritance helpers (T29, row 11 §4.1 / §4.3).
 *
 * Tests cover:
 *  1.  capture_ambient_chain_bottom_up: bottom-up order, realm->tag at [0].
 *  2.  capture_ambient_chain_skips_non_tag_kinds: TRY_FRAME entries are skipped.
 *  3.  capture_ambient_chain_truncation: SIZE_MAX returned on overflow.
 *  4.  attach_ambient_tags_basic: entries pushed, strand appears in tag member lists.
 *  5.  attach_ambient_tags_overflow: overflow triggers fatal + DEAD state.
 *  6.  spawned_strand_inherits_ambient_chain: end-to-end single-realm smoke. */

#include "utest.h"
#include "uvm.h"
#include "urealm.h"
#include "ustrand.h"
#include "ucleanup.h"
#include "utag.h"
#include "ugc.h"    /* UTYPE_TAG */
#include "urbi.h"

#include <stdlib.h>
#include <stddef.h>  /* SIZE_MAX */

#define UTEST(name) static void name(void)

/* === Helpers === */

/* Make and zero-init a stack-allocated UTag (not registered with GC — test only). */
static void
tag_init_local(UTag *t)
{
    t->type_tag             = UTYPE_TAG;
    t->gc_byte              = 0;
    t->pad0                 = 0;
    t->flags                = 0;
    t->pad1[0]              = 0;
    t->pad1[1]              = 0;
    t->pad1[2]              = 0;
    t->member_strands_head  = NULL;
    t->member_watchers_head = NULL;
    t->name.kind            = UVAL_NIL;
    t->name.v.i             = 0;
}

/* Push a synthetic TAG_SCOPE entry for `tag` onto `s`'s cleanup stack.
 * Returns the entry pointer or NULL on stack-full. */
static UCleanupEntry *
push_tag_scope(UStrand *s, UTag *tag)
{
    UCleanupEntry *e = strand_cleanup_push(s);
    if (!e) return NULL;
    e->kind        = (uint8_t)UCLEANUP_TAG_SCOPE;
    e->flags       = 0;
    e->owning_tag  = tag;
    e->strand_back = s;
    e->next_member = tag->member_strands_head;
    tag->member_strands_head = e;
    return e;
}

/* === Test cases === */

/* 1. capture_ambient_chain_bottom_up
 *
 * Setup: realm->tag (at index 0 after urbi_strand_create), then push outer,
 * then inner.  Capture should give [realm->tag, outer, inner] (bottom-up). */
UTEST(capture_ambient_chain_bottom_up)
{
    UVM vm;
    UTag outer, inner;
    UTag *chain[8];
    size_t n;

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT(r->tag != NULL);

    /* Create strand — auto-attaches realm->tag at depth 0. */
    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);
    UASSERT(s->cleanup_depth == 1);

    /* Push outer and inner on top. */
    tag_init_local(&outer);
    tag_init_local(&inner);
    UASSERT(push_tag_scope(s, &outer) != NULL);
    UASSERT(push_tag_scope(s, &inner) != NULL);
    UASSERT(s->cleanup_depth == 3);

    /* Capture bottom-up. */
    n = urbi_strand_capture_ambient_chain(s, chain, 8);
    UASSERT_EQ((int)n, 3);
    UASSERT(chain[0] == r->tag);  /* bottommost */
    UASSERT(chain[1] == &outer);
    UASSERT(chain[2] == &inner);  /* topmost */

    /* Cleanup. */
    urbi_strand_destroy(s);
    /* Verify inner + outer unlinked. */
    UASSERT(inner.member_strands_head == NULL);
    UASSERT(outer.member_strands_head == NULL);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 2. capture_ambient_chain_skips_non_tag_kinds
 *
 * Push: realm->tag (synthetic), TRY_FRAME, outer tag.
 * Capture should collect only the two TAG_SCOPE entries. */
UTEST(capture_ambient_chain_skips_non_tag_kinds)
{
    UVM vm;
    UTag outer;
    UCleanupEntry *te;
    UTag *chain[8];
    size_t n;

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);

    /* Push a TRY_FRAME (no owning_tag). */
    te = strand_cleanup_push(s);
    UASSERT(te != NULL);
    te->kind = (uint8_t)UCLEANUP_TRY_FRAME;
    te->flags = 0;

    /* Push outer tag scope. */
    tag_init_local(&outer);
    UASSERT(push_tag_scope(s, &outer) != NULL);

    UASSERT(s->cleanup_depth == 3);

    n = urbi_strand_capture_ambient_chain(s, chain, 8);
    UASSERT_EQ((int)n, 2);       /* only the two TAG_SCOPE entries */
    UASSERT(chain[0] == r->tag);
    UASSERT(chain[1] == &outer);

    urbi_strand_destroy(s);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 3. capture_ambient_chain_truncation: cap smaller than chain → SIZE_MAX. */
UTEST(capture_ambient_chain_truncation)
{
    UVM vm;
    UTag a, b, c;
    UTag *chain[2];  /* cap = 2, but we'll have 4 entries */
    size_t n;

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);

    tag_init_local(&a);
    tag_init_local(&b);
    tag_init_local(&c);
    UASSERT(push_tag_scope(s, &a) != NULL);
    UASSERT(push_tag_scope(s, &b) != NULL);
    UASSERT(push_tag_scope(s, &c) != NULL);
    /* depth = 4 (realm->tag + a + b + c), but chain cap = 2 */

    n = urbi_strand_capture_ambient_chain(s, chain, 2);
    UASSERT(n == (size_t)-1);    /* SIZE_MAX — truncation detected */

    urbi_strand_destroy(s);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 4. attach_ambient_tags_basic
 *
 * Create parent strand with realm->tag ambient.  Build a two-element chain
 * [realm->tag, outer].  Create a fresh child strand via ustrand_init (no ambient),
 * then call attach.  Verify child cleanup_depth == 2, and both tags have the
 * child's entry in their member_strands_head. */
UTEST(attach_ambient_tags_basic)
{
    UVM vm;
    UTag outer;
    UTag *chain[2];
    UStrand *child;

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT(r->tag != NULL);

    /* Parent strand for capturing the chain. */
    UStrand *parent = urbi_strand_create(r, NULL);
    UASSERT(parent != NULL);

    tag_init_local(&outer);
    UASSERT(push_tag_scope(parent, &outer) != NULL);

    /* Capture chain from parent. */
    size_t n = urbi_strand_capture_ambient_chain(parent, chain, 2);
    UASSERT_EQ((int)n, 2);
    UASSERT(chain[0] == r->tag);
    UASSERT(chain[1] == &outer);

    /* Create child via ustrand_init (no ambient auto-attach). */
    child = (UStrand *)vm.alloc_fn(NULL, sizeof(UStrand), vm.alloc_ud);
    UASSERT(child != NULL);
    ustrand_init(child, &vm);
    UASSERT(child->cleanup_depth == 0);

    /* Attach the two-element chain. */
    urbi_strand_attach_ambient_tags(child, chain, n);
    UASSERT_EQ((unsigned)child->cleanup_depth, 2u);
    UASSERT(child->fatal_status == UEXEC_OK);

    /* child should appear in realm->tag's member list. */
    UASSERT(r->tag->member_strands_head != NULL);
    /* Traverse: find the child's entry in realm->tag. */
    {
        UCleanupEntry *e = r->tag->member_strands_head;
        int found = 0;
        while (e != NULL) {
            if (e->strand_back == child) { found = 1; break; }
            e = e->next_member;
        }
        UASSERT(found);
    }
    /* child should appear in outer's member list. */
    UASSERT(outer.member_strands_head != NULL);
    {
        UCleanupEntry *e = outer.member_strands_head;
        int found = 0;
        while (e != NULL) {
            if (e->strand_back == child) { found = 1; break; }
            e = e->next_member;
        }
        UASSERT(found);
    }

    /* Cleanup — destroy child first, then parent (unlinks all), then realm. */
    ustrand_destroy(child, &vm);
    vm.alloc_fn(child, 0, vm.alloc_ud);
    urbi_strand_destroy(parent);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 5. attach_ambient_tags_overflow
 *
 * Build a chain of URBI_CLEANUP_MAX + 1 entries (all pointing to the same tag
 * for simplicity) and attach it to a fresh strand.  The first URBI_CLEANUP_MAX
 * pushes succeed but the last one triggers the overflow path. */
UTEST(attach_ambient_tags_overflow)
{
    UVM vm;
    UTag filler;
    UTag **big_chain;
    int big_n;
    UStrand *s;
    int i;

    uvm_init(&vm, NULL, NULL);

    tag_init_local(&filler);

    /* Allocate the chain array. */
    big_n = URBI_CLEANUP_MAX + 1;
    big_chain = (UTag **)malloc((size_t)big_n * sizeof(UTag *));
    UASSERT(big_chain != NULL);
    for (i = 0; i < big_n; i++) big_chain[i] = &filler;

    /* Create a strand via ustrand_init so cleanup_depth starts at 0. */
    s = (UStrand *)vm.alloc_fn(NULL, sizeof(UStrand), vm.alloc_ud);
    UASSERT(s != NULL);
    ustrand_init(s, &vm);
    UASSERT(s->cleanup_depth == 0);

    /* Attach big_n = URBI_CLEANUP_MAX+1 entries — must overflow. */
    urbi_strand_attach_ambient_tags(s, big_chain, (size_t)big_n);

    UASSERT(s->fatal_status == UEXEC_CANCEL);
    UASSERT_EQ((unsigned)s->state, (unsigned)USTRAND_STATE_DEAD);
    UASSERT_EQ((unsigned)s->fatal_value.kind, (unsigned)UVAL_NIL);

    /* Cleanup: ustrand_destroy unlinks any entries already pushed. */
    ustrand_destroy(s, &vm);
    vm.alloc_fn(s, 0, vm.alloc_ud);
    free(big_chain);
    uvm_destroy(&vm);
}

/* 6. spawned_strand_inherits_ambient_chain
 *
 * End-to-end smoke: create realm, create strand (inherits realm->tag),
 * verify cleanup_depth == 1 and the entry points back to the strand. */
UTEST(spawned_strand_inherits_ambient_chain)
{
    UVM vm;

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT(r->tag != NULL);

    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);

    /* Strand should have exactly one cleanup entry: the synthetic realm->tag. */
    UASSERT_EQ((unsigned)s->cleanup_depth, 1u);
    UASSERT(s->cleanup_base[0].kind == (uint8_t)UCLEANUP_TAG_SCOPE);
    UASSERT(s->cleanup_base[0].owning_tag  == r->tag);
    UASSERT(s->cleanup_base[0].strand_back == s);
    UASSERT(s->cleanup_base[0].flags == 0);      /* no onleave on synthetic */

    /* Strand must appear in realm->tag's member list. */
    UASSERT(r->tag->member_strands_head == &s->cleanup_base[0]);

    urbi_strand_destroy(s);
    /* After destroy, realm->tag's member list must be empty. */
    UASSERT(r->tag->member_strands_head == NULL);

    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* ============================================================
 * §9.4 gap-fill: synthetic entry shape + termination unlink
 * ============================================================ */

/* 7. synthetic_entries_no_onleave
 *
 * Verify that synthetic TAG_SCOPE entries created by urbi_strand_create /
 * urbi_strand_attach_ambient_tags have flags == 0 (no FLAG_HAS_ONLEAVE)
 * and handler_pc == 0. */
UTEST(synthetic_entries_no_onleave)
{
    UVM vm;

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);
    UASSERT(s->cleanup_depth >= 1u);

    /* Walk all TAG_SCOPE entries on the cleanup stack. */
    unsigned i;
    for (i = 0; i < s->cleanup_depth; i++) {
        UCleanupEntry *e = &s->cleanup_base[i];
        if (e->kind != (uint8_t)UCLEANUP_TAG_SCOPE) continue;
        /* Synthetic entries must have no onleave flag and no handler_pc. */
        UASSERT_EQ((unsigned)e->flags,          0u);
        UASSERT_EQ((unsigned)e->handler_pc,     0u);
        /* Register range fields must be zero. */
        UASSERT_EQ((unsigned)e->register_base,  0u);
        UASSERT_EQ((unsigned)e->register_count, 0u);
    }

    urbi_strand_destroy(s);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 8. synthetic_entries_unlink_on_termination
 *
 * Push a synthetic TAG_SCOPE entry for a local tag; destroy the strand;
 * verify the tag's member_strands_head is NULL afterward (strand_unlink_from_tags
 * ran). */
UTEST(synthetic_entries_unlink_on_termination)
{
    UVM vm;
    UTag local_tag;

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);

    /* Add a second synthetic entry for local_tag. */
    tag_init_local(&local_tag);
    UASSERT(push_tag_scope(s, &local_tag) != NULL);
    UASSERT(s->cleanup_depth == 2u);

    /* local_tag must now have s in its member list. */
    UASSERT(local_tag.member_strands_head != NULL);
    UASSERT(local_tag.member_strands_head->strand_back == s);

    /* Destroy strand — strand_unlink_from_tags must unlink from local_tag. */
    urbi_strand_destroy(s);

    UASSERT(local_tag.member_strands_head == NULL);
    /* realm->tag also cleared. */
    UASSERT(r->tag->member_strands_head == NULL);

    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* === Suite entry point === */

void
test_strand_spawn_inheritance_suite(void)
{
    printf("test_strand_spawn_inheritance\n");
    utest_run("capture_ambient_chain_bottom_up",
              capture_ambient_chain_bottom_up);
    utest_run("capture_ambient_chain_skips_non_tag_kinds",
              capture_ambient_chain_skips_non_tag_kinds);
    utest_run("capture_ambient_chain_truncation",
              capture_ambient_chain_truncation);
    utest_run("attach_ambient_tags_basic",
              attach_ambient_tags_basic);
    utest_run("attach_ambient_tags_overflow",
              attach_ambient_tags_overflow);
    utest_run("spawned_strand_inherits_ambient_chain",
              spawned_strand_inherits_ambient_chain);
    /* §9.4 gap-fill: synthetic entry shape + termination unlink */
    utest_run("synthetic_entries_no_onleave",          synthetic_entries_no_onleave);
    utest_run("synthetic_entries_unlink_on_termination",
              synthetic_entries_unlink_on_termination);
}
