/* SPDX-License-Identifier: BSD-3-Clause */
/* Gyro wrapper for STM32F429I-DISC1 (L3GD20 via SPI5).
 *
 * Exposes three host-fns gyro_x/y/z, each returning one axis as UVAL_FLOAT.
 * Three independent calls avoid the urbi_make_list public-API gap (lists
 * are stdlib-only at v0.8.2).  L3GD20 max output rate is 800 Hz; calling
 * three times per 50ms tick (60/sec) is well within the budget.
 *
 * port_gyro_init() drives BSP_GYRO_Init with default sensitivity (250 dps). */

#include "port_stm32f4.h"
#include "urbi/urbi.h"
#include "urbi/types.h"

#ifdef URBI_PORT_TEST
#  include "mock_bsp.h"
#else
#  include "stm32f429i_discovery_gyroscope.h"
#endif

void port_gyro_init(void) {
#ifndef URBI_PORT_TEST
    BSP_GYRO_Init();
#endif
}

static int gyro_axis(int axis, uint8_t nargs, UValue *out) {
    if (nargs != 0) { *out = urbi_make_nil(); return -1; }
    float xyz[3];
    BSP_GYRO_GetXYZ(xyz);
    *out = urbi_make_float((double)xyz[axis]);
    return 0;
}

int port_gyro_x_native(struct UVM *vm, UValue self,
                        UValue *args, uint8_t nargs, UValue *out) {
    (void)vm; (void)self; (void)args;
    return gyro_axis(0, nargs, out);
}

int port_gyro_y_native(struct UVM *vm, UValue self,
                        UValue *args, uint8_t nargs, UValue *out) {
    (void)vm; (void)self; (void)args;
    return gyro_axis(1, nargs, out);
}

int port_gyro_z_native(struct UVM *vm, UValue self,
                        UValue *args, uint8_t nargs, UValue *out) {
    (void)vm; (void)self; (void)args;
    return gyro_axis(2, nargs, out);
}
