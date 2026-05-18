/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock_bsp.h"
#include "../../components/stm32f4-hal-baremetal/include/port_stm32f4.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* urbi.h log-level constants — keep in sync with include/urbi/urbi.h */
#define URBI_LOG_DEBUG 0
#define URBI_LOG_INFO  1
#define URBI_LOG_WARN  2
#define URBI_LOG_ERROR 3

static void test_diag_info(void) {
    mock_bsp_reset();
    port_diag(NULL, URBI_LOG_INFO, "hello %d", 42);
    assert(mock_uart.call_count >= 1);
    assert(strstr(mock_uart.buf, "hello 42") != NULL);
    /* Expect [I] prefix somewhere */
    assert(strstr(mock_uart.buf, "[I]") != NULL ||
           strstr(mock_uart.buf, "INFO") != NULL);
    printf("test_diag_info PASS\n");
}

static void test_diag_error_routing(void) {
    mock_bsp_reset();
    port_diag(NULL, URBI_LOG_ERROR, "oops");
    assert(strstr(mock_uart.buf, "oops") != NULL);
    assert(strstr(mock_uart.buf, "[E]") != NULL ||
           strstr(mock_uart.buf, "ERROR") != NULL);
    printf("test_diag_error_routing PASS\n");
}

int main(void) {
    test_diag_info();
    test_diag_error_routing();
    return 0;
}
