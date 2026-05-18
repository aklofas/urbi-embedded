/* SPDX-License-Identifier: BSD-3-Clause */
/* Trivial smoke app: prove components/esp32-idf/ builds + links under xtensa-esp-elf.
 *
 * Only calls urbi_api_version() (NULL-tolerant getter). Does not exercise any
 * VM functionality — that lands in tests/qemu/reactive_smoke/ later in the
 * v0.7.2-esp32 plan (Phase 5).
 */

#include <stdio.h>

#include "urbi/version.h"

void app_main(void)
{
    int maj = 0;
    int min = 0;
    int pat = 0;
    urbi_api_version(&maj, &min, &pat);
    printf("urbi %d.%d.%d\n", maj, min, pat);
}
