/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode UModule — the front-end / back-end interface.  Freestanding.
 *
 * --- Inline-cache (IC) mirror layout ---
 * The pair (ic_count + ic_names) appears in three places, each owned by a
 * different layer.  All three are kept in sync because populating the per-VM
 * runtime IC tables (UProtoInstance) needs the names from the proto, and the
 * proto needs to be encoded into the bytecode wire format that the loader
 * decodes back into UProto / UModule:
 *
 *   1. UProto.ic_count      / UProto.ic_names      — per nested function
 *      (this header).  Populated by uemit at compile time, persisted in
 *      bytecode v1.3+, freed by umodule_destroy_proto_buffers.
 *   2. UModule.ic_count     / UModule.ic_names     — for the root chunk
 *      (this header).  Mirrors UProto's layout because the root chunk is
 *      not modeled as a UProto on disk; freed in umodule_destroy.
 *   3. UProtoInstance.ic_count + UIC entries[]     — runtime IC table per
 *      (vm, proto) pair (object/umoduleinstance.h).  Sized from #1 / #2 at
 *      module-instance creation; UIC.name is copied from ic_names.
 *
 * Mirror discipline: any change to UProto/UModule's IC field naming or
 * layout must be applied to all three sites and to the wire-format
 * encoder/decoder in uemit.c / umodule.c. */

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

   Version-mismatch policy: exact-match.  Any byte other than VERSION_BYTE is
   a hard ULOAD_UNSUPPORTED_VERSION reject — there is no best-effort or
   forward/backward compatibility.  Older modules silently loading would
   produce unknown opcodes, misread GC state, or wrongly-sized IC tables.
   Re-emit from source to migrate. */

#define URBI_BYTECODE_VERSION_MAJOR  1U
#define URBI_BYTECODE_VERSION_MINOR  5U
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
    OP_CALL     = 15,             /* ABC:  R[A], ..., R[A+C-2] :=
                                     R[A](R[A+1], ..., R[A+B-1])
                                     B = nargs+1, C = nresults+1            */
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
                                         (set at OP_CALL dispatch from
                                         vm->last_recv).  Emitted for AST_THIS
                                         inside a method body. */

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

    /* [runtime-only, NOT serialized] Intrusive list link used when this proto
     * is "stolen" from its owning UModule by urbi_steal_repl_protos before
     * umodule_destroy.  Stolen protos are threaded onto vm->stdlib_protos and
     * freed at urbi_vm_destroy.  NULL when the proto is still owned by its
     * originating module (the normal case).  Zero-initialized alongside the
     * rest of UProto at alloc time (umodule_alloc_nested_proto). */
    struct UProto *next_alloc;
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
    /* === Serialized fields (bytecode wire format v1.5) ============= */

    uint32_t  *instructions;
    size_t     instr_count;
    size_t     instr_cap;

    UValue    *constants;
    size_t     const_count;
    size_t     const_cap;

    /* Lua-5.5-style delta encoding; one byte per instruction (size == instr_count).
     * INT8_MIN (-128) is a sentinel: the source line at this pc is in abs_lines,
     * not recoverable by summing deltas from the previous checkpoint. */
    int8_t    *line_deltas;

    UAbsLine   *abs_lines;
    size_t     abs_line_count;
    size_t     abs_line_cap;

    uint8_t    max_reg;           /* VM allocates (max_reg + 1) register slots */
    uint8_t    nupvals;           /* upvalue count for root chunk (always 0) */
    uint8_t    nparams;           /* param count for root chunk (always 0) */
    char       *source_name;      /* owned (allocator-allocated, null-terminated); NULL if absent */

    /* Nested function protos: function definitions compiled inside this module.
     * Indexed by the Bx field of OP_CLOSURE instructions. */
    UProto    **nested;
    size_t      nested_count;
    size_t      nested_cap;

    /* === M4 v1.3 root-chunk IC fields === Mirror UProto.ic_count / ic_names
     * for the root chunk's GETSLOT/SETSLOT sites.  Populated by the emitter
     * via uemit_close_function on the top-level funcstate; consumed by
     * urbi_module_instance_create to populate proto_instances->entries[0].
     * Capped at 256 (encoding spec §3.4 — uint8 ic_index field). */
    uint16_t       ic_count;
    USymbol      **ic_names;     /* parallel array; len == ic_count; allocator-owned */
    /* Parallel string array; one entry per IC site; UTF-8, NUL-terminated.
     * Populated by the emitter (mirroring ic_names) and by the deserializer
     * (in lieu of ic_names, which stays NULL until module-instance create
     * interns the strings).  Owned by the module's allocator; each entry and
     * the array itself are freed in umodule_destroy. */
    char         **ic_name_strs;

    /* === Runtime / transient fields (NOT serialized) =============== */

    /* origin_vm [runtime-only]: per pre-m2-multi-vm-audit-design.md.
     * Set by uemit_init at compile time; remains NULL on freshly-deserialized
     * modules.  Used only for optional debug-assert paths; cross-VM module
     * use is UB but not dynamically checked.  Never persisted. */
    struct UVM *origin_vm;

    /* alloc_fn / alloc_ud [runtime-only]: pluggable allocator hook for owned
     * buffers.  Caller sets these BEFORE umodule_deserialize / uemit_init.
     * NULL alloc_fn → stdlib realloc (hosted builds only).  Never persisted;
     * loader/emitter use them to grow + free struct-internal buffers. */
    UModuleAllocFn alloc_fn;
    void         *alloc_ud;
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

/* Allocate a new UProto as module->nested[nested_count++].
 * Returns pointer to the new proto on success, NULL on OOM.
 * The proto is zero-initialized; alloc_fn/alloc_ud are copied from module.
 *
 * Watcher-detach interaction: condition/body/onleave protos for installed
 * at/whenever/waituntil watchers are created here, then later detached from
 * module->nested[] by strand_closure_unlink (src/watcher/uwatcher_install.c).
 * After detach, the corresponding nested[k] slot becomes NULL and ownership
 * transfers to the watcher (freed via pool_free on watcher recycle).
 * umodule_destroy is robust to NULL slots in nested[].  See also MOD-015. */
UProto *umodule_alloc_nested_proto(UModule *module);

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
 * Coverage at v1.5:
 *   - Header (24 bytes), metadata (max_reg, source_name), constants
 *     (UVAL_INT + UVAL_FLOAT only — see decode_constants comments),
 *     instructions (LE uint32 stream), syncline tables, nested[] proto
 *     section + per-proto + root-chunk ic_name_strs.
 *   - Verifier walks every instruction against the opcode-shape table
 *     (urbi_opcode_shapes[]); register operands < max_reg+1, Bx fields
 *     range-checked per UBxKind, last instruction must be OP_RET.
 *   - ic_names interning is deferred to urbi_module_instance_create
 *     (see object/umoduleinstance.h); deserialize itself does not need
 *     a VM. */
UModuleLoadError umodule_deserialize(UModule *module, const uint8_t *buf, size_t size,
                                   char *errmsg, size_t errcap);

/* Free all owned buffers and zero the struct (preserving nothing).
   Safe to call on a zero-initialized UModule. */
void umodule_destroy(UModule *module);

/* Return a static string such as "ULOAD_BAD_MAGIC" for debug. */
const char *umodule_load_error_name(UModuleLoadError code);

#ifdef __cplusplus
}
#endif

#endif
