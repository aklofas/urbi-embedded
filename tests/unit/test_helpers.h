/* SPDX-License-Identifier: BSD-3-Clause */
/* test_helpers.h — shared test utilities for urbi-embedded unit tests.
 *
 * Include this header instead of defining EXPECT_ABORT locally.
 * Requires: <sys/types.h>, <sys/wait.h>, <signal.h>, <unistd.h>,
 *           <string.h>, <stdio.h> (all available on the host test runner). */

#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

/* SIGABRT handler.  Coverage builds register a .gcda writer via atexit;
 * abort() does NOT run atexit handlers, so any line hit before abort() is
 * lost in the child unless we run atexit explicitly.  exit() (lowercase)
 * does run atexit then _exit — so swapping abort's terminate-with-SIGABRT
 * for exit(non-zero) preserves the "child exits abnormally" signal seen by
 * EXPECT_ABORT (WIFEXITED && WEXITSTATUS != 0) AND lets the gcov atexit
 * hook flush counters. */
static void abort_gcov_flush_handler(int sig) {
    (void)sig;
    exit(134);  /* runs atexit (writes .gcda); WEXITSTATUS sees 134, so the
                  * EXPECT_ABORT-detection (WIFEXITED && exit != 0) still
                  * fires — abort is still detected. */
}

/* EXPECT_ABORT: assert that expr causes abort (signal or non-zero exit).
 * Available in both URBI_DEBUG and non-DEBUG builds; unit tests always run
 * on hosted-POSIX (only freestanding cross-builds lack <sys/wait.h>).
 * T119 (COV-001) ungated this from URBI_DEBUG and added the gcov-flush
 * SIGABRT handler so coverage builds record the hosted-abort branch. */
#define EXPECT_ABORT(expr)                                               \
    do {                                                                 \
        utest_checks++;                                                  \
        pid_t _pid = fork();                                             \
        if (_pid == 0) {                                                 \
            struct sigaction _sa;                                        \
            memset(&_sa, 0, sizeof(_sa));                                \
            _sa.sa_handler = abort_gcov_flush_handler;                   \
            sigaction(SIGABRT, &_sa, NULL);                              \
            (expr);                                                      \
            _exit(0);                                                    \
        }                                                                \
        int _st = 0;                                                     \
        waitpid(_pid, &_st, 0);                                          \
        int _aborted = WIFSIGNALED(_st) ||                               \
                       (WIFEXITED(_st) && WEXITSTATUS(_st) != 0);        \
        if (!_aborted) {                                                 \
            utest_failures++;                                            \
            printf("  FAIL: %s:%d: " #expr " did not abort\n",           \
                   __FILE__, __LINE__);                                  \
            fflush(stdout);                                              \
        }                                                                \
    } while (0)

#endif /* TEST_HELPERS_H */
