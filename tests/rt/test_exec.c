#include "rtest.h"
#include "fakevm.h"
#include <string.h>

/* These suites drive the real entry point (uexec_run_source) end to end:
 * source in, value out, through the kept frontend and the new dispatch
 * loop.  The VM is heap-allocated via uvm_open here rather than through
 * fakevm_init, because uexec_run_source needs realms and spare strands. */

static void *count_alloc(void *p, size_t n, void *ud) {
    long *live = (long *)ud;
    if (n == 0) { if (p) (*live)--; free(p); return NULL; }
    void *q = realloc(p, n);
    if (q && !p) (*live)++;
    return q;
}

typedef struct { UVM *vm; URealm *realm; long live; } ExecFix;

static void fix_open(ExecFix *fx) {
    fx->live = 0;
    fx->vm = uvm_open(count_alloc, &fx->live);
    fx->realm = fx->vm ? urealm_new(fx->vm) : NULL;
}
static void fix_close(ExecFix *fx) { uvm_close(fx->vm); }

static int run(ExecFix *fx, const char *src, UValue *out) {
    char err[256] = {0};
    int rc = uexec_run_source(fx->vm, fx->realm, src, strlen(src), NULL, out, err, sizeof err);
    if (rc == URBI_ERR_COMPILE) printf("    compile error: %s\n", err);
    if (rc == URBI_ERR_UNCAUGHT_THROW) printf("    threw: %s\n", fx->vm->last_error);
    return rc;
}

static void int_arithmetic(void) {
    ExecFix fx; fix_open(&fx);
    UValue out;
    RT_EQ(run(&fx, "1 + 2 |", &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_INT);
    RT_EQ(out.v.i, 3);
    RT_EQ(run(&fx, "(1 + 2) * 3 - 4 |", &out), URBI_OK);
    RT_EQ(out.v.i, 5);
    /* Int/Int division always promotes to Float. */
    RT_EQ(run(&fx, "5 / 2 |", &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_FLOAT);
    RT_CHECK(out.v.f == 2.5);
    fix_close(&fx);
}

static void function_call_returns_value(void) {
    ExecFix fx; fix_open(&fx);
    UValue out;
    RT_EQ(run(&fx, "var f = function(x) { x * 2 } |", &out), URBI_OK);
    RT_EQ(run(&fx, "f(21) |", &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_INT);
    RT_EQ(out.v.i, 42);
    fix_close(&fx);
}

static void upvalue_outlives_defining_frame(void) {
    ExecFix fx; fix_open(&fx);
    UValue out;
    /* make_counter's `n` is captured by the inner closure and read back
     * after make_counter's own frame has returned and its registers have
     * been reused by three further calls. */
    RT_EQ(run(&fx, "var make = function() { var n = 0; function() { n = n + 1; n } } |", &out), URBI_OK);
    RT_EQ(run(&fx, "var c = make() |", &out), URBI_OK);
    RT_EQ(run(&fx, "c(); c(); c() |", &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_INT);
    RT_EQ(out.v.i, 3);
    /* A second counter must have its own cell. */
    RT_EQ(run(&fx, "var d = make(); d() |", &out), URBI_OK);
    RT_EQ(out.v.i, 1);
    fix_close(&fx);
}

static void type_error_is_a_throw(void) {
    ExecFix fx; fix_open(&fx);
    UValue out;
    int rc = run(&fx, "1 + \"a\" |", &out);
    RT_EQ(rc, URBI_ERR_UNCAUGHT_THROW);
    RT_CHECK(strstr(fx.vm->last_error, "TypeError") != NULL);
    /* The VM survives the throw and keeps evaluating. */
    RT_EQ(run(&fx, "40 + 2 |", &out), URBI_OK);
    RT_EQ(out.v.i, 42);
    fix_close(&fx);
}

static void division_by_zero_is_a_throw(void) {
    ExecFix fx; fix_open(&fx);
    UValue out;
    RT_EQ(run(&fx, "1 / 0 |", &out), URBI_ERR_UNCAUGHT_THROW);
    RT_CHECK(strstr(fx.vm->last_error, "DivisionByZero") != NULL);
    fix_close(&fx);
}

static void string_concat_and_compare(void) {
    ExecFix fx; fix_open(&fx);
    UValue out;
    RT_EQ(run(&fx, "\"ab\" + \"cd\" |", &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_STR);
    uint32_t len; const char *b = uv_str_bytes(out, &len);
    RT_EQ(len, 4u);
    RT_CHECK(memcmp(b, "abcd", 4) == 0);
    RT_EQ(run(&fx, "\"ab\" == \"ab\" |", &out), URBI_OK);
    RT_CHECK(out.kind == (uint8_t)UV_BOOL && out.v.i != 0);
    fix_close(&fx);
}

static void control_flow(void) {
    ExecFix fx; fix_open(&fx);
    UValue out;
    RT_EQ(run(&fx, "if (1 < 2) { 10 } else { 20 } |", &out), URBI_OK);
    RT_EQ(out.v.i, 10);
    RT_EQ(run(&fx, "if (2 < 1) { 10 } else { 20 } |", &out), URBI_OK);
    RT_EQ(out.v.i, 20);
    RT_EQ(run(&fx, "var i = 0; var acc = 0; while (i < 5) { acc = acc + i; i = i + 1 }; acc |", &out), URBI_OK);
    RT_EQ(out.v.i, 10);
    fix_close(&fx);
}

static void deep_recursion_and_reclaim(void) {
    ExecFix fx; fix_open(&fx);
    UValue out;
    RT_EQ(run(&fx, "var r = function(n) { if (n == 0) { 0 } else { r(n - 1) } } |", &out), URBI_OK);
    RT_EQ(run(&fx, "r(10000) |", &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_INT);
    RT_EQ(out.v.i, 0);
    /* The register stack really did grow past 10000 frames' worth of
     * windows, and the spare strand still holds that allocation. */
    RT_CHECK(fx.vm->spare != NULL && fx.vm->spare->stack_cap >= 10000u);
    /* Dropping the recursive closure lets the whole chunk go: the chunk
     * cell is reachable only through the closure's root back-pointer. */
    RT_EQ(run(&fx, "var r = nil |", &out), URBI_OK);
    uint32_t before = fx.vm->gc.cells_live;
    ugc_collect(fx.vm);
    RT_CHECK(fx.vm->gc.cells_live < before);
    fix_close(&fx);
    RT_EQ(fx.live, 0L);   /* every allocation handed back */
}

static void unknown_opcode_throws(void) {
    ExecFix fx; fix_open(&fx);
    UValue out;
    /* `,` compiles to OP_FORK_DETACH, which this build does not dispatch. */
    int rc = run(&fx, "1 , 2 |", &out);
    RT_CHECK(rc == URBI_ERR_UNCAUGHT_THROW || rc == URBI_ERR_COMPILE);
    if (rc == URBI_ERR_UNCAUGHT_THROW)
        RT_CHECK(strstr(fx.vm->last_error, "opcode not available") != NULL);
    fix_close(&fx);
}

RT_SUITE(rt_exec_suite) {
    rt_run("int_arithmetic", int_arithmetic);
    rt_run("function_call_returns_value", function_call_returns_value);
    rt_run("upvalue_outlives_defining_frame", upvalue_outlives_defining_frame);
    rt_run("type_error_is_a_throw", type_error_is_a_throw);
    rt_run("division_by_zero_is_a_throw", division_by_zero_is_a_throw);
    rt_run("string_concat_and_compare", string_concat_and_compare);
    rt_run("control_flow", control_flow);
    rt_run("deep_recursion_and_reclaim", deep_recursion_and_reclaim);
    rt_run("unknown_opcode_throws", unknown_opcode_throws);
}
