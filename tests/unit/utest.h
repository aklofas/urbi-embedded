/* SPDX-License-Identifier: BSD-3-Clause */
/* Minimal test harness. Header-only, zero dependencies. */

#ifndef UTEST_H
#define UTEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*utest_fn)(void);

typedef struct {
    const char *name;
    utest_fn fn;
} utest_case;

/* Linker-collected array of tests. Each UTEST() call appends one entry. */
extern utest_case utest_cases[];
extern int utest_count;

/* Internal counters. */
extern int utest_checks;
extern int utest_failures;
extern const char *utest_current;

#define UTEST_MAX_CASES 4096

#define UTEST(test_name)                                            \
    static void utest_##test_name(void);                            \
    __attribute__((constructor)) static void utest_register_##test_name(void) { \
        if (utest_count < UTEST_MAX_CASES) {                        \
            utest_cases[utest_count].name = #test_name;             \
            utest_cases[utest_count].fn = utest_##test_name;        \
            utest_count++;                                          \
        }                                                           \
    }                                                               \
    static void utest_##test_name(void)

#define UASSERT(cond)                                               \
    do {                                                            \
        utest_checks++;                                             \
        if (!(cond)) {                                              \
            utest_failures++;                                       \
            fprintf(stderr, "  FAIL: %s:%d: %s\n",                  \
                __FILE__, __LINE__, #cond);                         \
        }                                                           \
    } while (0)

#define UASSERT_EQ(a, b)                                            \
    do {                                                            \
        utest_checks++;                                             \
        long long _a = (long long)(a);                              \
        long long _b = (long long)(b);                              \
        if (_a != _b) {                                             \
            utest_failures++;                                       \
            fprintf(stderr,                                         \
                "  FAIL: %s:%d: %s == %s (got %lld, expected %lld)\n", \
                __FILE__, __LINE__, #a, #b, _a, _b);                \
        }                                                           \
    } while (0)

#define UASSERT_STR_EQ(a, b)                                        \
    do {                                                            \
        utest_checks++;                                             \
        const char *_a = (a);                                       \
        const char *_b = (b);                                       \
        if (strcmp(_a, _b) != 0) {                                  \
            utest_failures++;                                       \
            fprintf(stderr,                                         \
                "  FAIL: %s:%d: %s == %s (got \"%s\", expected \"%s\")\n", \
                __FILE__, __LINE__, #a, #b, _a, _b);                \
        }                                                           \
    } while (0)

#endif
