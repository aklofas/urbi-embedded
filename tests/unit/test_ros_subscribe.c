/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ros_subscribe.c — ros.subscribe creates a first-class Event, records it
 * in the bridge subs[] table, GC-roots it, and returns it. */
#ifdef URBI_ENABLE_ROS2

#include "utest.h"
#include "urbi/urbi.h"
#include "ros/uros_internal.h"
#include "ros/uros_mock.h"
#include "vm/uvm.h"
#include "object/uobject.h"     /* urbi_object_lookup */
#include "value/uintern.h"      /* ustr_intern */
#include "runtime/uclosure.h"   /* UClosure->native_fn direct dispatch */
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

static void *
ros_sub_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

static void
drain_vm_sub(struct UVM *vm)
{
    int i;
    for (i = 0; i < 200; i++) {
        UStepResult r = urbi_step(vm, 500, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT) break;
    }
}

/* Helper: reset the singleton bridge so each test starts clean. */
static void
reset_bridge(void)
{
    URosBridge *b = urbi_ros_bridge();
    if (b->inited) {
        b->tp.fini(b->tp.self);
        uros_mock_free(&b->tp);
        memset(b, 0, sizeof(*b));
    }
}

/* ros.subscribe records the subscription in the bridge and sets event != NULL. */
UTEST(ros_subscribe_records_in_bridge)
{
    reset_bridge();

    struct UVM *vm = urbi_vm_create(ros_sub_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);
    if (realm == NULL) { urbi_vm_free(vm); return; }

    char errbuf[512];
#define EVAL(src) \
    UASSERT_EQ(urbi_repl_eval(vm, realm, (src), strlen(src), \
                              errbuf, sizeof(errbuf)), URBI_OK); \
    drain_vm_sub(vm)

    EVAL("ros.init(\"n\")");
    EVAL("ros.subscribe(\"/scan\", \"std_msgs/Int32\")");

#undef EVAL

    URosBridge *b = urbi_ros_bridge();
    UASSERT_EQ(b->sub_count, 1);
    UASSERT_NE((long long)(b->subs[0].event), 0LL);

    urbi_vm_free(vm);
}

/* A second subscribe increments sub_count to 2 with distinct events. */
UTEST(ros_subscribe_two_distinct_entries)
{
    reset_bridge();

    struct UVM *vm = urbi_vm_create(ros_sub_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);
    if (realm == NULL) { urbi_vm_free(vm); return; }

    char errbuf[512];
#define EVAL(src) \
    UASSERT_EQ(urbi_repl_eval(vm, realm, (src), strlen(src), \
                              errbuf, sizeof(errbuf)), URBI_OK); \
    drain_vm_sub(vm)

    EVAL("ros.init(\"n\")");
    EVAL("ros.subscribe(\"/scan\", \"std_msgs/Int32\")");
    EVAL("ros.subscribe(\"/cmd\", \"std_msgs/Int32\")");

#undef EVAL

    URosBridge *b = urbi_ros_bridge();
    UASSERT_EQ(b->sub_count, 2);
    UASSERT_NE((long long)(b->subs[0].event), 0LL);
    UASSERT_NE((long long)(b->subs[1].event), 0LL);
    /* Events must be distinct objects. */
    UASSERT_NE((long long)(b->subs[0].event), (long long)(b->subs[1].event));

    urbi_vm_free(vm);
}

/* ===================================================================
 * GC-10/ROS-08: a post-commit OOM inside ros.subscribe (the "__sub_<h>"
 * intern or the set_local_slot that GC-roots the event) must roll back
 * the subs[] commit, mirroring the ros.service rollback.  Pre-fix the
 * two post-commit OOM exits leaked a committed subs[] entry pointing at
 * a destroyed transport endpoint + un-GC-rooted event.
 * =================================================================== */

typedef struct {
    int alloc_calls;
    int fail_at;  /* -1 means never fail; trigger NULL when alloc_calls > fail_at */
} SubAllocSpy;

static void *
sub_spy_alloc(void *ptr, size_t n, void *ud)
{
    SubAllocSpy *spy = (SubAllocSpy *)ud;
    if (n == 0) {
        free(ptr);
        return NULL;
    }
    if (ptr == NULL) {
        spy->alloc_calls++;
        if (spy->fail_at >= 0 && spy->alloc_calls > spy->fail_at)
            return NULL;
    }
    return realloc(ptr, n);
}

/* Build a fresh VM + realm on the spy allocator and run ros.init.  Both
 * phases of the OOM test call this with identical inputs so the per-call
 * allocation trace of the subsequent subscribe is identical. */
static struct UVM *
sub_oom_setup(SubAllocSpy *spy)
{
    reset_bridge();
    struct UVM *vm = urbi_vm_create(sub_spy_alloc, spy);
    if (vm == NULL) return NULL;
    struct URealm *realm = urbi_realm_create(vm);
    if (realm == NULL) { urbi_vm_free(vm); return NULL; }
    char errbuf[256];
    const char *src = "ros.init(\"n\")";
    if (urbi_repl_eval(vm, realm, src, strlen(src),
                       errbuf, sizeof errbuf) != URBI_OK) {
        urbi_vm_free(vm);
        return NULL;
    }
    drain_vm_sub(vm);
    return vm;
}

/* Fetch the registered ros.subscribe native closure off vm->ros_proto so the
 * test can invoke it directly (no per-call compile noise in the alloc trace;
 * same direct-dispatch pattern as test_event_runtime.c). */
static UClosure *
sub_lookup_subscribe(struct UVM *vm)
{
    if (vm->ros_proto == NULL) return NULL;
    USymbol *sym = (USymbol *)ustr_intern(vm, "subscribe", 9);
    if (sym == NULL) return NULL;
    UValue slot = urbi_make_nil();
    if (urbi_object_lookup(vm, (UObject *)vm->ros_proto, sym, &slot) != 0)
        return NULL;
    if (slot.kind != (uint8_t)UVAL_CLOSURE) return NULL;
    return (UClosure *)slot.v.p;
}

UTEST(ros_subscribe_oom_rolls_back_commit)
{
    /* Phase A: count the fresh allocations of one clean subscribe call on a
     * fresh VM.  The LAST fresh allocation of the call is always post-commit:
     * the "__sub_<h>" symbol is novel on a fresh VM, so its intern (or a
     * later set_local_slot allocation) is the final fresh alloc. */
    SubAllocSpy probe = { 0, -1 };
    struct UVM *vm = sub_oom_setup(&probe);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;

    UClosure *cl = sub_lookup_subscribe(vm);
    UASSERT_NE((long long)cl, 0LL);
    if (cl == NULL || cl->native_fn == NULL) { urbi_vm_free(vm); return; }

    UValue self = urbi_make_nil();
    UValue args[2];
    args[0] = urbi_make_str_interned(vm, "/scan", 5);
    args[1] = urbi_make_str_interned(vm, "std_msgs/Int32", 14);
    UValue out = urbi_make_nil();

    int before = probe.alloc_calls;
    int rc = cl->native_fn(vm, self, args, 2, &out);
    UASSERT_EQ(rc, UEXEC_OK);
    int delta = probe.alloc_calls - before;
    UASSERT(delta >= 2);  /* event create + "__sub_0" intern at minimum */
    URosBridge *b = urbi_ros_bridge();
    UASSERT_EQ(b->sub_count, 1);
    urbi_vm_free(vm);

    /* Phase B: identical setup; arm the spy so the last fresh allocation of
     * the clean trace fails — an OOM after the subs[] commit. */
    SubAllocSpy spy = { 0, -1 };
    vm = sub_oom_setup(&spy);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;

    cl = sub_lookup_subscribe(vm);
    UASSERT_NE((long long)cl, 0LL);
    if (cl == NULL || cl->native_fn == NULL) { urbi_vm_free(vm); return; }

    args[0] = urbi_make_str_interned(vm, "/scan", 5);
    args[1] = urbi_make_str_interned(vm, "std_msgs/Int32", 14);
    out = urbi_make_nil();
    b = urbi_ros_bridge();
    UASSERT_EQ(b->sub_count, 0);

    spy.fail_at = spy.alloc_calls + delta - 1;  /* allocs 1..fail_at succeed */
    rc = cl->native_fn(vm, self, args, 2, &out);
    UASSERT_EQ(rc, UEXEC_THROW);  /* (a) the call raised */
    UASSERT_EQ(b->sub_count, 0);  /* (b) commit rolled back (pre-fix: leaks 1) */

    /* (c) recovery: disarm the spy; the next subscribe must land at the index
     * the failed call would have used (sub_count must go 0 -> 1, not 1 -> 2). */
    spy.fail_at = -1;
    out = urbi_make_nil();
    rc = cl->native_fn(vm, self, args, 2, &out);
    UASSERT_EQ(rc, UEXEC_OK);
    UASSERT_EQ(b->sub_count, 1);
    UASSERT_NE((long long)b->subs[0].event, 0LL);

    urbi_vm_free(vm);  /* (d) no crash / UAF on teardown */
    reset_bridge();
}

void
test_ros_subscribe_suite(void)
{
    utest_run("ros_subscribe.records_in_bridge",
              ros_subscribe_records_in_bridge);
    utest_run("ros_subscribe.two_distinct_entries",
              ros_subscribe_two_distinct_entries);
    utest_run("ros_subscribe.oom_rolls_back_commit",
              ros_subscribe_oom_rolls_back_commit);
}

#else  /* !URBI_ENABLE_ROS2 */

void test_ros_subscribe_suite(void) { /* skipped: URBI_ENABLE_ROS2=0 */ }

#endif /* URBI_ENABLE_ROS2 */
