/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_ref.h — private inter-TU API for the per-VM reference table (Gap Q, v0.7.1).
 * Only included by src/vm/ translation units.
 * The public surface (urbi_ref / urbi_ref_get / urbi_unref) lives in <urbi/urbi.h>. */

#ifndef UVM_REF_H
#define UVM_REF_H

struct UVM;   /* forward; full definition in vm/uvm.h */
struct UGcRootCallback;

#include "gc/ugc.h"  /* UGcRootCallback, UGcRootProviderFn */

/* ref_table_walk_roots: GC root provider for the per-VM reference table.
 *
 * Registered at urbi_vm_init via urbi_gc_register_root_provider.
 * Calls cb(vm, &slot.value, ctx) for every in_use slot so the GC marks
 * those values live.  This is the load-bearing correctness piece for Gap Q
 * — without it, urbi_ref does not actually pin anything. */
void ref_table_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);

#endif /* UVM_REF_H */
