/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode UModule — the front-end / back-end interface.  Freestanding.
 *
 * --- Inline-cache (IC) layout post-Task-11 (v0.8.1-uproto-root) ---
 * The pair (ic_count + ic_names) appears in two places, each owned by a
 * different layer.  UModule no longer holds a copy — the root chunk is now
 * modeled as root_proto, a full UProto:
 *
 *   1. UProto.ic_count / UProto.ic_names — per-proto (both root and nested).
 *      Populated by uemit at compile time, persisted in bytecode v1.3+,
 *      freed by umodule_destroy_proto_buffers.
 *   2. UProtoInstance.ic_count + UIC entries[] — runtime IC table per
 *      (vm, proto) pair (object/umoduleinstance.h).  Sized from #1 at
 *      module-instance creation; UIC.name is copied from ic_names.
 *
 * Mirror discipline: any change to UProto IC field naming or layout must
 * be applied to all readers and to the wire-format encoder/decoder in
 * uemit.c / umodule.c. */

#ifndef UMODULE_H
#define UMODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
                ULOAD_UNSUPPORTED_VERSION.).

   Version-mismatch policy: exact-match.  Any byte other than VERSION_BYTE is
   a hard ULOAD_UNSUPPORTED_VERSION reject — there is no best-effort or
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
 * file produces a canary mismatch, returned as ULOAD_BAD_MAGIC.
 *
 * Defined as a static-const-array initializer in the header so both
 * the serializer (uemit_serialize.c) and deserializer (umodule.c)
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

/* --- tagged value shape shared between pool and runtime registers ---
 *
 * UValKind and UValue moved to <urbi/types.h> at v0.5.5 (T17) to break
 * the cycle where include/urbi/urbi.h pulled in this internal header
 * for UValue's definition.  Numeric values for UValKind are pinned by
 * the bytecode wire format; the kind-byte field comments below document
 * the runtime semantics still managed at this layer.
 *
 * Runtime-semantics notes for each UValKind discriminator:
 *   UVAL_NIL/INT/FLOAT/BOOL/STR — bytecode-pool kinds (constants)
 *   UVAL_CLOSURE — M2: function closure; runtime-only
 *   UVAL_VOID    — M2: result of `&` separator; runtime-only
 *   UVAL_STRAND  — M3: strand handle (OP_FORK_JOIN → OP_JOIN_WAIT).
 *                  Stores a UStrand* in v.p.  GC root walker skips M3
 *                  (strands are sched-managed, not GC cells).
 *                  TODO(M7+): revisit if strand handles become user-visible.
 *   UVAL_OBJECT  — M4: UObject pointer; runtime-only.  Receivers for
 *                  OP_GETSLOT/OP_SETSLOT live in registers tagged
 *                  UVAL_OBJECT.  Heap-bearing — UObject embeds UCell.
 *   UVAL_EVENT   — M5: UEvent pointer; runtime-only.  Heap-bearing.
 *                  Used by tag.enter / tag.leave getters and T53.
 *   UVAL_HOST_FN — M5: native host function slot; UHostFn cast to void*.
 *                  Used by uevent_native_register / utag_native_register.
 *                  NOT heap-bearing — function pointers are not GC cells.
 *   Kinds 0-10 in use at v0.5.5; kinds 11-15 reserved for future extension.
 *   In v0.5.5 bytecode constant pools, the loader rejects any kind >
 *   UVAL_STR (kinds 5-10 are runtime-only and never appear on disk). */
#include "urbi/types.h"

/* UUpvalCell, UCallFrame, UVM_MAX_FRAMES, UVM_STACK_CAP — placed here so
   UValue is in scope when uframe.h is processed (uframe.h uses UValue but
   cannot include umodule.h/uvalue.h to avoid a circular dependency). */
#include "runtime/uframe.h"

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

/* --- absolute-line checkpoint record --- */

typedef struct {
    uint32_t pc;
    uint32_t line;
} UAbsLine;

/* --- pluggable allocator (matches uarena pattern) --- */

typedef void *(*UModuleAllocFn)(void *ptr, size_t nbytes, void *ud);
/* Standard realloc semantics:
 *   ptr == NULL && nbytes > 0  : allocate fresh buffer; return non-NULL or NULL on OOM.
 *   ptr != NULL && nbytes == 0 : free ptr; return NULL.
 *   ptr != NULL && nbytes > 0  : reallocate ptr to nbytes (may move); return non-NULL or NULL on OOM.
 *   ptr == NULL && nbytes == 0 : no-op; return NULL.
 * ud is an opaque caller-supplied cookie passed through unchanged (same pattern as uarena). */

/* Forward declaration — USymbol is introduced in M4 (see uintern.h / object
 * model tasks).  UProto.ic_names below holds a parallel array of USymbol
 * pointers populated at emit time; populated by emit, consumed by IC fill at
 * module-instance load.  Defined as opaque here to keep umodule.h
 * dependency-free from the object/intern layer. */
struct USymbol;
typedef struct USymbol USymbol;

/* Forward declaration — UModuleInstance is introduced in M4 (see
 * object/umoduleinstance.h).  UProto.owning_module_instance (added v0.9.0)
 * holds a back-pointer to the runtime instance this proto was first
 * instantiated under.  Defined as opaque here to avoid a circular dependency
 * on object/ layer types. */
struct UModuleInstance;

/* --- UProto: nested function prototype (used for function definitions). ---
 * A UProto holds the bytecode, constants, and line info for one nested
 * function body.  The root chunk lives directly in UModule; nested
 * functions each get a heap-allocated UProto stored in UModule.nested[]. */

typedef struct UProto {
    uint32_t  *instructions;
    size_t     instr_count;
    size_t     instr_cap;

    UValue    *constants;
    size_t     const_count;
    size_t     const_cap;

    int8_t    *line_deltas;

    UAbsLine  *abs_lines;
    size_t     abs_line_count;
    size_t     abs_line_cap;

    uint8_t    max_reg;
    uint8_t    nupvals;          /* count of upvalues captured by this proto */
    uint8_t    nparams;          /* count of formal parameters */

    /* === M4 v1.3 additions (encoding spec §5.1) === */
    /* Number of GETSLOT/SETSLOT IC sites in this function.  Populated by the
     * emitter; the parallel ic_names[] array is sized to this count.  Capped
     * at 256 by the encoding spec §3.4 (an IC site index lives in a uint8). */
    uint16_t       ic_count;
    /* Parallel array, length == ic_count; set at emit time and consumed at
     * module-instance load to populate UIC.name for each IC site.  Owned by
     * the proto's allocator; freed in umodule_destroy_proto_buffers. */
    USymbol      **ic_names;
    /* Parallel string array; one entry per IC site; UTF-8, NUL-terminated.
     * Populated by the emitter (mirroring ic_names) and by the deserializer
     * (in lieu of ic_names, which stays NULL until module-instance create
     * interns the strings).  Owned by the proto's allocator; each entry and
     * the array itself are freed in umodule_destroy_proto_buffers. */
    char         **ic_name_strs;

    /* Allocator hook inherited from the owning module. */
    UModuleAllocFn alloc_fn;
    void          *alloc_ud;

    /* NEW (Phase 1 v0.8.1-uproto-root): recursive child protos.
     * For v0.8.1 tag: populated only on the root_proto (flat-on-root emitter
     * per spec §4.2).  Non-root UProtos: nested_count = 0, nested = NULL.
     * For root_proto, this holds the module's nested functions.
     * Truly-recursive emitter where Bx scopes per-enclosing-proto is
     * deferred per spec §11.3. */
    struct UProto **nested;
    size_t          nested_count;
    size_t          nested_cap;

    /* [runtime-only, NOT serialized] Intrusive list link with dual Variant B
     * semantics (spec §3.7 lifetime ordering invariant):
     *
     * (a) List link — when this proto is the root_proto of a rescued module,
     *     next_alloc threads it onto vm->rescued_protos.
     *     NULL while the proto is still owned by its originating UModule.
     *
     * (b) Self-link sentinel — set by umodule_destroy(m, NULL) (the vm=NULL
     *     defensive path) when root_proto->refcount > 0 but no vm is available
     *     to rescue immediately.  next_alloc == root_proto itself signals
     *     "destroy pending — promote to vm->rescued_protos when refcount hits 0
     *     during vm_destroy's stdlib_closures sweep".  Unambiguous because an
     *     in-module or in-list proto never points to itself.
     *
     * Zero-initialized alongside the rest of UProto at alloc time
     * (umodule_alloc_nested_proto). */
    struct UProto *next_alloc;

    /* [runtime-only, NOT serialized] Back-pointer to the root UProto of the
     * owning module.  NULL on the root proto itself; set to module->root_proto
     * on every nested proto at allocation time.  Used by Phase 2 refcount
     * bumpers to find the canonical refcount via (proto->root ?: proto).
     * Zero-initialized at alloc time; populated by uemit_finish and
     * umodule_deserialize post-pass. */
    struct UProto *root;

    /* [runtime-only, NOT serialized] Per-root-proto reference count for the
     * module-grain closure lifetime fix (v0.7.3 + v0.8.1).  Bumped at every
     * strand bind (uproto_root_of(proto)->refcount); decremented when the
     * strand or closure is released.  umodule_destroy checks this counter:
     * if 0, the root_proto is freed normally; if non-zero, it is rescued onto
     * vm->rescued_protos so surviving closures keep a valid backing proto.
     *
     * uint16_t with saturation at UINT16_MAX (logs URBI_LOG_WARN; proto leaks
     * — acceptable for the v1.0 timeframe). */
    uint16_t       refcount;

    /* [runtime-only, NOT serialized] DFS pre-order serial assigned at
     * UProto construction.  Root proto gets ic_index = 0; subsequent
     * UProto allocations get module->next_proto_serial++ via either the
     * emit path (umodule_alloc_nested_proto) or the deserialize path
     * (decode_nested_protos_into).  Used by OP_CLOSURE to bind
     * cl->proto_inst via UModuleInstance.proto_instances->entries[ic_index]
     * — a single index that works for both flat and recursive trees.
     * v0.8.5-recursive-emit. */
    uint16_t       ic_index;

    /* [runtime-only, NOT serialized] Back-pointer to the UModuleInstance
     * this UProto was first instantiated under.  Populated once at
     * urbi_module_instance_create time (tree walk over every proto).  Used
     * by OP_CLOSURE to bind cl->proto_inst without a fallback chain:
     * cl->proto_inst = &owning_module_instance->proto_instances->entries[ic_index].
     *
     * Lifetime contract: owning_module_instance is GC-managed and remains
     * valid as long as this UProto exists (the instance is kept reachable
     * via vm->module_instances_head; the module-destroy path unlinks the
     * instance from that list before the proto's refcount can hit 0).
     *
     * Zero-initialised at alloc time; populated lazily on first instance
     * creation.  NULL is the "not yet instantiated" state and is detected
     * by the OP_CLOSURE assert when read.  v0.9.0-repl. */
    struct UModuleInstance *owning_module_instance;
} UProto;

/* --- UClosure: runtime function value (proto + captured upvalues).
 * Forward declaration only — full struct definition lives in uclosure.h
 * (M4 split: UClosure embeds UCell as first member, which can't be done
 * here without a circular include via gc/ugc.h).  Files that only need
 * `UClosure *` use the typedef below; files that touch UClosure fields
 * include "uclosure.h" explicitly. */
typedef struct UClosure UClosure;

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
     * Freed by umodule_destroy_internal via umodule_destroy_proto_buffers.
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
    UModuleAllocFn alloc_fn;
    void         *alloc_ud;

    /* next_proto_serial [runtime-only, NOT serialized]: monotonic counter
     * holding the LAST ic_index assigned to a non-root UProto in this
     * module.  Bumped in umodule_alloc_nested_proto (emit path) and
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
} UModule;

/* --- errors --- */

typedef enum {
    ULOAD_OK = 0,
    ULOAD_BAD_MAGIC,              /* magic or canary mismatch */
    ULOAD_UNSUPPORTED_VERSION,
    ULOAD_FLAVOR_MISMATCH,        /* any descriptor field incl. endianness */
    ULOAD_TRUNCATED,
    ULOAD_CORRUPT_VARINT,
    ULOAD_CORRUPT_TAG,
    ULOAD_CORRUPT,                /* bad opcode / out-of-range reg / count mismatch / misaligned */
    ULOAD_OOM,
    ULOAD_INVALID_ARG,            /* NULL module / NULL buf etc.; distinct from TRUNCATED */
    ULOAD_OVERSIZED               /* count fields exceed compile-time per-proto caps */
} UModuleLoadError;

/* Per-proto cap on instruction count.  Bytecode-encoded as varint;
 * decoded into size_t.  The cap stops a malicious or corrupt module from
 * requesting an n_instr that would either overflow size_t on 32-bit
 * ports or balloon allocation past any plausible per-function budget.
 * 1 MiB instructions is well past any human-authored source. */
#define URBI_MAX_INSTRS_PER_PROTO ((size_t)(1U << 20))

/* --- Proto helpers --- */

/* uproto_root_of: returns the canonical-refcount target for proto.
 * For root protos: returns proto itself (proto->root == NULL).
 * For nested protos: returns the owning module's root_proto via back-pointer.
 * NULL-safe (returns NULL if proto is NULL).
 *
 * v0.8.1 Variant B Phase 2: all closure-related refcount inc/dec sites
 * route through this helper to ensure bumps land on root_proto.refcount
 * (the single canonical counter for the whole module grain). */
static inline UProto *
uproto_root_of(UProto *proto)
{
    if (!proto) return NULL;
    return proto->root ? proto->root : proto;
}

/* Refcount helpers — declared inline in the header so OP_CLOSURE's hot
 * path stays cheap.  See UProto.refcount above for the design. */
static inline void
umodule_proto_refcount_inc(UProto *p)
{
    if (p == NULL) return;
    if (p->refcount == UINT16_MAX) {
        /* Saturated: log once-per-proto, no further bumps.  The cell leaks
         * on the next module_destroy (transferred to stdlib_protos and
         * never freed because the count never reaches 0). */
        return;
    }
    p->refcount = (uint16_t)(p->refcount + 1U);
}

static inline void
umodule_proto_refcount_dec(UProto *p)
{
    if (p == NULL) return;
    if (p->refcount == 0U || p->refcount == UINT16_MAX) {
        /* Underflow guard + saturation: a 0 refcount on dec means somebody
         * forgot to bump (we'd corrupt the counter).  Saturation guard
         * preserves the "leak forever" contract for UINT16_MAX. */
        return;
    }
    p->refcount = (uint16_t)(p->refcount - 1U);
}

/* v0.8.1 Phase 2: strand-bind release helper.
 * Decrements root_proto->refcount and, when it reaches 0 with
 * module->destroy_requested previously-set, fires umodule_destroy_internal.
 * Callers must pass the still-valid module pointer; pass NULL for either
 * to no-op safely.  vm may be NULL in test contexts. */
void umodule_strand_refcount_dec(UModule *m, UProto *root_proto,
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
UProto *umodule_alloc_nested_proto(UModule *module, UProto *parent_proto);

/* Free a UProto's owned buffers.  Does NOT free the UProto struct itself
 * (it is owned by the module's nested[] array, or by a watcher pool slot
 * after strand_closure_unlink has detached it). */
void umodule_destroy_proto_buffers(UProto *proto, UModuleAllocFn alloc,
                                   void *alloc_ud);

/* --- API --- */

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
 *   - On success returns ULOAD_OK; `module` is fully populated.
 *   - NULL `module` or NULL `buf` returns ULOAD_INVALID_ARG (no partial
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
UModuleLoadError umodule_deserialize(UModule *module, const uint8_t *buf, size_t size,
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

/* Return a static string such as "ULOAD_BAD_MAGIC" for debug. */
const char *umodule_load_error_name(UModuleLoadError code);

#ifdef __cplusplus
}
#endif

#endif
