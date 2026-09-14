#ifndef RTEST_H
#define RTEST_H
#include <stdio.h>
#include <string.h>
extern int rt_checks, rt_failures;
#define RT_CHECK(cond) do { rt_checks++; if (!(cond)) { rt_failures++; \
    printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define RT_EQ(a, b) RT_CHECK((a) == (b))
#define RT_STREQ(a, b) RT_CHECK(strcmp((a), (b)) == 0)
void rt_run(const char *name, void (*fn)(void));
#define RT_SUITE(name) void name(void)
#endif
