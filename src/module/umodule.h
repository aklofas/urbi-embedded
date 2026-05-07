/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode UModule — the front-end / back-end interface.  Freestanding. */

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
   v1.4 = 0x14 (M5 — reactive opcodes 39-46, gc_byte bit 7, 4 new AST node kinds). */

#define URBI_BYTECODE_VERSION_MAJOR  1u
#define URBI_BYTECODE_VERSION_MINOR  4u
#define URBI_BYTECODE_VERSION_BYTE   ((URBI_BYTECODE_VERSION_MAJOR << 4u) | URBI_BYTECODE_VERSION_MINOR)

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

/* --- tagged value shape shared between pool and runtime registers --- */

typedef enum {
    UVAL_NIL     = 0,
    UVAL_INT     = 1,
    UVAL_FLOAT   = 2,
    UVAL_BOOL    = 3,
    UVAL_STR     = 4,
    UVAL_CLOSURE = 5,             /* M2: function closure (proto + upvalues); runtime-only */
    UVAL_VOID    = 6,             /* M2: result of `&` separator; runtime-only */
    UVAL_STRAND  = 7,             /* M3: strand handle (OP_FORK_JOIN → OP_JOIN_WAIT); runtime-only.
                                     Stores a UStrand* in v.p.  Walked by GC root walker:
                                     skipped at M3 (strands are sched-managed, not GC cells).
                                     TODO(M7+): revisit if strand handles become user-visible. */
    UVAL_OBJECT  = 8,             /* M4: UObject pointer; runtime-only.  Stores a UObject* in v.p.
                                     Receivers for OP_GETSLOT/OP_SETSLOT live in registers tagged
                                     UVAL_OBJECT.  Heap-bearing for the GC barrier — UObject embeds
                                     UCell as its first member, so uvalue_as_cell() works. */
    UVAL_EVENT   = 9,             /* M5: UEvent pointer; runtime-only.  Stores a UEvent* in v.p.
                                     Heap-bearing for the GC barrier — UEvent embeds UCell as its
                                     first member.  Used by tag.enter / tag.leave getters and
                                     urbi_native_event_new (T53). */
    UVAL_HOST_FN = 10             /* M5: native host function slot; runtime-only.  Stores a
                                     UHostFn (function pointer) in v.v.p cast to void*.
                                     Used by event_native_register / tag_native_register (T53/T54)
                                     to populate proto slots that OP_CALL can dispatch into.
                                     NOT heap-bearing — function pointers are not GC cells. */
    /* 11-15 reserved; loader rejects > UVAL_STR in constant pools at v1.0 */
} UValKind;

typedef struct {
    uint8_t  kind;                /* UValKind */
    uint8_t  _pad[7];             /* 8-byte align of .v */
    union {
        int64_t i;
#if URBI_FLOAT_TYPE == 8
        double  f;
#else
        float   f;
#endif
        void   *p;                /* UVAL_CLOSURE: pointer to UClosure (T14) */
    } v;
} UValue;                         /* 16 bytes */

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

    /* M4 reserves; v1.x backlog implements (collapsed GETSLOT+CALL). */
    OP_INVOKE           = 38,

    /* M5 reactive runtime — pre-M5 spec #2 (at/whenever/waituntil) */
    OP_AT_INSTALL              = 39,  /* ABC: cond_reg, body_reg, onleave_or_FF  */
    OP_AT_SYNC_INSTALL         = 40,  /* ABC: same shape as OP_AT_INSTALL        */
    OP_WHENEVER_INSTALL        = 41,  /* ABC: cond_reg, body_reg, onleave_or_FF  */
    OP_WAITUNTIL_INSTALL       = 42,  /* ABC: cond_reg, 0, 0                     */

    /* M5 reactive runtime — pre-M5 spec #3 (event syncEmit + tag.enter/leave) */
    OP_AT_EVENT_INSTALL        = 43,  /* ABC: event_reg, body_reg, onleave_or_FF */
    OP_AT_EVENT_SYNC_INSTALL   = 44,  /* ABC: same shape as OP_AT_EVENT_INSTALL  */

    /* M5 reactive runtime — pre-M5 spec #4 (slot-change events) */
    OP_GETSLOT_CHANGE_EVENT    = 45,  /* ABC: dst_reg, recv_reg, name_sym_id     */

    /* M5 reactive runtime — pre-M5 spec #5 (globals exposure) */
    OP_LOAD_REALM_GLOBAL       = 46,  /* ABC: dst_reg, sym_id_hi, sym_id_lo      */

    OP_MAX
} UOpcode;

/* --- instruction decode helpers (static inline; byte-aligned fields) --- */

static inline UOpcode  uinstr_op (uint32_t i) { return (UOpcode)(i & 0xFFu); }
static inline uint8_t  uinstr_a  (uint32_t i) { return (uint8_t)((i >> 8)  & 0xFFu); }
static inline uint8_t  uinstr_b  (uint32_t i) { return (uint8_t)((i >> 16) & 0xFFu); }
static inline uint8_t  uinstr_c  (uint32_t i) { return (uint8_t)((i >> 24) & 0xFFu); }
static inline uint16_t uinstr_bx (uint32_t i) { return (uint16_t)((i >> 16) & 0xFFFFu); }

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
     * the proto's allocator; freed in umodule_proto_destroy_buffers. */
    USymbol      **ic_names;

    /* Allocator hook inherited from the owning module. */
    UModuleAllocFn alloc_fn;
    void          *alloc_ud;
} UProto;

/* --- UClosure: runtime function value (proto + captured upvalues).
 * Forward declaration only — full struct definition lives in uclosure.h
 * (M4 split: UClosure embeds UCell as first member, which can't be done
 * here without a circular include via gc/ugc.h).  Files that only need
 * `UClosure *` use the typedef below; files that touch UClosure fields
 * include "uclosure.h" explicitly. */
typedef struct UClosure UClosure;

/* --- UModule struct --- */

typedef struct UModule {
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

    /* M2 addition — per pre-m2-multi-vm-audit-design.md.
     * Set by uemit_init at compile time; zero on freshly-deserialized
     * modules. Used only for optional debug-assert paths in v1.0;
     * cross-VM module use is UB but not dynamically checked. */
    struct UVM *origin_vm;

    /* allocator hook; NULL -> use stdlib realloc (hosted builds only) */
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
    ULOAD_OOM
} UModuleLoadError;

/* --- Proto helpers --- */

/* Allocate a new UProto as module->nested[nested_count++].
 * Returns pointer to the new proto on success, NULL on OOM.
 * The proto is zero-initialized; alloc_fn/alloc_ud are copied from module. */
UProto *umodule_alloc_nested_proto(UModule *module);

/* Free a UProto's owned buffers.  Does NOT free the UProto struct itself
 * (it is owned by the module's nested[] array). */
void umodule_proto_destroy_buffers(UProto *proto, UModuleAllocFn alloc,
                                   void *alloc_ud);

/* --- API --- */

/* Populate module from buf.  module must be zero-initialized before call;
   if module->alloc_fn is NULL on entry, the stdlib realloc is used
   (hosted builds only).  errmsg/errcap receive a human-readable
   diagnostic on failure; pass (NULL, 0) to suppress.
   On error the module is left empty (destroy is safe but a no-op). */
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
