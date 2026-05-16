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

#ifdef __cplusplus
extern "C" {
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

#ifdef __cplusplus
}
#endif

#endif /* URBI_PORT_ESP_IDF_H */
