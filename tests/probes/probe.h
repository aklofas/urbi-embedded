/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/probes/probe.h — what the four probes share.
 *
 * A probe is not a test.  A test says yes or no about behaviour; a probe
 * measures a number the project has committed to and fails the build when
 * the number moves the wrong way.  Each one prints what it measured even
 * when it passes, because the number is the point: the release notes and
 * docs/internals quote these, and a reader should be able to regenerate
 * them with `make test-probes` rather than trust a transcription.
 *
 * Everything here goes through the public API.  A probe that reached into
 * the core would measure the core's idea of its own footprint rather than
 * the embedder's. */

#ifndef URBI_PROBE_H
#define URBI_PROBE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "urbi/urbi.h"

/* --- a counting allocator ------------------------------------------------
 *
 * urbi_gc_stats reports what the COLLECTOR knows: cell bytes that survived
 * the last cycle plus the raw arrays those cells own.  That is the right
 * number for a leak probe and the wrong one for a footprint probe, which
 * has to include everything the VM asked the host for — the symbol table,
 * the boot chunk, the strand stacks.  So the footprint probes count bytes
 * at the allocator instead, which is exactly what an embedder's heap
 * high-water mark would see. */

typedef struct {
    size_t live;        /* bytes currently out on loan */
    size_t peak;        /* high-water mark */
    size_t blocks;
    size_t cap;         /* when nonzero, a request past it is refused */
    size_t refused;
} ProbeAlloc;

/* Each block is handed back with its size in a header, since realloc-shaped
 * allocators are not told the old size. */
typedef struct { size_t n; double _align; } ProbeHdr;

static inline void *probe_alloc(void *ptr, size_t n, void *ud)
{
    ProbeAlloc *a = (ProbeAlloc *)ud;
    ProbeHdr *h = ptr ? ((ProbeHdr *)ptr - 1) : NULL;
    if (n == 0) {
        if (h) { a->live -= h->n; a->blocks--; free(h); }
        return NULL;
    }
    size_t old = h ? h->n : 0;
    if (a->cap && a->live - old + n > a->cap) { a->refused++; return NULL; }
    ProbeHdr *nh = (ProbeHdr *)realloc(h, sizeof(ProbeHdr) + n);
    if (!nh) return NULL;
    nh->n = n;
    a->live += n - old;
    if (!h) a->blocks++;
    if (a->live > a->peak) a->peak = a->live;
    return nh + 1;
}

/* --- running a program ---------------------------------------------------
 *
 * Reports the failure itself and returns non-zero, so a probe that cannot
 * run its workload fails loudly rather than measuring an empty VM. */
static inline int probe_run(UVM *vm, const char *src)
{
    UValue out = urbi_make_nil();
    char err[256] = { 0 };
    int rc = urbi_run(vm, urbi_realm_main(vm), src, strlen(src), "<probe>",
                      &out, err, sizeof err);
    if (rc != URBI_OK) {
        UErrorInfo info;
        urbi_last_error(vm, &info);
        fprintf(stderr, "probe: program failed rc=%d err=%s last=%s\n",
                rc, err, info.message ? info.message : "");
    }
    return rc;
}

/* Live bytes as the collector sees them, after a full cycle.  bytes_live is
 * written BY a collection, so reading it without collecting first reports
 * the previous cycle's answer. */
static inline size_t probe_settled_bytes(UVM *vm)
{
    urbi_gc_collect(vm);
    UGcStats st;
    if (urbi_gc_stats(vm, &st) != URBI_OK) return 0;
    return st.bytes_live;
}

/* --- verdict ------------------------------------------------------------- */

static inline int probe_verdict(const char *name, int ok)
{
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

#endif /* URBI_PROBE_H */
