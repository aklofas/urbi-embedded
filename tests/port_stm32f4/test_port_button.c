/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock_bsp.h"
#include "../../components/stm32f4-hal-baremetal/include/port_stm32f4.h"
#include <stdio.h>
#include <assert.h>

/* port_button.c exposes the bridge function used by the EXTI handler;
 * declare it here for the test. */
extern void port_button_exti_handler(void);

static void test_button_press_emits_event_in_isr(void) {
    mock_bsp_reset();
    /* Set up: bind to a fake VM + event ID */
    void *fake_vm = (void *)0x1234;
    port_button_init((struct UVM *)fake_vm, 7);  /* event_id = 7 */

    /* Simulate ISR context */
    mock_ipsr = 17;  /* nonzero = handler mode */

    port_button_exti_handler();

    assert(mock_event_ring.call_count == 1);
    assert(mock_event_ring.last_event_id == 7);
    assert(mock_event_ring.last_vm == fake_vm);

    printf("test_button_press_emits_event_in_isr PASS\n");
}

int main(void) {
    test_button_press_emits_event_in_isr();
    return 0;
}
