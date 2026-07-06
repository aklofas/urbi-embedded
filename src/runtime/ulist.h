/* SPDX-License-Identifier: BSD-3-Clause */
/* Freestanding field-keyed singly-linked-list macros.
 *
 * Three operations, all O(1) or O(N):
 *
 *   URBI_SLIST_PUSH(head, node, link)
 *     Head-insert (LIFO, O(1)).  Sets node->link = head, then head = node.
 *     Use only on lists where insertion order does not matter or where
 *     LIFO is correct.  Do NOT use on FIFO lists (use explicit tail-append).
 *
 *   URBI_SLIST_UNLINK(head, node, link, T)
 *     Pointer-to-pointer walk unlink (O(N)).  Removes node from the list
 *     headed by head.  No-op if node is not on the list.  Does NOT clear
 *     node->link after removal; callers that require a clean link field must
 *     clear it themselves.  T is the element struct type (e.g. UWatcher).
 *
 *   URBI_SLIST_FOREACH_SAFE(it, tmp, head, link)
 *     Deletion-safe forward iteration.  Captures (it)->link into (tmp) before
 *     each body execution, so the body may unlink (it) without invalidating
 *     the traversal.  Do NOT modify (tmp) inside the body.  (it) and (tmp)
 *     must be declared by the caller as the appropriate pointer type before
 *     the loop.
 *
 * No dependencies.  All macros are do{}while(0)-wrapped (PUSH, UNLINK) or
 * expand to a for-loop (FOREACH_SAFE), making them safe in all statement
 * positions. */

#ifndef URBI_ULIST_H
#define URBI_ULIST_H

/* URBI_SLIST_PUSH — head-insert O(1).
 * head = pointer-to-list-head (lvalue of element pointer type).
 * node = element to insert.
 * link = name of the next-pointer field on the element struct. */
#define URBI_SLIST_PUSH(head, node, link) \
    do {                                   \
        (node)->link = (head);             \
        (head) = (node);                   \
    } while (0)

/* URBI_SLIST_UNLINK — pointer-to-pointer walk-and-remove O(N).
 * head = pointer-to-list-head (lvalue of element pointer type).
 * node = element to remove.
 * link = name of the next-pointer field on the element struct.
 * T    = element struct type (needed for the internal pp declaration). */
#define URBI_SLIST_UNLINK(head, node, link, T)                  \
    do {                                                         \
        T **_urbi_slist_pp_ = &(head); /* NOLINT(bugprone-macro-parentheses) — T is a type arg, not an expression */ \
        while (*_urbi_slist_pp_ != NULL &&                      \
               *_urbi_slist_pp_ != (node))                      \
            _urbi_slist_pp_ = &(*_urbi_slist_pp_)->link;        \
        if (*_urbi_slist_pp_ != NULL)                           \
            *_urbi_slist_pp_ = (node)->link;                    \
    } while (0)

/* URBI_SLIST_FOREACH_SAFE — deletion-safe forward iteration.
 * it   = iteration variable (element pointer, declared by caller).
 * tmp  = lookahead variable (element pointer, declared by caller).
 * head = list head (element pointer, not modified by the macro itself).
 * link = name of the next-pointer field on the element struct. */
#define URBI_SLIST_FOREACH_SAFE(it, tmp, head, link)    \
    for ((it) = (head);                                  \
         (it) != NULL && ((tmp) = (it)->link, 1);        \
         (it) = (tmp))

#endif /* URBI_ULIST_H */
