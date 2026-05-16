/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_demo_main — app_main boot sequence.
 *
 * T39 lands destructure_blob (12-byte ISR payload -> 3 UValue ints).
 * T40 lands app_main (urbi_vm_init -> port hooks -> events -> host fns ->
 * bytecode load -> peripherals -> task spawn).
 *
 * Pre-T40 (current commit) the c_log host fn lives here so the rewritten
 * eye_demo.u (post-T38) can reference `log(...)` from inside both at(...)
 * handlers.  T40 registers it via urbi_register(vm, realm, "log", c_log)
 * alongside the other host fns. */

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

/* destructure_blob: ISR-payload -> UValue-args destructure for "blob_seen".
 *
 * Matches the canonical urbi_event_payload_destructure_fn typedef at
 * <urbi/urbi.h>:502-505:
 *
 *     typedef int (*urbi_event_payload_destructure_fn)(
 *         struct UVM *vm,
 *         const urbi_event_payload_t *payload, size_t payload_len,
 *         UValue *out_args, int max_args, void *ud);
 *
 * The plan spec sketch (plans/2026-05-11-v0.7.2-esp32.md T39) used
 * `out` / `max` parameter names — same shape, just different identifiers.
 *
 * Payload contract: eye_camera.c:75-80 writes the centroid + area as three
 * uint32_t channels of the 16-byte urbi_event_payload_t.u32[] union:
 *
 *     p.u32[0] = blob.x       (0..319)
 *     p.u32[1] = blob.y       (0..239)
 *     p.u32[2] = blob.area    (pixel count)
 *     p.u32[3] = 0            (reserved / pad)
 *
 * urbiscript at-binding: `at (blob_seen ?(x, y, area)) { ... }` receives
 * three int args; the rewritten eye_demo.u uses (x, y, area) shape.
 *
 * Returns 3 on success (three UValues written), URBI_ERR_INVALID_ARG (-1)
 * if max_args < 3 or payload_len < 12 — the at-drain falls back to dropping
 * the body args for this occurrence per <urbi/urbi.h>:486-493.
 *
 * Thread safety: MAIN (called from the safepoint drain on the urbi task,
 * NOT from ISR context — the ring queues the raw bytes; this fn runs at
 * the next urbi_step). */
static int destructure_blob(struct UVM *vm,
                            const urbi_event_payload_t *payload,
                            size_t payload_len,
                            UValue *out_args, int max_args, void *ud)
{
    (void)vm;
    (void)ud;
    if (max_args < 3 || payload_len < 12 || payload == NULL || out_args == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    out_args[0] = urbi_make_int((int64_t)payload->u32[0]);
    out_args[1] = urbi_make_int((int64_t)payload->u32[1]);
    out_args[2] = urbi_make_int((int64_t)payload->u32[2]);
    return 3;
}
