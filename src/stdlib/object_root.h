/* SPDX-License-Identifier: BSD-3-Clause */
/* object_root.h — stdlib boot: Object root C-native methods.
 *
 * This module lands the nine root-level Object
 * methods as C-native UClosures: setSlot, getSlot, hasSlot, removeSlot,
 * clone, addProto, removeProto, protos, setProtos.
 *
 * Calling convention: each method matches the urbi_native_method_fn
 * signature defined in runtime/uclosure.h:
 *
 *   int (*)(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
 *
 * - vm: the current VM.
 * - self: the receiver (`this`).  For 1.clone(), self.kind == UVAL_INT.
 *         The receiver is supplied by the OP_CALL native dispatch path,
 *         which reads it from R[A+1] when the call site is method-flagged
 *         (preceded by OP_SELF, v1.6 S42); plain calls pass nil.
 * - args: caller's argument array, count == nargs.
 * - nargs: number of arguments.
 * - out: where to write the return value.
 *
 * Return:
 *   UEXEC_OK   — success; *out holds the return value.
 *   UEXEC_THROW — error raised; *out is left at urbi_make_nil() and the
 *                 OP_CALL arm propagates a TypeError to the strand.
 *
 * Error helpers (urbi_raise_arity / _type / _oom / _lookup) return
 * UEXEC_THROW; they clone the matching cached Exception-subclass proto
 * (via urbi_raise_typed) so scripted try/catch can intercept them. */

#ifndef URBI_STDLIB_OBJECT_ROOT_H
#define URBI_STDLIB_OBJECT_ROOT_H

#include <stdint.h>
#include <stddef.h>             /* size_t */

#include "runtime/uclosure.h"   /* urbi_native_method_fn typedef */
#include "value/uintern.h"      /* ustr_intern */
#include "urbi/types.h"         /* UValue, urbi_make_nil, UVAL_STR */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct UClosure;
struct UObject;
struct USymbol;

/* === urbi_native_closure_create ===
 *
 * Allocate a GC-managed UClosure whose native_fn pointer is set; proto /
 * proto_inst / upvals are NULL.  The GC sweep reclaims it when it becomes
 * unreachable (v0.8.4 Step C-3: migrated from alloc_fn + stdlib_closures
 * to urbi_gc_alloc, matching bytecode closures).
 *
 * Returns NULL on OOM. */
struct UClosure *urbi_native_closure_create(struct UVM *vm,
                                            urbi_native_method_fn fn);

/* === Shared native-method installer ======================================
 *
 * UNativeMethodDef: one {name, fn} entry for a method table.
 *
 * urbi_install_native_methods: installs table[0..count) as UVAL_CLOSURE
 * slots on proto.  Returns URBI_OK on success, URBI_ERR_OOM on any
 * allocation or intern failure (stops at the failing entry; earlier entries
 * stay installed — matches every caller's existing behavior).
 *
 * URBI_REGISTER_METHODS: convenience macro that infers count via sizeof. */

typedef struct {
    const char           *name;
    urbi_native_method_fn fn;
} UNativeMethodDef;

int urbi_install_native_methods(struct UVM *vm, struct UObject *proto,
                                const UNativeMethodDef *table, size_t count);

#define URBI_REGISTER_METHODS(vm, proto, tbl) \
    urbi_install_native_methods((vm), (proto), (tbl), \
                                sizeof(tbl) / sizeof((tbl)[0]))

/* === urbi_object_root_register ===
 *
 * Install the nine Object root C-native methods on vm->atom_object.
 * Idempotent: re-installs are silently overwritten (acceptable for the
 * current scaffolding; a future C-API iteration may tighten this).
 *
 * Returns URBI_OK on success or URBI_ERR_OOM on allocation failure.  On
 * failure, partial registration is acceptable since the same closure
 * bookkeeping list owns the partials; urbi_vm_destroy reclaims them. */
int urbi_object_root_register(struct UVM *vm);

/* === Native-method error helpers ===
 *
 * Each builds a typed Exception instance (via urbi_raise_typed) into *out and
 * returns UEXEC_THROW.  The UEXEC_THROW return is load-bearing: the OP_CALL
 * arm and operator-overload dispatch treat the nonzero code as raise/miss.
 * Only the *out value changed (was nil placeholder pre-v0.11.4). */
int urbi_raise_arity(struct UVM *vm, const char *fn_name,
                     uint8_t expected, uint8_t got, UValue *out);
int urbi_raise_type(struct UVM *vm, const char *msg, UValue *out);
int urbi_raise_oom(struct UVM *vm, UValue *out);
int urbi_raise_lookup(struct UVM *vm, struct USymbol *name, UValue *out);

/* v0.13.5: typed subclass raise helpers.  Same convention as
 * urbi_raise_type — build "<Subclass>: <msg>" and clone the cached proto.
 * INTERNAL (Tier-4 internal-leak), not public ABI surface. */
int urbi_raise_index(struct UVM *vm, const char *msg, UValue *out);
int urbi_raise_range(struct UVM *vm, const char *msg, UValue *out);
int urbi_raise_divzero(struct UVM *vm, const char *msg, UValue *out);

/* urbi_raise_typed — INTERNAL (not in the public ABI manifest).
 *
 * Clone exc_proto (a cached Exception-subclass proto), bind a `message`
 * string slot from msg, and write the typed instance into *out.  Returns
 * UEXEC_THROW.  Degraded fallback (vm/exc_proto NULL or clone OOM): leaves
 * *out = nil and prints msg where hosted. */
int urbi_raise_typed(struct UVM *vm, struct UObject *exc_proto,
                     UValue *out, const char *msg);

/* === urbi_proto_list_create ===
 *
 * Phase 3 synthetic helper: build a UObject that exposes a receiver's
 * proto chain as accessible-via-.size.  This remains a synthetic; a
 * proper List atom backing for the .protos view is a deferred follow-up.
 * Returns NULL on OOM. */
struct UObject *urbi_proto_list_create(struct UVM *vm, struct UObject *recv);

/* === Shared VM-dependent string constructor ===
 *
 * urbi_val_str_intern: intern s[0..n) into the VM's string table and return
 * a UVAL_STR UValue.  Needs a live vm (unlike the pure urbi_make_* inlines
 * in urbi/types.h).  On OOM sets *oom (when non-NULL) and returns nil.
 *
 * Three stdlib files (atoms.c, namespaces.c, primitives.c) had identical
 * private copies of this body; this shared inline replaces them all (C2/GV-01). */
static inline UValue
urbi_val_str_intern(struct UVM *vm, const char *s, size_t n, int *oom)
{
    UValue v = urbi_make_nil();
    USymbol *sym = (USymbol *)ustr_intern(vm, s, n);
    if (sym == NULL) {
        if (oom != NULL) *oom = 1;
        return v;
    }
    v.kind = (uint8_t)UVAL_STR;
    v.v.p  = sym;
    return v;
}

/* === Per-method prologue opt-in macros ===
 *
 * Fold the two-line arity/self-kind prologues that appear ~150 times across
 * the four stdlib files into concise, verifiable call sites.  Both macros
 * expand to EXACTLY the existing per-site idiom — same raise helper, same
 * error type, same message bytes — so converting a site is behavior-neutral.
 *
 * URBI_CHECK_ARITY(vm, fname, want, nargs, out)
 *   Equivalent to:
 *     if ((nargs) != (want)) return urbi_raise_arity((vm),(fname),(uint8_t)(want),(nargs),(out));
 *   Only converts sites using the simple != comparison; sites with >, <,
 *   or compound checks (like String.format's spec-count != argc) stay inline.
 *
 * URBI_CHECK_SELF(vm, self, kindconst, msg, out)
 *   Equivalent to:
 *     if ((self).kind != (uint8_t)(kindconst)) return urbi_raise_type((vm),(msg),(out));
 *   Only converts single-value kind checks; combined || checks stay inline. */
#define URBI_CHECK_ARITY(vm, fname, want, nargs, out) \
    do { \
        if ((nargs) != (uint8_t)(want)) \
            return urbi_raise_arity((vm), (fname), (uint8_t)(want), (nargs), (out)); \
    } while (0)

#define URBI_CHECK_SELF(vm, self, kindconst, msg, out) \
    do { \
        if ((self).kind != (uint8_t)(kindconst)) \
            return urbi_raise_type((vm), (msg), (out)); \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_OBJECT_ROOT_H */
