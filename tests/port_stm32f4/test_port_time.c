/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock_bsp.h"
#include "../../components/stm32f4-hal-baremetal/include/port_stm32f4.h"
#include <stdio.h>
#include <assert.h>

static void test_time_us_zero_at_startup(void) {
    mock_bsp_reset();
    mock_dwt_cyccnt = 0;
    assert(port_time_us(NULL) == 0);
    printf("test_time_us_zero_at_startup PASS\n");
}

static void test_time_us_180mhz_conversion(void) {
    mock_bsp_reset();
    /* 180_000_000 cycles at 180 MHz == 1_000_000 us == 1 second. */
    mock_dwt_cyccnt = 180000000;
    uint64_t us = port_time_us(NULL);
    assert(us == 1000000);
    printf("test_time_us_180mhz_conversion PASS\n");
}

static void test_time_us_monotonic(void) {
    mock_bsp_reset();
    mock_dwt_cyccnt = 1800;       /* 10 us */
    uint64_t t1 = port_time_us(NULL);
    mock_dwt_cyccnt = 3600;       /* 20 us */
    uint64_t t2 = port_time_us(NULL);
    assert(t2 > t1);
    printf("test_time_us_monotonic PASS (%llu -> %llu)\n",
           (unsigned long long)t1, (unsigned long long)t2);
}

int main(void) {
    test_time_us_zero_at_startup();
    test_time_us_180mhz_conversion();
    test_time_us_monotonic();
    return 0;
}
