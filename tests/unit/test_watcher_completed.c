/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: urbi_watcher_body_completed callback (spec #1 §6.2, T27).
 *
 * Cases:
 *   1. watcher_completed_clears_pointers:
 *      UEXEC_OK → both s->watcher_body_owner and w->body_strand NULL post-call.
 *   2. watcher_completed_respawns_when_pending_refire:
 *      PENDING_REFIRE set → body_strand non-NULL again (fresh strand), flag cleared.
 *   3. watcher_completed_suppresses_refire_under_pending_unregister:
 *      PENDING_UNREGISTER set → body_strand stays NULL, PENDING_REFIRE cleared.
 *   4. watcher_completed_logs_on_uncaught_throw:
 *      UEXEC_THROW → URBI_LOG_WARN fired at least once with "uncaught throw" message.
 *   5. watcher_completed_silent_on_tag_stop_and_cancel:
 *      TAG_STOP and CANCEL → no log emitted; both pointers cleared. */

#include "utest.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "module/umodule.h"
#include "runtime/uclosure.h"
#include "runtime/uframe.h"
#include "watcher/uwatcher.h"
#include "twatcher_install_helper.h"
#include "urbi/urbi.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

static void
make_trivial_closure_c(UClosure *cl, UProto *proto, uint32_t *instr_buf)
{
    instr_buf[0] = (uint32_t)OP_RET;

    memset(proto, 0, sizeof(*proto));
    proto->instructions = instr_buf;
    proto->instr_count  = 1;
    proto->constants    = NULL;
    proto->const_count  = 0;

    memset(cl, 0, sizeof(*cl));
    cl->proto   = proto;
    cl->nupvals = 0;
}

/* Build a minimal AT watcher backed by a real realm and body closure. */
static UWatcher *
test_make_dummy_watcher(struct UVM *vm, struct URealm *realm, UClosure *body_cl)
{
    UWatcher *w = urbi_watcher_install_for_test(
        vm, UWATCHER_AT,
        realm->tag,   /* owning_tag == realm->tag → no extra attach */
        NULL,         /* condition */
        body_cl,      /* body */
        NULL,         /* onleave */
        NULL, 0U);
    if (w)
        w->realm = realm;
    return w;
}

/* Allocate and arm a strand that pretends to be a watcher body strand.
 * Sets s->watcher_body_owner = w.  The strand is READY (started) so that
 * teardown via urbi_realm_destroy will find it on the realm list. */
static struct UStrand *
test_make_dummy_body_strand(struct UVM *vm, struct URealm *realm,
                             UClosure *body_cl, struct UWatcher *w)
{
    struct UStrand *s = urbi_strand_create(realm, body_cl);
    if (!s)
        return NULL;
    if (urbi_strand_arm_from_closure(s, body_cl) != 0) {
        urbi_strand_destroy(s);
        return NULL;
    }
    s->watcher_body_owner = w;
    /* Leave state as DORMANT — completed callback doesn't check state.
     * Do NOT call urbi_strand_start so the run-queue stays clean. */
    return s;
}

/* Log capture. */
static int        g_wc_log_warn;
static int        g_wc_log_total;
static const char *g_wc_last_msg;  /* points into the fmt literal */

static void
wc_capture_log(struct UVM *vm, int level, const char *fmt, ...)
{
    (void)vm;
    g_wc_log_total++;
    if (level == URBI_LOG_WARN) {
        g_wc_log_warn++;
        g_wc_last_msg = fmt;
    }
}

static void
wc_reset_log(void)
{
    g_wc_log_warn  = 0;
    g_wc_log_total = 0;
    g_wc_last_msg  = NULL;
}

/* ===================================================================
 * Test cases
 * =================================================================== */

/* 1. watcher_completed_clears_pointers
 *
 * With UEXEC_OK, both back-pointer and forward-pointer must be NULL after
 * urbi_watcher_body_completed returns.  No log must be emitted. */
UTEST(watcher_completed_clears_pointers)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_trivial_closure_c(&body_cl, &proto, instr);

    UWatcher *w = test_make_dummy_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);

    struct UStrand *s = test_make_dummy_body_strand(&vm, r, &body_cl, w);
    UASSERT(s != NULL);
    w->body_strand = s;

    wc_reset_log();
    vm.host_log_fn = wc_capture_log;

    s->fatal_status = UEXEC_OK;
    urbi_watcher_body_completed(&vm, s);

    UASSERT(w->body_strand == NULL);
    UASSERT(s->watcher_body_owner == NULL);
    UASSERT_EQ(g_wc_log_warn, 0);

    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 2. watcher_completed_respawns_when_pending_refire
 *
 * With PENDING_REFIRE set and no PENDING_UNREGISTER, the callback must
 * trigger a respawn: w->body_strand becomes non-NULL and points to a
 * different (fresh) strand.  The PENDING_REFIRE flag must be cleared. */
UTEST(watcher_completed_respawns_when_pending_refire)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_trivial_closure_c(&body_cl, &proto, instr);

    UWatcher *w = test_make_dummy_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);

    struct UStrand *s = test_make_dummy_body_strand(&vm, r, &body_cl, w);
    UASSERT(s != NULL);
    w->body_strand = s;
    w->flags |= URBI_WATCHER_PENDING_REFIRE;

    s->fatal_status = UEXEC_OK;
    urbi_watcher_body_completed(&vm, s);

    /* A fresh strand must have been spawned. */
    UASSERT(w->body_strand != NULL);
    /* It must be a different strand from the completed one. */
    UASSERT(w->body_strand != s);
    /* PENDING_REFIRE must be cleared. */
    UASSERT_EQ((unsigned)(w->flags & URBI_WATCHER_PENDING_REFIRE), 0U);
    /* Back-pointer on the old strand must be NULL. */
    UASSERT(s->watcher_body_owner == NULL);

    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 3. watcher_completed_suppresses_refire_under_pending_unregister
 *
 * With both PENDING_REFIRE and PENDING_UNREGISTER set, no respawn must occur.
 * body_strand stays NULL and PENDING_REFIRE is cleared. */
UTEST(watcher_completed_suppresses_refire_under_pending_unregister)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_trivial_closure_c(&body_cl, &proto, instr);

    UWatcher *w = test_make_dummy_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);

    struct UStrand *s = test_make_dummy_body_strand(&vm, r, &body_cl, w);
    UASSERT(s != NULL);
    w->body_strand = s;
    w->flags |= URBI_WATCHER_PENDING_REFIRE;
    w->flags |= URBI_WATCHER_PENDING_UNREGISTER;

    s->fatal_status = UEXEC_OK;
    urbi_watcher_body_completed(&vm, s);

    /* No respawn — body_strand must stay NULL. */
    UASSERT(w->body_strand == NULL);
    /* PENDING_REFIRE must be cleared. */
    UASSERT_EQ((unsigned)(w->flags & URBI_WATCHER_PENDING_REFIRE), 0U);
    /* Back-pointer on old strand must be NULL. */
    UASSERT(s->watcher_body_owner == NULL);

    /* Watcher was already flagged PENDING_UNREGISTER — skip unregister call
     * and let realm_destroy clean up strands. */
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 4. watcher_completed_logs_on_uncaught_throw
 *
 * UEXEC_THROW must emit exactly one URBI_LOG_WARN that mentions "uncaught throw". */
UTEST(watcher_completed_logs_on_uncaught_throw)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_trivial_closure_c(&body_cl, &proto, instr);

    UWatcher *w = test_make_dummy_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);

    struct UStrand *s = test_make_dummy_body_strand(&vm, r, &body_cl, w);
    UASSERT(s != NULL);
    w->body_strand = s;

    wc_reset_log();
    vm.host_log_fn = wc_capture_log;

    s->fatal_status = UEXEC_THROW;
    urbi_watcher_body_completed(&vm, s);

    UASSERT(g_wc_log_warn >= 1);
    UASSERT(g_wc_last_msg != NULL);
    /* The message literal must contain "uncaught throw". */
    {
        const char *p = g_wc_last_msg;
        int found = 0;
        /* Simple substring check without <string.h> strstr dependency. */
        const char *needle = "uncaught throw";
        size_t nlen = 14U; /* strlen("uncaught throw") */
        while (*p) {
            size_t i;
            for (i = 0; i < nlen; i++) {
                if (p[i] == '\0' || p[i] != needle[i]) break;
            }
            if (i == nlen) { found = 1; break; }
            p++;
        }
        UASSERT(found);
    }

    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 5. watcher_completed_silent_on_tag_stop_and_cancel
 *
 * TAG_STOP and CANCEL are silent — no URBI_LOG_WARN.  Both pointers cleared. */
UTEST(watcher_completed_silent_on_tag_stop_and_cancel)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_trivial_closure_c(&body_cl, &proto, instr);

    /* --- TAG_STOP subcase --- */
    {
        UWatcher *w = test_make_dummy_watcher(&vm, r, &body_cl);
        UASSERT(w != NULL);
        struct UStrand *s = test_make_dummy_body_strand(&vm, r, &body_cl, w);
        UASSERT(s != NULL);
        w->body_strand = s;

        wc_reset_log();
        vm.host_log_fn = wc_capture_log;

        s->fatal_status = UEXEC_TAG_STOP;
        urbi_watcher_body_completed(&vm, s);

        UASSERT_EQ(g_wc_log_warn, 0);
        UASSERT(w->body_strand == NULL);
        UASSERT(s->watcher_body_owner == NULL);

        urbi_watcher_unregister_internal(&vm, w);
    }

    /* --- CANCEL subcase --- */
    {
        UWatcher *w = test_make_dummy_watcher(&vm, r, &body_cl);
        UASSERT(w != NULL);
        struct UStrand *s = test_make_dummy_body_strand(&vm, r, &body_cl, w);
        UASSERT(s != NULL);
        w->body_strand = s;

        wc_reset_log();
        vm.host_log_fn = wc_capture_log;

        s->fatal_status = UEXEC_CANCEL;
        urbi_watcher_body_completed(&vm, s);

        UASSERT_EQ(g_wc_log_warn, 0);
        UASSERT(w->body_strand == NULL);
        UASSERT(s->watcher_body_owner == NULL);

        urbi_watcher_unregister_internal(&vm, w);
    }

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_watcher_completed_suite(void)
{
    printf("test_watcher_completed\n");
    utest_run("watcher_completed_clears_pointers",
              watcher_completed_clears_pointers);
    utest_run("watcher_completed_respawns_when_pending_refire",
              watcher_completed_respawns_when_pending_refire);
    utest_run("watcher_completed_suppresses_refire_under_pending_unregister",
              watcher_completed_suppresses_refire_under_pending_unregister);
    utest_run("watcher_completed_logs_on_uncaught_throw",
              watcher_completed_logs_on_uncaught_throw);
    utest_run("watcher_completed_silent_on_tag_stop_and_cancel",
              watcher_completed_silent_on_tag_stop_and_cancel);
}
