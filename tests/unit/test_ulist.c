/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for the intrusive singly-linked-list macros in src/runtime/ulist.h.
 *
 * Tests:
 *   push 3 elements / unlink middle, head, tail / FOREACH_SAFE with
 *   mid-iteration unlink / empty-list edge cases. */

#include "utest.h"
#include "runtime/ulist.h"
#include <stddef.h>

/* Minimal node type used throughout these tests. */
typedef struct TNode {
    int             val;
    struct TNode   *next;
} TNode;

/* Helper: count list length. */
static int
list_len(TNode *head)
{
    int n = 0;
    while (head != NULL) { n++; head = head->next; }
    return n;
}

/* Helper: find node with given val (NULL if absent). */
static TNode *
list_find(TNode *head, int val)
{
    while (head != NULL) {
        if (head->val == val) return head;
        head = head->next;
    }
    return NULL;
}

/* ---- PUSH ---- */

static void
ulist_push_three_elements(void)
{
    TNode a = {1, NULL}, b = {2, NULL}, c = {3, NULL};
    TNode *head = NULL;

    URBI_SLIST_PUSH(head, &a, next);
    URBI_SLIST_PUSH(head, &b, next);
    URBI_SLIST_PUSH(head, &c, next);

    /* Head-insert order: c → b → a */
    UASSERT_EQ(list_len(head), 3);
    UASSERT(head == &c);
    UASSERT(head->next == &b);
    UASSERT(head->next->next == &a);
    UASSERT(a.next == NULL);
}

/* ---- UNLINK ---- */

static void
ulist_unlink_middle(void)
{
    TNode a = {1, NULL}, b = {2, NULL}, c = {3, NULL};
    TNode *head = NULL;
    URBI_SLIST_PUSH(head, &a, next);
    URBI_SLIST_PUSH(head, &b, next);
    URBI_SLIST_PUSH(head, &c, next);
    /* List: c → b → a */

    URBI_SLIST_UNLINK(head, &b, next, TNode);

    UASSERT_EQ(list_len(head), 2);
    UASSERT(list_find(head, 2) == NULL);
    UASSERT(list_find(head, 1) != NULL);
    UASSERT(list_find(head, 3) != NULL);
}

static void
ulist_unlink_head(void)
{
    TNode a = {1, NULL}, b = {2, NULL}, c = {3, NULL};
    TNode *head = NULL;
    URBI_SLIST_PUSH(head, &a, next);
    URBI_SLIST_PUSH(head, &b, next);
    URBI_SLIST_PUSH(head, &c, next);
    /* List: c → b → a */

    URBI_SLIST_UNLINK(head, &c, next, TNode);

    UASSERT_EQ(list_len(head), 2);
    UASSERT(head == &b);
    UASSERT(list_find(head, 3) == NULL);
}

static void
ulist_unlink_tail(void)
{
    TNode a = {1, NULL}, b = {2, NULL}, c = {3, NULL};
    TNode *head = NULL;
    URBI_SLIST_PUSH(head, &a, next);
    URBI_SLIST_PUSH(head, &b, next);
    URBI_SLIST_PUSH(head, &c, next);
    /* List: c → b → a */

    URBI_SLIST_UNLINK(head, &a, next, TNode);

    UASSERT_EQ(list_len(head), 2);
    UASSERT(list_find(head, 1) == NULL);
    /* Tail of remaining list must now be b with b->next == NULL. */
    UASSERT(head->next == &b);
    UASSERT(b.next == NULL);
}

static void
ulist_unlink_absent_noop(void)
{
    TNode a = {1, NULL}, b = {2, NULL};
    TNode outsider = {99, NULL};
    TNode *head = NULL;
    URBI_SLIST_PUSH(head, &a, next);
    URBI_SLIST_PUSH(head, &b, next);

    /* Unlinking a node not on the list must be a no-op. */
    URBI_SLIST_UNLINK(head, &outsider, next, TNode);

    UASSERT_EQ(list_len(head), 2);
}

static void
ulist_unlink_single(void)
{
    TNode a = {1, NULL};
    TNode *head = NULL;
    URBI_SLIST_PUSH(head, &a, next);

    URBI_SLIST_UNLINK(head, &a, next, TNode);

    UASSERT(head == NULL);
}

/* ---- FOREACH_SAFE ---- */

static void
ulist_foreach_safe_read_only(void)
{
    TNode a = {1, NULL}, b = {2, NULL}, c = {3, NULL};
    TNode *head = NULL;
    URBI_SLIST_PUSH(head, &a, next);
    URBI_SLIST_PUSH(head, &b, next);
    URBI_SLIST_PUSH(head, &c, next);

    int sum = 0;
    TNode *it, *tmp;
    URBI_SLIST_FOREACH_SAFE(it, tmp, head, next) {
        sum += it->val;
    }
    UASSERT_EQ(sum, 6);
    UASSERT_EQ(list_len(head), 3);
}

static void
ulist_foreach_safe_mid_unlink(void)
{
    TNode a = {1, NULL}, b = {2, NULL}, c = {3, NULL};
    TNode *head = NULL;
    URBI_SLIST_PUSH(head, &a, next);
    URBI_SLIST_PUSH(head, &b, next);
    URBI_SLIST_PUSH(head, &c, next);
    /* List: c(3) → b(2) → a(1) */

    /* Remove all nodes with even val mid-iteration. */
    TNode *it, *tmp;
    URBI_SLIST_FOREACH_SAFE(it, tmp, head, next) {
        if (it->val % 2 == 0) {
            URBI_SLIST_UNLINK(head, it, next, TNode);
        }
    }
    UASSERT_EQ(list_len(head), 2);
    UASSERT(list_find(head, 2) == NULL);
    UASSERT(list_find(head, 1) != NULL);
    UASSERT(list_find(head, 3) != NULL);
}

static void
ulist_foreach_safe_empty(void)
{
    TNode *head = NULL;
    int count = 0;
    TNode *it, *tmp;
    URBI_SLIST_FOREACH_SAFE(it, tmp, head, next) {
        count++;
    }
    UASSERT_EQ(count, 0);
}

void
test_ulist_suite(void)
{
    utest_run("ulist.push_three_elements",      ulist_push_three_elements);
    utest_run("ulist.unlink_middle",             ulist_unlink_middle);
    utest_run("ulist.unlink_head",               ulist_unlink_head);
    utest_run("ulist.unlink_tail",               ulist_unlink_tail);
    utest_run("ulist.unlink_absent_noop",        ulist_unlink_absent_noop);
    utest_run("ulist.unlink_single",             ulist_unlink_single);
    utest_run("ulist.foreach_safe_read_only",    ulist_foreach_safe_read_only);
    utest_run("ulist.foreach_safe_mid_unlink",   ulist_foreach_safe_mid_unlink);
    utest_run("ulist.foreach_safe_empty",        ulist_foreach_safe_empty);
}
