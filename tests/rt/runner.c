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
extern void rt_gc_suite(void);
extern void rt_str_suite(void);
extern void rt_obj_suite(void);
extern void rt_list_suite(void);
extern void rt_strand_suite(void);
extern void rt_exec_suite(void);
extern void rt_sched_suite(void);
extern void rt_watch_suite(void);
extern void rt_unwind_suite(void);
extern void rt_api_suite(void);
extern void rt_realm_suite(void);
extern void rt_leaks_suite(void);
extern void rt_slotcache_suite(void);
extern void rt_ops_v2_suite(void);
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);   /* a crashing suite must not lose the log */ 
    rt_run("value", rt_value_suite);
    rt_run("gc", rt_gc_suite);
    rt_run("str", rt_str_suite);
    rt_run("obj", rt_obj_suite);
    rt_run("list", rt_list_suite);
    rt_run("strand", rt_strand_suite);
    rt_run("exec", rt_exec_suite);
    rt_run("sched", rt_sched_suite);
    rt_run("watch", rt_watch_suite);
    rt_run("unwind", rt_unwind_suite);
    rt_run("api", rt_api_suite);
    rt_run("realm", rt_realm_suite);
    rt_run("leaks", rt_leaks_suite);
    rt_run("slotcache", rt_slotcache_suite);
    rt_run("ops_v2", rt_ops_v2_suite);
    printf("rt: %d cases, %d failed, %d checks\n", cases, failed, rt_checks);
    return failed ? 1 : 0;
}
