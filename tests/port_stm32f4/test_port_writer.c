/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock_bsp.h"
#include "../../components/stm32f4-hal-baremetal/include/port_stm32f4.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

static void test_writer_basic(void) {
    mock_bsp_reset();
    const char *ch = "clog";
    const char *msg = "hello\n";
    port_writer(NULL, ch, strlen(ch), msg, strlen(msg), 0);
    assert(mock_uart.call_count >= 1);
    /* Expect the captured buffer to contain the message somewhere. */
    assert(strstr(mock_uart.buf, "hello") != NULL);
    printf("test_writer_basic PASS\n");
}

static void test_writer_channel_prefix(void) {
    mock_bsp_reset();
    port_writer(NULL, "cerr", 4, "oops", 4, 0);
    /* Channel prefix should appear: e.g. "[cerr] oops\n" or similar. */
    assert(strstr(mock_uart.buf, "cerr") != NULL);
    assert(strstr(mock_uart.buf, "oops") != NULL);
    printf("test_writer_channel_prefix PASS\n");
}

int main(void) {
    test_writer_basic();
    test_writer_channel_prefix();
    return 0;
}
