/* SPDX-License-Identifier: BSD-3-Clause */
/* include/urbi/types.h
 *
 * Stability: core (value types layout-pinned via _Static_assert).
 *
 * Public-facing type declarations needed by the rest of the public API.
 *
 * Internal headers (src/chunk/uproto.h, src/rt/uvalue.h) include this file
 * rather than redefining the types, so the frontend, the runtime and the
 * embedder all read one definition.
 *
 * Layout MUST match the internal canonical form byte-for-byte.  Any later
 * change to UValue layout requires updating this header, the internal
 * mirrors, and the bytecode wire format (a wire-format version bump).
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

/* === Opaque struct forward declarations ===
 *
 * Host code uses these as opaque pointers; full definitions live in
 * src/<subsys>/u<subsys>.h. */
struct UVM;
struct UStrand;
struct UTag;
struct URealm;
struct UProto;    /* a "module" IS its root UProto */
struct UClosure;
struct UObject;
struct UEvent;

/* The two handles the public API passes around by name.  The runtime
 * core completes the structs (src/rt/uexec.h) without re-typedef'ing
 * them. */
typedef struct UVM    UVM;
typedef struct URealm URealm;

/* === UValKind: tag byte for UValue's union discriminant ===
 *
 * Numeric values pinned by the bytecode wire format (uchunk.h is the
 * runtime mirror; this header is the consumer-facing copy). 11 and 15 are
 * reserved; the loader rejects > UVAL_STR in constant pools at v1.0.
 *
 * UVAL_TAG = 12 is runtime-only — it carries a UTag* in v.p and is never
 * serialized into constant pools (the loader already rejects > UVAL_STR
 * per the v1.0 contract, wire format v1.8 / 0x18 unchanged).  Slot 11 is
 * reserved for the public-only URBI_VALUE_PTR mirror.
 *
 * UVAL_SYM = 13 and UVAL_CELL = 14 are runtime-only, added for the
 * refound/core value representation (src/rt/uvalue.h); like UVAL_TAG they
 * are never serialized into constant pools. */
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
    UVAL_HOST_FN = 10,  /* retired: never produced by the runtime; the number stays reserved */
    /* slot 11 reserved for the public-only URBI_VALUE_PTR mirror */
    UVAL_TAG     = 12,  /* runtime-only (v0.10.2) — not serialized into
                           constant pools (loader rejects > UVAL_STR per
                           v1.0 contract).  Carries UTag* in v.p. */
    UVAL_SYM     = 13,  /* runtime-only (refound/core) — interned symbol. */
    UVAL_CELL    = 14   /* runtime-only (refound/core) — GC cell pointer. */
} UValKind;

/* === UValue: 16-byte tagged union ===
 *
 * 1 byte kind + implicit alignment padding + 8 byte payload.  The float
 * arm is always `double` (f64) — the old per-target f32 float flavor
 * (URBI_FLOAT_TYPE) has been retired; every build now shares one fixed
 * layout.  Layout mirrored exactly by src/chunk/uchunk.h. */
typedef struct UValue {
    uint8_t  kind;       /* UValKind */
    union {
        int64_t i;
        double  f;
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
    URBI_VALUE_STRAND  = 7,   /* == UVAL_STRAND */
    URBI_VALUE_OBJECT  = 8,   /* == UVAL_OBJECT */
    URBI_VALUE_EVENT   = 9,   /* == UVAL_EVENT */
    URBI_VALUE_PTR     = 11,  /* public-only: host opaque pointer, no UVAL_* mirror */
    URBI_VALUE_TAG     = 12,  /* == UVAL_TAG */
    URBI_VALUE_CELL    = 14   /* == UVAL_CELL: a runtime cell with no public kind of
                                 its own (a List, a Dictionary, ...) */
} urbi_value_kind_t;

URBI_STATIC_ASSERT((int)URBI_VALUE_INT     == (int)UVAL_INT,     "urbi_value_kind_t/UVAL_* drift: INT");
URBI_STATIC_ASSERT((int)URBI_VALUE_FLOAT   == (int)UVAL_FLOAT,   "urbi_value_kind_t/UVAL_* drift: FLOAT");
URBI_STATIC_ASSERT((int)URBI_VALUE_STR     == (int)UVAL_STR,     "urbi_value_kind_t/UVAL_* drift: STR");
URBI_STATIC_ASSERT((int)URBI_VALUE_OBJECT  == (int)UVAL_OBJECT,  "urbi_value_kind_t/UVAL_* drift: OBJECT");
URBI_STATIC_ASSERT((int)URBI_VALUE_EVENT   == (int)UVAL_EVENT,   "urbi_value_kind_t/UVAL_* drift: EVENT");
URBI_STATIC_ASSERT((int)URBI_VALUE_CLOSURE == (int)UVAL_CLOSURE, "urbi_value_kind_t/UVAL_* drift: CLOSURE");
URBI_STATIC_ASSERT((int)URBI_VALUE_TAG     == (int)UVAL_TAG,     "urbi_value_kind_t/UVAL_* drift: TAG");
URBI_STATIC_ASSERT((int)URBI_VALUE_STRAND  == (int)UVAL_STRAND,  "urbi_value_kind_t/UVAL_* drift: STRAND");
URBI_STATIC_ASSERT((int)URBI_VALUE_CELL    == (int)UVAL_CELL,    "urbi_value_kind_t/UVAL_* drift: CELL");

/* === Gap N: urbi_make_* value constructors (inline) ===
 *
 * Typed constructors for all UValue kinds exposed at the public API surface.
 * These are zero-overhead inlines that set kind + fill the appropriate
 * union arm.  urbi_make_str_interned is declared in <urbi/urbi.h> (requires
 * a live UVM for interning).
 *
 * Pointer-bearing constructors store via v.p.  A closure, an event and a
 * tag are runtime cells and travel as UVAL_CELL -- the cell's own subtype
 * is what urbi_value_is_closure/event/tag and urbi_value_kind read -- so
 * the value these constructors build is the one urbi_call, urbi_event_emit
 * and urbi_tag_stop accept back.  Boolean uses v.i with 0/1; the numeric
 * kinds use v.i and v.f. */
static inline UValue urbi_make_nil(void)
{
    UValue v;
    v.kind = (uint8_t)UVAL_NIL;
    v.v.i = 0;
    return v;
}

static inline UValue urbi_make_bool(bool b)
{
    UValue v;
    v.kind = (uint8_t)UVAL_BOOL;
    v.v.i = b ? 1 : 0;
    return v;
}

static inline UValue urbi_make_int(int64_t n)
{
    UValue v;
    v.kind = (uint8_t)UVAL_INT;
    v.v.i = n;
    return v;
}

static inline UValue urbi_make_float(double f)
{
    UValue v;
    v.kind = (uint8_t)UVAL_FLOAT;
    v.v.f = f;
    return v;
}

static inline UValue urbi_make_void(void)
{
    UValue v;
    v.kind = (uint8_t)UVAL_VOID;
    v.v.i = 0;
    return v;
}

static inline UValue urbi_make_ptr(void *p)
{
    UValue v;
    v.kind = (uint8_t)URBI_VALUE_PTR;
    v.v.p = p;
    return v;
}

static inline UValue urbi_make_object(struct UObject *o)
{
    UValue v;
    v.kind = (uint8_t)UVAL_OBJECT;
    v.v.p = (void *)o;
    return v;
}

static inline UValue urbi_make_event(struct UEvent *e)
{
    UValue v;
    v.kind = (uint8_t)UVAL_CELL;
    v.v.p = (void *)e;
    return v;
}

static inline UValue urbi_make_closure(struct UClosure *c)
{
    UValue v;
    v.kind = (uint8_t)UVAL_CELL;
    v.v.p = (void *)c;
    return v;
}

static inline UValue urbi_make_tag(struct UTag *tag)
{
    UValue v;
    v.kind = (uint8_t)UVAL_CELL;
    v.v.p = (void *)tag;
    return v;
}

/* === urbi_value_kind + urbi_value_as_* typed accessors ===
 *
 * urbi_value_kind: the kind an embedder dispatches on.  Strings come back
 * as URBI_VALUE_STR whether the runtime holds them interned or on the
 * heap; closures, events, tags and strands come back by their own kind
 * although the tag byte says UVAL_CELL; any other runtime cell is
 * URBI_VALUE_CELL.  The raw tag byte is still `v.kind`.
 *
 * urbi_value_as_*: access the payload without any kind check.  Caller MUST
 * verify kind first via urbi_value_kind() or urbi_value_is_*(); mismatched
 * access is undefined behaviour.  No checked variants are provided -- same
 * pattern as Lua's lua_type + lua_to* (caller performs the guard).
 *
 * urbi_value_as_str: the bytes of a string value (NUL-terminated; an
 * embedded NUL is preserved and counted in *out_len), valid until the next
 * collection unless the value is pinned with urbi_ref.  NULL with
 * *out_len = 0 for a non-string.  out_len may be NULL.
 *
 * urbi_value_kind and urbi_value_as_str read the runtime's cell layout and
 * are exported functions; the rest are inlines.
 *
 * urbi_value_as_bool: returns true/false from the v.i payload (0=false,
 * non-zero=true), consistent with internal val_bool convention. */
urbi_value_kind_t urbi_value_kind(UValue v);
const char *urbi_value_as_str(UValue v, size_t *out_len);

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

static inline struct UTag *urbi_value_as_tag(UValue v)
{
    return (struct UTag *)v.v.p;
}

/* Kind predicates.  The scalar ones are pure tag-byte comparisons and
 * inline; is_str accepts both string representations the runtime uses;
 * is_closure/is_event/is_tag/is_strand read the cell's subtype and are
 * exported functions.
 *
 * Embedders use these to dispatch on UValue kind without reaching for
 * urbi_value_kind() comparisons or internal UVAL_* constants.  Example:
 *
 *   if (urbi_value_is_int(v))        { int64_t n = urbi_value_as_int(v); }
 *   else if (urbi_value_is_float(v)) { double  f = urbi_value_as_float(v); }
 *   else if (urbi_value_is_str(v))   { size_t len; const char *s = urbi_value_as_str(v, &len); }
 *
 * UVAL_HOST_FN has no predicate: the runtime never produces that kind. */

static inline bool urbi_value_is_nil    (UValue v) { return v.kind == (uint8_t)UVAL_NIL;        }
static inline bool urbi_value_is_bool   (UValue v) { return v.kind == (uint8_t)UVAL_BOOL;       }
static inline bool urbi_value_is_int    (UValue v) { return v.kind == (uint8_t)UVAL_INT;        }
static inline bool urbi_value_is_float  (UValue v) { return v.kind == (uint8_t)UVAL_FLOAT;      }
static inline bool urbi_value_is_str    (UValue v) { return v.kind == (uint8_t)UVAL_STR || v.kind == (uint8_t)UVAL_SYM; }
static inline bool urbi_value_is_void   (UValue v) { return v.kind == (uint8_t)UVAL_VOID;       }
static inline bool urbi_value_is_object (UValue v) { return v.kind == (uint8_t)UVAL_OBJECT;     }
static inline bool urbi_value_is_ptr    (UValue v) { return v.kind == (uint8_t)URBI_VALUE_PTR;  }
bool urbi_value_is_closure(UValue v);
bool urbi_value_is_event  (UValue v);
bool urbi_value_is_tag    (UValue v);
bool urbi_value_is_strand (UValue v);

/* Per-realm limits enforced during source-text compilation. Zero in any
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
 * URBI_EVENT_PAYLOAD_MAX is the authoritative definition; the scheduler's
 * ISR ring (src/rt/usched.h) sizes its records from it.
 *
 * Alignment is achieved with __attribute__((aligned(8))) rather than C11
 * _Alignas to preserve -std=c99 compatibility (project convention). */
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
 * Numeric values are stable; new codes are appended, never renumbered. */
typedef enum {
    URBI_OK                             =  0,
    URBI_ERR_INVALID_ARG                = -1,
    URBI_ERR_OOM                        = -3,
    URBI_ERR_BYTECODE_VERSION_MISMATCH  = -4,
    URBI_ERR_COMPILE                    = -5,
    /* A throw reached the top frame of a strand.  urbi_last_error carries
     * the rendered exception. */
    URBI_ERR_UNCAUGHT_THROW             = -18,
    /* The requested subsystem is not available yet in this build — see the
     * per-function notes in <urbi/urbi.h>. */
    URBI_ERR_INVALID_STATE              = -27,
    /* Per-realm compile limits (UCompileBudget below). */
    URBI_ERR_COMPILE_BUDGET_DEPTH       = -22,
    URBI_ERR_COMPILE_BUDGET_NODES       = -23,
    URBI_ERR_COMPILE_BUDGET_SOURCE      = -24
} UErrCode;

/* === UCallbackSignal: positive return values for host callbacks ===
 *
 *   rc < 0  → error        rc == 0 → success        rc > 0 → signal */
typedef enum {
    URBI_CB_OK         = 0, /* callback succeeded; no side-effect */
    URBI_CB_UNREGISTER = 1, /* watcher callback: auto-unregister after this firing */
    URBI_CB_THROW      = 2  /* native: a script exception was raised */
} UCallbackSignal;

/* === UExecStatus: what a native or a synchronous call returned ===
 *
 * Natives return UEXEC_OK or UEXEC_THROW; the runtime's unwind state
 * uses the other two. */
typedef enum {
    UEXEC_OK     = 0,
    UEXEC_RETURN = 1,
    UEXEC_THROW  = 2,
    UEXEC_STOP   = 3
} UExecStatus;

/* === UVMAllocFn: pluggable allocator signature ===
 *
 * Standard realloc semantics:
 *   ptr == NULL, nbytes > 0   → allocate
 *   ptr != NULL, nbytes == 0  → free
 *   ptr != NULL, nbytes > 0   → reallocate
 */
typedef void *(*UVMAllocFn)(void *ptr, size_t nbytes, void *ud);

#ifdef __cplusplus
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif
#endif /* URBI_TYPES_H */
