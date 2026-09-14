#include "rtest.h"
#include <stdlib.h>
#include <string.h>

#include "urbi/urbi.h"

/* Exercises the public C API exactly as an embedder sees it: only
 * <urbi/urbi.h> is included here, no runtime-internal headers. */

static long api_live;

static void *api_alloc(void *p, size_t n, void *ud) {
    (void)ud;
    if (n == 0) { if (p) api_live--; free(p); return NULL; }
    void *q = realloc(p, n);
    if (q && !p) api_live++;
    return q;
}

static UVM *api_open(void) {
    api_live = 0;
    return urbi_open(api_alloc, NULL, NULL);
}

static int run_ok(UVM *vm, const char *src, UValue *out) {
    char err[256] = {0};
    int rc = urbi_run(vm, urbi_realm_main(vm), src, strlen(src), NULL, out, err, sizeof err);
    if (rc == URBI_ERR_COMPILE) printf("    compile error: %s\n", err);
    return rc;
}

static void fmt_is(UVM *vm, UValue v, const char *want) {
    char buf[256];
    size_t n = urbi_value_to_string(vm, v, buf, sizeof buf);
    RT_EQ(n, strlen(want));
    RT_STREQ(buf, want);
}

static void lifecycle_and_realms(void) {
    UVM *vm = api_open();
    RT_CHECK(vm != NULL);
    RT_CHECK(urbi_realm_main(vm) != NULL);
    RT_CHECK(!urbi_has_live_work(vm));

    /* A second realm has its own globals. */
    URealm *r2 = urbi_realm_new(vm);
    RT_CHECK(r2 != NULL && r2 != urbi_realm_main(vm));
    RT_EQ(urbi_global_set(vm, urbi_realm_main(vm), "shared", urbi_make_int(1)), URBI_OK);
    UValue v;
    RT_EQ(urbi_global_get(vm, urbi_realm_main(vm), "shared", &v), URBI_OK);
    RT_EQ(v.v.i, 1);
    RT_CHECK(urbi_global_get(vm, r2, "shared", &v) != URBI_OK);

    urbi_realm_free(vm, r2);
    urbi_realm_free(vm, urbi_realm_main(vm));          /* freeing main is a no-op */
    RT_CHECK(urbi_realm_main(vm) != NULL);

    /* Host hooks are settable and clearable at any time. */
    urbi_set_clock(vm, NULL, NULL);
    urbi_set_diag(vm, NULL, NULL);
    urbi_set_writer(vm, NULL, NULL);
    urbi_set_wake(vm, NULL, NULL);

    urbi_close(vm);
    RT_EQ(api_live, 0L);
}

static void run_and_format(void) {
    UVM *vm = api_open();
    UValue v;
    RT_EQ(run_ok(vm, "6 * 7 |", &v), URBI_OK);
    fmt_is(vm, v, "42");
    RT_EQ(run_ok(vm, "4 / 2 |", &v), URBI_OK);
    fmt_is(vm, v, "2.0");                 /* the Lua trailing-.0 rule */
    RT_EQ(run_ok(vm, "5 / 2 |", &v), URBI_OK);
    fmt_is(vm, v, "2.5");
    RT_EQ(run_ok(vm, "nil |", &v), URBI_OK);
    fmt_is(vm, v, "nil");
    RT_EQ(run_ok(vm, "1 < 2 |", &v), URBI_OK);
    fmt_is(vm, v, "true");
    RT_EQ(run_ok(vm, "\"a\\nb\" |", &v), URBI_OK);
    fmt_is(vm, v, "\"a\\nb\"");
    RT_EQ(run_ok(vm, "function() { 1 } |", &v), URBI_OK);
    fmt_is(vm, v, "<?>");                 /* closures and void share this spelling */

    /* A compile error reports a positioned diagnostic and leaves the VM usable. */
    char err[256] = {0};
    RT_EQ(urbi_run(vm, NULL, "1 +", 3, NULL, &v, err, sizeof err), URBI_ERR_COMPILE);
    RT_CHECK(strstr(err, "<stdin>:1:") != NULL);
    RT_EQ(run_ok(vm, "7 |", &v), URBI_OK);
    RT_EQ(v.v.i, 7);

    urbi_close(vm);
    RT_EQ(api_live, 0L);
}

static void compile_then_load(void) {
    UVM *vm = api_open();
    uint8_t *bytes = NULL;
    size_t n = 0;
    char err[256] = {0};
    RT_EQ(urbi_compile(vm, "20 + 22 |", 9, "<unit>", &bytes, &n, err, sizeof err), URBI_OK);
    RT_CHECK(bytes != NULL && n > 0);

    UValue v;
    RT_EQ(urbi_load(vm, urbi_realm_main(vm), bytes, n, &v), URBI_OK);
    RT_EQ(v.kind, (uint8_t)URBI_VALUE_INT);
    RT_EQ(v.v.i, 42);
    urbi_chunk_free(vm, bytes, n);

    /* Garbage is rejected, not executed. */
    const uint8_t junk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    RT_CHECK(urbi_load(vm, NULL, junk, sizeof junk, &v) != URBI_OK);

    /* A compile error produces no buffer to free. */
    bytes = NULL;
    RT_EQ(urbi_compile(vm, "1 +", 3, "<unit>", &bytes, &n, err, sizeof err), URBI_ERR_COMPILE);
    RT_CHECK(bytes == NULL);

    urbi_close(vm);
    RT_EQ(api_live, 0L);
}

static int host_double(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out) {
    (void)vm; (void)self;
    if (nargs != 1 || !urbi_value_is_int(args[0])) return urbi_throw(vm, "TypeError", "double expects an Integer");
    *out = urbi_make_int(urbi_value_as_int(args[0]) * 2);
    return UEXEC_OK;
}

static void host_functions(void) {
    UVM *vm = api_open();
    RT_EQ(urbi_register(vm, "twice", host_double, 1, 1), URBI_OK);

    UValue v;
    RT_EQ(run_ok(vm, "twice(21) |", &v), URBI_OK);
    RT_EQ(v.v.i, 42);

    /* The native's own throw reaches the caller as an uncaught throw. */
    RT_EQ(run_ok(vm, "twice(nil) |", &v), URBI_ERR_UNCAUGHT_THROW);
    UErrorInfo info;
    RT_EQ(urbi_last_error(vm, &info), URBI_ERR_UNCAUGHT_THROW);
    RT_CHECK(strstr(info.message, "TypeError") != NULL);
    urbi_clear_error(vm);
    RT_EQ(urbi_last_error(vm, NULL), URBI_OK);

    /* Wrong arity is an ArityError, not a crash. */
    RT_EQ(run_ok(vm, "twice(1, 2) |", &v), URBI_ERR_UNCAUGHT_THROW);

    /* A path whose leading component does not resolve is refused. */
    RT_CHECK(urbi_register(vm, "NoSuch.thing", host_double, 1, 1) != URBI_OK);
    RT_EQ(urbi_register(vm, NULL, host_double, 1, 1), URBI_ERR_INVALID_ARG);

    urbi_close(vm);
    RT_EQ(api_live, 0L);
}

static void globals_slots_and_call(void) {
    UVM *vm = api_open();
    UValue f;
    RT_EQ(run_ok(vm, "var f = function(x) { x + 1 } |", &f), URBI_OK);

    /* The closure is reachable as a global and callable from C. */
    UValue g = urbi_make_nil();
    RT_EQ(urbi_global_get(vm, NULL, "f", &g), URBI_OK);
    UValue arg = urbi_make_int(41), out;
    RT_EQ(urbi_call(vm, NULL, g, urbi_make_nil(), &arg, 1, &out), URBI_OK);
    RT_EQ(out.v.i, 42);
    /* Calling a non-closure is an argument error, not a throw. */
    RT_EQ(urbi_call(vm, NULL, urbi_make_int(1), urbi_make_nil(), NULL, 0, &out), URBI_ERR_INVALID_ARG);

    /* Slot access on a non-object is refused rather than dereferenced.
     * The object path needs a script-visible object, which arrives with
     * the standard library; the .chk corpus covers it from there. */
    UValue got;
    RT_CHECK(urbi_slot_get(vm, urbi_make_int(1), "x", &got) != URBI_OK);
    RT_CHECK(urbi_slot_set(vm, urbi_make_int(1), "x", urbi_make_int(2)) != URBI_OK);
    RT_CHECK(urbi_slot_get(vm, urbi_make_nil(), NULL, &got) != URBI_OK);

    /* A string built from C survives a collection while referenced. */
    UValue s = urbi_make_string(vm, "held", 4);
    urbi_ref(vm, s);
    urbi_gc_collect(vm);
    fmt_is(vm, s, "\"held\"");
    urbi_unref(vm, s);

    UGcStats st;
    RT_EQ(urbi_gc_stats(vm, &st), URBI_OK);
    RT_CHECK(st.cycles > 0);
    RT_EQ(urbi_gc_stats(vm, NULL), URBI_ERR_INVALID_ARG);

    urbi_close(vm);
    RT_EQ(api_live, 0L);
}

static void pending_subsystems_report_invalid_state(void) {
    UVM *vm = api_open();
    UValue v = urbi_make_nil();
    RT_EQ(urbi_step(vm, 100, NULL), URBI_ERR_INVALID_STATE);
    RT_EQ(urbi_event_new(vm, NULL, "e", &v), URBI_ERR_INVALID_STATE);
    RT_EQ(urbi_event_emit(vm, v, v), URBI_ERR_INVALID_STATE);
    RT_EQ(urbi_inject_event(vm, 0, NULL, 0), URBI_ERR_INVALID_STATE);
    RT_EQ(urbi_watch(vm, NULL, "x", NULL, NULL), URBI_ERR_INVALID_STATE);
    RT_EQ(urbi_tag_new(vm, NULL, "t", &v), URBI_ERR_INVALID_STATE);
    RT_EQ(urbi_tag_stop(vm, v), URBI_ERR_INVALID_STATE);
    RT_EQ(urbi_tag_block(vm, v), URBI_ERR_INVALID_STATE);
    RT_EQ(urbi_tag_unblock(vm, v), URBI_ERR_INVALID_STATE);
    RT_EQ(urbi_tag_freeze(vm, v), URBI_ERR_INVALID_STATE);
    RT_EQ(urbi_tag_unfreeze(vm, v), URBI_ERR_INVALID_STATE);
    /* urbi_throw outside a running strand has nowhere to deposit. */
    RT_EQ(urbi_throw(vm, "TypeError", "nope"), URBI_ERR_INVALID_STATE);
    urbi_close(vm);
    RT_EQ(api_live, 0L);
}

static void version_and_null_arguments(void) {
    RT_CHECK(urbi_version() != NULL && urbi_version()[0] != '\0');
    int major = -1, minor = -1, patch = -1;
    urbi_api_version(&major, &minor, &patch);
    RT_CHECK(major >= 0 && minor >= 0 && patch >= 0);
    urbi_api_version(NULL, NULL, NULL);

    /* Every entry point tolerates a NULL VM rather than dereferencing it. */
    RT_CHECK(urbi_open(NULL, NULL, NULL) == NULL);
    urbi_close(NULL);
    RT_CHECK(urbi_realm_main(NULL) == NULL);
    RT_CHECK(urbi_realm_new(NULL) == NULL);
    urbi_realm_free(NULL, NULL);
    RT_CHECK(!urbi_has_live_work(NULL));
    RT_EQ(urbi_last_error(NULL, NULL), URBI_ERR_INVALID_ARG);
    urbi_clear_error(NULL);
    urbi_gc_collect(NULL);
    RT_EQ(urbi_gc_stats(NULL, NULL), URBI_ERR_INVALID_ARG);
    urbi_set_clock(NULL, NULL, NULL);
    urbi_set_diag(NULL, NULL, NULL);
    urbi_set_writer(NULL, NULL, NULL);
    urbi_set_wake(NULL, NULL, NULL);
    RT_EQ(urbi_run(NULL, NULL, "1", 1, NULL, NULL, NULL, 0), URBI_ERR_INVALID_ARG);
    RT_EQ(urbi_compile(NULL, "1", 1, NULL, NULL, NULL, NULL, 0), URBI_ERR_INVALID_ARG);
    RT_EQ(urbi_load(NULL, NULL, NULL, 0, NULL), URBI_ERR_INVALID_ARG);
    RT_EQ(urbi_call(NULL, NULL, urbi_make_nil(), urbi_make_nil(), NULL, 0, NULL), URBI_ERR_INVALID_ARG);
    RT_EQ(urbi_global_get(NULL, NULL, NULL, NULL), URBI_ERR_INVALID_ARG);
    RT_EQ(urbi_global_set(NULL, NULL, NULL, urbi_make_nil()), URBI_ERR_INVALID_ARG);
    RT_EQ(urbi_slot_get(NULL, urbi_make_nil(), NULL, NULL), URBI_ERR_INVALID_ARG);
    RT_EQ(urbi_slot_set(NULL, urbi_make_nil(), NULL, urbi_make_nil()), URBI_ERR_INVALID_ARG);
    RT_EQ(urbi_register(NULL, NULL, NULL, 0, 0), URBI_ERR_INVALID_ARG);
    RT_EQ(urbi_throw(NULL, NULL, NULL), URBI_ERR_INVALID_ARG);
    urbi_chunk_free(NULL, NULL, 0);
    RT_CHECK(urbi_value_is_nil(urbi_make_string(NULL, NULL, 0)));

    /* A zero-capacity buffer writes nothing rather than overrunning. */
    RT_EQ(urbi_value_to_string(NULL, urbi_make_int(1), NULL, 0), 0u);
}

RT_SUITE(rt_api_suite) {
    rt_run("lifecycle_and_realms", lifecycle_and_realms);
    rt_run("run_and_format", run_and_format);
    rt_run("compile_then_load", compile_then_load);
    rt_run("host_functions", host_functions);
    rt_run("globals_slots_and_call", globals_slots_and_call);
    rt_run("pending_subsystems_report_invalid_state", pending_subsystems_report_invalid_state);
    rt_run("version_and_null_arguments", version_and_null_arguments);
}
