/* SPDX-License-Identifier: BSD-3-Clause */

#include "urbi.h"
#include "uvm.h"

#if __STDC_HOSTED__
#  include <stdio.h>
#  include <stdlib.h>
#endif

#define URBI_VERSION "0.1.0-skeleton"

const char *urbi_version(void) { return URBI_VERSION; }

/* urbi_panic: fatal runtime error.
 * Hosted: writes msg to stderr, then aborts.
 * Freestanding: spins forever (no OS abort). */
void
urbi_panic(const char *msg)
{
#if __STDC_HOSTED__
    fputs(msg, stderr);
    fputc('\n', stderr);
    abort();
#else
    (void)msg;
    /* Freestanding: no abort() available.  Spin to halt execution.
     * Embedded BSPs may override by wrapping or patching this symbol. */
    for (;;) { /* spin */ }
#endif
}

/* urbi_set_isr_check_fn: install an ISR-context predicate.
 * Pass NULL to disable ISR checking (the default after uvm_init). */
void
urbi_set_isr_check_fn(struct UVM *vm, bool (*fn)(void))
{
    if (!vm) return;
    vm->isr_check_fn = fn;
}

/* urbi_set_callback_watchdog_mode: select WARN or ASSERT on slow callback. */
void
urbi_set_callback_watchdog_mode(struct UVM *vm, uint8_t mode)
{
    if (!vm) return;
    vm->callback_watchdog_mode = mode;
}

/* urbi_call_host_with_watchdog: URBI_DEBUG build implementation.
 * Times fn() using vm->host_time_us; logs or panics if elapsed exceeds
 * vm->callback_warn_us.  Non-debug builds use the macro in urbi.h. */
#ifdef URBI_DEBUG
UValue
urbi_call_host_with_watchdog(struct UVM *vm, struct UStrand *s,
                             UHostFn fn, int argc, UValue *argv)
{
    uint64_t t0      = vm->host_time_us();
    UValue   r       = fn(s, argc, argv);
    uint64_t elapsed = vm->host_time_us() - t0;
    if (elapsed > (uint64_t)vm->callback_warn_us) {
        if (vm->callback_watchdog_mode == URBI_WATCHDOG_ASSERT) {
            urbi_panic("host callback exceeded watchdog threshold");
        } else if (vm->host_log_fn) {
            vm->host_log_fn(vm, URBI_LOG_WARN,
                            "host callback exceeded %u us (took %llu us)",
                            vm->callback_warn_us, (unsigned long long)elapsed);
        }
    }
    return r;
}
#endif /* URBI_DEBUG */
