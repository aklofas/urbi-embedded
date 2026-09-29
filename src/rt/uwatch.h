/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uwatch.h — the reactive runtime: one watcher type, one dirty
 * count, one drain.
 *
 * ONE WATCHER TYPE.  `at`, `at sync`, `whenever`, `waituntil` and the
 * one-shots differ by `mode` and by which of `cond` / `event` is set.
 * There is no separate event-watcher struct, no per-watcher read set, no
 * cascade rescan and no generation stamp: the old core had all four and
 * they were where its reactive bugs lived.
 *
 * THE DIRTY SET.  A slot read inside a running condition marks its object
 * `UOBJ_F_WATCHED` (sticky, never cleared).  A slot write to an object
 * carrying that bit bumps `UWatchState.ndirty`.  A drain with a non-zero
 * count evaluates EVERY armed condition watcher exactly once and acts on
 * the edge against `last`; a write that happens during the drain bumps
 * the count again and is therefore seen by the NEXT drain, never by the
 * one in progress.  See rt/uexec.h for why the set is a count rather than
 * a list of objects.
 *
 * WHERE A DRAIN HAPPENS.  Two places, and both are safepoints:
 *
 *   - `usched_step`, once before the run queue and once after every
 *     strand slice.  This is what makes a host write between steps reach
 *     a parked `waituntil`, and what runs a watcher body after the strand
 *     that flipped its condition has finished.
 *   - OP_YIELD, i.e. the `;` separator, on a strand that may be
 *     descheduled.  This is what `at sync` means: the body runs to
 *     completion before the next statement of the installing strand, and
 *     `at sync (c) body` therefore observes the same sequence point the
 *     separator already denotes.
 *
 * EDGES.  `at` fires its body on the rising edge.  `at sync` runs it
 * inline on a spare strand instead, which also makes the `;` inside such
 * a body a plain sequence point (OP_YIELD on a spare is a no-op) -- the
 * answer this task records for REVIVAL section 14's `S-atsync-atomic`:
 * at-sync bodies run to completion on the spare strand.  `whenever` fires
 * on the rising edge and RE-FIRES when its body strand dies while the
 * condition still holds, which makes it a reactive loop rather than a
 * per-safepoint counter.  `waituntil` wakes its waiters and deletes
 * itself.  A falling edge runs `onleave`, once, and only after a body has
 * run.  A `whenever` whose body is still running when the condition falls
 * runs `onleave` when that body dies.
 *
 * A CONDITION THAT THROWS kills nothing.  The throw is absorbed at the
 * spare-strand boundary, reported once through the diag hook, and the
 * watcher is DISARMED -- a condition that raises would otherwise raise
 * again on every drain for the life of the VM.
 *
 * Layering: uwatch ranks above uexec and below urealm.  The exec core and
 * the scheduler reach it through the declarations in rt/uexec.h. */

#ifndef URT_WATCH_H
#define URT_WATCH_H
#include "rt/uexec.h"

/* UWatchMode and UWatchState live in rt/uexec.h: the install opcodes name
 * the modes and the state is a by-value member of UVM. */

struct UWatcher {
    UCell     cell;
    uint8_t   mode;         /* UWatchMode */
    uint8_t   armed;        /* 0 once cancelled or spent; the sweep unlinks it */
    uint8_t   last;         /* the condition's truth at the previous drain */
    uint8_t   fired;        /* a body has run since the last onleave */
    uint8_t   leave_pending; /* the condition fell while body_strand was alive; onleave runs when it dies */

    UClosure *cond;         /* NULL for an event watcher */
    UEvent   *event;        /* NULL for a condition watcher */
    UClosure *body, *onleave;

    UTag     *tag;          /* the ambient tag at install; tag.stop() cancels */
    URealm   *realm;        /* the world the condition and the body run in */
    UStrand  *body_strand;  /* the `whenever` body currently in flight */
    UStrand  *waiters;      /* strands parked in waituntil, via their own .link */

    /* The value an in-flight fire is carrying: an emit payload, or the
     * condition's value for a host watch.  It lives on the watcher rather
     * than in a C local because usched_spawn allocates twice before it
     * copies the argument window, and the watcher is a GC root. */
    UValue    payload;

    /* A host watch installed through urbi_watch.  The callback replaces
     * the body closure: there is nothing for a native body to be spawned
     * ON, and routing it through a fake native closure would only add a
     * cell whose sole job is to find these two fields again. */
    int     (*host_cb)(UVM *, void *, UValue);
    void     *host_ud;

    struct UWatcher *next;            /* vm->watch.all */
    struct UWatcher *next_on_event;   /* event->watchers */
};

/* Install a host watch: `cond` is evaluated like any other condition and
 * `cb` is called with its value on each rising edge.  NULL on OOM. */
UWatcher *uwatch_install_host(UVM *vm, URealm *realm, UClosure *cond,
                              int (*cb)(UVM *, void *, UValue), void *ud);

#endif
