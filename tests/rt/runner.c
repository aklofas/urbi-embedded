#include "rtest.h"
int rt_checks = 0, rt_failures = 0;
static int cases = 0, failed = 0;
void rt_run(const char *name, void (*fn)(void)) {
    int before = rt_failures; fn(); cases++;
    if (rt_failures > before) { failed++; printf("  FAIL %s\n", name); }
    else printf("  PASS %s\n", name);
}
/* RT_SUITES: one extern + one call per suite; tasks append here. */
extern void rt_value_suite(void);
int main(void) {
    rt_run("value", rt_value_suite);
    printf("rt: %d cases, %d failed, %d checks\n", cases, failed, rt_checks);
    return failed ? 1 : 0;
}
