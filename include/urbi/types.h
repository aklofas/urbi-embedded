/* SPDX-License-Identifier: BSD-3-Clause */
/* include/urbi/types.h
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

/* === urbi_value_nil: canonical zero-init UValue ===
 *
 * Sole nil constructor: explicitly clears kind + pad + union payload so the
 * resulting UValue is bit-equivalent across compilers (some C99 aggregate-
 * init forms can leave _pad in implementation-defined state when the union
 * is partially initialised).
 *
 * Use this helper everywhere a "nil" UValue is needed instead of
 * `UValue v = {0};` aggregate init.  Closes FOUND-019 + FOUND-048 (Wave 5). */
static inline UValue urbi_value_nil(void) {
    UValue v;
    v.kind = (uint8_t)UVAL_NIL;
    for (size_t i = 0; i < sizeof(v._pad); i++) v._pad[i] = 0;
    v.v.i = 0;
    return v;
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
_Static_assert(sizeof(UValue) == 16,
               "UValue must be exactly 16 bytes (ABI pin)");
_Static_assert(offsetof(UValue, v) == 8,
               "UValue.v must be at offset 8 (ABI pin)");
_Static_assert(offsetof(UValue, kind) == 0,
               "UValue.kind must be at offset 0 (ABI pin)");
#endif

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
    URBI_ERR_API_VERSION_MISMATCH       = -16
} UErrCode;

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
