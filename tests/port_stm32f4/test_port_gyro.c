/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock_bsp.h"
#include "../../components/stm32f4-hal-baremetal/include/port_stm32f4.h"
#include "../../include/urbi/types.h"
#include "../../include/urbi/urbi.h"
#include <stdio.h>
#include <assert.h>
#include <math.h>

static void test_gyro_each_axis_returns_correct_value(void) {
    mock_bsp_reset();
    mock_gyro.fake_xyz[0] = 0.1f;
    mock_gyro.fake_xyz[1] = 0.2f;
    mock_gyro.fake_xyz[2] = 0.3f;

    UValue out;
    int rc;

    UValue nil_self = urbi_make_nil();
    rc = port_gyro_x_native(NULL, nil_self, NULL, 0, &out);
    assert(rc == 0);
    assert(urbi_value_kind(out) == URBI_VALUE_FLOAT);
    assert(fabsf((float)urbi_value_as_float(out) - 0.1f) < 1e-6f);

    rc = port_gyro_y_native(NULL, nil_self, NULL, 0, &out);
    assert(rc == 0);
    assert(fabsf((float)urbi_value_as_float(out) - 0.2f) < 1e-6f);

    rc = port_gyro_z_native(NULL, nil_self, NULL, 0, &out);
    assert(rc == 0);
    assert(fabsf((float)urbi_value_as_float(out) - 0.3f) < 1e-6f);

    /* Three independent BSP calls expected (one per accessor). */
    assert(mock_gyro.call_count == 3);

    printf("test_gyro_each_axis_returns_correct_value PASS\n");
}

static void test_gyro_wrong_arity(void) {
    mock_bsp_reset();
    UValue dummy = urbi_make_int(0);
    UValue nil_self = urbi_make_nil();
    UValue out;
    int rc = port_gyro_x_native(NULL, nil_self, &dummy, 1, &out);
    assert(rc == -1);
    printf("test_gyro_wrong_arity PASS\n");
}

int main(void) {
    test_gyro_each_axis_returns_correct_value();
    test_gyro_wrong_arity();
    return 0;
}
