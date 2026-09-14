/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/rt/test_unwind.c — the cleanup-stack unwinder.
 *
 * Everything here drives real source through uexec_run_source, the same
 * entry point tests/rt/test_exec.c uses, so what is pinned is the
 * observable contract (value in, value or error out) rather than the
 * walker's internals.
 *
 * Two contracts these cases depend on, both inherited from the old core
 * and pinned by the .chk corpus:
 *   - An uncaught throw of a NON-object value recovers to nil and reports
 *     URBI_OK (control_transfer/throw_uncaught.chk); only an exception
 *     OBJECT escaping surfaces as URBI_ERR_UNCAUGHT_THROW.
 *   - `try` is an expression: its value is the body's, or the catch
 *     body's when the catch absorbed (exceptions/try_value.chk). */

#include "rtest.h"
#include <stdlib.h>
#include <string.h>

#include "urbi/urbi.h"
#include "rt/urealm.h"

static void *uw_alloc(void *p, size_t n, void *ud) {
    long *live = (long *)ud;
    if (n == 0) { if (p) (*live)--; free(p); return NULL; }
    void *q = realloc(p, n);
    if (q && !p) (*live)++;
    return q;
}

typedef struct { UVM *vm; URealm *realm; long live; } UwFix;

static void fix_open(UwFix *fx) {
    fx->live = 0;
    fx->vm = uvm_open(uw_alloc, &fx->live);
    fx->realm = fx->vm ? urealm_new(fx->vm) : NULL;
}
static void fix_close(UwFix *fx) { uvm_close(fx->vm); }

static int run(UwFix *fx, const char *src, UValue *out) {
    char err[256] = {0};
    int rc = uexec_run_source(fx->vm, fx->realm, src, strlen(src), NULL, out, err, sizeof err);
    if (rc == URBI_ERR_COMPILE) printf("    compile error: %s\n", err);
    if (rc == URBI_ERR_UNCAUGHT_THROW) printf("    threw: %s\n", fx->vm->last_error);
    return rc;
}

/* Runs `src` and checks it produced the integer `want`. */
static void run_int(UwFix *fx, const char *src, int64_t want) {
    UValue out = uv_nil();
    if (run(fx, src, &out) != URBI_OK) { RT_CHECK(0); printf("    src: %s\n", src); return; }
    RT_EQ(out.kind, (uint8_t)UV_INT);
    if (out.kind == (uint8_t)UV_INT && out.v.i != want) {
        RT_CHECK(0);
        printf("    src: %s -> %ld, want %ld\n", src, (long)out.v.i, (long)want);
    } else {
        RT_CHECK(1);
    }
}

/* (a) a throw inside try reaches the catch handler, and the try's value
 *     is the catch body's. */
static void catch_absorbs_a_throw(void) {
    UwFix fx; fix_open(&fx);
    run_int(&fx, "try { throw 1 } catch (var e) { e + 1 } |", 2);
    fix_close(&fx);
}

/* (b) a VM-raised runtime error is an ordinary catchable throw. */
static void runtime_type_error_is_catchable(void) {
    UwFix fx; fix_open(&fx);
    run_int(&fx, "var r = 0; try { 1 + \"a\" } catch (var e) { r = 1 }; r |", 1);
    fix_close(&fx);
}

/* (c) Phase 0 bug B1: a slot write against a non-object used to be fatal;
 *     it is a TypeError like any other. */
static void slot_write_on_an_atom_is_catchable(void) {
    UwFix fx; fix_open(&fx);
    run_int(&fx, "var r = 0; try { var o = 1; o.x = 2 } catch (var e) { r = 1 }; r |", 1);
    fix_close(&fx);
}

/* (d) a finally runs on the way out even when nothing catches.  The
 *     scalar throw escapes to the nil-recovery contract, so the run
 *     itself reports URBI_OK with a nil value. */
static void finally_runs_on_an_uncaught_throw(void) {
    UwFix fx; fix_open(&fx);
    UValue out = uv_nil();
    RT_EQ(run(&fx, "var r = 0 |", &out), URBI_OK);
    RT_EQ(run(&fx, "try { throw 1 } finally { r = 4 } |", &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_NIL);
    run_int(&fx, "r |", 4);
    fix_close(&fx);
}

/* (e) the inner finally runs during the walk, then the outer catch
 *     absorbs the still-pending throw. */
static void finally_then_outer_catch(void) {
    UwFix fx; fix_open(&fx);
    run_int(&fx,
            "var r = 0; try { try { throw 1 } finally { r = r + 1 } } catch (var e) { r = r + 10 }; r |",
            11);
    fix_close(&fx);
}

/* (f) a throw in a callee unwinds through the callee's frame to a
 *     handler in the caller. */
static void throw_crosses_a_call_frame(void) {
    UwFix fx; fix_open(&fx);
    run_int(&fx, "var f = function() { throw 7 }; var r = 0; try { f() } catch (var e) { r = e }; r |", 7);
    /* Two frames deep. */
    run_int(&fx,
            "var inner = function() { throw 21 }; var outer = function() { inner() };"
            "var r2 = 0; try { outer() } catch (var e) { r2 = e }; r2 |",
            21);
    fix_close(&fx);
}

/* (g) a return that crosses a try-with-finally runs the finally and still
 *     delivers the return value. */
static void return_through_a_finally(void) {
    UwFix fx; fix_open(&fx);
    run_int(&fx,
            "var side = 0; var f = function() { try { return 5 } finally { side = 9 } };"
            "var got = f(); got * 100 + side |",
            509);
    fix_close(&fx);
}

/* (h) a native's throw is catchable from script. */
static int uw_native_boom(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out) {
    (void)self; (void)args; (void)nargs; (void)out;
    return urbi_throw(vm, "TypeError", "native said no");
}

static void native_throw_is_catchable(void) {
    UwFix fx; fix_open(&fx);
    UValue out = uv_nil();
    RT_EQ(run(&fx, "var Probe = Object.clone() |", &out), URBI_OK);
    RT_EQ(urbi_register(fx.vm, "Probe.boom", uw_native_boom, 0, 0), URBI_OK);
    run_int(&fx, "var r = 0; try { Probe.boom() } catch (var e) { r = 1 }; r |", 1);
    fix_close(&fx);
}

/* (i) a thrown scalar arrives at the catch variable unchanged. */
static void thrown_scalar_reaches_the_catch_variable(void) {
    UwFix fx; fix_open(&fx);
    run_int(&fx, "var r = 0; try { throw 42 } catch (var e) { if (e == 42) { r = 1 } }; r |", 1);
    fix_close(&fx);
}

/* (j) an exception OBJECT that escapes is reported, and every finally on
 *     the way out has already run when it is. */
static void escaping_exception_is_reported_after_finallys(void) {
    UwFix fx; fix_open(&fx);
    UValue out = uv_nil();
    RT_EQ(run(&fx, "var r = 0 |", &out), URBI_OK);
    /* A VM-raised TypeError is an exception object; the finally around it
     * must have run by the time the run returns. */
    RT_EQ(run(&fx, "try { 1 + \"a\" } finally { r = 3 } |", &out), URBI_ERR_UNCAUGHT_THROW);
    RT_CHECK(strstr(fx.vm->last_error, "TypeError") != NULL);
    run_int(&fx, "r |", 3);
    fix_close(&fx);
}

/* (k) a throw raised inside a finally body replaces the unwind that was
 *     in flight (REVIVAL C-1 replace-on-raise). */
static void throw_in_a_finally_replaces_the_pending_one(void) {
    UwFix fx; fix_open(&fx);
    run_int(&fx,
            "var r = 0; try { try { throw 1 } finally { throw 2 } } catch (var e) { r = e }; r |",
            2);
    /* And a return in flight is replaced the same way. */
    run_int(&fx,
            "var g = function() { try { return 5 } finally { throw 9 } };"
            "var r2 = 0; try { g() } catch (var e) { r2 = e }; r2 |",
            9);
    fix_close(&fx);
}

/* (l) three nested trys, the innermost catch re-throwing. */
static void nested_try_rethrow(void) {
    UwFix fx; fix_open(&fx);
    run_int(&fx,
            "var t = 0;"
            "try { try { try { throw 1 } catch (var a) { t = t + 1; throw a + 10 } }"
            "      catch (var b) { t = t + 2; throw b + 100 } }"
            "catch (var c) { t = t + c }; t |",
            114);
    fix_close(&fx);
}

/* The strand keeps running normally after a caught throw, and the walker
 * leaves no cleanup entries behind that a later unwind could trip on. */
static void execution_resumes_after_a_catch(void) {
    UwFix fx; fix_open(&fx);
    run_int(&fx, "var u = 0; try { 1.noSuchSlot } catch (var e) {}; u = 2; u |", 2);
    run_int(&fx, "var n = 0; var i = 0; while (i < 3) { try { throw i } catch (var e) { n = n + e }; i = i + 1 }; n |", 3);
    fix_close(&fx);
}

RT_SUITE(rt_unwind_suite) {
    rt_run("catch_absorbs_a_throw", catch_absorbs_a_throw);
    rt_run("runtime_type_error_is_catchable", runtime_type_error_is_catchable);
    rt_run("slot_write_on_an_atom_is_catchable", slot_write_on_an_atom_is_catchable);
    rt_run("finally_runs_on_an_uncaught_throw", finally_runs_on_an_uncaught_throw);
    rt_run("finally_then_outer_catch", finally_then_outer_catch);
    rt_run("throw_crosses_a_call_frame", throw_crosses_a_call_frame);
    rt_run("return_through_a_finally", return_through_a_finally);
    rt_run("native_throw_is_catchable", native_throw_is_catchable);
    rt_run("thrown_scalar_reaches_the_catch_variable", thrown_scalar_reaches_the_catch_variable);
    rt_run("escaping_exception_is_reported_after_finallys", escaping_exception_is_reported_after_finallys);
    rt_run("throw_in_a_finally_replaces_the_pending_one", throw_in_a_finally_replaces_the_pending_one);
    rt_run("nested_try_rethrow", nested_try_rethrow);
    rt_run("execution_resumes_after_a_catch", execution_resumes_after_a_catch);
}
