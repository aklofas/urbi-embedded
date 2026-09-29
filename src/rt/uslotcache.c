/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uslotcache.c — where a proto's cache array comes from and goes. */

#include <string.h>
#include "rt/uslotcache.h"
#include "rt/uexec.h"

/* Not before the stdlib has booted: the boot chunk touches hundreds of
 * sites exactly once, and an array for each would be paid for by every
 * VM and used by none. */
USlotCache *uslotcache_alloc(struct UVM *vm, UProto *p) {
    if (!vm->stdlib_booted || p->ic_count == 0) return NULL;
    size_t n = (size_t)p->ic_count * sizeof(USlotCache);
    USlotCache *a = (USlotCache *)ugc_raw_alloc(vm, n);
    if (a == NULL) return NULL;
    memset(a, 0, n);
    p->site_cache = a;
    return a;
}

void uslotcache_free_tree(struct UVM *vm, UProto *root) {
    if (root == NULL) return;
    if (root->site_cache) {
        ugc_raw_free(vm, root->site_cache, (size_t)root->ic_count * sizeof(USlotCache));
        root->site_cache = NULL;
    }
    for (size_t k = 0; k < root->nested_count; k++) uslotcache_free_tree(vm, root->nested[k]);
}

void uslotcache_clear_tree(struct UVM *vm, UProto *root) {
    if (root == NULL) return;
    if (root->site_cache) memset(root->site_cache, 0, (size_t)root->ic_count * sizeof(USlotCache));
    for (size_t k = 0; k < root->nested_count; k++) uslotcache_clear_tree(vm, root->nested[k]);
}
