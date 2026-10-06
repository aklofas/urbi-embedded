/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock_bsp.h"

uint32_t mock_dwt_cyccnt;

void mock_bsp_reset(void) {
    mock_dwt_cyccnt = 0;
}
