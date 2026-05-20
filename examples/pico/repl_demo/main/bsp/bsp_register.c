/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/bsp/bsp_register.c
 *
 * Single setup call that wires LED + temperature + button + tick
 * fixtures into the given VM's global realm.  Call from main() AFTER
 * urbi_vm_init + urbi_stdlib_boot, BEFORE the REPL listener starts.
 *
 * The tick hardware is NOT armed here — call bsp_tick_start(vm)
 * separately, after bsp_register returns success.  Separating "wire
 * up host-fns + events" from "start the ISR" lets main.c bring the
 * REPL listener fully online (so an `at(tick)` registered from a
 * connected client at startup sees ticks from t0) without racing the
 * ISR against urbi_event_register's allocation path. */

#include "bsp_register.h"
#include "bsp_led.h"
#include "bsp_temp.h"
#include "bsp_button.h"
#include "bsp_tick.h"

int bsp_register(struct UVM *vm)
{
    int err;
    err = bsp_led_register(vm);
    if (err != 0) {
        return err;
    }
    err = bsp_temp_register(vm);
    if (err != 0) {
        return err;
    }
    err = bsp_button_register(vm);
    if (err != 0) {
        return err;
    }
    err = bsp_tick_register(vm);
    if (err != 0) {
        return err;
    }
    return 0;
}
