/* SPDX-License-Identifier: BSD-3-Clause */
/* Minimal test harness. Header-only, zero dependencies, pure C99. */

#ifndef UTEST_H
#define UTEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Counters — defined in runner.c. */
extern int utest_checks;
extern int utest_failures;
extern int utest_cases_run;
extern int utest_cases_failed;

/* Run one test case. Called from each test file's suite function. */
void utest_run(const char *name, void (*fn)(void));

#define UASSERT(cond)                                               \
    do {                                                            \
        utest_checks++;                                             \
        if (!(cond)) {                                              \
            utest_failures++;                                       \
            printf("  FAIL: %s:%d: %s\n",                           \
                __FILE__, __LINE__, #cond);                         \
            fflush(stdout);                                         \
        }                                                           \
    } while (0)

#define UASSERT_EQ(a, b)                                            \
    do {                                                            \
        utest_checks++;                                             \
        long long _a = (long long)(a);                              \
        long long _b = (long long)(b);                              \
        if (_a != _b) {                                             \
            utest_failures++;                                       \
            printf(                                                 \
                "  FAIL: %s:%d: %s == %s (got %lld, expected %lld)\n", \
                __FILE__, __LINE__, #a, #b, _a, _b);                \
            fflush(stdout);                                         \
        }                                                           \
    } while (0)

#define UASSERT_STR_EQ(a, b)                                        \
    do {                                                            \
        utest_checks++;                                             \
        const char *_a = (a);                                       \
        const char *_b = (b);                                       \
        if (strcmp(_a, _b) != 0) {                                  \
            utest_failures++;                                       \
            printf(                                                 \
                "  FAIL: %s:%d: %s == %s (got \"%s\", expected \"%s\")\n", \
                __FILE__, __LINE__, #a, #b, _a, _b);                \
            fflush(stdout);                                         \
        }                                                           \
    } while (0)

#endif
