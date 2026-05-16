/* SPDX-License-Identifier: BSD-3-Clause */
/* ESP-IDF port glue — function prototypes for urbi <-> ESP-IDF integration.
 *
 * Embedders include this header from their app_main / FreeRTOS task code and
 * pass the wrappers below into the corresponding urbi register / init hooks
 * (urbi_vm_init, urbi_set_writer, urbi_set_time_us, urbi_set_wake_fn, ...).
 *
 * Each wrapper has a signature compatible with the urbi public hook typedef
 * it satisfies (UVMAllocFn, urbi_writer_fn, ...).  See include/urbi/types.h
 * and include/urbi/urbi.h for the canonical typedefs. */
#ifndef URBI_PORT_ESP_IDF_H
#define URBI_PORT_ESP_IDF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* FreeRTOS header pulled in so StackType_t is in scope for the
 * URBI_STACK_WORDS default below and for embedders that declare static
 * stack buffers (StackType_t[]) in their app_main. */
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration so the runtime-diag callback signature compiles
 * without dragging the full urbi public API into every TU that uses
 * port glue.  Embedders that actually call urbi_set_diag_fn will
 * #include <urbi/urbi.h> for the setter itself anyway. */
struct UVM;

/* URBI_STACK_WORDS: size of the urbi task's stack in StackType_t words.
 * Default is 8 KB / sizeof(StackType_t).  Embedders override via -D at
 * build time when they need a different stack budget.
 *
 * Exposed here (rather than file-local in port_freertos_task.c) so
 * app_main can declare a matching `static StackType_t buf[URBI_STACK_WORDS]`
 * for xTaskCreateStatic.  The matching default lives in port_freertos_task.c;
 * both #ifndef guards see the same effective value because this header is
 * included before that TU is compiled. */
#ifndef URBI_STACK_WORDS
#  define URBI_STACK_WORDS  (8U * 1024U / sizeof(StackType_t))
#endif

/* PSRAM-backed allocator hook.  Signature matches UVMAllocFn
 * (include/urbi/types.h) — pass directly to urbi_vm_init's alloc_fn slot.
 *
 * Uses MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT.  On boards without PSRAM
 * (or with PSRAM disabled in sdkconfig) heap_caps_malloc returns NULL and
 * urbi_vm_init will fail allocation.  For internal-SRAM-only boards
 * (~64 KB heap, ~50 ns access vs ~150 ns PSRAM through the cache), write
 * a parallel wrapper using MALLOC_CAP_INTERNAL. */
void *port_psram_alloc(void *ptr, size_t nbytes, void *ud);

/* Monotonic-microseconds time source.  Signature matches urbi_time_us_fn
 * (include/urbi/urbi.h) — pass to urbi_set_time_us.  Backed by
 * esp_timer_get_time(); resolution is 1 µs, wrap is ~292 000 years. */
uint64_t port_time_us(void);

/* Channel-dispatching writer.  Signature matches urbi_writer_fn
 * (include/urbi/urbi.h) — pass to urbi_set_writer.  Routes "cerr" through
 * ESP_LOGE, "clog" through ESP_LOGI, all other channels through printf
 * (UART/USB-CDC console). */
void port_writer(void *ud,
                 const char *channel, size_t channel_len,
                 const char *msg,     size_t msg_len,
                 uint64_t ts_us);

/* ISR-context predicate.  Signature matches the predicate type accepted by
 * urbi_set_isr_check_fn(vm, fn) — `bool (*)(void)` — pass directly.
 * Backed by xPortInIsrContext(); returns true when called from an ESP-IDF
 * ISR (including nested ISR dispatch). */
bool port_in_isr(void);

/* FreeRTOS task entry that drives urbi_step in a budget-bounded loop.
 * Pass to xTaskCreate / xTaskCreateStatic as the task entry; `arg` must
 * be the `struct UVM *` to step.  Parks on a task-notification when the
 * VM is QUIESCENT / WAKE_AT; calls esp_restart on URBI_STEP_FATAL.
 *
 * Compile-time tunables: URBI_STACK_WORDS (default 8 KB / sizeof(StackType_t))
 * and URBI_STEP_BUDGET (default 256).  Override via -D... at build time. */
void port_urbi_task_body(void *arg);

/* Wake-from-injection callback.  Signature matches urbi_wake_fn
 * (include/urbi/urbi.h:612) — `void (*)(void *ud)`.  `ud` must point to
 * a TaskHandle_t for the urbi task.  ISR-safe (dispatches via
 * xPortInIsrContext + vTaskNotifyGiveFromISR + portYIELD_FROM_ISR).
 * Install via urbi_set_wake_fn(vm, port_wake_from_inject, &urbi_task_handle). */
void port_wake_from_inject(void *ud);

/* Runtime diagnostic channel routed to ESP_LOG.  Signature matches
 * urbi_diag_fn (include/urbi/urbi.h) — pass directly to urbi_set_diag_fn.
 * Maps URBI_LOG_DEBUG/INFO/WARN/ERROR to ESP_LOGI/I/W/E with the
 * "urbi-runtime" tag.  vsnprintf into a 192-byte stack buffer; truncates
 * silently past that.
 *
 * Without this (or some equivalent), the runtime's URBI_LOG_WARN messages
 * — watcher body throws, spawn OOM, watchdog warnings — drop on the floor
 * because the host_log_fn default is NULL with no script-side sink. */
void port_diag_to_esp(struct UVM *vm, int level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* URBI_PORT_ESP_IDF_H */
