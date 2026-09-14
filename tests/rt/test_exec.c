#include "rtest.h"
#include "fakevm.h"
#include <stdlib.h>
#include <string.h>

#include "urbi/urbi.h"
#include "rt/urealm.h"
#include "emit/ufront.h"
#include "chunk/uchunk.h"

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
    RT_CHECK(strstr(fx.vm->last_error, "DivByZero") != NULL);
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
    /* `at (cond) body` compiles to OP_AT_INSTALL, which this build does
     * not dispatch -- the reactive runtime is its own task. */
    int rc = run(&fx, "at (1 == 1) 2 |", &out);
    RT_CHECK(rc == URBI_ERR_UNCAUGHT_THROW || rc == URBI_ERR_COMPILE);
    if (rc == URBI_ERR_UNCAUGHT_THROW)
        RT_CHECK(strstr(fx.vm->last_error, "opcode not available") != NULL);
    fix_close(&fx);
}

/* Regression: the loader's OP_JMP bound check resolved forward and
 * backward offsets with the same rule, so a forward jump landing on
 * exactly instr_count passed every verifier pass and the dispatch loop
 * then fetched instructions[instr_count] -- the uninitialised slack
 * between instr_count and instr_cap. */
static void forward_jmp_past_end_is_rejected(void) {
    ExecFix fx; fix_open(&fx);
    const char *src = "var i = 0; while (i < 3) { i = i + 1 } |";
    char err[256] = {0};
    UProto *root = NULL;
    RT_EQ(ufront_compile(fx.vm, src, strlen(src), "<unit>", &root, err, sizeof err), URBI_OK);

    /* Retarget the first forward JMP so it resolves to exactly
     * instr_count.  Forward offsets are relative to the instruction after
     * the jump, so the encoded offset is instr_count - k - 1. */
    int patched = 0;
    for (size_t k = 0; k < root->instr_count; k++) {
        uint32_t ins = root->instructions[k];
        if ((ins & 0xFFu) != (uint32_t)OP_JMP) continue;
        int off = (int)((ins >> 16) & 0xFFFFu) - 32768;
        if (off < 0) continue;                      /* a back-edge; leave it alone */
        int want = (int)root->instr_count - (int)k - 1;
        root->instructions[k] = (ins & 0x0000FFFFu) | ((uint32_t)(want + 32768) << 16);
        patched = 1;
        break;
    }
    RT_CHECK(patched);

    ptrdiff_t need = ufront_serialize(root, NULL, 0);
    RT_CHECK(need > 0);
    unsigned char *buf = (unsigned char *)malloc((size_t)need);
    RT_CHECK(buf != NULL);
    RT_CHECK(ufront_serialize(root, buf, (size_t)need) == need);
    uchunk_destroy(root, NULL);

    UValue out;
    int rc = urbi_load(fx.vm, fx.realm, buf, (size_t)need, &out);
    RT_CHECK(rc != URBI_OK);
    RT_CHECK(strstr(fx.vm->last_error, "OP_JMP") != NULL);
    free(buf);
    fix_close(&fx);
}

static int reg_probe(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out) {
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = uv_int(1);
    return UEXEC_OK;
}

/* Regression: urbi_register used to pin its owner object through
 * urbi_ref and unpin it unconditionally, which silently released a pin
 * the embedder had taken.  It roots on a spare strand now, so a host pin
 * survives the call and only the host's own urbi_unref clears it. */
static void register_preserves_a_host_pin(void) {
    ExecFix fx; fix_open(&fx);

    /* An object reachable from nothing but this local. */
    UObject *o = uobj_new(fx.vm, NULL);
    RT_CHECK(o != NULL);
    UValue ov = uv_obj(o);
    urbi_ref(fx.vm, ov);

    /* The pin alone keeps it alive across a collection. */
    ugc_collect(fx.vm);
    RT_CHECK((o->cell.flags & UCELL_F_PINNED) != 0);
    RT_EQ(o->cell.type, (uint8_t)UCELL_OBJ);

    /* Make it reachable by name so urbi_register's path walk finds it. */
    RT_EQ(urbi_global_set(fx.vm, fx.realm, "Probe", ov), URBI_OK);
    RT_EQ(urbi_register(fx.vm, "Probe.ping", reg_probe, 0, 0), URBI_OK);

    /* The host's pin is still there. */
    RT_CHECK((o->cell.flags & UCELL_F_PINNED) != 0);
    ugc_collect(fx.vm);
    RT_EQ(o->cell.type, (uint8_t)UCELL_OBJ);

    UValue v;
    RT_EQ(run(&fx, "Probe.ping() |", &v), URBI_OK);
    RT_EQ(v.v.i, 1);

    urbi_unref(fx.vm, ov);
    RT_CHECK((o->cell.flags & UCELL_F_PINNED) == 0);
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
    rt_run("forward_jmp_past_end_is_rejected", forward_jmp_past_end_is_rejected);
    rt_run("register_preserves_a_host_pin", register_preserves_a_host_pin);
}
