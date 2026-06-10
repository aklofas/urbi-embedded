/* SPDX-License-Identifier: BSD-3-Clause */
/* src/chunk/uproto_ref.c — typed-handle API for UProto reference counting.
 *
 * Implements urbi_proto_ref_acquire/release (closure-bind) and
 * urbi_proto_strand_ref_acquire/release (strand-bind) declared in uproto.h.
 *
 * Design: runtime-invariants audit F1, F3 (v0.10.1-invariants W4).
 *
 * Saturation handling: UINT16_MAX is a permanent-leak sentinel for v1.0
 * (see design-risks register).  Saturation is logged to stderr on hosted
 * builds; silent on freestanding (no vm available at this call site).
 *
 * Underflow handling: dec on a zero refcount is a programming error
 * (paired acquire/release violated).  URBI_REQUIRE fires in all build
 * modes (host, release, freestanding) to surface the violation.
 *
 * Debug accounting (URBI_DEBUG builds):
 *   g_closure_ref_total — net closure-bind inc minus dec; must be zero at
 *                         urbi_vm_destroy (all GC closures finalized by then).
 *   g_strand_ref_total  — net strand-bind inc minus dec; must be zero at
 *                         urbi_vm_destroy (all strands destroyed by then).
 * Both counters are checked by urbi_proto_ref_assert_balanced().
 *
 * Intentionally-leaked rescued_protos carry no outstanding refs by vm_destroy:
 * all strands are destroyed first (urealm_teardown_all), all GC closures
 * are finalized (urbi_gc_destroy).  No whitelist is needed.
 */

#include "chunk/uproto.h"
#include "urbi/require.h"

/* Saturation diagnostic — uses stderr on hosted builds (no vm here). */
#if __STDC_HOSTED__ && !defined(URBI_BYTECODE_ONLY)
#  include <stdio.h>
static void proto_saturation_warn(const UProto *p, urbi_proto_ref_owner_t owner)
{
    fprintf(stderr,
            "urbi: UProto refcount saturation: proto=%p owner=%d"
            " (proto leaks — v1.0 deferral)\n",
            (const void *)p, (int)owner);
}
#else
static void proto_saturation_warn(const UProto *p, urbi_proto_ref_owner_t owner)
{
    /* Freestanding: no stderr.  Saturation is a slow-leak condition, not an
     * abort-worthy failure; embedder can detect via a host_log_fn callback. */
    (void)p;
    (void)owner;
}
#endif

/* Aggregated net counters — URBI_DEBUG builds only.
 * Closure-bind: incremented in urbi_proto_ref_acquire, decremented in
 *   urbi_proto_ref_release.  Net must be zero at vm-destroy.
 * Strand-bind:  incremented in urbi_proto_strand_ref_acquire, decremented in
 *   urbi_proto_strand_ref_release (called from uproto_strand_refcount_dec).
 *   Net must be zero after the LAST vm is destroyed.
 * Using aggregated totals (not per-owner) because the three strand-bind
 * acquire sites (STRAND / FORK / TRANSIENT) all release via a single shared
 * helper (uproto_strand_refcount_dec → urbi_proto_strand_ref_release).
 *
 * g_active_vm_count tracks how many VMs are alive simultaneously.  The
 * balanced check only fires when the count drops to zero (last VM destroyed)
 * so that multi-VM test scenarios do not produce false positives from
 * closures still alive in peer VMs. */
#ifdef URBI_DEBUG
static int g_closure_ref_total = 0;  /* audit-globals-allow: URBI_DEBUG-only leak-balance counter (doc above) */
static int g_strand_ref_total  = 0;  /* audit-globals-allow: URBI_DEBUG-only leak-balance counter (doc above) */
static int g_active_vm_count   = 0;  /* audit-globals-allow: URBI_DEBUG-only leak-balance counter (doc above) */
#endif

/* === Closure-bind acquire/release ========================================= */

void
urbi_proto_ref_acquire(UProto *p, urbi_proto_ref_owner_t owner)
{
    if (p == NULL) return;
    if (p->refcount == UINT16_MAX) {
        proto_saturation_warn(p, owner);
        return;  /* do not increment — preserve "leak forever" contract */
    }
    p->refcount = (uint16_t)(p->refcount + 1U);
#ifdef URBI_DEBUG
    g_closure_ref_total++;
#endif
}

void
urbi_proto_ref_release(UProto *p, urbi_proto_ref_owner_t owner)
{
    if (p == NULL) return;
    URBI_REQUIRE(p->refcount > 0U,
                 "UProto closure-bind refcount underflow — "
                 "urbi_proto_ref_release without matching acquire");
    if (p->refcount == UINT16_MAX) {
        /* Saturated — dec is a no-op; proto stays leaked per design. */
        (void)owner;
        return;
    }
    p->refcount = (uint16_t)(p->refcount - 1U);
#ifdef URBI_DEBUG
    g_closure_ref_total--;
#endif
    /* Sentinel-promotion (deferred-destroy) is handled by the caller
     * (uclosure_destroy in utypes_init.c) which checks rp->refcount == 0
     * and rp->next_alloc == rp after this call.  This function only
     * decrements; it does not trigger uproto_destroy_buffers. */
    (void)owner;
}

/* === Strand-bind acquire/release ========================================== */

void
urbi_proto_strand_ref_acquire(UProto *p, urbi_proto_ref_owner_t owner)
{
    if (p == NULL) return;
    if (p->refcount == UINT16_MAX) {
        proto_saturation_warn(p, owner);
        return;
    }
    p->refcount = (uint16_t)(p->refcount + 1U);
#ifdef URBI_DEBUG
    g_strand_ref_total++;
#endif
    (void)owner;
}

void
urbi_proto_strand_ref_release(UProto *p, urbi_proto_ref_owner_t owner)
{
    /* Called from uproto_strand_refcount_dec (uchunk_io.c) AFTER that helper
     * has already applied the underflow guard.  This function applies the
     * typed-handle underflow check first (so any future direct caller also
     * gets protection), then decrements.
     *
     * The deferred-destroy trigger (next_alloc == p sentinel) is handled by
     * the caller (uproto_strand_refcount_dec) after this function returns. */
    if (p == NULL) return;
    URBI_REQUIRE(p->refcount > 0U,
                 "UProto strand-bind refcount underflow — "
                 "urbi_proto_strand_ref_release without matching acquire");
    if (p->refcount == UINT16_MAX) {
        (void)owner;
        return;
    }
    p->refcount = (uint16_t)(p->refcount - 1U);
#ifdef URBI_DEBUG
    g_strand_ref_total--;
#endif
    (void)owner;
}

/* === Debug VM lifecycle hooks + balanced-check ============================ */

#ifdef URBI_DEBUG
void
urbi_proto_ref_vm_born(void)
{
    g_active_vm_count++;
}

void
urbi_proto_ref_vm_gone(void)
{
    if (g_active_vm_count > 0)
        g_active_vm_count--;
    /* Only assert balance when the last active VM is gone.
     * With multiple concurrent VMs, peer-VM closures are still alive
     * and the totals will be non-zero until all VMs are destroyed. */
    if (g_active_vm_count == 0)
        urbi_proto_ref_assert_balanced();
}

void
urbi_proto_ref_assert_balanced(void)
{
    URBI_REQUIRE(g_closure_ref_total == 0,
                 "UProto closure-bind refcount imbalance — "
                 "urbi_proto_ref_acquire/release not paired across all VMs");
    URBI_REQUIRE(g_strand_ref_total == 0,
                 "UProto strand-bind refcount imbalance — "
                 "urbi_proto_strand_ref_acquire/release not paired across all VMs");
}
#endif /* URBI_DEBUG */
