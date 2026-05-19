/* SPDX-License-Identifier: BSD-3-Clause */
/* UChunk (UModule) — bytecode loader front-end / back-end interface.
 * Freestanding.  Includes uproto.h for UProto + per-proto helpers. */

#ifndef UCHUNK_H
#define UCHUNK_H

#include "uproto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- bytecode format version (loader rejects anything other than VERSION_BYTE) ---
   Encoding: VERSION_BYTE = (major << 4) | minor.  Hard breaks require a minor bump.
   v1.0 = 0x10 (M1), v1.1 = 0x11 (M2), v1.2 = 0x12 (M3 — control transfer),
   v1.3 = 0x13 (M4 — UProto.ic_count + UProto.ic_names side table),
   v1.4 = 0x14 (M5 — reactive opcodes 39-46, gc_byte bit 7, 4 new AST node kinds),
   v1.5 = 0x15 (v0.5.6 Wave 4 — wire-format completion: nested protos + per-proto
                + root ic_name_strs, header reserved bytes 16-23 strictly zero,
                opcode-shape table verifier, OP_INVOKE retired, M5 reactive
                opcodes renumbered 39-46 -> 38-45).
   v1.6 = 0x16 (v0.7.2 S42 — method-call ABI cleanup: new OP_SELF (47) loads
                method + receiver into adjacent registers; OP_CALL gains a
                method-flag bit (C & 0x80) so the receiver is read from
                R[A+1] explicitly instead of the now-deleted vm->last_recv
                global.  Eliminates the silent-elision bug where intervening
                OP_GETSLOTs in argument evaluation clobbered last_recv before
                the outer OP_CALL.  OP_MAX = 48.).
   v1.7 = 0x17 (v0.8.1-uproto-root Phase 3 — UModule body shrinks to header
                + source_name + recursive root_proto block.  Per-field
                duplication of chunk-top fields removed from the UModule
                wire section; root_proto is now serialized as a standard
                UProto block (max_reg, nupvals, nparams, constants,
                instructions, synclines, ic_names, nested_count, nested[]).
                Non-root UProtos write nested_count = 0 (flat-on-root
                emitter per spec §4.2).  v1.6 rejected as
                UCHUNK_LOAD_UNSUPPORTED_VERSION.).

   Version-mismatch policy: exact-match.  Any byte other than VERSION_BYTE is
   a hard UCHUNK_LOAD_UNSUPPORTED_VERSION reject — there is no best-effort or
   forward/backward compatibility.  Older modules silently loading would
   produce unknown opcodes, misread GC state, or wrongly-sized IC tables.
   Re-emit from source to migrate. */

#define URBI_BYTECODE_VERSION_MAJOR  1U
#define URBI_BYTECODE_VERSION_MINOR  7U
#define URBI_BYTECODE_VERSION_BYTE   ((URBI_BYTECODE_VERSION_MAJOR << 4U) | URBI_BYTECODE_VERSION_MINOR)

/* --- Header canary bytes (offsets 6-11) ---
 *
 * The 6-byte sequence detects FTP/Windows-paste corruption on transfer.
 * `\x19\x93` is binary noise; `\r\n` is munged to `\n` by FTP ASCII
 * mode; `\x1A\n` is the DOS EOF + LF.  Any text-mode mangling of the
 * file produces a canary mismatch, returned as UCHUNK_LOAD_BAD_MAGIC.
 *
 * Defined as a static-const-array initializer in the header so both
 * the serializer (uemit_serialize.c) and deserializer (uchunk_io.c)
 * consume the same constant rather than duplicating the byte sequence. */
#define URBI_BYTECODE_CANARY_LEN 6U
static const uint8_t URBI_BYTECODE_CANARY[URBI_BYTECODE_CANARY_LEN] = {
    0x19U, 0x93U, '\r', '\n', 0x1AU, '\n'
};

/* --- bytecode flavor knobs (compile-time-pinned to host or cross target) --- */

#ifndef URBI_INT_WIDTH
#define URBI_INT_WIDTH 8          /* i64 on every v1 target */
#endif

#ifndef URBI_FLOAT_TYPE
#define URBI_FLOAT_TYPE 8         /* 8 = f64, 4 = f32; overridden per target at M7 */
#endif

#ifndef URBI_INSTR_WIDTH
#define URBI_INSTR_WIDTH 4        /* uint32 always */
#endif

#ifndef URBI_ENDIANNESS
#define URBI_ENDIANNESS 0         /* 0 = little, 1 = big; v1 ships little-only */
#endif

/* --- opcode set (M1 reserves slots 0-7; 8-255 reserved for M2+) --- */

typedef enum {
    OP_LOADK = 0,                 /* ABx:  R[A] := K[Bx]                 */
    OP_MOVE  = 1,                 /* ABC:  R[A] := R[B]                  */
    OP_ADD   = 2,                 /* ABC:  R[A] := R[B] + R[C]           */
    OP_SUB   = 3,                 /* ABC:  R[A] := R[B] - R[C]           */
    OP_MUL   = 4,                 /* ABC:  R[A] := R[B] * R[C]           */
    OP_DIV   = 5,                 /* ABC:  R[A] := R[B] / R[C]           */
    OP_NEG   = 6,                 /* ABC:  R[A] := -R[B]    (C=0)        */
    OP_RET   = 7,                 /* ABC:  return R[A]      (B=C=0)      */

    /* --- M2 additions (v1.1 bytecode) --- */
    OP_LOADNIL  = 8,              /* ABC:  R[A] := nil                       */
    OP_LOADBOOL = 9,              /* ABC:  R[A] := (B != 0); if C, pc++      */
    OP_LOADVOID = 10,             /* ABC:  R[A] := void   (& separator)      */
    OP_GETUPVAL = 11,             /* ABC:  R[A] := upvalue[B]                */
    OP_SETUPVAL = 12,             /* ABC:  upvalue[B] := R[A]                */
    OP_CLOSURE  = 13,             /* ABx:  R[A] := closure(proto[Bx]) +
                                     reads NUP "pseudo-instructions" of
                                     upvalue descriptors immediately
                                     following (Lua-5.5 prelude pattern) */
    OP_CLOSE    = 14,             /* ABC:  close upvalues for R >= R[A]      */
    OP_CALL     = 15,             /* ABC:  R[A], ..., R[A+(C&0x7F)-2] :=
                                     R[A](R[A+1], ..., R[A+B-1]).
                                     B = nargs + 1 (plain) or
                                         nargs + 2 (method: callee + self + args).
                                     C low 7 bits = nresults + 1.
                                     C bit 7 (0x80) = method-call flag.  When
                                         set, R[A+1] holds the receiver (placed
                                         by a preceding OP_SELF) and explicit
                                         args start at R[A+2]; the receiver is
                                         passed as `self` to native_fn and
                                         saved into the new bytecode frame's
                                         .recv field.  When clear, args start
                                         at R[A+1] and self is nil. */
    OP_JMP      = 16,             /* ABx:  pc += signed(Bx) - 32768          */
    OP_TEST     = 17,             /* ABC:  if (truthy(R[A]) == C) pc++       */
    OP_TESTSET  = 18,             /* ABC:  if (truthy(R[B]) == C) pc++
                                     else R[A] := R[B]                       */
    OP_EQ       = 19,             /* ABC:  if ((R[B]==R[C]) != A) pc++       */
    OP_NEQ      = 20,             /* ABC:  if ((R[B]!=R[C]) != A) pc++       */
    OP_LT       = 21,             /* ABC:  if ((R[B]<R[C])  != A) pc++       */
    OP_LE       = 22,             /* ABC:  if ((R[B]<=R[C]) != A) pc++       */
    OP_YIELD    = 23,             /* ABC:  yield to scheduler (no-op M2)     */

    /* --- Reserved (emit-time error EMIT_UNSUPPORTED_AST at M2) --- */
    OP_FORK_DETACH = 24,          /* M3 — `,` separator runtime              */
    OP_FORK_JOIN   = 25,          /* M3 — `&` separator runtime              */
    OP_JOIN_WAIT   = 26,          /* M3 — `&` join-point                     */
    OP_GETSLOT     = 27,          /* M4 — slot read with IC                  */
    OP_SETSLOT     = 28,          /* M4 — slot write with IC                 */

    /* === M3 row 7 control-transfer opcodes (v1.2 hard break per T1) ===
     *
     * Encoding layout:
     *   OP_THROW          ABx:  A = reg_value  (Bx unused / 0)
     *   OP_TAG_STOP       ABC:  A = reg_tag, B = reg_value, C = 0
     *   OP_TRY_BEGIN      ABx:  A = flags byte, Bx = handler PC (0-65535)
     *   OP_TRY_END        ABC:  A = B = C = 0 (no operands)
     *   OP_PUSH_TAG       ABx:  A[7:4]=flags nibble, A[3:0]=tag_reg nibble,
     *                           Bx = onleave PC (0-65535).
     *                           tag_reg is limited to [0,15]; flags to [0,15].
     *                           T30 revisits if wider range needed.
     *   OP_POP_TAG        ABC:  A = reg_tag, B = C = 0
     *   OP_PUSH_FRAME_GUARD ABC: A = register_base, B = register_count, C = 0
     *   OP_RESUME         ABC:  A = reg_state, B = C = 0
     */
    OP_THROW            = 29,   /* A:    R[A] is the throw value             */
    OP_TAG_STOP         = 30,   /* A B:  R[A] tag, R[B] value               */
    OP_TRY_BEGIN        = 31,   /* A Bx: A=flags, Bx=handler PC             */
    OP_TRY_END          = 32,   /* —:    pop top cleanup entry               */
    OP_PUSH_TAG         = 33,   /* A Bx: A[7:4]=flags, A[3:0]=tag_reg;
                                          Bx=onleave PC                      */
    OP_POP_TAG          = 34,   /* A:    A=tag_reg                           */
    OP_PUSH_FRAME_GUARD = 35,   /* A B:  register_base, register_count       */
    OP_RESUME           = 36,   /* A:    restore unwind state from R[A]      */

    /* === M3 T10 empirical addition — needed for catch-binding ===
     * OP_LOAD_CATCH_VALUE ABC: A = destination register, B = C = 0.
     * Copies s->catch_value (written by the unwind walker on catch absorption)
     * into R[A].  Emitted as the first instruction of every catch handler body
     * so that the catch variable `e` receives the thrown value. */
    OP_LOAD_CATCH_VALUE = 37,   /* A:    R[A] := s->catch_value             */

    /* Slot 38 was OP_INVOKE (M4 reserve for collapsed GETSLOT+CALL).
     * Retired at v0.5.6 T16; the gap was collapsed at v0.5.6 T17 by
     * renumbering M5 reactive opcodes 39-46 down to 38-45.  Opcode space
     * was contiguous 0-45 (OP_MAX = 46) before v0.6.2 Phase 2 added
     * OP_LOAD_RECV at slot 46 (OP_MAX = 47). */

    /* M5 reactive runtime — pre-M5 spec #2 (at/whenever/waituntil) */
    OP_AT_INSTALL              = 38,  /* ABC: cond_reg, body_reg, onleave_or_FF  */
    OP_AT_SYNC_INSTALL         = 39,  /* ABC: same shape as OP_AT_INSTALL        */
    OP_WHENEVER_INSTALL        = 40,  /* ABC: cond_reg, body_reg, onleave_or_FF  */
    OP_WAITUNTIL_INSTALL       = 41,  /* ABC: cond_reg, 0, 0                     */

    /* M5 reactive runtime — pre-M5 spec #3 (event syncEmit + tag.enter/leave) */
    OP_AT_EVENT_INSTALL        = 42,  /* ABC: event_reg, body_reg, onleave_or_FF */
    OP_AT_EVENT_SYNC_INSTALL   = 43,  /* ABC: same shape as OP_AT_EVENT_INSTALL  */

    /* M5 reactive runtime — pre-M5 spec #4 (slot-change events) */
    OP_GETSLOT_CHANGE_EVENT    = 44,  /* ABC: dst_reg, recv_reg, name_sym_id     */

    /* M5 reactive runtime — pre-M5 spec #5 (globals exposure) */
    OP_LOAD_REALM_GLOBAL       = 45,  /* A: dst_reg; B,C reserved (sym_id wire
                                         extension deferred — needs concrete
                                         realm symbol-table layout, see
                                         backlog) */

    /* v0.6.2 Phase 2 — `this` keyword (Gap #3) */
    OP_LOAD_RECV               = 46,  /* A: dst_reg; loads the receiver stored
                                         in the current call frame's .recv field
                                         (set at OP_CALL dispatch from R[A+1]
                                         when OP_CALL's C carries the method
                                         flag).  Emitted for AST_THIS inside a
                                         method body. */

    /* v0.7.2 S42 — method-call ABI cleanup (wire v1.6) */
    OP_SELF                    = 47,  /* ABC: A=dst_reg, B=recv_reg, C=ic_index.
                                         R[A+1] := R[B]; R[A] := lookup_slot(
                                         R[B], K[ic_index]).  Atom receivers
                                         (UVAL_INT/FLOAT/STR/BOOL) routed via
                                         urbi_atom_proto_for_value identically
                                         to OP_GETSLOT.  Emitted as the prelude
                                         to a method-flagged OP_CALL so the VM
                                         can read self from R[A+1] without
                                         relying on the deprecated
                                         vm->last_recv side channel. */

    OP_MAX
} UOpcode;

/* --- instruction decode helpers (static inline; byte-aligned fields) --- */

static inline UOpcode  uinstr_op (uint32_t i) { return (UOpcode)(i & 0xFFU); }
static inline uint8_t  uinstr_a  (uint32_t i) { return (uint8_t)((i >> 8)  & 0xFFU); }
static inline uint8_t  uinstr_b  (uint32_t i) { return (uint8_t)((i >> 16) & 0xFFU); }
static inline uint8_t  uinstr_c  (uint32_t i) { return (uint8_t)((i >> 24) & 0xFFU); }
static inline uint16_t uinstr_bx (uint32_t i) { return (uint16_t)((i >> 16) & 0xFFFFU); }

static inline uint32_t uinstr_enc_abc (UOpcode op, uint8_t a, uint8_t b, uint8_t c) {
    return (uint32_t)op
         | ((uint32_t)a << 8)
         | ((uint32_t)b << 16)
         | ((uint32_t)c << 24);
}
static inline uint32_t uinstr_enc_abx (UOpcode op, uint8_t a, uint16_t bx) {
    return (uint32_t)op
         | ((uint32_t)a << 8)
         | ((uint32_t)bx << 16);
}

/* Forward declaration — URealm is introduced in M8 (see runtime/urealm.h).
 * UModule.owning_realm (added v0.9.0) and UModule.next_in_realm thread
 * modules onto the realm's loaded_protos_head list.  Defined as opaque
 * to avoid circular dependency. */
struct URealm;

/* --- UModule struct ---
 *
 * Field-ownership convention: fields above the SERIALIZED/RUNTIME divider are
 * persisted to bytecode on emit and re-populated by umodule_deserialize.
 * Fields below the divider are runtime/transient — set by the emitter or
 * loader caller, never written to disk.  Emit/deserialize do not touch
 * runtime fields; they are the caller's responsibility to initialize. */

typedef struct UModule {
    /* Task 11 (v0.8.1-uproto-root): UModule is now a thin loader shell.
     * All chunk-top data (instructions, constants, nested[], IC tables,
     * line-info, max_reg, nupvals, nparams) lives on root_proto.
     * UModule retains only: root_proto, source_name, origin_vm, alloc_fn,
     * alloc_ud.  refcount + destroy_requested deleted; root_proto->refcount
     * is the canonical module-grain reference counter. */

    /* root_proto: the root UProto for this module.
     * Allocated by uemit_init (at compile time) or umodule_deserialize (at
     * load time).  Owns instructions, constants, nested[], IC tables, etc.
     * Freed by umodule_destroy_internal via uproto_destroy_buffers.
     * NULL only if uemit_init / umodule_deserialize failed (OOM). */
    struct UProto *root_proto;

    /* source_name: allocator-owned NUL-terminated string; NULL if absent.
     * Stays on the module shell (not owned by root_proto). */
    char       *source_name;

    /* origin_vm [runtime-only]: set by uemit_init at compile time; NULL on
     * freshly-deserialized modules.  Never persisted. */
    struct UVM *origin_vm;

    /* alloc_fn / alloc_ud [runtime-only]: pluggable allocator hook for owned
     * buffers.  Caller sets these BEFORE umodule_deserialize / uemit_init.
     * NULL alloc_fn → stdlib realloc (hosted builds only).  Never persisted;
     * loader/emitter use them to grow + free struct-internal buffers. */
    UChunkAllocFn alloc_fn;
    void         *alloc_ud;

    /* next_proto_serial [runtime-only, NOT serialized]: monotonic counter
     * holding the LAST ic_index assigned to a non-root UProto in this
     * module.  Bumped in uproto_alloc_nested (emit path) and
     * decode_nested_protos_into (deserialize path).  Both paths walk the
     * tree in DFS pre-order so serial assignment is identical regardless
     * of load source.  Root proto's ic_index = 0 is set explicitly at
     * root-proto allocation; the first nested allocation produces
     * ic_index = 1.  v0.8.5-recursive-emit. */
    uint16_t       next_proto_serial;

    /* total_proto_count [runtime-only, NOT serialized]: equals
     * next_proto_serial + 1 (i.e., includes the root).  Stamped at
     * uemit_finish (emit path) and at successful umodule_deserialize
     * (load path).  Used by urbi_get_or_create_module_instance to size
     * proto_instances->entries[] under recursive nesting.
     * v0.8.5-recursive-emit. */
    uint16_t       total_proto_count;

    /* [runtime-only, NOT serialized] Realm-lifecycle linkage.  A UModule is
     * threaded onto its owning_realm's loaded_protos_head list at every
     * urbi_run_chunk / urbi_repl_eval / urbi_load_module entry.  Cleared
     * by urbi_unload when the module leaves a realm.  v0.9.0-repl. */
    struct UModule *next_in_realm;
    struct URealm  *owning_realm;

    /* [runtime-only, NOT serialized] True when the UModule shell itself was
     * heap-allocated by the runtime (e.g. via urbi_repl_eval or
     * urbi_module_from_bytes).  When set, urbi_unload frees the shell via
     * vm->alloc_fn after umodule_destroy.  Caller-allocated (stack or static)
     * modules leave this false; their shell is freed by the caller.
     * Fits in natural padding after total_proto_count on 64-bit hosts.
     * v0.9.0-repl (CHSTR-027). */
    bool           shell_heap_allocated;
} UModule;

/* --- errors --- */

typedef enum {
    UCHUNK_LOAD_OK = 0,
    UCHUNK_LOAD_BAD_MAGIC,              /* magic or canary mismatch */
    UCHUNK_LOAD_UNSUPPORTED_VERSION,
    UCHUNK_LOAD_FLAVOR_MISMATCH,        /* any descriptor field incl. endianness */
    UCHUNK_LOAD_TRUNCATED,
    UCHUNK_LOAD_CORRUPT_VARINT,
    UCHUNK_LOAD_CORRUPT_TAG,
    UCHUNK_LOAD_CORRUPT,                /* bad opcode / out-of-range reg / count mismatch / misaligned */
    UCHUNK_LOAD_OOM,
    UCHUNK_LOAD_INVALID_ARG,            /* NULL module / NULL buf etc.; distinct from TRUNCATED */
    UCHUNK_LOAD_OVERSIZED               /* count fields exceed compile-time per-proto caps */
} UChunkLoadError;

/* Per-proto cap on instruction count.  Bytecode-encoded as varint;
 * decoded into size_t.  The cap stops a malicious or corrupt module from
 * requesting an n_instr that would either overflow size_t on 32-bit
 * ports or balloon allocation past any plausible per-function budget.
 * 1 MiB instructions is well past any human-authored source. */
#define URBI_MAX_INSTRS_PER_PROTO ((size_t)(1U << 20))

/* --- API --- */

/* v0.8.1 Phase 2: strand-bind release helper.
 * Decrements root_proto->refcount and, when it reaches 0 with
 * module->destroy_requested previously-set, fires umodule_destroy_internal.
 * Callers must pass the still-valid module pointer; pass NULL for either
 * to no-op safely.  vm may be NULL in test contexts. */
void uproto_strand_refcount_dec(UModule *m, UProto *root_proto,
                                 struct UVM *vm);

/* Allocate a new UProto as root_proto->nested[nested_count++].
 * Returns pointer to the new proto on success, NULL on OOM.
 * The proto is zero-initialized; alloc_fn/alloc_ud are copied from module.
 *
 * Watcher-detach interaction: condition/body/onleave protos for installed
 * at/whenever/waituntil watchers are created here, then later detached from
 * root_proto->nested[] by strand_closure_unlink.
 * After detach, the corresponding nested[k] slot becomes NULL and ownership
 * transfers to the watcher (freed via pool_free on watcher recycle).
 * umodule_destroy is robust to NULL slots in nested[].  See also MOD-015. */
/* v0.8.5: parent_proto explicitly selects the nested[] array to grow
 * (module->root_proto for top-level function literals, the enclosing
 * UProto for nested function literals).  Each call increments
 * module->next_proto_serial and assigns the new proto's ic_index from
 * the post-increment value, matching DFS pre-order. */
UProto *uproto_alloc_nested(UModule *module, UProto *parent_proto);

/* Free a UProto's owned buffers.  Does NOT free the UProto struct itself
 * (it is owned by the module's nested[] array, or by a watcher pool slot
 * after strand_closure_unlink has detached it). */
void uproto_destroy_buffers(UProto *proto, UChunkAllocFn alloc,
                                   void *alloc_ud);

/* Populate `module` from `buf`.  `module` MUST be zero-initialized before
 * call.  If `module->alloc_fn` is NULL on entry, the stdlib `realloc` is
 * used (hosted builds only); freestanding callers MUST set `alloc_fn`
 * before calling.
 *
 * `errmsg` / `errcap` receive a human-readable diagnostic on failure.
 * Pass `(NULL, 0)` to suppress.  A non-NULL `errmsg` with `errcap == 0`
 * is silently treated as suppression.
 *
 * Error semantics:
 *   - On success returns UCHUNK_LOAD_OK; `module` is fully populated.
 *   - NULL `module` or NULL `buf` returns UCHUNK_LOAD_INVALID_ARG (no partial
 *     state — there is no module to populate).
 *   - On any other failure returns a non-OK code; `module` may hold
 *     PARTIAL buffers from the section that completed before the
 *     failure.  `umodule_destroy(module)` is safe in EITHER case and
 *     is the correct cleanup path even after a failed deserialize.
 *
 * Coverage at v1.7:
 *   - Header (24 bytes), source_name, root_proto block (recursive UProto:
 *     max_reg, nupvals, nparams, constants, instructions, synclines,
 *     ic_name_strs, nested_count, nested[]).
 *   - Verifier walks every instruction against the opcode-shape table
 *     (urbi_opcode_shapes[]); register operands < max_reg+1, Bx fields
 *     range-checked per UBxKind, last instruction must be OP_RET.
 *   - ic_names interning is deferred to urbi_module_instance_create
 *     (see object/umoduleinstance.h); deserialize itself does not need
 *     a VM. */
UChunkLoadError umodule_deserialize(UModule *module, const uint8_t *buf, size_t size,
                                   char *errmsg, size_t errcap);

/* umodule_destroy — release all owned buffers.  If vm is non-NULL and
 * root_proto->refcount > 0, the root_proto is rescued onto vm->rescued_protos
 * (surviving closures still reference it); the module shell is freed.
 *
 * vm-NULL contract (caller must guarantee):
 *   - The module has either never been run, OR
 *   - Every UClosure that pointed at any proto in this module has been freed
 *     BEFORE this call.
 *
 * Live-vm callsites should always pass the vm pointer; reserve NULL for
 * failed-compile cleanup where the module was never bound to any vm. */
void umodule_destroy(UModule *module, struct UVM *vm);

/* Return a static string such as "UCHUNK_LOAD_BAD_MAGIC" for debug. */
const char *umodule_load_error_name(UChunkLoadError code);

#ifdef __cplusplus
}
#endif

#endif /* UCHUNK_H */
