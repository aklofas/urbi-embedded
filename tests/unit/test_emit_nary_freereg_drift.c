/* SPDX-License-Identifier: BSD-3-Clause */
/* test_emit_nary_freereg_drift — regression for the AST_NARY between-
 * statement reset that left freereg ABOVE the local-zone floor after a
 * bare-call statement (discovered 2026-05-16 via the eye_demo BlobScan
 * urbiscript stress test on ESP32-S3-EYE hardware).
 *
 * Pre-fix shape:
 *
 *   class BS {
 *     var init = function(s) { this.subsample = s; this };
 *     var scan = function() {
 *       Realm.helper.noop();    // bare-call with discarded result
 *       var s = this.subsample;
 *       var iters = 0;
 *       var y = 0;
 *       while (y < N) {
 *         var x = 0;
 *         while (x < N) {
 *           iters = iters + 1;
 *           x = x + s
 *         };
 *         y = y + s
 *       };
 *       Realm.iters_seen = iters
 *     }
 *   };
 *
 * Pre-fix, emit_call_arm for the bare `Realm.helper.noop()` left
 * fs->freereg at callee_reg+2 (above the call result), without
 * resetting back to the local-zone boundary.  emit_nary_arm between
 * children only synced next_reg = freereg (not freereg = fs_temp_floor),
 * so subsequent `var s`, `var iters`, `var y` got slot numbers above
 * what fs_temp_floor (count-based: nactvar + r_global_slot) reported.
 *
 * Result: when emit_while_arm opened the body block, it reset freereg =
 * fs_temp_floor — which UNDER-ESTIMATED the actual top of locals by the
 * drift amount.  `var x = 0` inside the body then allocated at a slot
 * already occupied by `iters` (or similar).  At runtime, x and iters
 * shared a register; the inner-loop counter aliased the outer counter;
 * only the "diagonal" (x == y == iters/N) pixels were scanned.
 *
 * Symptom on the eye_demo: BlobScan returned iters=240 instead of 57,600
 * for a subsample=1 scan, with centroid_x == centroid_y in every fire.
 *
 * Fix: emit_nary_arm now mirrors emit_block_arm's between-statement reset:
 *
 *     e->current_fs->freereg = fs_temp_floor(e->current_fs);
 *     e->next_reg = e->current_fs->freereg;
 *
 * (previously only the second line ran).
 *
 * This test exercises the exact shape: bare-call at the top of a class
 * method, followed by var-decl chain, followed by nested while loops,
 * then asserts iters reaches the expected count. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"

#include <stdint.h>

#define UTEST(name) static void name(void)

UTEST(nary_freereg_drift_bare_call_then_var_chain)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    /* Seed Realm.iters_seen = -1 so we can tell apart "scan() never
     * wrote" (= -1), "scan ran but aliased x/iters" (= small N like
     * 20 instead of 400), and "scan ran correctly" (= 400). */
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(
        &vm, r, "iters_seen", 10, utest_e2e_make_int(-1)));

    /* The Helper class + Realm.helper instance + the bare-call .noop()
     * are the trigger setup; the BlobScan-style class has the actual
     * nested-loop scan we measure.  Use 20x20 so the test runs fast
     * (~10ms instead of multi-second) and 400 is unambiguous. */
    int rc = utest_e2e_compile_and_run(&vm,
        "class Helper { var noop = function () { 0 } };"
        "Realm.helper = Helper.new();"
        ""
        "class BS {"
        "  var init = function(s) { this.subsample = s; this };"
        "  var scan = function() {"
        "    Realm.helper.noop();"   /* bare-call trigger */
        "    var s = this.subsample;"
        "    var iters = 0;"
        "    var y = 0;"
        "    while (y < 20) {"
        "      var x = 0;"
        "      while (x < 20) {"
        "        iters = iters + 1;"
        "        x = x + s"
        "      };"
        "      y = y + s"
        "    };"
        "    Realm.iters_seen = iters"
        "  }"
        "};"
        "Realm.b = BS.new().init(1);"
        "Realm.b.scan()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UValue v = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "iters_seen", 10, &v));
    UASSERT_EQ((int)UVAL_INT, (int)v.kind);
    /* Pre-fix this asserted as 20 (only outer loop ran with x aliased
     * to iters; inner exited immediately after first increment).
     * Post-fix asserts 400 (20*20 = full grid). */
    UASSERT_EQ(400LL, v.v.i);

    urbi_vm_destroy(&vm);
}

/* Sibling test: the same drift can be triggered by ANY bare-statement
 * (not just a call) that leaves freereg above the floor.  Cover the
 * pattern with a member-set whose RHS is a chained call expression — a
 * second realistic shape that exercises the same emit_nary_arm reset. */
UTEST(nary_freereg_drift_member_set_then_var_chain)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(
        &vm, r, "iters_seen", 10, utest_e2e_make_int(-1)));

    int rc = utest_e2e_compile_and_run(&vm,
        "class Maker { var make = function () { 42 } };"
        "Realm.m = Maker.new();"
        ""
        "class BS {"
        "  var init = function() { this };"
        "  var scan = function() {"
        "    Realm.scratch = Realm.m.make();"   /* member-set with call RHS */
        "    var iters = 0;"
        "    var y = 0;"
        "    while (y < 10) {"
        "      var x = 0;"
        "      while (x < 10) {"
        "        iters = iters + 1;"
        "        x = x + 1"
        "      };"
        "      y = y + 1"
        "    };"
        "    Realm.iters_seen = iters"
        "  }"
        "};"
        "Realm.b = BS.new().init();"
        "Realm.b.scan()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UValue v = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "iters_seen", 10, &v));
    UASSERT_EQ((int)UVAL_INT, (int)v.kind);
    UASSERT_EQ(100LL, v.v.i);

    urbi_vm_destroy(&vm);
}

void
test_emit_nary_freereg_drift_suite(void)
{
    utest_run("nary_freereg_drift: bare-call + var-chain + nested while",
              nary_freereg_drift_bare_call_then_var_chain);
    utest_run("nary_freereg_drift: member-set RHS-call + var-chain",
              nary_freereg_drift_member_set_then_var_chain);
}
