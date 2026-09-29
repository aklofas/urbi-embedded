/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/probes/leaks.c — does ten thousand iterations cost more than one?
 *
 * tests/rt/test_leaks.c asks the same question at a thousand iterations and
 * compares two runs of the same program.  This asks it at ten thousand and
 * compares before against after inside ONE run, which is the shape a robot
 * actually has: a loop that never ends, on a device with no swap.  A cell
 * that survives one iteration by mistake costs 2 KB here and a crash on the
 * device three days in.
 *
 * The seven workloads are the allocating shapes the core has: a string
 * concatenation, a list literal, an object clone, a dictionary insert, a
 * detached strand, a joined one, and a `whenever` re-firing its own body.
 * Each allocates, drops its result immediately, and must give every byte
 * back.
 *
 * Slack is 2 KB.  The loop itself is not free — the chunk that defines it,
 * the symbols it interns, the integer that counts it — and a collection is
 * allowed to leave a partly-used block behind.  What 2 KB cannot hide is a
 * per-iteration leak: ten thousand iterations leaking a single 16-byte
 * value would be 160 KB.
 *
 * Giving the memory back AFTER a forced collection is half the question.
 * The other half is whether the loop needed it in the first place: a
 * collector that never ran until the end would pass the first check and
 * still take the device's heap in the middle of the loop.  So each
 * workload also reports the PEAK the allocator saw while the loop ran,
 * with no collection forced, against what urbi_open left live plus
 * 256 KB -- and the loop runs under a ceiling, so a regression is
 * refused rather than allowed to grow. */

#include "probe.h"

#define ITERATIONS  10000
#define LEAK_SLACK  2048u
#define PEAK_SLACK  ((size_t)256 * 1024)
#define CEILING     ((size_t)1024 * 1024)

/* Each workload is a function of n so the loop lives in a frame, which is
 * where a real program's short-lived values live too — a chunk-top `var`
 * would stay reachable from the realm and leak by definition. */
static const struct { const char *name; const char *body; } WORKLOADS[] = {
    { "string concat", "var s = \"x\" + k.asString()" },
    { "list literal",  "var l = [k, k + 1, k + 2]" },
    { "object clone",  "var o = Object.clone()" },
    { "dict build",    "var d = Dict.new(); d.set(\"k\", k)" },
    { "fork",          "{ 1 , 2 }" },
    { "fork join",     "{ 1 } & { 2 }" },
};
#define NWORKLOADS ((int)(sizeof WORKLOADS / sizeof WORKLOADS[0]))

/* The peak half: prints the high-water mark the loop reached and fails
 * it when it passed `boot` (what urbi_open left live) plus the slack, or
 * when anything was refused. */
static int peak_verdict(const char *name, const ProbeAlloc *a, size_t boot)
{
    printf("  %-14s peak %lu bytes during the loop, cap %lu (urbi_open's %lu + %lu)\n",
           name, (unsigned long)a->peak, (unsigned long)(boot + PEAK_SLACK),
           (unsigned long)boot, (unsigned long)PEAK_SLACK);
    int ok = a->peak <= boot + PEAK_SLACK && a->refused == 0;
    if (!ok)
        fprintf(stderr, "leaks: %s peaked at %lu bytes (%lu refused), cap is %lu\n",
                name, (unsigned long)a->peak, (unsigned long)a->refused,
                (unsigned long)(boot + PEAK_SLACK));
    return ok;
}

static int probe_one(const char *name, const char *body)
{
    ProbeAlloc a = { 0, 0, 0, 0, 0 };
    UVM *vm = urbi_open(probe_alloc, &a, NULL);
    if (!vm) { fprintf(stderr, "leaks: urbi_open failed\n"); return 1; }
    size_t boot = a.live;

    char src[512];
    /* One warm-up iteration first, so the before-reading already includes
     * everything the body needs permanently: its symbols, the protos it
     * reaches, the closure itself. */
    int n = snprintf(src, sizeof src,
                     "var loop = function(n) { var k = 0; while (k < n) { %s; k = k + 1 }; k }",
                     body);
    if (n < 0 || (size_t)n >= sizeof src) {
        fprintf(stderr, "leaks: workload source too long\n");
        urbi_close(vm);
        return 1;
    }
    if (probe_run(vm, src) != URBI_OK) { urbi_close(vm); return 1; }
    if (probe_run(vm, "loop(1)") != URBI_OK) { urbi_close(vm); return 1; }

    size_t before = probe_settled_bytes(vm);
    a.peak = a.live;
    a.cap = boot + CEILING;
    int rc = probe_run(vm, "loop(10000)");
    a.cap = 0;
    int peak_ok = peak_verdict(name, &a, boot);
    if (rc != URBI_OK) { urbi_close(vm); return 1; }
    size_t after = probe_settled_bytes(vm);

    size_t delta = after > before ? after - before : 0;
    printf("  %-14s %lu -> %lu bytes live over %d iterations (delta %lu)\n",
           name, (unsigned long)before, (unsigned long)after,
           ITERATIONS, (unsigned long)delta);

    int ok = delta < LEAK_SLACK && peak_ok;
    if (delta >= LEAK_SLACK)
        fprintf(stderr, "leaks: %s grew %lu bytes, slack is %u\n",
                name, (unsigned long)delta, LEAK_SLACK);

    urbi_close(vm);
    if (a.live != 0) {
        fprintf(stderr, "leaks: %s left %lu bytes out on loan after close\n",
                name, (unsigned long)a.live);
        ok = 0;
    }
    return ok ? 0 : 1;
}

/* A `whenever` whose body clears its own condition after `limit` rounds:
 * a reactive loop, run by the host raising the condition and taking one
 * unbudgeted step.  It is not a function of n like the others, so it gets
 * its own driver; the measurements are the same two. */
static int whenever_round(UVM *vm, long rounds)
{
    URealm *r = urbi_realm_main(vm);
    if (urbi_global_set(vm, r, "c", urbi_make_int(0)) != URBI_OK) return 1;
    if (urbi_global_set(vm, r, "limit", urbi_make_int(rounds)) != URBI_OK) return 1;
    if (urbi_global_set(vm, r, "g", urbi_make_int(1)) != URBI_OK) return 1;
    (void)urbi_step(vm, 0, NULL);
    UValue c = urbi_make_nil();
    if (urbi_global_get(vm, r, "c", &c) != URBI_OK || c.kind != UVAL_INT || c.v.i != rounds) {
        fprintf(stderr, "leaks: whenever ran %lld rounds of %ld\n",
                c.kind == UVAL_INT ? (long long)c.v.i : -1LL, rounds);
        return 1;
    }
    return 0;
}

static int probe_whenever(void)
{
    const char *name = "whenever";
    ProbeAlloc a = { 0, 0, 0, 0, 0 };
    UVM *vm = urbi_open(probe_alloc, &a, NULL);
    if (!vm) { fprintf(stderr, "leaks: urbi_open failed\n"); return 1; }
    size_t boot = a.live;
    if (probe_run(vm, "var g = 0; var c = 0; var limit = 1") != URBI_OK
        || probe_run(vm, "whenever (g == 1) { c = c + 1; if (c >= limit) g = 0 }") != URBI_OK
        || whenever_round(vm, 1) != 0) { urbi_close(vm); return 1; }

    size_t before = probe_settled_bytes(vm);
    a.peak = a.live;
    a.cap = boot + CEILING;
    int rc = whenever_round(vm, ITERATIONS);
    a.cap = 0;
    int ok = peak_verdict(name, &a, boot) && rc == 0;
    size_t after = probe_settled_bytes(vm);

    size_t delta = after > before ? after - before : 0;
    printf("  %-14s %lu -> %lu bytes live over %d iterations (delta %lu)\n",
           name, (unsigned long)before, (unsigned long)after,
           ITERATIONS, (unsigned long)delta);
    if (delta >= LEAK_SLACK) {
        fprintf(stderr, "leaks: %s grew %lu bytes, slack is %u\n",
                name, (unsigned long)delta, LEAK_SLACK);
        ok = 0;
    }
    urbi_close(vm);
    if (a.live != 0) {
        fprintf(stderr, "leaks: %s left %lu bytes out on loan after close\n",
                name, (unsigned long)a.live);
        ok = 0;
    }
    return ok ? 0 : 1;
}

int main(void)
{
    printf("leak probes (%d iterations, %u byte slack):\n", ITERATIONS, LEAK_SLACK);
    int bad = 0;
    for (int i = 0; i < NWORKLOADS; i++)
        bad += probe_one(WORKLOADS[i].name, WORKLOADS[i].body);
    bad += probe_whenever();
    return probe_verdict("leaks", bad == 0);
}
