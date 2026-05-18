/* SPDX-License-Identifier: BSD-3-Clause */
/* test_port_gyro.c — host-side unit test for port_gyro_read_native.
 *
 * Design note: the plan spec assumed urbi_make_list / urbi_value_is_list /
 * urbi_value_list_get APIs from v0.7.1, but those were never shipped.
 * List objects are stdlib UObject-backed containers; there is no public C
 * API to construct or inspect them without a live VM + internal headers.
 *
 * Adapted approach: port_gyro_read_native returns urbi_make_ptr() pointing
 * to an internal float[3] buffer (x, y, z in rad/s).  The test verifies the
 * ptr kind and casts back.  Production script code reads the axes via the
 * companion gyro_x / gyro_y / gyro_z host-fn accessors registered alongside
 * gyro_read().
 *
 * This keeps the test free of VM init overhead and avoids internal headers. */
#include "mock_bsp.h"
#include "../../components/stm32f4-hal-baremetal/include/port_stm32f4.h"
#include "../../include/urbi/types.h"
#include "../../include/urbi/urbi.h"
#include <stdio.h>
#include <assert.h>
#include <math.h>

static void test_gyro_read_returns_3_floats(void) {
    mock_bsp_reset();
    mock_gyro.fake_xyz[0] = 0.1f;
    mock_gyro.fake_xyz[1] = 0.2f;
    mock_gyro.fake_xyz[2] = 0.3f;

    UValue out;
    int rc = port_gyro_read_native(NULL, NULL, 0, &out);
    assert(rc == 0);
    assert(mock_gyro.call_count == 1);

    /* Result is a UVAL_PTR pointing to an internal float[3] (x, y, z). */
    assert(urbi_value_kind(out) == URBI_VALUE_PTR);
    const float *xyz = (const float *)urbi_value_as_ptr(out);
    assert(xyz != NULL);
    assert(fabsf(xyz[0] - 0.1f) < 1e-6f);
    assert(fabsf(xyz[1] - 0.2f) < 1e-6f);
    assert(fabsf(xyz[2] - 0.3f) < 1e-6f);

    printf("test_gyro_read_returns_3_floats PASS\n");
}

static void test_gyro_read_wrong_arity(void) {
    mock_bsp_reset();
    UValue dummy = urbi_make_nil();
    UValue out;
    int rc = port_gyro_read_native(NULL, &dummy, 1, &out);
    assert(rc != 0);
    assert(mock_gyro.call_count == 0);
    printf("test_gyro_read_wrong_arity PASS\n");
}

int main(void) {
    test_gyro_read_returns_3_floats();
    test_gyro_read_wrong_arity();
    return 0;
}
