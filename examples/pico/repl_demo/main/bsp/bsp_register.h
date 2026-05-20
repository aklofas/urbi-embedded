/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/bsp/bsp_register.h
 *
 * Single entry point that wires all four BSP fixtures (led / temp /
 * button / tick) into the global realm of the given VM.  Call AFTER
 * urbi_vm_init + urbi_stdlib_boot, BEFORE the REPL listener starts.
 *
 * Surface model: the v0.7.1 embedding API exposes flat top-level
 * realm-globals only (urbi_register / urbi_realm_set_global) — there
 * is no public property-getter or sub-object-method installer.  Each
 * fixture therefore registers flat names: `led_on`, `led_off`,
 * `led_toggle`, `led_pwm`, `temp_celsius`, `button_pressed`, plus the
 * named events `pressed` and `tick`.  Documented in each fixture .c. */

#ifndef URBI_PICO_BSP_REGISTER_H
#define URBI_PICO_BSP_REGISTER_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

/* Wire all four BSP fixtures into vm's global realm.  Returns 0 (URBI_OK)
 * on success; on first per-fixture failure, returns that fixture's
 * error code (negative UErrCode) without attempting later fixtures.
 *
 * Call ordering: urbi_vm_init → urbi_stdlib_boot → bsp_register →
 * bsp_tick_start → REPL listener. */
int bsp_register(struct UVM *vm);

/* Arm TIMER_IRQ_0 to fire every 100 ms.  Must be called AFTER
 * bsp_register so that the named events and host-fns are available.
 * Returns 0 on success, non-zero on hardware-init failure. */
int bsp_tick_start(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_PICO_BSP_REGISTER_H */
