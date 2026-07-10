/* SPDX-License-Identifier: BSD-3-Clause */
/* include/urbi/types.h
 *
 * Stability: core (value types layout-pinned via _Static_assert).
 *
 * Public-facing type declarations needed by the rest of the public API.
 *
 * Created at v0.5.5 (Wave 3) to break the cycle where include/urbi/urbi.h
 * pulled in src/sched/ustrand.h to get UValue + UExecStatus declarations.
 * That made the public header non-self-contained — external consumers
 * using only -Iinclude could not resolve sibling internal includes.
 *
 * Internal headers (src/chunk/uchunk.h, src/sched/ustrand.h, src/vm/uvm.h)
 * include this file rather than redefining the types, ensuring single
 * source of truth.
 *
 * Layout MUST match the internal canonical form byte-for-byte; v0.5.5
 * captures the canonical form here.  Any later change to UValue layout
 * requires updating this header, the internal mirrors, and the bytecode
 * wire format (Wave 4 territory).
 *
 * Closes the structural half of API-012, INC-003.
 */

#ifndef URBI_TYPES_H
#define URBI_TYPES_H

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility push(default)   /* v1.0: export only public-header symbols */
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === URBI_PUBLIC — explicit shared-object export marker (v1.0) ===
 * The library is compiled with -fvisibility=hidden, and every public header is
 * wrapped in `#pragma GCC visibility push(default)`, so all declared public API
 * already carries default visibility (exported) while the ~300 internal
 * cross-TU helpers stay hidden — an embedder who builds a .so around liburbi.a
 * gets only the documented urbi_* surface.  URBI_PUBLIC is provided for any
 * symbol an embedder chooses to re-export explicitly; it expands to nothing on
 * compilers without visibility support. */
#if defined(__GNUC__) || defined(__clang__)
#  define URBI_PUBLIC __attribute__((visibility("default")))
#else
#  define URBI_PUBLIC
#endif

/* === URBI_STATIC_ASSERT — C11 _Static_assert wrapper ===
 *
 * The codebase targets -std=c99, but uses C11's _Static_assert pervasively
 * to pin layout invariants. GCC accepts _Static_assert in C99 mode but
 * emits "ISO C99 does not support _Static_assert" under -Wpedantic. The
 * __extension__ prefix tells GCC the use is deliberate, suppressing the
 * warning without disabling -Wpedantic for the file. */
#if defined(__GNUC__) || defined(__clang__)
#  define URBI_STATIC_ASSERT(cond, msg) __extension__ _Static_assert((cond), msg)
#else
#  define URBI_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

/* === Float-type configuration ===
 *
 * URBI_FLOAT_TYPE selects the size of the float arm in UValue's union:
 *   8 → double (f64)  -- default for hosted builds
 *   4 → float  (f32)  -- typically set on 32-bit cross-targets via
 *                        -DURBI_FLOAT_TYPE=4 in build flags.
 * The canonical definition lives in src/chunk/uchunk.h; this header
 * supplies the same default so external consumers see the same layout. */
#ifndef URBI_FLOAT_TYPE
#define URBI_FLOAT_TYPE 8
#endif

/* === Opaque struct forward declarations ===
 *
 * Host code uses these as opaque pointers; full definitions live in
 * src/<subsys>/u<subsys>.h. */
struct UVM;
struct UStrand;
struct UTag;
struct URealm;
struct UProto;    /* v0.9.2: replaced UModule — a "module" IS its root UProto */
struct UClosure;
struct UObject;
struct UEvent;

/* === UValKind: tag byte for UValue's union discriminant ===
 *
 * Numeric values pinned by the bytecode wire format (uchunk.h is the
 * runtime mirror; this header is the consumer-facing copy). 11-15 are
 * reserved; the loader rejects > UVAL_STR in constant pools at v1.0.
 *
 * UVAL_TAG = 12 is runtime-only — it carries a UTag* in v.p and is never
 * serialized into constant pools (the loader already rejects > UVAL_STR
 * per the v1.0 contract, wire format v1.8 / 0x18 unchanged).  Slot 11 is
 * reserved for the public-only URBI_VALUE_PTR mirror. */
typedef enum {
    UVAL_NIL     = 0,
    UVAL_INT     = 1,
    UVAL_FLOAT   = 2,
    UVAL_BOOL    = 3,
    UVAL_STR     = 4,
    UVAL_CLOSURE = 5,
    UVAL_VOID    = 6,
    UVAL_STRAND  = 7,
    UVAL_OBJECT  = 8,
    UVAL_EVENT   = 9,
    UVAL_HOST_FN = 10,
    /* slot 11 reserved for the public-only URBI_VALUE_PTR mirror */
    UVAL_TAG     = 12   /* runtime-only (v0.10.2) — not serialized into
                           constant pools (loader rejects > UVAL_STR per
                           v1.0 contract).  Carries UTag* in v.p. */
} UValKind;

/* === UValue: 16-byte tagged union ===
 *
 * 1 byte kind + 7 byte pad + 8 byte payload.  Layout mirrored exactly by
 * src/chunk/uchunk.h. */
typedef struct {
    uint8_t  kind;       /* UValKind */
    uint8_t  _pad[7];
    union {
        int64_t i;
#if URBI_FLOAT_TYPE == 8
        double  f;
#else
        float   f;
#endif
        void   *p;
    } v;
} UValue;

/* === urbi_value_kind_t: public mirror of the internal UValKind enum ===
 *
 * Numeric values are identical to the corresponding UVAL_* constants so
 * code using either name works without conversion.  Six compile-time
 * assertions enforce this invariant for the kinds exposed to embedders.
 *
 * URBI_VALUE_PTR is a public-only kind (no internal UVAL_PTR counterpart):
 * embedders use it to store arbitrary host C pointers as UValues.  Numeric
 * value 11 is one past UVAL_HOST_FN=10 and reserved here; the internal VM
 * never produces UValues with this kind. */
typedef enum {
    URBI_VALUE_NIL     = 0,   /* == UVAL_NIL */
    URBI_VALUE_INT     = 1,   /* == UVAL_INT */
    URBI_VALUE_FLOAT   = 2,   /* == UVAL_FLOAT */
    URBI_VALUE_BOOL    = 3,   /* == UVAL_BOOL */
    URBI_VALUE_STR     = 4,   /* == UVAL_STR */
    URBI_VALUE_CLOSURE = 5,   /* == UVAL_CLOSURE */
    URBI_VALUE_VOID    = 6,   /* == UVAL_VOID */
    URBI_VALUE_OBJECT  = 8,   /* == UVAL_OBJECT */
    URBI_VALUE_EVENT   = 9,   /* == UVAL_EVENT */
    URBI_VALUE_PTR     = 11,  /* public-only: host opaque pointer, no UVAL_* mirror */
    URBI_VALUE_TAG     = 12   /* == UVAL_TAG; runtime-only UTag* (v0.10.2) */
} urbi_value_kind_t;

URBI_STATIC_ASSERT((int)URBI_VALUE_INT     == (int)UVAL_INT,     "urbi_value_kind_t/UVAL_* drift: INT");
URBI_STATIC_ASSERT((int)URBI_VALUE_FLOAT   == (int)UVAL_FLOAT,   "urbi_value_kind_t/UVAL_* drift: FLOAT");
URBI_STATIC_ASSERT((int)URBI_VALUE_STR     == (int)UVAL_STR,     "urbi_value_kind_t/UVAL_* drift: STR");
URBI_STATIC_ASSERT((int)URBI_VALUE_OBJECT  == (int)UVAL_OBJECT,  "urbi_value_kind_t/UVAL_* drift: OBJECT");
URBI_STATIC_ASSERT((int)URBI_VALUE_EVENT   == (int)UVAL_EVENT,   "urbi_value_kind_t/UVAL_* drift: EVENT");
URBI_STATIC_ASSERT((int)URBI_VALUE_CLOSURE == (int)UVAL_CLOSURE, "urbi_value_kind_t/UVAL_* drift: CLOSURE");
URBI_STATIC_ASSERT((int)URBI_VALUE_TAG     == (int)UVAL_TAG,     "urbi_value_kind_t/UVAL_* drift: TAG");

/* === Gap N: urbi_make_* value constructors (inline) ===
 *
 * Typed constructors for all UValue kinds exposed at the public API surface.
 * These are zero-overhead inlines that set kind + clear pad + fill the
 * appropriate union arm.  urbi_make_str_interned is declared in
 * <urbi/urbi.h> (requires a live UVM for interning).
 *
 * urbi_make_nil replaces the pre-v0.7.1 urbi_value_nil() (renamed for
 * consistency with the Gap N family; pre-v1.0 escape clause).
 *
 * Pointer-bearing constructors (object/event/closure/ptr) store via v.p.
 * Boolean uses v.i with 0/1 (same convention as internal val_bool).
 * Numeric kinds (int, float) use v.i and v.f respectively. */
static inline UValue urbi_make_nil(void)
{
    UValue v;
    v.kind = (uint8_t)UVAL_NIL;
    for (size_t _pi = 0; _pi < sizeof(v._pad); _pi++) v._pad[_pi] = 0;
    v.v.i = 0;
    return v;
}

static inline UValue urbi_make_bool(bool b)
{
    UValue v;
    v.kind = (uint8_t)UVAL_BOOL;
    for (size_t _pi = 0; _pi < sizeof(v._pad); _pi++) v._pad[_pi] = 0;
    v.v.i = b ? 1 : 0;
    return v;
}

static inline UValue urbi_make_int(int64_t n)
{
    UValue v;
    v.kind = (uint8_t)UVAL_INT;
    for (size_t _pi = 0; _pi < sizeof(v._pad); _pi++) v._pad[_pi] = 0;
    v.v.i = n;
    return v;
}

static inline UValue urbi_make_float(double f)
{
    UValue v;
    v.kind = (uint8_t)UVAL_FLOAT;
    for (size_t _pi = 0; _pi < sizeof(v._pad); _pi++) v._pad[_pi] = 0;
#if URBI_FLOAT_TYPE == 8
    v.v.f = f;
#else
    v.v.f = (float)f;   /* explicit narrowing on f32 builds (-Wfloat-conversion clean) */
#endif
    return v;
}

static inline UValue urbi_make_void(void)
{
    UValue v;
    v.kind = (uint8_t)UVAL_VOID;
    for (size_t _pi = 0; _pi < sizeof(v._pad); _pi++) v._pad[_pi] = 0;
    v.v.i = 0;
    return v;
}

static inline UValue urbi_make_ptr(void *p)
{
    UValue v;
    v.kind = (uint8_t)URBI_VALUE_PTR;
    for (size_t _pi = 0; _pi < sizeof(v._pad); _pi++) v._pad[_pi] = 0;
    v.v.p = p;
    return v;
}

static inline UValue urbi_make_object(struct UObject *o)
{
    UValue v;
    v.kind = (uint8_t)UVAL_OBJECT;
    for (size_t _pi = 0; _pi < sizeof(v._pad); _pi++) v._pad[_pi] = 0;
    v.v.p = (void *)o;
    return v;
}

static inline UValue urbi_make_event(struct UEvent *e)
{
    UValue v;
    v.kind = (uint8_t)UVAL_EVENT;
    for (size_t _pi = 0; _pi < sizeof(v._pad); _pi++) v._pad[_pi] = 0;
    v.v.p = (void *)e;
    return v;
}

static inline UValue urbi_make_closure(struct UClosure *c)
{
    UValue v;
    v.kind = (uint8_t)UVAL_CLOSURE;
    for (size_t _pi = 0; _pi < sizeof(v._pad); _pi++) v._pad[_pi] = 0;
    v.v.p = (void *)c;
    return v;
}

/* urbi_make_tag — runtime-only UTag* wrapper (v0.10.2).
 * UVAL_TAG values are never serialized into constant pools — the constant-pool
 * loader at v1.0 already rejects > UVAL_STR (wire format v1.8 / 0x18
 * unchanged).  Use only for runtime values returned by Tag.new() etc. */
static inline UValue urbi_make_tag(struct UTag *tag)
{
    UValue v;
    v.kind = (uint8_t)UVAL_TAG;
    for (size_t _pi = 0; _pi < sizeof(v._pad); _pi++) v._pad[_pi] = 0;
    v.v.p = (void *)tag;
    return v;
}

/* === Gap O: urbi_value_kind + urbi_value_as_* typed accessors (inline) ===
 *
 * urbi_value_kind: extract the public kind enum from a UValue.
 *
 * urbi_value_as_*: access the payload without any kind check.  Caller MUST
 * verify kind first via urbi_value_kind(); mismatched access is undefined
 * behaviour.  No checked variants are provided — same pattern as Lua's
 * lua_type + lua_to* (caller performs the guard).
 *
 * urbi_value_as_str: the interned string stored in UVAL_STR values is a
 * NUL-terminated const char* held in v.p.  The inline returns the pointer
 * directly and computes length via an inline NUL-scan loop (no <string.h>
 * dependency — freestanding compatible).  No USymbol struct layout is
 * exposed because USymbol is an opaque typedef (the intern table stores
 * raw const char* blocks, not a struct-with-len); this is simpler and
 * avoids adding struct layout to the public ABI.
 *
 * KNOWN LIMITATION: the NUL-scan is correct today because the lexer
 * rejects embedded NULs (the \0 / \xNN string escapes are still on the
 * v1.x lex backlog — see LEX-035).  When those escapes land, intern keys
 * may contain embedded NULs and this accessor will silently return a
 * truncated length.  Tracked in docs/urbi-embedded-design-risks.md as
 * "urbi_value_as_str NUL-scan fragility".
 *
 * urbi_value_as_bool: returns true/false from the v.i payload (0=false,
 * non-zero=true), consistent with internal val_bool convention. */
static inline urbi_value_kind_t urbi_value_kind(UValue v)
{
    return (urbi_value_kind_t)v.kind;
}

static inline bool urbi_value_as_bool(UValue v)
{
    return v.v.i != 0;
}

static inline int64_t urbi_value_as_int(UValue v)
{
    return v.v.i;
}

static inline double urbi_value_as_float(UValue v)
{
    return v.v.f;
}

static inline void *urbi_value_as_ptr(UValue v)
{
    return v.v.p;
}

static inline const char *urbi_value_as_str(UValue v, size_t *out_len)
{
    const char *s = (const char *)v.v.p;
    if (out_len) {
        size_t n = 0;
        if (s) { while (s[n] != '\0') n++; }
        *out_len = n;
    }
    return s;
}

static inline struct UObject *urbi_value_as_object(UValue v)
{
    return (struct UObject *)v.v.p;
}

static inline struct UEvent *urbi_value_as_event(UValue v)
{
    return (struct UEvent *)v.v.p;
}

static inline struct UClosure *urbi_value_as_closure(UValue v)
{
    return (struct UClosure *)v.v.p;
}

/* === urbi_value_is_* predicate family (v0.10.3) ===
 *
 * Pure tag comparison; no validation of the payload.  Header-only static
 * inlines — zero-overhead at any optimisation level.
 *
 * Ordered by UValKind numeric value (not by urbi_make_* declaration order,
 * which is lexical; not by urbi_value_as_* declaration order).
 *
 * Embedders use these to dispatch on UValue kind without reaching for
 * urbi_value_kind() comparisons or internal UVAL_* constants.  Example:
 *
 *   if (urbi_value_is_int(v))        { int64_t n = urbi_value_as_int(v); }
 *   else if (urbi_value_is_float(v)) { double  f = urbi_value_as_float(v); }
 *   else if (urbi_value_is_str(v))   { size_t len; const char *s = urbi_value_as_str(v, &len); }
 *
 * Closes api-ergonomics F1 (value-ctor / accessor asymmetry).
 *
 * urbi_value_is_strand and urbi_value_is_host_fn are diagnostic-only
 * predicates: the corresponding kinds (UVAL_STRAND / UVAL_HOST_FN) can
 * appear in slots visible to callbacks but have no public constructors.
 * The section marker helps merge-conflict resolution when other worktrees
 * touch adjacent regions of this header. */

static inline bool urbi_value_is_nil    (UValue v) { return v.kind == (uint8_t)UVAL_NIL;        }
static inline bool urbi_value_is_bool   (UValue v) { return v.kind == (uint8_t)UVAL_BOOL;       }
static inline bool urbi_value_is_int    (UValue v) { return v.kind == (uint8_t)UVAL_INT;        }
static inline bool urbi_value_is_float  (UValue v) { return v.kind == (uint8_t)UVAL_FLOAT;      }
static inline bool urbi_value_is_str    (UValue v) { return v.kind == (uint8_t)UVAL_STR;        }
static inline bool urbi_value_is_closure(UValue v) { return v.kind == (uint8_t)UVAL_CLOSURE;    }
static inline bool urbi_value_is_void   (UValue v) { return v.kind == (uint8_t)UVAL_VOID;       }
static inline bool urbi_value_is_strand (UValue v) { return v.kind == (uint8_t)UVAL_STRAND;     }
static inline bool urbi_value_is_object (UValue v) { return v.kind == (uint8_t)UVAL_OBJECT;     }
static inline bool urbi_value_is_event  (UValue v) { return v.kind == (uint8_t)UVAL_EVENT;      }
static inline bool urbi_value_is_host_fn(UValue v) { return v.kind == (uint8_t)UVAL_HOST_FN;   }
static inline bool urbi_value_is_ptr    (UValue v) { return v.kind == (uint8_t)URBI_VALUE_PTR;  }
static inline bool urbi_value_is_tag    (UValue v) { return v.kind == (uint8_t)UVAL_TAG;        }

/* === UValue layout pin ===
 *
 * Compile-time assertion that mirrors the runtime invariants tested in
 * tests/unit/test_uvalue_layout.c. Catches header/lib mismatches at
 * compile time when host code includes this header against a different
 * library build.
 *
 * Behind URBI_API_PIN_LAYOUT (default ON). Hosts that intentionally rebuild
 * with non-standard alignment / packing can define this to 0 to skip. */
#ifndef URBI_API_PIN_LAYOUT
#define URBI_API_PIN_LAYOUT 1
#endif

#if URBI_API_PIN_LAYOUT
URBI_STATIC_ASSERT(sizeof(UValue) == 16,
               "UValue must be exactly 16 bytes (ABI pin)");
URBI_STATIC_ASSERT(offsetof(UValue, v) == 8,
               "UValue.v must be at offset 8 (ABI pin)");
URBI_STATIC_ASSERT(offsetof(UValue, kind) == 0,
               "UValue.kind must be at offset 0 (ABI pin)");
#endif

/* === UCompileBudget — per-realm parse-time guard (v0.9.1) ===
 *
 * Per-realm limits enforced during source-text compilation. Zero in any
 * field means "unlimited" for that limit. urbi_realm_create_repl auto-
 * applies URBI_DEFAULT_REPL_BUDGET; the global Realm has no budget by
 * default (trusted host code).
 *
 * Three limits, evaluated in order:
 *   max_parser_depth  — recursive-descent stack ceiling
 *                       (URBI_ERR_COMPILE_BUDGET_DEPTH)
 *   max_ast_nodes     — total AST allocations per compile
 *                       (URBI_ERR_COMPILE_BUDGET_NODES)
 *   max_source_bytes  — checked once at urbi_repl_eval entry
 *                       (URBI_ERR_COMPILE_BUDGET_SOURCE) */
typedef struct {
    uint32_t max_parser_depth;
    uint32_t max_ast_nodes;
    uint32_t max_source_bytes;
} UCompileBudget;

/* === Named-event ID (Gap B) ===
 *
 * urbi_event_id_t: opaque handle returned by urbi_event_register.
 * Stable for the lifetime of the UVM; used to route urbi_inject_event calls
 * through the named-event drain in O(1) without string lookup at ISR time.
 *
 * URBI_EVENT_ID_INVALID: sentinel returned on registration failure. */
typedef uint16_t urbi_event_id_t;
#define URBI_EVENT_ID_INVALID ((urbi_event_id_t)0xFFFF)

/* === ISR event payload contract (Gap C) ===
 *
 * urbi_event_payload_t is the typed-union form of the raw bytes passed to
 * urbi_inject_event.  Embedders writing typed payloads from ISR context
 * (e.g. IMU readings as float[4], GPIO state as uint32_t) cast their data
 * to this union before injecting.
 *
 * Size and alignment are compile-time-pinned via URBI_STATIC_ASSERT below so
 * any future change to URBI_EVENT_PAYLOAD_MAX or URBI_EVENT_PAYLOAD_ALIGN
 * is caught at compile time rather than silently breaking ISR-side code.
 *
 * URBI_EVENT_PAYLOAD_MAX is the authoritative definition; the internal
 * header src/event/uevent_ring.h defers to this value via an #ifndef guard.
 *
 * Alignment is achieved with __attribute__((aligned(8))) rather than C11
 * _Alignas to preserve -std=c99 compatibility (project convention, see
 * uevent_ring.h alignment note). */
#define URBI_EVENT_PAYLOAD_MAX   16
#define URBI_EVENT_PAYLOAD_ALIGN 8

typedef union {
    uint8_t  bytes[URBI_EVENT_PAYLOAD_MAX];
    uint32_t u32  [URBI_EVENT_PAYLOAD_MAX / sizeof(uint32_t)];
    uint64_t u64  [URBI_EVENT_PAYLOAD_MAX / sizeof(uint64_t)];
    float    f32  [URBI_EVENT_PAYLOAD_MAX / sizeof(float)];
    double   f64  [URBI_EVENT_PAYLOAD_MAX / sizeof(double)];
    void    *ptr  [URBI_EVENT_PAYLOAD_MAX / sizeof(void *)];
} __attribute__((aligned(URBI_EVENT_PAYLOAD_ALIGN))) urbi_event_payload_t;

URBI_STATIC_ASSERT(sizeof(urbi_event_payload_t)  == URBI_EVENT_PAYLOAD_MAX,
               "ISR payload size pinned at 16 bytes");
URBI_STATIC_ASSERT(__alignof__(urbi_event_payload_t) == URBI_EVENT_PAYLOAD_ALIGN,
               "ISR payload alignment pinned at 8 bytes");

/* === UErrCode: public error codes ===
 *
 * Functions in the public C API return int: 0 = URBI_OK, negative = error.
 * New codes are appended; never reordered (numeric stability).
 *
 * URBI_ERR_RESERVED_10 was URBI_ERR_OUT_OF_MEMORY pre-v0.5.5; the two OOM
 * codes were collapsed into a single URBI_ERR_OOM at -3.  The slot is held
 * to preserve numeric stability of the surrounding enumerators. */
typedef enum {
    URBI_OK                             =  0,
    URBI_ERR_INVALID_ARG                = -1,
    URBI_ERR_STRAND_FATAL               = -2,
    URBI_ERR_OOM                        = -3,
    /* URBI_ERR_BYTECODE_VERSION_MISMATCH: returned by the public-API
     * translation helper urbi_chunk_translate_load_err when the internal
     * loader reports UCHUNK_LOAD_UNSUPPORTED_VERSION (see src/chunk/uchunk_io.c).
     * The deserialize-bytes entry point is available via urbi_chunk_from_bytes;
     * the translation helper exists so any caller
     * has a single mapping site to route through.  Closes API-005. */
    URBI_ERR_BYTECODE_VERSION_MISMATCH  = -4,
    URBI_ERR_COMPILE                    = -5,
    URBI_ERR_CLEANUP_OVERFLOW           = -6,
    URBI_ERR_EVENT_PAYLOAD_TOO_LARGE    = -7,
    URBI_ERR_EVENT_RING_FULL            = -8,
    URBI_ERR_PROTECTED_SLOT             = -9,
    URBI_ERR_RESERVED_10                = -10,
    URBI_ERR_CONST_SLOT_WRITE           = -11,
    URBI_ERR_SLOT_NOT_FOUND             = -12,
    URBI_ERR_SHAPE_BOUNDS               = -13,  /* slot index past v1.0 packed-flag cap */
    URBI_ERR_PROTO_DEPTH                = -14,  /* prototype-graph resolve-stack overflow */
    /* URBI_ERR_STDLIB_BOOT_FAILED: returned by urbi_stdlib_boot (and any
     * caller that propagates it) when the embedded stdlib bytecode blob
     * fails to deserialize or bind during VM/realm bootstrap.  Distinct
     * from URBI_ERR_OOM (which signals allocation failure during boot)
     * and URBI_ERR_BYTECODE_VERSION_MISMATCH (which is reachable through
     * urbi_chunk_translate_load_err for the file-load surface).
     * The empty-blob path never reaches it. */
    URBI_ERR_STDLIB_BOOT_FAILED         = -15,
    /* URBI_ERR_API_VERSION_MISMATCH: returned by urbi_aux_check_version
     * when the runtime library's API version
     * disagrees with what the embedder compiled against. MINOR-additive
     * per the bump policy in <urbi/version.h>. */
    URBI_ERR_API_VERSION_MISMATCH       = -16,
    /* URBI_ERR_EVENT_NAME_TAKEN: returned by urbi_event_register (Gap B)
     * when name is already registered in the event registry for this VM. */
    URBI_ERR_EVENT_NAME_TAKEN           = -17,
    /* v0.13.4: root strand died with an uncaught script throw; the thrown
     * value is delivered via the out-param when non-NULL. */
    URBI_ERR_UNCAUGHT_THROW             = -18,
    /* URBI_ERR_HEAP_LOCKED: returned by operations that require a live heap
     * (allocation or registry mutation) when urbi_lock_heap has been called.
     * Covers urbi_event_unregister and future Gap-B unregister paths. */
    URBI_ERR_HEAP_LOCKED                = -19,
    /* v0.8.0: urbi_run_chunk's internal driver loop exhausted its outer
     * cap (URBI_LOADER_OUTER_CAP * URBI_LOADER_INNER_BUDGET ≈ 10M
     * instructions) without the loader strand reaching a parked or dead
     * state.  Almost certainly an infinite loop at chunk-top with no
     * yield points.  Host may call urbi_step manually to continue the
     * strand, or destroy the realm/vm to abort it. */
    URBI_ERR_LOADER_BUDGET              = -20,
    /* v0.9.1: OP_SETSLOT denied because the receiver UObject carries the
     * UPROTO_FLAG_READONLY (= URBI_OBJ_FLAG_READONLY) bit.  Spec §4.2;
     * raised when urbiscript tries to mutate a frozen builtin atom proto
     * such as Object / Number / String. */
    URBI_ERR_FROZEN_PROTO               = -21,
    /* v0.9.1: per-realm compile-budget triggers.  Reported as the result
     * of urbi_repl_eval / urbi_compile_source when the corresponding limit
     * is exceeded.  See <urbi/types.h> UCompileBudget. */
    URBI_ERR_COMPILE_BUDGET_DEPTH       = -22,
    URBI_ERR_COMPILE_BUDGET_NODES       = -23,
    URBI_ERR_COMPILE_BUDGET_SOURCE      = -24,
    /* v0.9.1: urbi_repl_serve refused a non-loopback bind without an
     * auth_token (default-secure posture).  Embedder must either set
     * cfg->auth_token or restrict cfg->bind_addr to "127.0.0.1" / "::1"
     * / a Unix-socket path starting with '/'.
     * v0.10.6: URBI_ERR_INVALID_CONFIG is a synonym for this code;
     * the canonical name remains URBI_ERR_INSECURE_CONFIG. */
    URBI_ERR_INSECURE_CONFIG            = -25,
#define URBI_ERR_INVALID_CONFIG URBI_ERR_INSECURE_CONFIG
    /* v0.10.3: returned by urbi_aux_value_to_* checked accessors when
     * the UValue kind does not match the requested type.  Embedders use
     * urbi_value_is_*() to guard before calling unchecked urbi_value_as_*;
     * or call urbi_aux_value_to_*() directly and handle this code.
     * Closes api-ergonomics F1. */
    URBI_ERR_TYPE                       = -26,
    /* v0.10.3: returned by urbi_strand_destroy (and similar lifecycle
     * functions) in debug builds when the strand is in an unsafe state for
     * the requested operation.  For example, urbi_strand_destroy on a READY
     * or RUNNING strand returns URBI_ERR_INVALID_STATE in -DURBI_DEBUG builds.
     * Release builds treat the call as a no-op (pre-v1.0 permissive posture).
     * Closes api-ergonomics F8. */
    URBI_ERR_INVALID_STATE              = -27
} UErrCode;

/* === UCallbackSignal: positive return values for host callbacks ===
 *
 * v0.10.3: All public API functions return int with one convention:
 *   URBI_OK  (0)        — success.
 *   negative URBI_ERR_* — failure; also published to urbi_last_error ring.
 *   positive UCallbackSignal — ONLY for urbi_native_method_fn and
 *                              urbi_watcher_fn returns; signals a successful
 *                              operation with a side-effect.
 *
 * Positive values are in a separate namespace from UErrCode's negative range
 * so a callback return can be unambiguously classified:
 *   rc < 0 → error
 *   rc == 0 → URBI_CB_OK / URBI_OK
 *   rc > 0 → callback signal (auto-unregister, host throw, etc.)
 *
 * UVM internal code that previously used UVMError or UExecStatus to classify
 * results uses int + URBI_OK / URBI_ERR_* / UCallbackSignal constants now.
 * UExecStatus is retained for the strand-unwind-status public API family
 * (urbi_strand_unwind_status, urbi_strand_is_fatal) pending migration. */
typedef enum {
    URBI_CB_OK         = 0, /* callback succeeded; no side-effect */
    URBI_CB_UNREGISTER = 1, /* watcher_fn: auto-unregister after this firing */
    URBI_CB_THROW      = 2  /* native_method_fn: host raised a script exception */
} UCallbackSignal;

/* ===================================================================
 * Deprecated compatibility aliases
 *
 * The following symbols are retained only for host source compatibility.
 * New code should use the canonical UErrCode / UCallbackSignal spellings:
 *
 *   UVM_OK                       -> URBI_OK
 *   UVM_TYPE_ERROR               -> URBI_ERR_STRAND_FATAL
 *   UVM_OOM                      -> URBI_ERR_OOM
 *   UVMError                     -> int
 *   URBI_ERR_WATCHER_UNREGISTER  -> URBI_CB_UNREGISTER
 *
 * The interpreter's own sources use the canonical spellings exclusively;
 * these aliases exist purely so existing embedder code keeps compiling.
 * They may be removed in a future release.
 * =================================================================== */

/* Legacy alias: URBI_ERR_WATCHER_UNREGISTER was -18 pre-v0.10.3.
 * Now maps to URBI_CB_UNREGISTER (positive 1) so callback return semantics
 * unify with the positive-signal convention.  Retained for one release cycle
 * so existing host code using the old name still compiles without change.
 * New code should use URBI_CB_UNREGISTER directly.
 * (may be decorated with URBI_DEPRECATED in a future release.) */
#define URBI_ERR_WATCHER_UNREGISTER  ((int)URBI_CB_UNREGISTER)

/* === UExecStatus: strand-level execution status ===
 *
 * Mirror of the internal enum at src/sched/ustrand.h.  Numeric values are
 * not pinned cross-version; the enum is purely symbolic.
 *
 * v0.10.3: UExecStatus is retained as a deprecated alias for
 * source compatibility for one release cycle.  New code should use the
 * public mirror UStrandUnwind below.  The internal scheduler's enum in
 * src/sched/ustrand.h is unchanged. */
typedef enum {
    UEXEC_OK       = 0,
    UEXEC_RETURN,
    UEXEC_THROW,
    UEXEC_TAG_STOP,
    UEXEC_CANCEL
} UExecStatus;

/* === UStrandUnwind — public mirror of UExecStatus (v0.10.3) ===
 *
 * Public mirror of the internal UExecStatus enum.  Numeric values are
 * identical to UExecStatus constants so existing code using UEXEC_* still
 * compares correctly.  New code should use URBI_UNWIND_* constants.
 *
 * Used as the return type of urbi_strand_unwind_status(); replaces the
 * UExecStatus surface that was pending migration per the note in
 * the urbi.h header comment.
 *
 * UExecStatus is retained as a deprecated alias (source compat, one cycle). */
typedef enum {
    URBI_UNWIND_OK       = 0,  /* == UEXEC_OK */
    URBI_UNWIND_RETURN   = 1,  /* == UEXEC_RETURN */
    URBI_UNWIND_THROW    = 2,  /* == UEXEC_THROW */
    URBI_UNWIND_TAG_STOP = 3,  /* == UEXEC_TAG_STOP */
    URBI_UNWIND_CANCEL   = 4   /* == UEXEC_CANCEL */
} UStrandUnwind;

/* === UStrandState — public strand lifecycle state (v0.10.3) ===
 *
 * Observable state of a strand as returned by urbi_strand_state().
 * Maps the internal USTRAND_* state nibble values (src/sched/ustrand.h)
 * to a public enum without exposing the packed-byte encoding.
 *
 * URBI_STRAND_BLOCKED is reserved; the cooperative scheduler uses
 * URBI_STRAND_WAITING for all blocked-on-event / sleep / join states.
 * The distinction between blocked sub-reasons is not exposed at v1.0. */
typedef enum {
    URBI_STRAND_DORMANT  = 0,  /* allocated, not yet started */
    URBI_STRAND_READY    = 1,  /* on run-queue, awaiting dispatch */
    URBI_STRAND_RUNNING  = 2,  /* currently executing */
    URBI_STRAND_BLOCKED  = 3,  /* reserved (RT scheduler sub-state) */
    URBI_STRAND_WAITING  = 4,  /* sleeping / waiting for event / join */
    URBI_STRAND_DEAD     = 5   /* terminated — safe to destroy */
} UStrandState;

/* === UVMError: retired — replaced by int + URBI_OK / URBI_ERR_* ===
 *
 * urbi_vm_run now returns int (URBI_OK / URBI_ERR_OOM / URBI_ERR_STRAND_FATAL).
 * UVMError is retired from the public API surface in v0.10.3.
 * The vm->last_error internal field is now typed int.
 *
 * Legacy shims below preserve source compatibility for one release cycle.
 * New code should use URBI_OK / URBI_ERR_OOM / URBI_ERR_STRAND_FATAL. */
/* UVM_OK is now URBI_OK (0). */
#define UVM_OK         (URBI_OK)
/* UVM_TYPE_ERROR corresponds to a strand fatal (unhandled throw / type error). */
#define UVM_TYPE_ERROR (URBI_ERR_STRAND_FATAL)
/* UVM_OOM corresponds to an out-of-memory failure. */
#define UVM_OOM        (URBI_ERR_OOM)
/* UVMError typedef retained as int alias so old declarations compile. */
typedef int UVMError;

/* === UVMAllocFn: pluggable allocator signature ===
 *
 * Standard realloc semantics:
 *   ptr == NULL, nbytes > 0   → allocate
 *   ptr != NULL, nbytes == 0  → free
 *   ptr != NULL, nbytes > 0   → realloc
 *
 * Returns NULL on allocation failure.  Mirror of src/vm/uvm.h —
 * published here at v0.5.5 to support urbi_vm_init in the public header. */
typedef void *(*UVMAllocFn)(void *ptr, size_t nbytes, void *ud);

#ifdef __cplusplus
}
#endif

/* === URBI_FLOAT_TYPE link-time guard (audit-1 F2, roadmap F7) ===
 *
 * Every TU that includes this header (other than uabi_guards.c itself)
 * references the symbol that matches the active URBI_FLOAT_TYPE value.
 * src/runtime/uabi_guards.c defines exactly one such symbol per build.
 * If embedder and library disagree on URBI_FLOAT_TYPE the reference
 * is undefined at link time, producing a diagnostic name like
 *   urbi_abi_requires_float_type_8 (undefined)
 * rather than silent UVAL_FLOAT truncation.
 *
 * Suppressed when URBI_INTERNAL_GUARD_REF=1 (i.e. inside uabi_guards.c
 * itself, which defines the symbols and must not also reference them). */
#ifndef URBI_INTERNAL_GUARD_REF
#  if URBI_FLOAT_TYPE == 4
extern const int urbi_abi_requires_float_type_4;
static const int *urbi_abi_float_guard_ref __attribute__((unused)) =
    &urbi_abi_requires_float_type_4;
#  elif URBI_FLOAT_TYPE == 8
extern const int urbi_abi_requires_float_type_8;
static const int *urbi_abi_float_guard_ref __attribute__((unused)) =
    &urbi_abi_requires_float_type_8;
#  endif
#endif /* !URBI_INTERNAL_GUARD_REF */


#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif
#endif /* URBI_TYPES_H */
