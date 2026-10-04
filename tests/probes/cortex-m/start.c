/* SPDX-License-Identifier: BSD-3-Clause */
/* Minimal Cortex-M reset path for the qemu probe runs: copy .data, zero
 * .bss, enable the FPU, bring up newlib's semihosting handles, call main
 * and exit through semihosting so qemu stops.  Nothing here is a port. */
#include <stdint.h>
#include <stdlib.h>
extern uint32_t __etext, __data_start__, __data_end__, __bss_start__, __bss_end__, __StackTop;
extern void __libc_init_array(void);
extern void initialise_monitor_handles(void);
extern int main(void);
void _init(void) {}
void _fini(void) {}
void Reset_Handler(void)
{
    *(volatile uint32_t *)0xE000ED88 |= (0xFu << 20);   /* CPACR: CP10/CP11 full access */
    uint32_t *s = &__etext, *d = &__data_start__;
    while (d < &__data_end__) *d++ = *s++;
    for (d = &__bss_start__; d < &__bss_end__;) *d++ = 0;
    __libc_init_array();
    initialise_monitor_handles();
    exit(main());
}
void Default_Handler(void) { for (;;) {} }
__attribute__((section(".isr_vector")))
const void *vectors[16] = { &__StackTop, Reset_Handler, Default_Handler, Default_Handler };
