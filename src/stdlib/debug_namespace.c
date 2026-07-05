/* SPDX-License-Identifier: BSD-3-Clause */
/* src/stdlib/debug_namespace.c — Debug urbiscript namespace (v0.9.1 Task 22).
 *
 * Each Debug.X() method calls the corresponding urbi_introspect_X primitive
 * into a scratch buffer, then returns the JSON output as a urbi String.
 *
 * Why a string and not a Dict/List?  v1.0 has no UVAL_LIST or UVAL_DICT
 * — lists and dicts are UObjects in the URBI_ATOM_LIST / URBI_ATOM_DICT
 * families with their methods backed by C-native impls in containers.c.
 * Constructing a List/Dict from C requires walking those internals
 * imperatively, which would significantly inflate the v0.9.1 patch.
 * The string-return contract matches the dispatcher's introspect-op JSON
 * envelope verbatim — clients that need structured access either parse
 * the string client-side or use the dispatcher path directly.  v1.x can
 * upgrade to first-class structured returns when the container surface
 * grows a C-allocator entry point. */

#ifdef URBI_ENABLE_REPL

#include "stdlib/debug_namespace.h"
#include "stdlib/object_root.h"   /* urbi_native_closure_create + raise helpers */

#include "chunk/uchunk.h"
#include "object/uobject.h"
#include "realm/urealm.h"
#include "repl/ujson.h"
#include "repl/urepl_introspect.h"
#include "runtime/uclosure.h"
#include "runtime/umacros.h"
#include "runtime/uperf.h"        /* urbi_perf_reset — Debug.profileReset */
#include "sched/ustrand.h"        /* UEXEC_* */
#include "urbi/trace.h"           /* URBI_TP_STR — Debug.trace marker */
#include "urbi/object.h"          /* URBI_ATOM_OBJECT */
#include "urbi/types.h"
#include "urbi/urbi.h"
#include "value/uintern.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Scratch buffer for each introspect call.  16 KiB is large enough for
 * the worst-case "watchers on a busy realm" output; if a future workload
 * needs more we can bump this or fall back to heap-alloc. */
#define DEBUG_BUF_SIZE  16384

/* Convert a JSON byte slice into a urbi String UValue.  Defensively
 * validates the JSON via ujson_parse — if it doesn't parse we treat the
 * introspect output as corrupt and return nil.  This is a belt-and-braces
 * check; the primitives are unit-tested separately. */
static UValue
debug_emit_string(UVM *vm, const char *json, size_t len)
{
    UValue nil = urbi_make_nil();
    if (json == NULL || len == 0) return nil;
    UJsonNode *root = NULL;
    UJsonErr err = UJSON_OK;
    if (ujson_parse(json, len, &root, &err) != 0) {
        /* Validate-only.  Discard. */
        return nil;
    }
    ujson_free_node(root);

    /* Intern and wrap. */
    USymbol *sym = (USymbol *)ustr_intern(vm, json, len);
    if (sym == NULL) return nil;
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_STR;
    v.v.p = sym;
    return v;
}

/* --- The 7 zero-arg native methods ------------------------------------ */

#define DEBUG_NATIVE_ZEROARG(NAME, FN_CALL)                                 \
static int                                                                  \
debug_##NAME##_native(UVM *vm, UValue self, UValue *args, uint8_t nargs,    \
                      UValue *out)                                          \
{                                                                            \
    (void)self; (void)args;                                                  \
    if (nargs != 0) return urbi_raise_arity(vm, "Debug." #NAME, 0, nargs, out); \
    char buf[DEBUG_BUF_SIZE];                                                \
    size_t n = 0;                                                            \
    if ((FN_CALL) != URBI_OK) { *out = urbi_make_nil(); return UEXEC_OK; }   \
    *out = debug_emit_string(vm, buf, n);                                    \
    return UEXEC_OK;                                                         \
}

DEBUG_NATIVE_ZEROARG(coros,    urbi_introspect_coros   (vm, buf, sizeof(buf), &n))
DEBUG_NATIVE_ZEROARG(tags,     urbi_introspect_tags    (vm, buf, sizeof(buf), &n))
DEBUG_NATIVE_ZEROARG(watchers, urbi_introspect_watchers(vm, buf, sizeof(buf), &n))
DEBUG_NATIVE_ZEROARG(events,   urbi_introspect_events  (vm, buf, sizeof(buf), &n))
DEBUG_NATIVE_ZEROARG(profile,  urbi_introspect_profile (vm, buf, sizeof(buf), &n))
DEBUG_NATIVE_ZEROARG(gc,       urbi_introspect_gc      (vm, buf, sizeof(buf), &n))
DEBUG_NATIVE_ZEROARG(lobbies,  urbi_introspect_lobbies (vm, buf, sizeof(buf), &n))
DEBUG_NATIVE_ZEROARG(memCheck, urbi_introspect_memcheck(vm, buf, sizeof(buf), &n))

/* --- One-arg natives: stack(coro_id), slots(path) --------------------- */

static int
debug_stack_native(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                   UValue *out)
{
    (void)self;
    if (nargs != 1) return urbi_raise_arity(vm, "Debug.stack", 1, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Debug.stack: coro_id must be Integer", out);
    char buf[DEBUG_BUF_SIZE];
    size_t n = 0;
    if (urbi_introspect_stack(vm, (uint32_t)args[0].v.i,
                              buf, sizeof(buf), &n) != URBI_OK) {
        *out = urbi_make_nil();
        return UEXEC_OK;
    }
    *out = debug_emit_string(vm, buf, n);
    return UEXEC_OK;
}

static int
debug_slots_native(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                   UValue *out)
{
    (void)self;
    if (nargs != 1) return urbi_raise_arity(vm, "Debug.slots", 1, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "Debug.slots: path must be String", out);
    const char *path = (const char *)args[0].v.p;
    if (path == NULL) {
        *out = urbi_make_nil();
        return UEXEC_OK;
    }
    URealm *r = NULL;
    if (vm->cur_strand != NULL && vm->cur_strand->realm != NULL) {
        r = vm->cur_strand->realm;
    } else {
        r = urbi_realm_global(vm);
    }
    char buf[DEBUG_BUF_SIZE];
    size_t n = 0;
    if (urbi_introspect_slots(vm, r, path, strlen(path),
                              buf, sizeof(buf), &n) != URBI_OK) {
        *out = urbi_make_nil();
        return UEXEC_OK;
    }
    *out = debug_emit_string(vm, buf, n);
    return UEXEC_OK;
}

/* --- Debug.trace(label): inject a USER-channel trace marker ------------ */

static int
debug_trace_native(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                   UValue *out)
{
    (void)self;
    if (nargs != 1) return urbi_raise_arity(vm, "Debug.trace", 1, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "Debug.trace: label must be String", out);
    {
        const char *s = (const char *)args[0].v.p;
        if (s != NULL)
            URBI_TP_STR(vm, URBI_TRACE_USER, URBI_LOG_INFO,
                        URBI_TP_USER_MARKER, s, 0);
    }
    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* --- Debug.profileReset(): zero VM-domain perf counters, bump epoch ---- */

static int
debug_profile_reset_native(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                           UValue *out)
{
    (void)self; (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Debug.profileReset", 0, nargs, out);
#if URBI_PERF_COUNTERS
    urbi_perf_reset(vm);
#endif
    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* --- Method table --------------------------------------------------------- */

static const UNativeMethodDef DEBUG_METHODS[] = {
    { "coros",    debug_coros_native    },
    { "tags",     debug_tags_native     },
    { "watchers", debug_watchers_native },
    { "events",   debug_events_native   },
    { "profile",  debug_profile_native  },
    { "gc",       debug_gc_native       },
    { "lobbies",  debug_lobbies_native  },
    { "stack",    debug_stack_native    },
    { "slots",    debug_slots_native    },
    { "trace",    debug_trace_native    },
    { "profileReset", debug_profile_reset_native },
    { "memCheck", debug_memCheck_native }
};
int
urbi_debug_namespace_register(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->debug_proto != NULL) return URBI_OK;  /* idempotent */
    UObject *proto = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
    if (proto == NULL) return URBI_ERR_OOM;
    /* Bind via VM pointer BEFORE install so the GC walker sees the cell
     * if install triggers a collection. */
    vm->debug_proto = proto;
    int rc = URBI_REGISTER_METHODS(vm, proto, DEBUG_METHODS);
    if (rc != URBI_OK) {
        /* Leave the (partially-populated) proto attached; GC will reap it
         * when subsequent VM destroy walks the chain.  Surfacing OOM via
         * the rc is sufficient. */
        return rc;
    }
    return URBI_OK;
}

int
urbi_debug_namespace_register_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->debug_proto == NULL) return URBI_OK;  /* no proto → nothing to bind */
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_OBJECT;
    v.v.p = vm->debug_proto;
    return urbi_realm_set_global(vm, realm, "Debug", 5, v);
}

#else  /* !URBI_ENABLE_REPL */

/* When REPL is disabled this TU has no symbols.  ISO C forbids a
 * translation unit with no declarations; emit a typedef so the file
 * remains pickable by the wildcard glob without tripping -Wpedantic /
 * clang-diagnostic-empty-translation-unit. */
typedef int urbi_debug_namespace_disabled_marker_t;

#endif /* URBI_ENABLE_REPL */
