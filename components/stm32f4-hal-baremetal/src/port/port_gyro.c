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
#ifndef URBI_PORT_TEST
    /* v0.8.2 bring-up debug: print first 8 calls and every 64th
     * thereafter on the X axis only (avoid drowning UART with all
     * three axes).  Confirms body strands are firing + shows whether
     * gyro returns plausible values for tilt. */
    if (axis == 0) {
        static uint32_t c = 0;
        c++;
        if (c <= 8U || (c & 0x3FU) == 1U) {
            char b[80];
            const char *d = "0123456789ABCDEF";
            int n = 0;
            const char *t = "gyro n=";
            while (t[n]) { b[n] = t[n]; n++; }
            for (int k = 28; k >= 0; k -= 4) b[n++] = d[(c >> k) & 0xF];
            /* Print raw f32 bit-patterns; user decodes mentally or via
             * `python -c "import struct; print(struct.unpack('f', bytes.fromhex(...)))"` */
            const char *t2 = " x=";
            int j = 0; while (t2[j]) { b[n++] = t2[j++]; }
            uint32_t xb = *(uint32_t *)&xyz[0];
            for (int k = 28; k >= 0; k -= 4) b[n++] = d[(xb >> k) & 0xF];
            const char *t3 = " y=";
            j = 0; while (t3[j]) { b[n++] = t3[j++]; }
            uint32_t yb = *(uint32_t *)&xyz[1];
            for (int k = 28; k >= 0; k -= 4) b[n++] = d[(yb >> k) & 0xF];
            const char *t4 = " z=";
            j = 0; while (t4[j]) { b[n++] = t4[j++]; }
            uint32_t zb = *(uint32_t *)&xyz[2];
            for (int k = 28; k >= 0; k -= 4) b[n++] = d[(zb >> k) & 0xF];
            b[n++] = '\r'; b[n++] = '\n';
            port_writer(NULL, "gyro", 4, b, (size_t)n, 0);
        }
    }
#endif
    *out = urbi_make_float((double)xyz[axis]);
    return 0;
}

int port_gyro_x_native(struct UVM *vm, UValue self,
                        UValue *args, uint8_t nargs, UValue *out) {
    (void)vm; (void)self; (void)args;
    int rc = gyro_axis(0, nargs, out);
#ifndef URBI_PORT_TEST
    /* v0.8.2 bring-up debug: print the value we ACTUALLY return so we
     * can rule out the host-fn vs the VM dispatch as the source of
     * rc=2.  First 3 calls only. */
    static int dbg_n = 0;
    if (dbg_n < 3) {
        dbg_n++;
        char buf[40];
        const char *d = "0123456789ABCDEF";
        int n = 0;
        const char *t = "gx returning rc=";
        while (t[n]) { buf[n] = t[n]; n++; }
        buf[n++] = d[(rc >> 4) & 0xF];
        buf[n++] = d[rc & 0xF];
        buf[n++] = ' '; buf[n++] = 'n'; buf[n++] = 'a'; buf[n++] = '=';
        buf[n++] = d[(nargs >> 4) & 0xF];
        buf[n++] = d[nargs & 0xF];
        buf[n++] = '\r'; buf[n++] = '\n';
        port_writer(NULL, "gx", 2, buf, (size_t)n, 0);
    }
#endif
    return rc;
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
