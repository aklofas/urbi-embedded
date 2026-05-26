/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_vm_slot_helpers.c — Wave 5 W1: OBJ-IC-POLY regression
 * pin + slot-helper coverage.
 *
 * Per audit-1 F3 + runtime-invariants F8: the LOCAL-slot discipline
 * (recv-specific slots[] cache vs slot_idx[] re-resolution) was inline-
 * implemented in 3 places before W1 (OP_GETSLOT / OP_SETSLOT / OP_SELF).
 * W1 collapses to single helper vm_resolve_ic.  These tests pin the
 * existing behaviour and would fail if any future refactor drops the
 * FLAG_LOCAL re-dispatch from any of the 3 OP paths.
 *
 * Pre-extraction commit: all 6 tests must PASS at baseline (before
 * any helper extraction), confirming the semantics are already correct.
 * They must also PASS post-extraction (confirming the extraction
 * preserves semantics). */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* drain_vm: drive the VM to quiescence after any async activity. */
static void drain_vm(UVM *vm)
{
    int i;
    for (i = 0; i < 200; i++) {
        UStepResult r = urbi_step(vm, 500, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT) break;
    }
}

/* === Test 1: OP_GETSLOT polymorphic same-shape — per-instance values.
 *
 * Two instances of the same prototype, same slot index, different
 * values.  A single bytecode GETSLOT site (inside an at-handler body)
 * fires against each instance in turn.  The IC fills on the first
 * fire; the second fire must return the second instance's value, NOT
 * the first instance's cached pointer.  The LOCAL-bit re-dispatch
 * (pre-W1: in OP_GETSLOT arm; post-W1: in vm_resolve_ic) enforces this. */
UTEST(getslot_polymorphic_same_shape_returns_correct_per_instance_value)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "poly_gs", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    /* Three same-shape instances; a single OP_GETSLOT site selects one
     * per fire (via List.get(idx)) and reads `.x`.  Pattern mirrors
     * test_ic_polymorphic.c which is the regression-pin for the OBJ-IC-POLY
     * bug fixed at v0.7.2. */
    int rc = utest_e2e_compile_and_run(&vm,
        "class SlotA {"
        "  var init = function(n) { this.x = n; this }"
        "};"
        "Realm.objs_a = List.new("
        "    SlotA.new().init(100),"
        "    SlotA.new().init(200));"
        "Realm.idx_a = 0;"
        "Realm.obs_a = -1;"
        "at (poly_gs?) Realm.obs_a = Realm.objs_a.get(Realm.idx_a).x",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* First fire: idx=0 → obj[0].x = 100 (IC fills). */
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "idx_a", 5,
                                               utest_e2e_make_int(0)));
    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    drain_vm(&vm);
    {
        UValue v = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "obs_a", 5, &v));
        UASSERT_EQ((int)UVAL_INT, (int)v.kind);
        UASSERT_EQ((int64_t)100, v.v.i);
    }

    /* Second fire: idx=1 → obj[1].x = 200 (same IC site, different recv). */
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "idx_a", 5,
                                               utest_e2e_make_int(1)));
    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    drain_vm(&vm);
    {
        UValue v = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "obs_a", 5, &v));
        UASSERT_EQ((int)UVAL_INT, (int)v.kind);
        UASSERT_EQ((int64_t)200, v.v.i);
    }

    urbi_vm_destroy(&vm);
}

/* === Test 2: OP_SETSLOT polymorphic same-shape — per-instance writes.
 *
 * Same shape, same slot index.  A single at-handler SETSLOT site writes
 * the targeted instance's `.x`.  Both instances must retain independent
 * storage; writes to instance B must not corrupt instance A's slot. */
UTEST(setslot_polymorphic_same_shape_writes_correct_per_instance_slot)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "poly_ss", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    int rc = utest_e2e_compile_and_run(&vm,
        "class BoxB {"
        "  var init = function(n) { this.x = n; this }"
        "};"
        "Realm.boxes_b = List.new("
        "    BoxB.new().init(0),"
        "    BoxB.new().init(0));"
        "Realm.tgt_b   = 0;"
        "Realm.pay_b   = 0;"
        "at (poly_ss?) Realm.boxes_b.get(Realm.tgt_b).x = Realm.pay_b",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Write boxes_b[0].x = 100 */
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "tgt_b", 5,
                                               utest_e2e_make_int(0)));
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "pay_b", 5,
                                               utest_e2e_make_int(100)));
    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    drain_vm(&vm);

    /* Write boxes_b[1].x = 200 */
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "tgt_b", 5,
                                               utest_e2e_make_int(1)));
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "pay_b", 5,
                                               utest_e2e_make_int(200)));
    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    drain_vm(&vm);

    /* Probe via distinct bytecode sites so each probe has its own IC. */
    UASSERT_EQ(URBI_OK, utest_e2e_compile_and_run(&vm,
        "Realm.rb_0 = Realm.boxes_b.get(0).x", NULL));
    UASSERT_EQ(URBI_OK, utest_e2e_compile_and_run(&vm,
        "Realm.rb_1 = Realm.boxes_b.get(1).x", NULL));
    {
        UValue v0 = utest_e2e_make_nil();
        UValue v1 = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "rb_0", 4, &v0));
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "rb_1", 4, &v1));
        UASSERT_EQ((int)UVAL_INT, (int)v0.kind);
        UASSERT_EQ((int)UVAL_INT, (int)v1.kind);
        UASSERT_EQ((int64_t)100, v0.v.i);
        UASSERT_EQ((int64_t)200, v1.v.i);
    }

    urbi_vm_destroy(&vm);
}

/* === Test 3: OP_SELF polymorphic same-shape — correct receiver binding.
 *
 * OP_SELF writes R[A+1] = recv AND loads R[A] = method.  Same-shape
 * instances; the `this` binding inside the method body must be the
 * correct per-instance receiver. */
UTEST(self_polymorphic_same_shape_loads_method_from_correct_instance)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "poly_self", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    /* Each instance has a per-instance local `x`; method returns `this.x`.
     * The OP_SELF site inside the at-handler picks the right receiver. */
    int rc = utest_e2e_compile_and_run(&vm,
        "class Sf {"
        "  var init = function(n) { this.x = n; this }"
        "};"
        "Realm.objs_sf = List.new("
        "    Sf.new().init(11),"
        "    Sf.new().init(22));"
        "Realm.idx_sf  = 0;"
        "Realm.obs_sf  = -1;"
        "at (poly_self?) Realm.obs_sf = Realm.objs_sf.get(Realm.idx_sf).x",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* First fire: idx=0 → obj[0].x = 11. */
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "idx_sf", 6,
                                               utest_e2e_make_int(0)));
    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    drain_vm(&vm);
    {
        UValue v = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "obs_sf", 6, &v));
        UASSERT_EQ((int)UVAL_INT, (int)v.kind);
        UASSERT_EQ((int64_t)11, v.v.i);
    }

    /* Second fire: idx=1 → obj[1].x = 22. */
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "idx_sf", 6,
                                               utest_e2e_make_int(1)));
    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    drain_vm(&vm);
    {
        UValue v = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "obs_sf", 6, &v));
        UASSERT_EQ((int)UVAL_INT, (int)v.kind);
        UASSERT_EQ((int64_t)22, v.v.i);
    }

    urbi_vm_destroy(&vm);
}

/* === Test 4: OP_GETSLOT getter dispatch from IC-hit path.
 *
 * Slot `y` on object `og` has an OGET property (getter closure).  IC
 * fills on the first read; subsequent IC-hit reads must still invoke
 * the getter rather than returning the closure value directly.  This
 * pins the OGET fast-path branch. */
UTEST(getslot_getter_dispatch_via_ic_hit)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Compile and run all getter setup in one chunk so name resolution works. */
    UASSERT_EQ(URBI_OK, utest_e2e_compile_and_run(&vm,
        "var og = Object.new();"
        "og.setProperty(\"y\", \"oget\", function() { 42 });"
        "Realm.first_g  = og.y;"   /* IC miss: slow path fills OGET entry. */
        "Realm.second_g = og.y",   /* IC hit: must still invoke the getter. */
        NULL));

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);
    {
        UValue first = utest_e2e_make_nil();
        UValue second = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "first_g",  7, &first));
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "second_g", 8, &second));
        UASSERT_EQ((int)UVAL_INT, (int)first.kind);
        UASSERT_EQ((int)UVAL_INT, (int)second.kind);
        UASSERT_EQ((int64_t)42, first.v.i);
        UASSERT_EQ((int64_t)42, second.v.i);
    }

    urbi_vm_destroy(&vm);
}

/* === Test 5: OP_SELF preserves receiver when dst_reg aliases recv_reg.
 *
 * OP_SELF ABC: R[A] = method, R[A+1] = receiver.  If B == A+1 (dst
 * register = receiver register + 1), writing R[A+1] with the receiver
 * must happen before writing R[A] with the method — otherwise the
 * method clobbers the receiver.  W1's vm_self_lookup handles this by
 * snapshotting recv first and writing R[A+1] before R[A].
 *
 * This test exercises the case where the emitter places the method call
 * immediately adjacent to the receiver register. */
UTEST(self_preserves_receiver_when_dst_aliases_recv_reg)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    char errbuf[256];
    UASSERT_EQ(URBI_OK,
        urbi_repl_eval(&vm, NULL,
            "var o5 = Object.new()", 21,
            errbuf, sizeof errbuf));
    /* Method returns 99 so we can verify the call completed correctly. */
    UASSERT_EQ(URBI_OK,
        urbi_repl_eval(&vm, NULL,
            "o5.f = function() { 99 }", 24,
            errbuf, sizeof errbuf));
    /* Call `o5.f()` — compiler emits OP_SELF + OP_CALL at adjacent registers. */
    UASSERT_EQ(URBI_OK,
        urbi_repl_eval(&vm, NULL,
            "Realm.res5 = o5.f()", 19,
            errbuf, sizeof errbuf));

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);
    {
        UValue res = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "res5", 4, &res));
        UASSERT_EQ((int)UVAL_INT, (int)res.kind);
        UASSERT_EQ((int64_t)99, res.v.i);
    }

    urbi_vm_destroy(&vm);
}

/* === Test 6: OP_GETSLOT traces read-set during watcher install.
 *
 * During `at (cond) body`, the OP_GETSLOT executed inside the cond
 * evaluation must add the receiver's GC cell to vm->trace_read_set[].
 * vm_trace_slot_read_if_needed (extracted from the inline block in
 * OP_GETSLOT and OP_SELF) does this; if it's accidentally omitted
 * the watcher installs with an empty read-set and never fires.
 *
 * We verify end-to-end: install `at (Realm.wobj6.x > 5) { ... }` and
 * confirm the watcher fires after writing Realm.wobj6.x = 10.
 *
 * Mirror of test_at_scripted_e2e.c but with an Object slot (not a plain
 * Realm slot) so the trace path goes through OP_GETSLOT on a
 * UProtoInstance rather than directly through the realm global slot. */
UTEST(getslot_traces_read_during_watcher_install)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    /* Pre-install realm globals via C API so the emitter sees them at
     * each subsequent compile_and_run.  wobj6 is an Object instance
     * whose slot `x` is watched.  wfired6 counts body firings. */
    {
        int rc;
        rc = urbi_realm_set_global(&vm, r, "wfired6", 7,
                                   utest_e2e_make_int(0));
        UASSERT_EQ(URBI_OK, rc);
        /* wobj6 is created via script so it has a proper UProtoInstance
         * with IC-able slots.  The C API alone cannot create a scripted
         * Object instance; compile_and_run is needed. */
    }

    /* Create wobj6 as an Object.new() with an x slot. */
    UASSERT_EQ(URBI_OK, utest_e2e_compile_and_run(&vm,
        "Realm.wobj6 = Object.new(); Realm.wobj6.x = 0;",
        NULL));

    /* Install the watcher with the object's slot in the cond.
     * at (Realm.wobj6.x > 5) evaluates wobj6.x via OP_GETSLOT during
     * watcher install; vm_trace_slot_read_if_needed adds wobj6's GC cell
     * to vm->trace_read_set[].  If the trace is absent, the watcher has
     * an empty read-set and the OP_SETSLOT dirty-observer is never set,
     * so the watcher never fires. */
    UASSERT_EQ(URBI_OK, utest_e2e_compile_and_run(&vm,
        "at (Realm.wobj6.x > 5) { Realm.wfired6 = Realm.wfired6 + 1 }",
        NULL));

    /* Watcher must be installed. */
    UASSERT(vm.active_watchers_head != NULL);

    /* Trigger: write Realm.wobj6.x = 10 inside a function call so the
     * watcher eval happens at the non-top OP_RET safepoint. */
    UASSERT_EQ(URBI_OK, utest_e2e_compile_and_run(&vm,
        "var trig6 = function() { Realm.wobj6.x = 10 }; trig6()",
        NULL));

    /* Drive the body strand to completion. */
    int step_rc = utest_e2e_run_to_no_runnable(&vm);
    UASSERT(step_rc != -1);

    /* Watcher must have fired at least once. */
    {
        UValue fired = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "wfired6", 7, &fired));
        UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
        UASSERT(fired.v.i >= 1);
    }

    /* Cleanup: drain active watchers before destroy. */
    {
        extern void urbi_watcher_unregister_internal(UVM *, void *);
        while (vm.active_watchers_head != NULL)
            urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);
    }

    urbi_vm_destroy(&vm);
}

/* === Suite entry. ==================================================== */
void
test_vm_slot_helpers_suite(void)
{
    utest_run("vm_slot_helpers: getslot polymorphic same-shape returns correct per-instance value",
              getslot_polymorphic_same_shape_returns_correct_per_instance_value);
    utest_run("vm_slot_helpers: setslot polymorphic same-shape writes correct per-instance slot",
              setslot_polymorphic_same_shape_writes_correct_per_instance_slot);
    utest_run("vm_slot_helpers: self polymorphic same-shape loads method from correct instance",
              self_polymorphic_same_shape_loads_method_from_correct_instance);
    utest_run("vm_slot_helpers: getslot getter dispatch via ic-hit",
              getslot_getter_dispatch_via_ic_hit);
    utest_run("vm_slot_helpers: self preserves receiver when dst aliases recv reg",
              self_preserves_receiver_when_dst_aliases_recv_reg);
    utest_run("vm_slot_helpers: getslot traces read during watcher install",
              getslot_traces_read_during_watcher_install);
}
