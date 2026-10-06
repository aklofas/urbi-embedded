/* SPDX-License-Identifier: BSD-3-Clause */
/* Gyro wrapper for STM32F429I-DISC1 (L3GD20 via SPI5).
 *
 * Exposes three natives gyro_x/y/z, each returning one axis as a float.
 * Three independent calls keep each native a plain scalar return; the
 * runtime enforces the zero-argument arity they are registered with.
 * L3GD20 max output rate is 800 Hz; calling three times per 50ms tick
 * (60/sec) is well within the budget.
 *
 * port_gyro_init() drives BSP_GYRO_Init with default sensitivity (250 dps). */

#include "port_stm32f4.h"
#include "urbi/urbi.h"
#include "urbi/types.h"

#include "stm32f429i_discovery_gyroscope.h"

void port_gyro_init(void) {
    BSP_GYRO_Init();
}

static int gyro_axis(int axis, UValue *out) {
    float xyz[3];
    BSP_GYRO_GetXYZ(xyz);
    *out = urbi_make_float((double)xyz[axis]);
    return UEXEC_OK;
}

int port_gyro_x_native(struct UVM *vm, UValue self,
                        UValue *args, uint8_t nargs, UValue *out) {
    (void)vm; (void)self; (void)args; (void)nargs;
    return gyro_axis(0, out);
}

int port_gyro_y_native(struct UVM *vm, UValue self,
                        UValue *args, uint8_t nargs, UValue *out) {
    (void)vm; (void)self; (void)args; (void)nargs;
    return gyro_axis(1, out);
}

int port_gyro_z_native(struct UVM *vm, UValue self,
                        UValue *args, uint8_t nargs, UValue *out) {
    (void)vm; (void)self; (void)args; (void)nargs;
    return gyro_axis(2, out);
}
