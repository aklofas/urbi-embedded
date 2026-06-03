/* SPDX-License-Identifier: BSD-3-Clause */
/* Regression test for the STM32F4 mandelbrot scheduler hang found during the
 * v1.0.0 hardware bring-up (design-risks v1.0-stm32f4-hang).
 *
 * The mandelbrot demo's main loop runs in the LOADER strand:
 *
 *   while (true) {
 *     Realm.redraw_requested = false;
 *     render_all();                       // native draw calls
 *     waituntil (Realm.redraw_requested)  // condition-form, in the loader strand
 *   }
 *
 * woken by a host-injected event handler that mutates the flag and ends with a
 * NATIVE call (the gyro intensity-bar paint):
 *
 *   at (tick?) { Realm.bar = Realm.bar + 1; Realm.redraw_requested = true; paint() }
 *
 * Two distinct, pre-existing reactive-runtime bugs combined to freeze the demo
 * (both regressed during the v0.10.x reactive refactor; the demo last ran on HW
 * at v0.8.2, and no host test exercised "condition-form waituntil in a loop
 * woken by a native-ending event handler"):
 *
 *   Bug #1 — native call is not a safepoint: a successful native call in
 *     OP_CALL returns via NEXT(), not `goto safepoint`, so an event-handler
 *     body whose state mutation is followed only by a native call never runs
 *     the watcher eval pass.  The loader's waituntil condition is never
 *     re-evaluated and the loader never wakes (frame stuck at 1).  Fixed by a
 *     post-slice reactive backstop in urbi_step (src/vm/ustep.c).
 *
 *   Bug #2 — at-event watcher bodies were not GC-rooted: walk_uevent shaded the
 *     pool-allocated UWatcher cell (a no-op) but never marked w->body, so the
 *     handler closure was collected on the first GC cycle (triggered here by
 *     the per-iteration waituntil cond-closure churn).  w->body then dangled
 *     and the handler ran a freed/reused proto without effect (frame stuck ~5,
 *     handler silently dead).  Fixed by marking watcher children in walk_uevent
 *     (src/object/utypes_init.c).
 *
 * Pass criteria: driving urbi_step device-style while injecting `tick` events
 * must keep the loader loop progressing (frame advances roughly once per tick)
 * far past the bug-2 wedge point, with the handler still firing.
 */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "chunk/uchunk.h"
#include "value/uarena.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Native invoked at the end of the event handler body and inside the render
 * loop — the analogue of lcd_fill_rect.  A successful native call is the path
 * that bug #1 left out of the safepoint set. */
static long g_paint_calls;
static int
paint_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    g_paint_calls++;
    if (out) *out = urbi_make_nil();
    return 0;
}

static long
get_global_int(struct UVM *vm, struct URealm *r, const char *name)
{
    UValue v = urbi_make_nil();
    if (urbi_realm_get_global(vm, r, name, (uint32_t)strlen(name), &v) != URBI_OK)
        return -1;
    if (v.kind != (uint8_t)UVAL_INT)
        return -2;
    return (long)v.v.i;
}

/* Mandelbrot reactive skeleton: condition-form waituntil in an infinite loop in
 * the loader strand, woken by a native-ending at(event?) handler.  `render`
 * yields between native "tiles" (the `;` separators) and re-installs a fresh
 * waituntil cond closure every iteration — the churn that triggers the GC pass
 * exposing bug #2. */
static const char *MANDELBROT_SKELETON =
"Realm.redraw_requested = false;\n"
"Realm.bar = 0;\n"
"Realm.frame = 0;\n"
"Realm.render = function () {\n"
"  var i = 0;\n"
"  while (i < 4) {\n"
"    if (Realm.redraw_requested == false) {\n"
"      paint();\n"
"      i = i + 1\n"
"    } else {\n"
"      i = 4\n"
"    }\n"
"  }\n"
"};\n"
"at (tick?) {\n"
"  Realm.bar = Realm.bar + 1;\n"
"  Realm.redraw_requested = true;\n"
"  paint()\n"
"};\n"
"while (true) {\n"
"  Realm.redraw_requested = false;\n"
"  Realm.frame = Realm.frame + 1;\n"
"  Realm.render();\n"
"  waituntil (Realm.redraw_requested)\n"
"};\n";

UTEST(waituntil_loader_loop_wakes_on_native_ending_handler)
{
    g_paint_calls = 0;

    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UASSERT_EQ(URBI_OK, urbi_register(&vm, r, "paint", paint_native));
    urbi_event_id_t tick = urbi_event_register(&vm, r, "tick", NULL, NULL);
    UASSERT(tick != URBI_EVENT_ID_INVALID);

    /* Compile + run via the loader-strand path: the first render pass runs,
     * then the loader parks in waituntil (stays parked for the test). */
    UArena arena;
    UProto module = {0};
    uarena_init(&arena, 4096);
    UASSERT_EQ(URBI_OK, utest_e2e_compile_and_run_with_module(
        &vm, &arena, &module, MANDELBROT_SKELETON, NULL));

    /* Device-style pump: inject a `tick` (TIM2 ISR analogue), drive urbi_step.
     * 60 ticks, 5 steps each — well past the ~5-cycle bug-2 wedge point. */
    const int TICKS = 60;
    const int STEPS_PER_TICK = 5;
    for (int t = 0; t < TICKS; t++) {
        UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)tick, NULL, 0U));
        for (int s = 0; s < STEPS_PER_TICK; s++) {
            uint64_t wake = 0;
            UStepResult st = urbi_step(&vm, 256U, &wake);
            UASSERT(st != URBI_STEP_FATAL);
        }
    }

    long bar   = get_global_int(&vm, r, "bar");
    long frame = get_global_int(&vm, r, "frame");

    /* Handler fired on (almost) every tick: bug #2 (GC-collected handler) would
     * freeze bar at ~5. */
    UASSERT(bar >= TICKS - 2);

    /* Loader loop kept waking from waituntil on each native-ending handler:
     * bug #1 would freeze frame at 1; bug #2 at ~5.  Post-fix it tracks the
     * tick count (one extra for the initial render pass). */
    UASSERT(frame >= TICKS - 2);

    /* Sanity: native calls actually ran (render 4/frame + handler 1/tick). */
    UASSERT(g_paint_calls >= (long)TICKS * 4);

    /* Cleanup: the loader strand is parked-forever (still bound to module).
     * uchunk_destroy releases the module internals; urbi_vm_destroy then kills
     * all realm strands (dropping the binding) — same accepted teardown order
     * as test_loader_strand_persistence's parked-loader cases. */
    uarena_destroy(&arena);
    uchunk_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

void
test_waituntil_loader_wake_suite(void)
{
    utest_run("waituntil_loader_wake: loader loop wakes on native-ending at-event handler",
              waituntil_loader_loop_wakes_on_native_ending_handler);
}
