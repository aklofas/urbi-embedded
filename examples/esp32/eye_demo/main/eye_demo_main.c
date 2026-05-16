/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_demo_main — app_main boot sequence.
 *
 * Placeholder TU.  T39/T40 fill this in with:
 *   - destructure_blob (12-byte payload -> 3 UValue ints)
 *   - app_main (urbi_vm_init -> port hooks -> events -> host fns ->
 *     bytecode load -> peripherals -> task spawn)
 *
 * Existing as a stub at T38 only so that CMake configure can succeed
 * — the SRCS list references this file, and ESP-IDF errors out at
 * configure time if a listed source doesn't exist.  The link step will
 * still fail until T40 lands app_main, which is the expected
 * pre-Phase-5-completion state.
 *
 * T39 PARTIAL: c_log host fn lives here pre-T40 because the rewritten
 * eye_demo.u (post-T38) references `log(...)` from inside both at(...)
 * handlers.  T40 will register it via urbi_register(vm, realm, "log",
 * c_log) alongside the other host fns. */

#include <stddef.h>      /* size_t */

#include "esp_log.h"

#include "urbi/urbi.h"
#include "urbi/types.h"

static const char *TAG = "eye_demo";

/* c_log: urbiscript-visible logging shim.
 *
 * Signature matches urbi_native_method_fn (include/urbi/urbi.h:295) —
 * see eye_display.c:161-188 for the spec-vs-real-API drift note that
 * applies here too.
 *
 * Argument convention: a single UVAL_STR message.  Extra args are
 * ignored; nargs == 0 is a no-op.  Logged at ESP_LOG_INFO via the
 * "eye_demo" tag so menuconfig log-level filters apply uniformly with
 * the camera / display tasks.
 *
 * Returns UEXEC_OK and writes UVAL_NIL into *out.  Never raises.
 *
 * Concurrency: MAIN (script context).  ESP_LOGI is thread-safe under
 * the default vprintf path, but this fn is only called from the urbi
 * task — no cross-task contention to reason about. */
int c_log(struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm;
    (void)self;
    if (nargs >= 1U && args != NULL) {
        size_t len = 0U;
        const char *s = urbi_value_as_str(args[0], &len);
        if (s != NULL) {
            ESP_LOGI(TAG, "%.*s", (int)len, s);
        }
    }
    if (out != NULL) {
        *out = urbi_make_nil();
    }
    return UEXEC_OK;
}
