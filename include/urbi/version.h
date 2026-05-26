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
 *   2. v0.7.3-bugfixes — uchunk_destroy signature change (0/7/3 → 0/7/4).
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
 *   9. v0.9.2-uproto-only — UModule struct deleted; UProto absorbs root
 *      metadata (source_name, origin_vm, next_proto_serial, total_proto_count,
 *      next_in_realm, owning_realm, heap_allocated).  Public API: 7 functions
 *      renamed (urbi_module_* → urbi_chunk_*; urbi_load_module →
 *      urbi_load_chunk; urbi_get_or_create_module_instance →
 *      urbi_get_or_create_chunk_instance).  Type renames: UModuleInstance →
 *      UChunkInstance; UModuleAllocFn → UChunkAllocFn (UAllocFn was already
 *      taken in uarena.h); UModuleLoadError → UChunkLoadError + ULOAD_* →
 *      UCHUNK_LOAD_*.  UStrand.module field deleted.  Wire format v1.7 →
 *      v1.8 (semantic bump only — byte layout unchanged).  9th use of
 *      pre-v1.0 escape clause.  (0/12/0 → 0/13/0)
 *  10. v0.9.4-pico-example — URBI_REPL_COOPERATIVE_ONLY=1 build path.
 *      Internal struct fields of UReplServer / UReplReader / UReplQueue /
 *      UReplRingbuf use urbi_mutex_t / urbi_cond_t / urbi_thread_t typedefs
 *      that resolve to pthread types (~40 bytes each on Linux x86-64) on
 *      POSIX builds and 1-byte empty stubs on cooperative-only builds.
 *      Struct layouts therefore differ between the two modes — same silent-
 *      divergence trap class as URBI_FLOAT_TYPE.  10th use of pre-v1.0
 *      escape clause.  (0/13/0 → 0/14/0)
 *  11. v0.10.2-reactive W0 — UWatcherInstallResult gains
 *      URBI_INSTALL_NO_OBSERVABLE_CELLS (value 5); uwatcher.h adds
 *      UWATCHER_WHENEVER_EVENT (7); uchunk.h adds OP_WHENEVER_EVENT_INSTALL
 *      (48) and bumps OP_MAX 48→49; wire format v1.8→v1.9
 *      (URBI_BYTECODE_VERSION_MINOR 8→9).  No struct layout change; no
 *      function signature change.  11th use of pre-v1.0 escape clause.
 *      (0/14/0 → 0/14/1)
 *  12. v0.10.3-api-opacity — Wave 4 of v0.10.x architectural refactor
 *      arc.  Combined 12th + 13th use of pre-v1.0 escape clause:
 *      W3 retires UVMError + UExecStatus from the public surface (kept
 *      as typedef int + #define shims for one release cycle); 5
 *      callback setters gain trailing void *ud (urbi_set_diag_fn,
 *      _time_us, _watcher_body_done_fn, _isr_check_fn,
 *      _register_event_drain); new UCallbackSignal enum
 *      (URBI_CB_OK/_UNREGISTER/_THROW); URBI_ERR_WATCHER_UNREGISTER
 *      retained as #define alias for URBI_CB_UNREGISTER.
 *      W5 changes 17 functions to take vm as new first arg
 *      (urbi_strand_* family + urbi_throw/_return_val/_tag_stop_local
 *      + urbi_realm_set/get_compile_budget + urbi_tag_info +
 *      urbi_chunk_from_bytes/_free + sched priority/sched_class);
 *      3 void-returning functions change to int (strand_destroy/throw/
 *      return_val).  Adds UStrandState enum + urbi_strand_state(vm, s);
 *      adds UStrandUnwind public mirror enum + URBI_ERR_INVALID_STATE
 *      (-27); routes urbi_chunk_from_bytes alloc through vm->alloc_fn.
 *      W1 adds opaque urbi_vm_create/_free/_sizeof/_alignof
 *      (urbi_vm_init/_destroy retained as URBI_ADVANCED for static
 *      embedders).  W4 adds 13 urbi_value_is_* predicates + 9
 *      urbi_aux_value_to_* checked accessors + URBI_ERR_TYPE (-26).
 *      W7 adds URBI_EXPERIMENTAL/_ADVANCED/_DEPRECATED attribute
 *      macros + docs/api-surface-tiers.md manifest + test-api-manifest
 *      CI gate.  W2 removes 4 internal-header includes from <urbi/gc.h>
 *      + <urbi/sched.h> + adds test-external-embed-iinclude CI gate.
 *      W6 rewrites docs/embedding-guide.md against post-Wave-4 surface
 *      + tightens test-embedding-guide CFLAGS to -Iinclude only.
 *      Wire format unchanged at v1.9 / 0x19 (C-API only wave).
 *      (0/14/1 → 0/15/0)
 *  13. v0.10.4-vm-decomp — Wave 5 of v0.10.x architectural refactor
 *      arc.  Behaviour-preserving decomposition of the VM monolith.
 *      W1 extracts slot-access helpers from src/vm/uvm.c (2040 →
 *      1728 lines; original ≤1100 plan target was arithmetically
 *      unachievable within W1's scope of 3 OP arms — see milestone
 *      retrospective).  LOCAL-slot discipline lives in vm_resolve_ic
 *      exclusively (closes OBJ-IC-POLY regression risk).  W2 extracts
 *      UWatcherState off UVM root (10 fields relocated; ~79 src + ~131
 *      test callsites swept).  W3 extracts UReplState + UTestHooks off
 *      UVM root (UTestHooks unconditionally allocated — URBI_DEBUG
 *      gating from plan would null-deref under test-asan).  No public-
 *      API change; no signature change; no wire format change.
 *      struct UVM size shifts (visible to embedders calling
 *      urbi_vm_sizeof() per W1 of v0.10.3-api-opacity).  14th use of
 *      pre-v1.0 escape clause.  (0/15/0 → 0/16/0)
 *  14. v0.10.5-legacy-decisions — Wave 6 of v0.10.x architectural refactor
 *      arc.  Per-construct decisions on every legacy-language compatibility
 *      gap.  11 worktrees: W6 block-comment non-nesting LOCKED (dropped);
 *      W7 CallMessage permanently dropped (migration design + PORT_NOTES
 *      refresh); W4 angle literals deg/rad/grad implemented; W2 quoted
 *      identifiers implemented; W3 assert keyword implemented (lowered to
 *      if-throw, no new opcode); W5 catch (var e [if guard]) + try-else
 *      implemented; W11 top-level this/Lobby migrated to Realm (named
 *      singleton); W8 tag-expr member-expr implemented (onleave deferred-
 *      v1.x); W10 list/dict literals + subscript + var-obj-slot all
 *      implemented via stdlib-call lowering (no opcodes); W1 for-each +
 *      break/continue + switch implemented (C-style for / loop / do-recv
 *      deferred-v1.x); W9 at(e?(var x)) + whenever(e?(var x)) payload +
 *      whenever-else + waituntil(e?) implemented (W9.2 ~duration + W9.4
 *      watch(expr) + W9.5 $wheneverOn/Off deferred-v1.x).  ABI MINOR bump
 *      for new public token + AST + parser surface.  No wire format
 *      change (no new opcodes in this wave).  docs/language-compatibility-
 *      matrix.md fully populated; v1.0 conformance denominator now
 *      computable.  15th use of pre-v1.0 escape clause.  (0/16/0 → 0/17/0)
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
#define URBI_API_VERSION_MINOR  17
#define URBI_API_VERSION_PATCH  0
#define URBI_API_VERSION_NUM    ((URBI_API_VERSION_MAJOR * 10000) \
                                + (URBI_API_VERSION_MINOR *   100) \
                                +  URBI_API_VERSION_PATCH)

/* Runtime getter. NULL-tolerant per arg. */
void urbi_api_version(int *out_major, int *out_minor, int *out_patch);

/* === W7/v0.10.3: API tier annotation macros ===
 *
 * URBI_EXPERIMENTAL — RESERVED for v1.x; do not depend on across releases.
 *                     Compiler emits a deprecation warning when used.
 *                     Suppress with -Wno-deprecated-declarations.
 *
 * URBI_ADVANCED     — Stable but non-hot-path.  Most embedders do not need
 *                     this; reach for it only when the basic API is not
 *                     sufficient.  No warning emitted; the macro is
 *                     documentation only.
 *
 * URBI_DEPRECATED   — Scheduled for removal in a future MAJOR bump.
 *                     Compiler emits a deprecation warning.  Suppress with
 *                     -Wno-deprecated-declarations.
 *
 * The macros work on GCC and Clang; on other compilers they expand to nothing
 * (no warning, no behaviour change).
 *
 * The authoritative tier manifest is docs/api-surface-tiers.md.
 * CI gate: make test-api-manifest. */
#if defined(__GNUC__) || defined(__clang__)
#  define URBI_EXPERIMENTAL  __attribute__((deprecated("RESERVED for v1.x — may change before v1.0")))
#  define URBI_ADVANCED      /* documentation only — no compiler warning */
#  define URBI_DEPRECATED    __attribute__((deprecated))
#else
#  define URBI_EXPERIMENTAL
#  define URBI_ADVANCED
#  define URBI_DEPRECATED
#endif

#ifdef __cplusplus
}
#endif

#endif /* URBI_VERSION_H */
