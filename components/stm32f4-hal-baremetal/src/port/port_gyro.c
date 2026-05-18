/* SPDX-License-Identifier: BSD-3-Clause */
/* Gyro wrapper for STM32F429I-DISC1 (L3GD20 via SPI5).
 *
 * Exposes gyro_read() host-fn returning the raw angular-velocity vector as a
 * urbi_make_ptr() pointing to an internal float[3] {omega_x, omega_y, omega_z}
 * in millidegrees/sec (BSP_GYRO_GetXYZ raw units; calibration is demo-side).
 *
 * Design note: the original plan assumed urbi_make_list / urbi_value_list_*
 * APIs (spec §table gyro_read row), but those were never shipped — List is a
 * stdlib UObject container with no public C construction API.  Returning a
 * UVAL_PTR is the only public-API-safe way to hand 3 floats to a native host
 * function without a live VM.  Production scripts read the axes via companion
 * gyro_x / gyro_y / gyro_z native accessors registered alongside gyro_read()
 * in main.c.  Revisit if urbi_make_list lands in v0.9+.
 *
 * port_gyro_init() drives BSP_GYRO_Init (250 dps full-scale, default ODR). */

#include "port_stm32f4.h"
#include "urbi/types.h"

#ifdef URBI_PORT_TEST
#  include "mock_bsp.h"
#else
#  include "stm32f429i_discovery_gyroscope.h"
#endif

/* Module-static result buffer.  Single-VM cooperative scheduler: no
 * concurrent native calls, so one buffer is safe.  Caller must read the
 * values before the next gyro_read() invocation. */
static float s_gyro_xyz[3];

void port_gyro_init(void) {
#ifndef URBI_PORT_TEST
    BSP_GYRO_Init();
#endif
}

int port_gyro_read_native(struct UVM *vm, UValue *args, int nargs,
                          UValue *out) {
    (void)vm;
    (void)args;
    if (nargs != 0) {
        *out = urbi_make_nil();
        return -1;
    }

    BSP_GYRO_GetXYZ(s_gyro_xyz);
    *out = urbi_make_ptr(s_gyro_xyz);
    return 0;
}
