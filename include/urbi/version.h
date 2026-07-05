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
 *  16. v0.10.6-stabilization — Wave 7 of v0.10.x architectural refactor
 *      arc; the arc-closing wave.  W1 listener-teardown race fix
 *      (urepl_request_teardown + urepl_session_reap_pending; reader
 *      threads request, VM thread reaps under sessions_mutex).  W2
 *      ABI freeze pin (_Static_assert in version.h + docs/api-stability.md
 *      + test-abi-freeze gate).  W3 wire-format freeze pin
 *      (_Static_assert in uchunk_io.c + v1.8 → v1.9 doc-drift fix +
 *      test-wire-freeze gate).  W4 REPL security gates: 6 named tests
 *      (loopback-no-token / token-mismatch-teardown / rate-limit /
 *      compile-budget-denial / malformed-NDJSON-tolerance / output-
 *      isolation) + 5 OOM-injection tests + UReplConfig gains
 *      rate_limit_per_second int field (POSIX-only enforcement,
 *      cooperative builds have no network threat model);
 *      URBI_ERR_INVALID_CONFIG added as #define alias for the pre-
 *      existing URBI_ERR_INSECURE_CONFIG (-25), no new error slot.
 *      W5 release-readiness.md fully populated (32/32 rows resolved);
 *      coverage policy Path A enforced at --fail-under-line 85 (line
 *      87% at baseline; aspirational 90% as v1.x target); branch +
 *      condition coverage removed from v1.0 claims;
 *      test-stdlib-bytecode-fresh + test-dependency-pins gates added.
 *      W6 design-risks register triaged (8 v1.0-rc / v0.9.x / "Handle
 *      before v1.0" entries closed/downgraded/mapped to release-
 *      readiness; workspace-root, no commit trail).  Public-ABI driver
 *      is the W4 UReplConfig.rate_limit_per_second field addition —
 *      struct layout change visible across the boundary, additive at
 *      the tail so old embedders zero-init it to unlimited.  No wire
 *      format change at v1.9 / 0x19 (W3 pins the existing format).
 *      16th and FINAL use of pre-v1.0 escape clause as the symbolic
 *      ABI freeze pin; further pre-v1.0 changes follow the post-freeze
 *      policy at docs/api-stability.md §3.  (0/17/0 → 0/18/0)
 *  18. v0.10.10-job-introspection — D7 full-ship Cat. E ratification:
 *      Job proto + Job.current/jobs/tags/uid/status (call-style methods,
 *      not auto-invoked getters — wrap-native-closures-as-getters bridge
 *      defers v1.x; see workspace-root design-risks v0.10.10-A).
 *      detach/disown lazy-arg overlay wrappers + 2 C-natives; scopeTag
 *      realm global; Lobby.connectionTag slot.  All script-side surface —
 *      NO new public C API symbols.  PATCH bump only.  (0/19/0 → 0/19/1)
 *  17. v0.10.9-tag-state — MINOR bump for new public C API surface:
 *      urbi_tag_block(vm, tag, resume_value), urbi_tag_unblock(vm, tag),
 *      urbi_tag_freeze(vm, tag), urbi_tag_unfreeze(vm, tag) — 4 new
 *      symbols backing the D1 SUSPENDED-machinery ratification (real
 *      block/unblock + freeze/unfreeze cross-strand suspend via
 *      USTRAND_REASON_BLOCK + USTRAND_REASON_FREEZE).  UStrand size
 *      pin 3896 → 3912 (gains `unblock_value` UValue field for the
 *      resume-value stash; C API plumbs the value through, script-side
 *      delivery on SUSPENDED→READY defers v1.x — see workspace-root
 *      design-risks v0.10.9-C).  UTag flag UTAG_FLAG_BLOCKED (0x04) added
 *      (UTAG_FLAG_FROZEN at 0x02 was pre-existing from v0.10.2 W4).
 *      Symmetric with urbi_tag_stop family.  17th use of pre-v1.0
 *      escape clause; first post-freeze MINOR break per docs/api-
 *      stability.md §3 (the freeze pin from v0.10.6 W2 explicitly
 *      allows further MINORs with enumerated rationale — supersedes
 *      the prior "16th and FINAL" framing).  Wire format unchanged at
 *      v1.9 / 0x19.  (0/18/2 → 0/19/0)
 *  19. v0.10.11-channel-and-isA — D6 + isA + D5 Cat. E ratification:
 *      Channel proto + cout/cerr/clog + '<<' infix sugar; isA universal
 *      type-test on Object root; Object atom unfrozen (Lobby stays
 *      frozen).  Plus bundled v0.10.10 carry-overs: Makefile -MMD -MP
 *      auto-deps + test_repl_stop_path UAF fix.  All script-side
 *      surface — NO new public C API symbols.  PATCH bump only.
 *      (0/19/1 → 0/19/2)
 *  20. v0.10.12-cat-e-activation — final tag of the 4-tag Cat. E
 *      ratification arc.  Fixture-and-doc-only tag: D2 cross-spec
 *      at→event chain activate-now via 3-fixture Realm-capture rewrite
 *      (W1), at.sync 4-fixture syntax normalization to canonical `at sync`
 *      keyword form per §S-watcher-3 (W2), Cat. E doc-sweep close-out (W3).
 *      No new public C API symbols.  No new opcodes.  PATCH bump only.
 *      (0/19/2 → 0/19/3)
 *  28. v0.11.4-cat-f — catchable structured exceptions: VM-internal error
 *      sites that previously fatal-HALTed the strand (the native-method raise
 *      helpers + the 6 slot-access TypeError sites) now raise catchable typed
 *      Exception instances; cached typeerror/arityerror/lookuperror/oomerror
 *      protos resolved-by-name after the stdlib bake + added to the GC root
 *      set; the strand's catch_value added to the GC root walker; four new
 *      Exception subclasses (RuntimeError, SchedulingError, SyntaxError,
 *      OutOfMemoryError); uncaught typed throws print a "!!! <message>"
 *      diagnostic at top level.  Two internal exported symbols
 *      (urbi_raise_typed, urbi_exception_subclass_protos_resolve) already
 *      manifested in Tier 4 of docs/api-surface-tiers.md — internal-leak,
 *      not public surface.  New test-only chk host-driver directives
 *      (## host: advance-clock / expect-host-call).  PATCH bump only —
 *      zero new public C API symbols; no new opcodes; wire unchanged at
 *      v1.9 / 0x19.  (0/19/3 → 0/20/4 — MINOR was already at 20 from the
 *      v0.11.x tooling arc; PATCH increments to 4)
 * Strict policy goes live at v1.0.0.
 *
 * Holding a pointer to an opaque type is part of the ABI; reading through
 * it is not.
 */

#ifndef URBI_VERSION_H
#define URBI_VERSION_H

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility push(default)   /* v1.0: export only public-header symbols */
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define URBI_API_VERSION_MAJOR  0
#define URBI_API_VERSION_MINOR  23
#define URBI_API_VERSION_PATCH  6
#define URBI_API_VERSION_NUM    ((URBI_API_VERSION_MAJOR * 10000) \
                                + (URBI_API_VERSION_MINOR *   100) \
                                +  URBI_API_VERSION_PATCH)

/* === W2/v0.10.6: ABI freeze pin ============================================
 *
 * This is the symbolic pre-v1.0 freeze.  Any change to the public C API
 * after this tag is a "post-freeze breaking change" and must:
 *
 *   1. Be enumerated in CHANGELOG.md with an explicit "freeze override"
 *      comment referencing docs/api-stability.md §3.
 *   2. Bump the expected values in this _Static_assert.
 *   3. Bump URBI_API_VERSION_MAJOR/MINOR/PATCH per the policy in this
 *      file's leading comment.
 *
 * Failing to bump the static_assert breaks the build, which is the point —
 * deliberate intent at every change, not silent ABI drift.
 *
 * Pin target: v0.10.15-vm-decomp-2 ships at 0/19/6.
 *
 * The pin landed in commit `bdad57c` (W2 of v0.10.6) at 0/17/0; bumped to
 * 0/18/0 at v0.10.6 wave wrap-up to capture the W4 UReplConfig.rate_limit_per_second
 * field addition; bumped to 0/18/1 at v0.10.7 wave wrap-up — PATCH-only,
 * no public surface change (audit followup wave fixes latent bugs and
 * doc/gate drift without touching the C API); bumped to 0/18/2 at
 * v0.10.8-string-concat — PATCH-only, OP_ADD atom fast path adds runtime
 * String + String concatenation (S-string-plus) without touching the
 * public C API surface; bumped to 0/19/0 at v0.10.9-tag-state — MINOR,
 * 4 new public C API symbols (urbi_tag_block/_unblock/_freeze/_unfreeze)
 * backing the D1 SUSPENDED-machinery ratification.  First post-freeze
 * MINOR break per docs/api-stability.md §3; supersedes v0.10.6's
 * aspirational "16th and FINAL" framing.  Bumped to 0/19/1 at
 * v0.10.10-job-introspection — PATCH-only, D7 full-ship Cat. E
 * ratification.  All new surface (Job proto, detach/disown,
 * scopeTag, connectionTag) is script-side; zero new public C API
 * symbols.  Bumped to 0/19/2 at v0.10.11-channel-and-isA — PATCH-only,
 * D6 + isA + D5 Cat. E ratification (Channel proto, cout/cerr/clog,
 * '<<' infix, isA(), Object unfreeze) plus bundled carry-overs
 * (Makefile -MMD/-MP + REPL UAF fix).  All script-side; zero new
 * public C API symbols.  Bumped to 0/19/3 at v0.10.12-cat-e-activation —
 * PATCH-only, final tag of the 4-tag Cat. E ratification arc.  Fixture-
 * and-doc-only tag: D2 cross-spec at→event chain activate-now (W1),
 * at.sync keyword normalization (W2), Cat. E close-out (W3).  Zero new
 * public C API symbols; no functional changes.
 *
 * Bumped to 0/19/4 at v0.10.13-hygiene — escape #21.  PATCH-only post-
 * Cat. E hygiene + targeted runtime bug fix: markdownlint MD004 per-file
 * override for CHANGELOG; make all builds urbi CLI binary (closes
 * v0.10.7-H); String.asString stdlib overlay (closes v0.10.11-A);
 * slot-change first-install double-fire suppression (closes v0.10.7-C
 * — the one runtime semantic change).  Zero new public C API symbols;
 * two new chk fixtures (string_asstring, slot_change_no_install_emit);
 * one replaced unit test.
 *
 * Bumped to 0/19/5 at v0.10.14-prerc-infra — escape #22.  PATCH-only,
 * first tag of the pre-v1.0-rc stabilization arc.  Three worktrees:
 * STYLE.md subsystem-layout doc correction (W1); REPL reader-thread
 * output backpressure rework — staging buffer + POLLOUT-driven flush
 * replacing the EAGAIN spin, fixing a teardown-latency/liveness bug,
 * NOT byte loss (W2); a C chk host-driver (tests/integration/
 * chk_host_driver.c: realm/run/step directives) activating 5 T39-
 * blocked scheduler/multi-realm fixtures (W3).  All internal /
 * test-side / doc; zero new public C API symbols.
 *
 * Bumped to 0/19/6 at v0.10.15-vm-decomp-2 — escape #23.  PATCH-only, final
 * tag of the pre-v1.0-rc stabilization arc.  Internal VM dispatch extraction
 * round 2: OP_PUSH_TAG/OP_POP_TAG → src/vm/uvm_tag_scope; the seven reactive-
 * install arms → src/vm/uvm_reactive_install (behavior-preserving, per-stage
 * zero-delta gated).  Plus two tag/unwind semantic fixes on the extracted
 * seam: OP_PUSH_TAG now binds the `t:` scope to the user tag in R[tag_reg]
 * (closes v0.10.9-B), and tag.stop() inside try/finally runs the finally during
 * the TAG_STOP unwind (closes v0.10.7-B, latent-fixed by the binding).  New
 * cleanup-entry flag FLAG_TAG_USER_OWNED is internal (src/runtime/ucleanup.h),
 * not public.  Zero new public C API symbols; no new opcodes; wire unchanged
 * at v1.9 / 0x19.
 *
 * Bumped to 0/20/0 at v0.11.0-trace-spine — escape #24.  MINOR bump: new
 * public (EXPERIMENTAL) header include/urbi/trace.h adds the trace
 * control/drain/stats API (urbi_trace_set_level / _get_level / _set_level_all /
 * _snapshot / _stats / _channel_name) plus the URBI_TP tracepoint macro family.
 * The subsystem is compile-gated by URBI_TRACE (default off ⇒ zero code, zero
 * UVM delta); the control API has no-op stubs in the off build so embedder code
 * links either way.  First tag of the v0.11.x tooling arc.  Wire unchanged at
 * v1.9 / 0x19; no new opcodes.
 *
 * Bumped to 0/20/1 at v0.11.1-perf-counters — escape #25.  PATCH-only, second
 * tag of the v0.11.x tooling arc.  Adds VM-domain performance counters: a
 * URBI_PERF_COUNTERS compile gate (default off ⇒ URBI_PERF_INC is (void)0,
 * gated UPerfCounters field absent from UVM), always-on GC cycle/slice counts +
 * cycle timing, and the deliberately-stubbed Debug.profile() seam filled
 * (counters + epoch) plus Debug.gc() timing fields and a Debug.profileReset()
 * method.  All counters are EXCLUDED from urbi_get_determinism_checksum.
 * Counters are internal (src/runtime/uperf.h); surfaced only via Debug.*
 * script methods.  Zero new public C API symbols; wire unchanged at
 * v1.9 / 0x19; no new opcodes.
 *
 * Bumped to 0/20/2 at v0.11.2-host-tooling — escape #26.  PATCH-only, third
 * tag of the v0.11.x tooling arc.  Host-side tooling only: a Python
 * Perfetto/Chrome-Trace decoder (tools/urbi-trace-decode.py), GDB
 * pretty-printers + walkers (tools/gdb/urbi.py), a --trace/--trace-out capture
 * path on the urbi CLI (built via `make urbi-trace` with -DURBI_TRACE=1), and a
 * --dump-on-fatal best-effort host dump.  The CLI flags use only already-
 * exported public symbols (urbi_trace_set_level/_set_level_all/_snapshot/_stats/
 * _channel_name + urbi_repl_eval/urbi_realm_global); the decoder and GDB scripts
 * are host artifacts.  Also corrected the long-standing UTraceRecord "24-byte"
 * comment drift to its real 32-byte size (comment-only).  Zero new public C API
 * symbols; wire unchanged at v1.9 / 0x19; no new opcodes.
 *
 * Bumped to 0/20/3 at v0.11.3-memory-debug — escape #27.  PATCH-only, fourth
 * and final tag of the v0.11.x tooling arc.  On-target memory debugging behind
 * the new URBI_MEM_DEBUG compile gate (default off ⇒ zero bytes, byte-identical
 * UAllCellsNode + UVM): allocation owner-tagging on the existing UAllCellsNode
 * sidecar, trailing redzones, poison-on-free + freed-cell quarantine (UAF
 * detection), heap-lock violation recording, and host-handle/pin leak reporting.
 * Surfaced GDB-first (urbi-heap full cell walk + urbi-allocs + urbi-leaks in
 * tools/gdb/urbi.py) plus one Debug.memCheck() script trigger.  All new
 * functions are internal (umemdbg_* / urbi_gc_mem_validate / urbi_gc_count_pinned);
 * zero new public C API symbols.  The memdbg state is excluded from
 * urbi_get_determinism_checksum (proven by test-determinism-memdebug).  Wire
 * unchanged at v1.9 / 0x19; no new opcodes.
 *
 * Bumped to 0/20/4 at v0.11.4-cat-f — escape #28.  PATCH-only.  Catchable
 * structured exceptions: VM-internal error sites that previously fatal-HALTed
 * the strand or printed diagnostic strings to stderr now raise catchable typed
 * Exception instances (the urbi_raise_type/_arity/_oom/_lookup native-method
 * helpers + the 6 slot-access TypeError sites); cached
 * typeerror/arityerror/lookuperror/oomerror protos resolved-by-name after the
 * stdlib bake + added to the GC root set; the strand catch_value added to the
 * GC root walker; four new Exception subclasses (RuntimeError, SchedulingError,
 * SyntaxError, OutOfMemoryError); uncaught typed throws print "!!! <message>"
 * at top level.  The structured-throw work added two internal exported symbols
 * (urbi_raise_typed, urbi_exception_subclass_protos_resolve) already manifested
 * in Tier 4 of docs/api-surface-tiers.md — internal-leak, not public surface,
 * so this is a PATCH bump and NOT a §3 freeze override.  Plus two test-only chk
 * host-driver directives (## host: advance-clock / expect-host-call).  Zero new
 * public C API symbols; no new opcodes; wire unchanged at v1.9 / 0x19.
 *
 * Bumped to 0/21/0 at v0.12.0-ros-foundation — escape #29.  MINOR bump for
 * 3 new public C API symbols in the optional ROS2 bridge (include/urbi/ros.h,
 * gated behind URBI_ENABLE_ROS2): urbi_ros_register, urbi_ros_register_globals,
 * urbi_ros_pump.  Per the v0.11.0-trace-spine precedent (new public symbols
 * behind a compile gate => MINOR escape).  The bridge itself is a self-
 * contained optional component; the core VM has zero reference to it.  Wire
 * format unchanged at v1.9 / 0x19 (no new opcodes).  29th use of pre-v1.0
 * escape clause.  See docs/api-stability.md §3 + §6 escape ledger entry #29.
 *
 * Bumped to 0/21/1 at v0.12.1-ros-dds — escape #30.  PATCH-only: NO new public
 * C API symbols.  The real rcl/rclc/Fast-DDS transport backend (behind
 * URBI_ROS_BACKEND=rcl), the rosidl-targeting codegen, the internal List
 * C-builder (src/value/ulist_build.h), and the object-based URosTransport seam
 * are all INTERNAL — no change to include/urbi/ros.h or any other public
 * header.  Wire format unchanged at v1.9 / 0x19.  30th use of the pre-v1.0
 * escape clause.  See docs/api-stability.md §6 escape ledger entry #30.
 *
 * Bumped to 0/22/0 at v0.12.2-urobotics — escape #31 (MINOR).  Two new public
 * C API symbols in the new gated header include/urbi/urobotics.h:
 * urbi_urobotics_register + urbi_urobotics_run.  Both exist only in
 * URBI_ENABLE_UROBOTICS builds (absent from the default gate-off liburbi.a) and
 * are called only from stdlib boot + realm-globals population — but per the
 * v0.11.0-trace-spine / v0.12.0-ros-foundation precedent, new public symbols
 * behind a compile gate count as MINOR.  The Standard Robotics API facet
 * overlay itself is pure urbiscript baked into a separate bytecode blob
 * (urbi_urobotics_bytecode); the core VM has zero reference to it.  Wire format
 * unchanged at v1.9 / 0x19; no new opcodes.  31st use of the pre-v1.0 escape
 * clause.  See docs/api-stability.md §6 escape ledger entry #31.
 *
 * Bumped to 0/22/1 at v0.12.3-ros-demo-and-contract — escape #32.  PATCH-only:
 * NO new public C API symbols.  The facet<->ROS2 binding contract
 * (Robotics.bindInput / bindOutput) is pure urbiscript in the urobotics overlay
 * blob; the two new natives (ros.__injectMsg / __lastPublished) are mock-only,
 * gated test hooks inside the ROS2 component — not in include/urbi/, absent from
 * the default gate-off build + the public manifest.  Wire format unchanged at
 * v1.9 / 0x19; no new opcodes.  32nd use of the pre-v1.0 escape clause.  See
 * docs/api-stability.md §6 escape ledger entry #32.
 *
 * Bumped to 0/23/0 at v1.0.0 / M10 (B6a) — escape #33 (MINOR), the FINAL use of
 * the pre-v1.0 escape clause before the 1/0/0 freeze.  Two freeze-window ABI
 * changes: (1) renamed urbi_set_time_us -> urbi_set_clock_fn (the _fn convention;
 * it installs a urbi_time_us_fn callback), a remove+add; (2) the library is now
 * compiled -fvisibility=hidden with the public headers wrapped in
 * `#pragma GCC visibility push(default)`, so the ~423 internal cross-TU symbols
 * (Tier 4) no longer escape when liburbi.a is linked into a shared object — only
 * the documented urbi_* surface exports.  No wire change (v1.9 / 0x19).  The
 * NEXT bump is the 1/0/0 freeze (B6), after which the escape clause is retired.
 * See docs/api-stability.md §6 escape ledger entry #33.
 */
/*
 * v0.13.4-error-surfacing — PATCH bump 0/23/4 → 0/23/5 (NOT an escape).
 * New URBI_ERR_UNCAUGHT_THROW = -18 enumerator: `urbi_vm_run`,
 * `urbi_run_chunk`, and the CLI `-e`/file entry points now return this
 * code when a script throw is uncaught; the interactive REPL's
 * nil-recovery path is unchanged.  Tag 5 of the v0.13.x pre-release
 * hardening arc.  Wire format unchanged at v1.9 / 0x19.  No new public
 * C functions.  See docs/api-stability.md post-escape note.
 *
 * v0.13.5-conformance-and-stdlib — PATCH bump 0/23/5 → 0/23/6 (NOT an
 * escape).  Tag 6 of the v0.13.x pre-release hardening arc.  Legacy-
 * conformance pass: truthiness table aligned to legacy (nil/void/false
 * falsy; zero truthy); statement-operand fold for `;` / `,` / `|`;
 * unbraced single-statement bodies accepted; `switch` gains a mandatory
 * `default:` arm; default parameters; universal `asString` fallback;
 * `String.format` `%s`/`%d`/`%f`/`%i`/`%g` formatting; compat aliases
 * (`println`/`echo`/`display`); `List.sort(comparator)`; typed
 * exceptions from div-by-zero; RegExp budget cap; emit diagnostics with
 * source positions; message-text polish; tag-watcher persistence
 * (v0.13.4-A closed); REPL line-cap hardening.  No new public C API
 * symbols; no new opcodes; wire format unchanged at v1.9 / 0x19.  Not a
 * pre-v1.0 escape.
 */
_Static_assert(URBI_API_VERSION_MAJOR == 0
            && URBI_API_VERSION_MINOR == 23
            && URBI_API_VERSION_PATCH == 6,
    "ABI freeze pin: see docs/api-stability.md §3 before bumping");

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


#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif
#endif /* URBI_VERSION_H */
