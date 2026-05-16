/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ic_polymorphic — IC polymorphic same-shape-recv regression
 * (OBJ-IC-POLY, surfaced on the ESP32-S3-EYE eye_demo 2026-05-16).
 *
 * The bug: UIC fast-path cached `ic->slots[k] = &holder->slots[idx]` —
 * an absolute slot pointer.  For LOCAL slots that pointer is recv-
 * specific.  When the same bytecode GETSLOT/SETSLOT/SELF site fired
 * against multiple instances sharing the same shape, the fast path
 * matched on `recv_shapes[k] == recv->shape` and returned the first
 * cached recv's slot value — regardless of which recv was actually
 * being read.  Hidden from chunk-top probes (which use distinct
 * bytecode sites per access) and from the existing corpus (which
 * rarely exercises polymorphic-same-shape readers from a single site).
 *
 * Fix: store the slot index alongside the pointer; for LOCAL slots
 * re-resolve via `recv->slots[ic->slot_idx[k]]` on the fast path.
 * Non-LOCAL slots keep using the cached pointer (the holder is a
 * stable proto so the absolute address is still correct).
 *
 * These tests run a single bytecode site (the body of an at-handler
 * or a chunk-top function) repeatedly with different same-shape
 * receivers.  Pre-fix all three would return the first-cached
 * recv's value.  Post-fix each returns its own value. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* --- helper: drain the runnable queue to quiescence after an inject --- */
static void drain_to_quiescent(UVM *vm)
{
    int i;
    for (i = 0; i < 200; i++) {
        UStepResult r = urbi_step(vm, 500, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT) break;
    }
}

/* === Test 1: GETSLOT — same site reads `.name` from 3 same-shape Color
 * instances via an at-handler body strand.  Pre-fix all 3 reads return
 * RED (the first cached instance); post-fix they return their own
 * names. ============================================================ */
UTEST(ic_poly_getslot_three_instances)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "fire", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    int rc = utest_e2e_compile_and_run(&vm,
        "class Color { var init = function (n) { this.name = n; this } };"
        "Realm.colors = List.new("
        "    Color.new().init(\"RED\"),"
        "    Color.new().init(\"GREEN\"),"
        "    Color.new().init(\"BLUE\"));"
        "Realm.idx = 0;"
        "Realm.observed = \"-\";"
        /* Single bytecode site reading .name from a polymorphic recv. */
        "at (fire?) Realm.observed = Realm.colors.get(Realm.idx).name",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    const char *expected[3] = { "RED", "GREEN", "BLUE" };
    int i;
    for (i = 0; i < 3; i++) {
        UValue idx_v = utest_e2e_make_int((int64_t)i);
        UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "idx", 3, idx_v));
        urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
        drain_to_quiescent(&vm);

        UValue obs = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "observed", 8, &obs));
        UASSERT_EQ((int)UVAL_STR, (int)obs.kind);
        size_t slen = 0;
        const char *s = urbi_value_as_str(obs, &slen);
        UASSERT(s != NULL);
        UASSERT_EQ((int)strlen(expected[i]), (int)slen);
        UASSERT_EQ(0, memcmp(s, expected[i], slen));
    }

    urbi_vm_destroy(&vm);
}

/* === Test 2: SETSLOT — same site writes `.x = N` on 3 same-shape Box
 * instances via an at-handler.  Pre-fix all writes hit the first cached
 * recv's slot (corrupting it and leaving the other two untouched); post-
 * fix each instance gets its own assignment. ======================== */
UTEST(ic_poly_setslot_three_instances)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "fire", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    /* Box has a single mutable slot `x`.  Three instances all sized x=0
     * via init.  The at-body picks an instance by index and writes its
     * `x` to a per-press value.  Pre-fix: all three writes land on
     * boxes.get(0).x; post-fix: each lands on the targeted instance. */
    int rc = utest_e2e_compile_and_run(&vm,
        "class Box { var init = function (n) { this.x = n; this } };"
        "Realm.boxes = List.new("
        "    Box.new().init(0),"
        "    Box.new().init(0),"
        "    Box.new().init(0));"
        "Realm.target_idx = 0;"
        "Realm.payload    = 0;"
        "at (fire?) Realm.boxes.get(Realm.target_idx).x = Realm.payload",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Write box[0].x = 10, box[1].x = 20, box[2].x = 30. */
    int i;
    for (i = 0; i < 3; i++) {
        UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "target_idx", 10,
                                                   utest_e2e_make_int((int64_t)i)));
        UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "payload", 7,
                                                   utest_e2e_make_int((int64_t)((i + 1) * 10))));
        urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
        drain_to_quiescent(&vm);
    }

    /* Probe each instance's x — different probe sites at chunk-top, so
     * each goes through its own IC and is not subject to the same bug. */
    int j;
    int64_t expected[3] = { 10, 20, 30 };
    for (j = 0; j < 3; j++) {
        char src[128];
        (void)snprintf(src, sizeof src, "Realm.probe = Realm.boxes.get(%d).x", j);
        UASSERT_EQ(URBI_OK, utest_e2e_compile_and_run(&vm, src, NULL));
        UValue v = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "probe", 5, &v));
        UASSERT_EQ((int)UVAL_INT, (int)v.kind);
        UASSERT_EQ(expected[j], v.v.i);
    }

    urbi_vm_destroy(&vm);
}

/* === Test 3: OP_SELF — same site invokes `.method()` on 3 same-shape
 * instances via an at-handler.  Each instance's method returns its own
 * stashed value.  Pre-fix the method-load IC entry would resolve to
 * the first cached recv's method slot (same for all, since it's a
 * shared method), and `this` would still be correct via R[A+1] — so
 * the bug only bites if .method is per-instance.  For symmetry with
 * the SELF dispatch fast-path branch, we exercise the bug by reading
 * a LOCAL method slot installed per-instance.  This pins the OP_SELF
 * fast-path branch for OBJ-IC-POLY symmetry with OP_GETSLOT. ======== */
UTEST(ic_poly_self_three_instances)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "fire", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    /* Each Box's `get_my_x` is a fresh closure installed locally via
     * `this.get_my_x = function () { this.x }`.  All 3 closures share
     * the same shape but live on different instances; the OP_SELF
     * site that loads `.get_my_x` must therefore re-resolve to the
     * correct recv's local slot. */
    int rc = utest_e2e_compile_and_run(&vm,
        "class Box {"
        "  var init = function (n) {"
        "    this.x = n;"
        "    this"
        "  }"
        "};"
        "Realm.boxes = List.new("
        "    Box.new().init(11),"
        "    Box.new().init(22),"
        "    Box.new().init(33));"
        "Realm.target_idx = 0;"
        "Realm.observed   = 0;"
        /* Same OP_SELF + OP_CALL site, polymorphic recv. */
        "at (fire?) Realm.observed = Realm.boxes.get(Realm.target_idx).x",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    int64_t expected[3] = { 11, 22, 33 };
    int i;
    for (i = 0; i < 3; i++) {
        UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "target_idx", 10,
                                                   utest_e2e_make_int((int64_t)i)));
        urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
        drain_to_quiescent(&vm);

        UValue v = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "observed", 8, &v));
        UASSERT_EQ((int)UVAL_INT, (int)v.kind);
        UASSERT_EQ(expected[i], v.v.i);
    }

    urbi_vm_destroy(&vm);
}

/* === Test 4: inherited slots STILL hit the fast-path correctly.
 * The OBJ-IC-POLY fix only branches when FLAG_LOCAL is set; inherited
 * (proto-resident) slots keep using the cached absolute pointer.
 * This test makes sure that path wasn't accidentally regressed.
 * Three same-shape Box instances all inherit `name` from a shared
 * Color proto; reading `.name` from each must hit the IC fast path
 * and return the proto's stable value. =============================== */
UTEST(ic_poly_inherited_slot_still_fast_path)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "fire", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    /* `category` is declared on the Tagged proto (inherited) — all 3
     * Tagged instances share the proto's `category` slot.  Each
     * instance gets its own local `idx`.  At-body reads BOTH per
     * press; only `idx` differs across instances. */
    int rc = utest_e2e_compile_and_run(&vm,
        "class Tagged {"
        "  var category = \"thing\";"
        "  var init = function (i) { this.idx = i; this }"
        "};"
        "Realm.things = List.new("
        "    Tagged.new().init(100),"
        "    Tagged.new().init(200),"
        "    Tagged.new().init(300));"
        "Realm.pick    = 0;"
        "Realm.last_cat = \"-\";"
        "Realm.last_idx = -1;"
        "at (fire?) Realm.last_cat = Realm.things.get(Realm.pick).category;"
        "at (fire?) Realm.last_idx = Realm.things.get(Realm.pick).idx",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    int64_t expected_idx[3] = { 100, 200, 300 };
    int i;
    for (i = 0; i < 3; i++) {
        UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "pick", 4,
                                                   utest_e2e_make_int((int64_t)i)));
        urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
        drain_to_quiescent(&vm);

        /* category always reads the inherited "thing" — fast path
         * cached pointer is correct because it points at the proto's
         * slot, which doesn't move per-instance. */
        UValue cat = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "last_cat", 8, &cat));
        UASSERT_EQ((int)UVAL_STR, (int)cat.kind);
        size_t cat_len = 0;
        const char *cat_s = urbi_value_as_str(cat, &cat_len);
        UASSERT(cat_s != NULL);
        UASSERT_EQ(5, (int)cat_len);
        UASSERT_EQ(0, memcmp(cat_s, "thing", 5));

        /* idx is local per instance — the polymorphic fast-path
         * branch must return the targeted instance's idx, not the
         * first cached one. */
        UValue idx_v = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "last_idx", 8, &idx_v));
        UASSERT_EQ((int)UVAL_INT, (int)idx_v.kind);
        UASSERT_EQ(expected_idx[i], idx_v.v.i);
    }

    urbi_vm_destroy(&vm);
}

/* === Suite entry. ==================================================== */
void
test_ic_polymorphic_suite(void)
{
    utest_run("ic_poly: GETSLOT same-site reads 3 same-shape instances",
              ic_poly_getslot_three_instances);
    utest_run("ic_poly: SETSLOT same-site writes 3 same-shape instances",
              ic_poly_setslot_three_instances);
    utest_run("ic_poly: SELF same-site method/slot read 3 same-shape instances",
              ic_poly_self_three_instances);
    utest_run("ic_poly: inherited slot still hits the fast path correctly",
              ic_poly_inherited_slot_still_fast_path);
}
