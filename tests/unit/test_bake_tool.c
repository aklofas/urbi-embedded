/* SPDX-License-Identifier: BSD-3-Clause */
/* test_bake_tool.c — Phase 3 (Wave 2 / M6) bake-tool integration tests.
 *
 * Two cases:
 *
 *   1. runs_on_empty_order_file: invokes tools/urbi-compile-stdlib
 *      against an empty order file; asserts exit 0 + that the emitted
 *      .gen.c declares a 0-length blob.
 *
 *   2. deterministic_3_runs: cross-checks the standalone bake-smoke
 *      script (tests/scripts/bake_smoke.sh) for the 3-run byte-identity
 *      contract.  Exists at the unit-test layer too so that a single
 *      `make test` run catches non-determinism without requiring the
 *      full releasetest sweep.
 *
 * Both cases use system() to invoke the tool / script.  The tool's
 * binary path is relative to the repo root, which is the test runner's
 * cwd at `make test` time. */

#include "utest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int
file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void
bake_runs_on_empty_order_file(void)
{
    /* Skip the test if the bake tool has not been built — the smoke
     * gate runs before this test in releasetest, but a bare `make test`
     * may run this case without first building the tool. */
    if (!file_exists("./tools/urbi-compile-stdlib")) {
        printf("    SKIP: ./tools/urbi-compile-stdlib not built\n");
        return;
    }

    /* Empty order file. */
    int rc = system("printf '' > /tmp/test_bake_empty_order.txt");
    UASSERT_EQ(rc, 0);

    rc = system("./tools/urbi-compile-stdlib "
                "/tmp/test_bake_empty_order.txt "
                "src/stdlib "
                "/tmp/test_bake_empty_blob.gen.c "
                ">/dev/null 2>&1");
    UASSERT_EQ(rc, 0);

    /* Output should declare an empty blob. */
    FILE *f = fopen("/tmp/test_bake_empty_blob.gen.c", "r");
    UASSERT(f != NULL);
    if (f != NULL) {
        char buf[8192];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        UASSERT(strstr(buf, "urbi_stdlib_bytecode_len = 0") != NULL);
    }

    /* Cleanup. */
    remove("/tmp/test_bake_empty_order.txt");
    remove("/tmp/test_bake_empty_blob.gen.c");
}

static void
bake_deterministic_3_runs(void)
{
    /* Same skip condition as above — bake_smoke.sh fails fast if the
     * tool is not built, but we want a clean SKIP rather than a FAIL
     * in that case. */
    if (!file_exists("./tools/urbi-compile-stdlib")) {
        printf("    SKIP: ./tools/urbi-compile-stdlib not built\n");
        return;
    }

    int rc = system("tests/scripts/bake_smoke.sh >/dev/null 2>&1");
    UASSERT_EQ(rc, 0);
}

void test_bake_tool_suite(void);
void
test_bake_tool_suite(void)
{
    utest_run("bake_runs_on_empty_order_file",
              bake_runs_on_empty_order_file);
    utest_run("bake_deterministic_3_runs",
              bake_deterministic_3_runs);
}
