/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/probes/lookup_bench.c — how fast is the re-founded core, against
 * the core it replaced?
 *
 * tests/probes/baseline-timings.md records what the OLD runtime did with
 * these two programs, measured on this machine before it was deleted, five
 * runs, median taken.  This probe runs the same two programs the same way
 * — as `urbi -e "<source>"` subprocesses, so process startup is inside both
 * numbers — and compares medians.
 *
 * ---------------------------------------------------------------------
 * The 20 percent gate
 * ---------------------------------------------------------------------
 *
 * The re-foundation spec asks for both programs to land within 20
 * percent of the old core.  The core as first re-founded did not (it had
 * no slot cache and dispatched through a plain switch); it does now,
 * through a one-entry cache per slot-access site (src/rt/uslotcache.h),
 * threaded dispatch, and a fast path for a yield with nobody to yield
 * to.  The ceilings below ARE the spec's 1.2: a change that makes either
 * program slower than that fails here.
 *
 * ---------------------------------------------------------------------
 * Run this alone
 * ---------------------------------------------------------------------
 *
 * `make test-bench` -- not inside `make test`, which is itself one gate
 * of releasetest's parallel sweep.  Measured under -j32 beside the other
 * gates' compiles this probe reported 4.99x; solo, on the same tree,
 * 1.46x.  A wall-clock number taken on a saturated box is the box's
 * number.
 *
 * Nor inside releasetest, which CI runs: the baseline is seconds on the
 * machine that recorded it, and a slower runner fails the ratchet with
 * nothing regressed.  Run it on that machine.
 *
 * Even solo it is noisy: a shared machine, a 21-millisecond workload.
 * The ceilings carry slack for that, which is why they are not pinned at
 * exactly the measured ratio. */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

#define RUNS 5

/* The medians from tests/probes/baseline-timings.md, in seconds. */
typedef struct {
    const char *name;
    const char *file;
    double      baseline;   /* old-core median, seconds */
    double      ceiling;    /* ratchet: fail above this multiple of baseline */
} Bench;

/* Spec target, for the report line; the ceilings below are pinned to it. */
#define SPEC_RATIO 1.20

static const Bench BENCHES[] = {
    /* 3-deep proto chain, 200k reads + 100k read-modify-writes of one slot. */
    { "lookup_bench",    "lookup_bench.u",    0.021, 1.20 },
    /* 320x125 Mandelbrot: arithmetic and calls, almost no allocation. */
    { "mandelbrot_host", "mandelbrot_host.u", 0.209, 1.20 },
};
#define NBENCH ((int)(sizeof BENCHES / sizeof BENCHES[0]))

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* One `urbi -e <src>` run, wall clock in seconds.  Negative on failure. */
static double time_one(const char *urbi, const char *src)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid_t pid = fork();
    if (pid < 0) return -1.0;
    if (pid == 0) {
        /* The benchmarks print their checksum; the probe prints timings.
         * Keeping both would bury the numbers this probe exists for. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        char *argv[4];
        argv[0] = (char *)urbi;
        argv[1] = (char *)"-e";
        argv[2] = (char *)src;
        argv[3] = NULL;
        execv(urbi, argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1.0;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1.0;
    return (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *urbi = (argc > 1) ? argv[1] : "build/host/urbi";
    const char *dir  = (argc > 2) ? argv[2] : "tests/probes";

    printf("lookup benchmark (%d runs, median; baselines from "
           "tests/probes/baseline-timings.md):\n", RUNS);

    int bad = 0;
    for (int b = 0; b < NBENCH; b++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, BENCHES[b].file);
        char *src = read_file(path);
        if (!src) { fprintf(stderr, "lookup_bench: cannot read %s\n", path); return 1; }

        double t[RUNS];
        for (int i = 0; i < RUNS; i++) {
            t[i] = time_one(urbi, src);
            if (t[i] < 0.0) {
                fprintf(stderr, "lookup_bench: %s did not run cleanly under %s\n",
                        BENCHES[b].file, urbi);
                free(src);
                return 1;
            }
        }
        free(src);
        qsort(t, RUNS, sizeof t[0], cmp_double);
        double median = t[RUNS / 2];
        double ratio  = median / BENCHES[b].baseline;

        printf("  %-16s old %.3f s -> new %.3f s  = %.2fx"
               "  (spec %.2fx %s)\n",
               BENCHES[b].name, BENCHES[b].baseline, median, ratio,
               SPEC_RATIO,
               ratio <= BENCHES[b].ceiling ? "PASS" : "FAIL");

        if (ratio > BENCHES[b].ceiling) {
            fprintf(stderr, "lookup_bench: %s at %.2fx is above the %.2fx ratchet\n",
                    BENCHES[b].name, ratio, BENCHES[b].ceiling);
            bad++;
        }
    }
    printf("lookup_bench: %s\n", bad == 0 ? "PASS" : "FAIL");
    return bad == 0 ? 0 : 1;
}
