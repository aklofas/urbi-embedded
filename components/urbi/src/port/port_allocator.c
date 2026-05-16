/* SPDX-License-Identifier: BSD-3-Clause */
/* ESP-IDF port: PSRAM-backed allocator hook for urbi_vm_init.
 *
 * Implements the UVMAllocFn contract (include/urbi/types.h):
 *   ptr == NULL, nbytes > 0   → allocate
 *   ptr != NULL, nbytes == 0  → free
 *   ptr != NULL, nbytes > 0   → realloc
 *
 * Backing store: MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT — external PSRAM with
 * byte-granularity access.  Targets without PSRAM (or with PSRAM disabled
 * in sdkconfig) will see heap_caps_malloc return NULL; in that case the
 * embedder should swap MALLOC_CAP_SPIRAM → MALLOC_CAP_INTERNAL for the
 * ~64 KB internal-SRAM heap.  Per spec §4.3 of the v0.7.2-esp32 plan. */

#include "esp_heap_caps.h"

#include "port_esp_idf.h"

void *port_psram_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) {
        if (ptr) heap_caps_free(ptr);
        return NULL;
    }
    if (!ptr) {
        return heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return heap_caps_realloc(ptr, nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
