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
 * The five workloads are the allocating shapes the core has: a string
 * concatenation, a list literal, an object clone, a dictionary insert, and
 * a forked strand.  Each allocates, drops its result immediately, and must
 * give every byte back.
 *
 * Slack is 2 KB.  The loop itself is not free — the chunk that defines it,
 * the symbols it interns, the integer that counts it — and a collection is
 * allowed to leave a partly-used block behind.  What 2 KB cannot hide is a
 * per-iteration leak: ten thousand iterations leaking a single 16-byte
 * value would be 160 KB. */

#include "probe.h"

#define ITERATIONS  10000
#define LEAK_SLACK  2048u

/* Each workload is a function of n so the loop lives in a frame, which is
 * where a real program's short-lived values live too — a chunk-top `var`
 * would stay reachable from the realm and leak by definition. */
static const struct { const char *name; const char *body; } WORKLOADS[] = {
    { "string concat", "var s = \"x\" + k.asString()" },
    { "list literal",  "var l = [k, k + 1, k + 2]" },
    { "object clone",  "var o = Object.clone()" },
    { "dict build",    "var d = Dict.new(); d.set(\"k\", k)" },
    { "fork",          "{ 1 , 2 }" },
};
#define NWORKLOADS ((int)(sizeof WORKLOADS / sizeof WORKLOADS[0]))

static int probe_one(const char *name, const char *body)
{
    ProbeAlloc a = { 0, 0, 0 };
    UVM *vm = urbi_open(probe_alloc, &a, NULL);
    if (!vm) { fprintf(stderr, "leaks: urbi_open failed\n"); return 1; }

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
    if (probe_run(vm, "loop(10000)") != URBI_OK) { urbi_close(vm); return 1; }
    size_t after = probe_settled_bytes(vm);

    size_t delta = after > before ? after - before : 0;
    printf("  %-14s %lu -> %lu bytes live over %d iterations (delta %lu)\n",
           name, (unsigned long)before, (unsigned long)after,
           ITERATIONS, (unsigned long)delta);

    int ok = delta < LEAK_SLACK;
    if (!ok)
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

int main(void)
{
    printf("leak probes (%d iterations, %u byte slack):\n", ITERATIONS, LEAK_SLACK);
    int bad = 0;
    for (int i = 0; i < NWORKLOADS; i++)
        bad += probe_one(WORKLOADS[i].name, WORKLOADS[i].body);
    return probe_verdict("leaks", bad == 0);
}
