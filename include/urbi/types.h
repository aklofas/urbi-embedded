/* SPDX-License-Identifier: BSD-3-Clause */
/* include/urbi/types.h
 *
 * Stability: core (value types layout-pinned via _Static_assert; see T6).
 *
 * Public-facing type declarations needed by the rest of the public API.
 *
 * Created at v0.5.5 (Wave 3) to break the cycle where include/urbi/urbi.h
 * pulled in src/sched/ustrand.h to get UValue + UExecStatus declarations.
 * That made the public header non-self-contained — external consumers
 * using only -Iinclude could not resolve sibling internal includes.
 *
 * Internal headers (src/module/umodule.h, src/sched/ustrand.h, src/vm/uvm.h)
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
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
 * The canonical definition lives in src/module/umodule.h; this header
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
struct UModule;
struct UClosure;
struct UObject;
struct UEvent;

/* === UValKind: tag byte for UValue's union discriminant ===
 *
 * Numeric values pinned by the bytecode wire format (umodule.h is the
 * runtime mirror; this header is the consumer-facing copy). 11-15 are
 * reserved; the loader rejects > UVAL_STR in constant pools at v1.0. */
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
    UVAL_HOST_FN = 10
} UValKind;

/* === UValue: 16-byte tagged union ===
 *
 * 1 byte kind + 7 byte pad + 8 byte payload.  Layout mirrored exactly by
 * src/module/umodule.h. */
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
    URBI_VALUE_PTR     = 11   /* public-only: host opaque pointer, no UVAL_* mirror */
} urbi_value_kind_t;

URBI_STATIC_ASSERT((int)URBI_VALUE_INT     == (int)UVAL_INT,     "urbi_value_kind_t/UVAL_* drift: INT");
URBI_STATIC_ASSERT((int)URBI_VALUE_FLOAT   == (int)UVAL_FLOAT,   "urbi_value_kind_t/UVAL_* drift: FLOAT");
URBI_STATIC_ASSERT((int)URBI_VALUE_STR     == (int)UVAL_STR,     "urbi_value_kind_t/UVAL_* drift: STR");
URBI_STATIC_ASSERT((int)URBI_VALUE_OBJECT  == (int)UVAL_OBJECT,  "urbi_value_kind_t/UVAL_* drift: OBJECT");
URBI_STATIC_ASSERT((int)URBI_VALUE_EVENT   == (int)UVAL_EVENT,   "urbi_value_kind_t/UVAL_* drift: EVENT");
URBI_STATIC_ASSERT((int)URBI_VALUE_CLOSURE == (int)UVAL_CLOSURE, "urbi_value_kind_t/UVAL_* drift: CLOSURE");

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

/* === UValue layout pin (Wave 1 T6) ===
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
 * uevent_ring.h T25 / EVENT-003 note). */
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
 * URBI_ERR_RESERVED_10 was URBI_ERR_OUT_OF_MEMORY pre-v0.5.5; T8 collapsed
 * the two OOM codes into a single URBI_ERR_OOM at -3.  The slot is held
 * to preserve numeric stability of the surrounding enumerators. */
typedef enum {
    URBI_OK                             =  0,
    URBI_ERR_INVALID_ARG                = -1,
    URBI_ERR_STRAND_FATAL               = -2,
    URBI_ERR_OOM                        = -3,
    /* URBI_ERR_BYTECODE_VERSION_MISMATCH: returned by the public-API
     * translation helper urbi_load_translate_load_err when the internal
     * loader reports ULOAD_UNSUPPORTED_VERSION (see src/module/uchunk.c).
     * The deserialize-bytes entry point itself is still M6 work in
     * progress; the translation helper exists now so any future caller
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
    URBI_ERR_SHAPE_BOUNDS               = -13,  /* T68: slot index past v1.0 packed-flag cap */
    URBI_ERR_PROTO_DEPTH                = -14,  /* T68: prototype-graph resolve-stack overflow */
    /* URBI_ERR_STDLIB_BOOT_FAILED: returned by urbi_stdlib_boot (and any
     * caller that propagates it) when the embedded stdlib bytecode blob
     * fails to deserialize or bind during VM/realm bootstrap.  Distinct
     * from URBI_ERR_OOM (which signals allocation failure during boot)
     * and URBI_ERR_BYTECODE_VERSION_MISMATCH (which is reachable through
     * urbi_load_translate_load_err for the file-load surface).  M6
     * Phase 4 reserves this code; the empty-blob path never reaches it. */
    URBI_ERR_STDLIB_BOOT_FAILED         = -15,
    /* URBI_ERR_API_VERSION_MISMATCH: returned by urbi_aux_check_version
     * (lands in Phase 2 T13) when the runtime library's API version
     * disagrees with what the embedder compiled against. MINOR-additive
     * per the bump policy in <urbi/version.h>. */
    URBI_ERR_API_VERSION_MISMATCH       = -16,
    /* URBI_ERR_EVENT_NAME_TAKEN: returned by urbi_event_register (Gap B)
     * when name is already registered in the event registry for this VM. */
    URBI_ERR_EVENT_NAME_TAKEN           = -17,
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
    URBI_ERR_LOADER_BUDGET              = -20
} UErrCode;

/* URBI_ERR_WATCHER_UNREGISTER: sentinel return code for urbi_watcher_fn
 * callbacks (Gap J, v0.7.1).  A host-side watcher callback returns this
 * value to request auto-unregistration after this firing.  It is NOT a
 * UErrCode enum member because it must pass through the `int` return of
 * urbi_watcher_fn without conflicting with URBI_OK (0) or any negative
 * error.  Chosen as -18 (first slot outside UErrCode range, held by
 * Sub-Bundle 2 for this purpose). */
#define URBI_ERR_WATCHER_UNREGISTER  (-18)

/* === UExecStatus: strand-level execution status ===
 *
 * Mirror of the internal enum at src/sched/ustrand.h.  Numeric values are
 * not pinned cross-version; the enum is purely symbolic. */
typedef enum {
    UEXEC_OK       = 0,
    UEXEC_RETURN,
    UEXEC_THROW,
    UEXEC_TAG_STOP,
    UEXEC_CANCEL
} UExecStatus;

/* === UVMError: VM-run result code ===
 *
 * Returned by urbi_vm_run.  Mirror of the internal enum at src/vm/uvm.h. */
typedef enum {
    UVM_OK         = 0,
    UVM_TYPE_ERROR,
    UVM_OOM
} UVMError;

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

#endif /* URBI_TYPES_H */
