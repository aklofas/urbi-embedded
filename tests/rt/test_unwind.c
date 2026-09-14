/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/rt/test_unwind.c — the cleanup-stack unwinder.
 *
 * Everything here drives real source through uexec_run_source, the same
 * entry point tests/rt/test_exec.c uses, so what is pinned is the
 * observable contract (value in, value or error out) rather than the
 * walker's internals.
 *
 * Two contracts these cases depend on:
 *   - An uncaught throw of ANY value kills the strand and is reported as
 *     URBI_ERR_UNCAUGHT_THROW (spec section 9).  The old core swallowed
 *     non-object throws and answered nil; the re-foundation reports them.
 *     vm->last_error holds the exception's `message` for an object and
 *     the formatted value otherwise, which the REPL renders as
 *     "!!! <that>".
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

/* (d) a finally runs on the way out even when nothing catches, and the
 *     throw is then reported rather than swallowed. */
static void finally_runs_on_an_uncaught_throw(void) {
    UwFix fx; fix_open(&fx);
    UValue out = uv_nil();
    RT_EQ(run(&fx, "var r = 0 |", &out), URBI_OK);
    RT_EQ(run(&fx, "try { throw 1 } finally { r = 4 } |", &out), URBI_ERR_UNCAUGHT_THROW);
    RT_STREQ(fx.vm->last_error, "1");
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
    /* The fixture VM is unbooted (no stdlib prototypes), so the host
     * object the native hangs off is built directly, exactly as
     * test_exec.c's register_preserves_a_host_pin does. */
    UObject *o = uobj_new(fx.vm, NULL);
    RT_CHECK(o != NULL);
    UValue ov = uv_obj(o);
    urbi_ref(fx.vm, ov);
    RT_EQ(urbi_global_set(fx.vm, fx.realm, "Probe", ov), URBI_OK);
    RT_EQ(urbi_register(fx.vm, "Probe.boom", uw_native_boom, 0, 0), URBI_OK);
    run_int(&fx, "var r = 0; try { Probe.boom() } catch (var e) { r = 1 }; r |", 1);
    urbi_unref(fx.vm, ov);
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

/* A nested proto's line table has to stand on its own: the walker decodes
 * it from line 0, so the emitter cannot let it continue the enclosing
 * proto's line state.  The literal opens on line 4 and raises on line 6.
 *
 * The `var seed` on line 1 is load-bearing.  Without a statement ahead of
 * the literal the enclosing proto has emitted nothing, the emitter's
 * prev_line is still 0, and the child's first instruction gets an
 * absolute checkpoint that masks the bug entirely. */
static const char *const uw_nested_src =
    "var seed = 1;\n"                                     /* 1 */
    "\n"                                                  /* 2 */
    "\n"                                                  /* 3 */
    "var f = function() {\n"                              /* 4 */
    "  var q = 1;\n"                                      /* 5 */
    "  q + \"a\"\n"                                       /* 6 */
    "}\n";                                                /* 7 */

static void nested_proto_reports_its_own_source_line(void) {
    UwFix fx; fix_open(&fx);
    UValue out = uv_nil();
    RT_EQ(run(&fx, uw_nested_src, &out), URBI_OK);
    RT_EQ(run(&fx, "f() |", &out), URBI_ERR_UNCAUGHT_THROW);
    if (strstr(fx.vm->last_error, "line 6: ") == NULL) {
        RT_CHECK(0);
        printf("    last_error: %s\n", fx.vm->last_error);
    } else {
        RT_CHECK(1);
    }
    fix_close(&fx);
}

/* The `line` slot on the exception object carries the same position on
 * its own, so a handler can read it without parsing the message. */
static void the_exception_carries_its_line(void) {
    UwFix fx; fix_open(&fx);
    UValue out = uv_nil();
    RT_EQ(run(&fx, uw_nested_src, &out), URBI_OK);
    run_int(&fx, "var n = 0; try { f() } catch (var e) { n = e.line }; n |", 6);
    fix_close(&fx);
}

/* Spec section 9: an uncaught throw of ANY value is fatal, and the
 * diagnostic is the formatted value when it is not an exception object. */
static void an_escaping_scalar_throw_is_fatal(void) {
    UwFix fx; fix_open(&fx);
    UValue out = uv_nil();
    RT_EQ(run(&fx, "throw 42 |", &out), URBI_ERR_UNCAUGHT_THROW);
    RT_STREQ(fx.vm->last_error, "42");
    RT_EQ(run(&fx, "throw \"boom\" |", &out), URBI_ERR_UNCAUGHT_THROW);
    RT_STREQ(fx.vm->last_error, "\"boom\"");
    RT_EQ(run(&fx, "throw true |", &out), URBI_ERR_UNCAUGHT_THROW);
    RT_STREQ(fx.vm->last_error, "true");
    fix_close(&fx);
}

/* The same throw has to be reported the same way through every entry
 * point -- spec section 9's "batch and REPL paths are the same path". */
static void every_entry_point_reports_the_same_escape(void) {
    UwFix fx; fix_open(&fx);
    UValue fn = uv_nil();
    RT_EQ(run(&fx, "var boom = function() { throw 42 }; boom |", &fn), URBI_OK);

    UValue res = uv_nil();
    RT_EQ(urbi_call(fx.vm, fx.realm, fn, uv_nil(), NULL, 0, &res), URBI_ERR_UNCAUGHT_THROW);
    UErrorInfo info;
    urbi_last_error(fx.vm, &info);
    RT_EQ(info.code, URBI_ERR_UNCAUGHT_THROW);
    RT_STREQ(info.message, "42");   /* the REPL renders this as "!!! 42" */

    /* urbi_load walks the same mapping. */
    uint8_t *bytes = NULL; size_t nbytes = 0;
    char err[128] = {0};
    RT_EQ(urbi_compile(fx.vm, "throw 42", 8, NULL, &bytes, &nbytes, err, sizeof err), URBI_OK);
    RT_EQ(urbi_load(fx.vm, fx.realm, bytes, nbytes, &res), URBI_ERR_UNCAUGHT_THROW);
    urbi_last_error(fx.vm, &info);
    RT_STREQ(info.message, "42");
    urbi_chunk_free(fx.vm, bytes, nbytes);
    fix_close(&fx);
}

/* A caught throw leaves no trace on the error channel: uexec_throw records
 * eagerly, so the successful return has to clear it. */
static void the_error_channel_clears_on_success(void) {
    UwFix fx; fix_open(&fx);
    UValue out = uv_nil();
    run_int(&fx, "var b = 0; try { 1 + \"z\" } catch (var e) { b = 1 }; b |", 1);
    UErrorInfo info;
    urbi_last_error(fx.vm, &info);
    RT_EQ(info.code, URBI_OK);
    RT_CHECK(info.message[0] == '\0');

    RT_EQ(run(&fx, "1 + 1 |", &out), URBI_OK);
    urbi_last_error(fx.vm, &info);
    RT_EQ(info.code, URBI_OK);
    RT_CHECK(info.message[0] == '\0');
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
    rt_run("nested_proto_reports_its_own_source_line", nested_proto_reports_its_own_source_line);
    rt_run("the_exception_carries_its_line", the_exception_carries_its_line);
    rt_run("an_escaping_scalar_throw_is_fatal", an_escaping_scalar_throw_is_fatal);
    rt_run("every_entry_point_reports_the_same_escape", every_entry_point_reports_the_same_escape);
    rt_run("the_error_channel_clears_on_success", the_error_channel_clears_on_success);
}
