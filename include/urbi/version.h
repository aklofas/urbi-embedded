/* SPDX-License-Identifier: BSD-3-Clause */
/* Public C API version macros + getter.
 *
 * Stability: core.
 *
 * Separate from URBI_BYTECODE_VERSION_BYTE (wire-format byte for .uc blobs)
 * and urbi_version() (project release string). Tracks the C-API contract
 * across MAJOR.MINOR.PATCH per the policy in CONTRIBUTING.md.
 *
 * MAJOR — removed function, changed signature, removed/renumbered enum
 *         value, struct-layout change visible across the boundary,
 *         removed URBI_ERR_* slot.
 * MINOR — additive: new function, new enum value appended, new URBI_ERR_*
 *         slot at the next free index, new build flag.
 * PATCH — bug fix only, no header change at all.
 *
 * Pre-v1.0 escape clause: while URBI_API_VERSION_MAJOR == 0, MINOR or
 * PATCH bumps MAY break ABI per standard semver convention — each bump
 * enumerates breakages in CHANGELOG. Uses to date:
 *   1. v0.7.2-esp32 — S41 urbi_set_diag_fn addition (0/7/1 → 0/7/3).
 *   2. v0.7.3-bugfixes — umodule_destroy signature change (0/7/3 → 0/7/4).
 *   3. v0.8.0-loader-strand — UModule.refcount + destroy_requested fields;
 *      urbi_run_chunk const-revert; URBI_ERR_LOADER_BUDGET addition
 *      (0/7/4 → 0/7/5).
 *   4. v0.8.1-uproto-root — UProto gains nested[]/root/next_alloc/refcount
 *      fields (layout shift); UModule loses nested[]/nested_count/nested_cap
 *      + stdlib_protos + stdlib_nested_arrays + stdlib_closures reorder;
 *      vm->rescued_protos replaces vm->stdlib_protos as sole deferred-
 *      destroy mechanism; wire format v1.6 → v1.7 (flat-on-root emitter).
 *      Minor-field bump (0/7/5 → 0/8/0) reflects structural significance.
 *   5. v0.8.4-closure-lifetime — Option B: UClosure + UUpvalCell promoted to
 *      GC-managed cells (urbi_gc_alloc).  UClosure loses next_alloc field
 *      (-8 B); UStrand loses closure_list + closed_cells fields (-16 B);
 *      UVM loses stdlib_closures + stdlib_upvalues fields (-16 B).  Public-
 *      symbol-visible signature changes: vm_alloc_closure drops list_head
 *      param; vm_close_upvalues drops closed_list param.  Internal API
 *      surface (host-facing C API) unchanged.  URBI_WATCHER_OWNS_* flag
 *      macros deleted (3 bits freed in UWatcher.flags).  Minor-field bump
 *      (0/8/0 → 0/9/0) reflects internal struct shrinkage + lifetime-model
 *      change despite no public-symbol additions.
 *   6. v0.8.5-recursive-emit — UClosure loses origin_nested +
 *      origin_nested_count fields (-16 B with alignment); UProto gains
 *      ic_index field (+2 B); UModule gains next_proto_serial +
 *      total_proto_count fields (+4 B).  uproto_alloc_nested
 *      signature changes from (module) to (module, parent_proto) — internal
 *      symbol not exposed by the public `include/urbi/` headers.  OP_CLOSURE dispatch arm
 *      rewrites to resolve Bx against executing_proto->nested[] (per-parent
 *      index) instead of cur_cl->origin_nested (flat root index).  Wire
 *      format unchanged at v1.7 / 0x17; on-disk bytes change for any source
 *      with nested function literals.  Minor-field bump (0/9/0 → 0/10/0)
 *      reflects UClosure layout shrinkage + the internal alloc_nested
 *      signature change.  No public-symbol additions or removals.
 *   7. v0.9.0-repl-foundation — UClosure 56 -> 48 B: origin_module_instance
 *      retired; UProto + UModule + URealm gain runtime back-pointers;
 *      ULexer gains transient syncline state.  (0/10/0 → 0/11/0)
 *   8. v0.9.1-repl-service Phase 1 — URealm gains has_compile_budget +
 *      compile_budget (UCompileBudget) + writer_fn + writer_ud fields
 *      (+24 B on 64-bit with alignment).  Public-API additions:
 *      urbi_realm_set_writer / set_compile_budget / get_compile_budget;
 *      urbi_vm_write_in_realm; URBI_DEFAULT_REPL_BUDGET exported const.
 *      New UErrCode values: URBI_ERR_FROZEN_PROTO (-21),
 *      URBI_ERR_COMPILE_BUDGET_DEPTH / NODES / SOURCE (-22..-24).
 *      UObject.flags gains URBI_OBJ_FLAG_READONLY (bit 7; UPROTO_FLAG_READONLY
 *      public spelling).  UCompileBudget struct added to <urbi/types.h>.
 *      Breaking-from-bytecode: OP_SETSLOT now raises TypeError when the
 *      receiver UObject carries URBI_OBJ_FLAG_READONLY — the 15 builtin
 *      atom protos (Object, Integer, Float, String, Boolean, Nil, Void,
 *      List, Dict, Symbol, Tag, Event, Mutex, Date, Duration) are marked
 *      readonly at urbi_stdlib_boot.  Global stays mutable per spec §4.1.
 *      (0/11/0 → 0/12/0)
 * Strict policy goes live at v1.0.0.
 *
 * Holding a pointer to an opaque type is part of the ABI; reading through
 * it is not.
 */

#ifndef URBI_VERSION_H
#define URBI_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

#define URBI_API_VERSION_MAJOR  0
#define URBI_API_VERSION_MINOR  12
#define URBI_API_VERSION_PATCH  0
#define URBI_API_VERSION_NUM    ((URBI_API_VERSION_MAJOR * 10000) \
                                + (URBI_API_VERSION_MINOR *   100) \
                                +  URBI_API_VERSION_PATCH)

/* Runtime getter. NULL-tolerant per arg. */
void urbi_api_version(int *out_major, int *out_minor, int *out_patch);

#ifdef __cplusplus
}
#endif

#endif /* URBI_VERSION_H */
