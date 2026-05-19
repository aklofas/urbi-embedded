/* SPDX-License-Identifier: BSD-3-Clause */
/* src/stdlib/lobby_native.c — v0.9.1 Phase 5: Lobby proto + native primitive.
 *
 * See lobby_native.h for the contract and rationale.  This TU implements:
 *
 *   __builtin_lobby_send(msg, tag, prefix) — C-native method installed on
 *     the Lobby proto.  Formats "[%08llu:tag] prefix msg\n" (or without
 *     the ":tag" segment when tag is empty) and routes via
 *     urbi_vm_write_in_realm so per-session output reaches the right
 *     client when REPL is enabled, falling through to the VM-wide writer
 *     otherwise.
 *
 *   urbi_lobby_native_register — allocates vm->lobby_proto and installs
 *     the primitive.
 *
 *   urbi_lobby_native_register_globals — binds "Lobby" as a realm-global
 *     pointing at vm->lobby_proto.  Mirrors the M6 Phase 9 post-loop hook
 *     pattern (slots 15+).
 *
 *   urbi_lobby_register_session / unregister_session — host-side mutators
 *     that append/remove a session's global_object on the Lobby.lobbies
 *     List.  No-op when Lobby.lobbies is not yet populated (early init).
 *
 *   urbi_lobby_invoke_handleDisconnect — runs `Realm.handleDisconnect()`
 *     against the session realm via urbi_repl_eval.  `Realm` is the
 *     realm-self-reference (urealm_globals.c's is_self_ref entry), so it
 *     points at the session's global_object — the lobby instance — and
 *     `this` inside the handler resolves to that same instance.  When the
 *     lobby.u overlay has been baked, the default handler fires
 *     onDisconnect!(this).  User-overridden per-instance handlers win
 *     over the proto default via the standard slot-lookup rules.
 *
 * Why a `Realm.handleDisconnect()` script (versus calling the closure
 * directly from C):
 *   - No public C closure-invoke API exists yet (the runtime invokes
 *     closures only via OP_CALL inside a strand).
 *   - urbi_repl_eval is the established programmatic-entry path with
 *     proper strand setup, error capture, and budget enforcement.
 *   - The 26-byte source string compiles in well under the default
 *     URBI_DEFAULT_REPL_BUDGET (256 depth / 100000 nodes / 1 MiB bytes).
 */

#include "stdlib/lobby_native.h"
#include "stdlib/containers.h"        /* urbi_stdlib_list_append_value */
#include "stdlib/object_root.h"       /* urbi_native_closure_create + raise helpers */

#include "module/umodule.h"           /* UValue / UVAL_* */
#include "object/uobject.h"           /* urbi_object_alloc / set_protos_single / set_local_slot / resolve_slot */
#include "realm/urealm.h"             /* URealm + global_object */
#include "runtime/uclosure.h"         /* urbi_native_method_fn */
#include "runtime/umacros.h"          /* urbi_strlen */
#include "sched/ustrand.h"            /* UEXEC_OK / UEXEC_THROW + UStrand */
#include "urbi/object.h"              /* URBI_ATOM_OBJECT */
#include "urbi/types.h"               /* urbi_make_nil */
#include "urbi/urbi.h"                /* URBI_OK / URBI_ERR_* / urbi_realm_set_global / urbi_vm_write_in_realm / urbi_repl_eval */
#include "value/uintern.h"            /* ustr_intern + USymbol */
#include "vm/uvm.h"                   /* UVM */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>                    /* snprintf */
#include <string.h>                   /* strlen */

/* === UValue construction helpers (private; mirror primitives.c) ========= */

static UValue
val_obj_local(UObject *o)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_OBJECT;
    v.v.p  = o;
    return v;
}

/* === __builtin_lobby_send native method =================================
 *
 * Signature on the urbi side: __builtin_lobby_send(msg, tag, prefix) -> nil
 *
 * msg, tag, prefix are expected to be String values (UVAL_STR — interned
 * NUL-terminated C strings via USymbol).  The lobby.u overlay always
 * passes asString(msg) so non-string args go through the standard
 * coercion path before reaching here.
 *
 * Output framing (spec §9.2):
 *   "[%08llu:tag] prefix msg\n"   when tag is non-empty
 *   "[%08llu] prefix msg\n"        when tag is empty
 * where %08llu is ms-since-boot (host_time_us / 1000).
 *
 * Routes through urbi_vm_write_in_realm against the current strand's
 * realm so per-session writers (installed by urepl_session_create) win
 * over the VM-wide writer.  Channel is "clog" — the conventional default
 * for log-style output that mirrors the legacy `echo` / `send` framing. */

static int
builtin_lobby_send(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                   UValue *out)
{
    (void)self;
    if (nargs != 3)
        return urbi_raise_arity(vm, "__builtin_lobby_send", 3, nargs, out);

    /* All three args must be UVAL_STR.  The lobby.u overlay sites that
     * call this primitive guarantee that already, so a violation here
     * indicates a user that resolved the slot by name and called it with
     * non-string args — treat as TypeError to surface the misuse. */
    if (args[0].kind != (uint8_t)UVAL_STR
            || args[1].kind != (uint8_t)UVAL_STR
            || args[2].kind != (uint8_t)UVAL_STR) {
        return urbi_raise_type(vm,
            "__builtin_lobby_send: msg/tag/prefix must be String", out);
    }

    const char *msg    = (const char *)args[0].v.p;
    const char *tag    = (const char *)args[1].v.p;
    const char *prefix = (const char *)args[2].v.p;
    if (msg == NULL || tag == NULL || prefix == NULL) {
        return urbi_raise_type(vm,
            "__builtin_lobby_send: NULL string", out);
    }

    /* host_time_us is non-NULL in hosted builds (uvm_init wires a libc
     * default); freestanding ports without a time source see NULL and we
     * substitute 0 for the timestamp.  v1.0 design contract from §6 of
     * urbi.h. */
    uint64_t ms = 0U;
    if (vm->host_time_us != NULL) {
        ms = vm->host_time_us() / 1000ULL;
    }

    /* Format the framed line into a stack-bounded buffer.  v0.9.1 wire-
     * line cap is 1 MiB per spec §6, but typical script-side echo writes
     * are O(100 B).  Truncate any oversize message with a "[truncated]"
     * marker rather than emitting a partial line. */
    char framed[1024];
    size_t tag_len = strlen(tag);
    int n;
    if (tag_len > 0U) {
        n = snprintf(framed, sizeof(framed),
                     "[%08llu:%s] %s %s\n",
                     (unsigned long long)ms, tag, prefix, msg);
    } else {
        n = snprintf(framed, sizeof(framed),
                     "[%08llu] %s %s\n",
                     (unsigned long long)ms, prefix, msg);
    }
    /* snprintf returns the would-be length on overflow; clamp to the
     * buffer (minus the NUL).  The truncated form drops the trailing
     * newline which is acceptable — the consumer ringbuf line discipline
     * splits on bytes, not on \n. */
    if (n < 0) {
        return urbi_raise_oom(vm, out);
    }
    size_t out_len = (size_t)n;
    if (out_len >= sizeof(framed)) {
        out_len = sizeof(framed) - 1U;
    }

    /* Route via per-realm writer fallback chain (realm -> VM -> default).
     * The current strand's realm is the session's realm when this fires
     * inside a REPL eval (writer points at session->output ringbuf).
     * Outside a REPL session (e.g. unit tests, scripts loaded by the
     * global Realm) cur_strand may be NULL; fall back to vm->global_realm
     * which routes through vm->writer_fn. */
    URealm *r = NULL;
    if (vm->cur_strand != NULL && vm->cur_strand->realm != NULL) {
        r = vm->cur_strand->realm;
    }
    urbi_vm_write_in_realm(vm, r, "clog", 4, framed, out_len);

    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* === Method-table install (single-entry; mirrors atom_protos.c shape) === */

static int
install_native_method(UVM *vm, UObject *proto, const char *name,
                      urbi_native_method_fn fn)
{
    UClosure *cl = urbi_native_closure_create(vm, fn);
    if (cl == NULL) return URBI_ERR_OOM;
    USymbol *sym = (USymbol *)ustr_intern(vm, name, urbi_strlen(name));
    if (sym == NULL) return URBI_ERR_OOM;
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_CLOSURE;
    v.v.p  = cl;
    if (urbi_object_set_local_slot(vm, proto, sym, v) != 0)
        return URBI_ERR_OOM;
    return URBI_OK;
}

/* === urbi_lobby_native_register ========================================= */

int
urbi_lobby_native_register(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->lobby_proto != NULL) return URBI_OK;  /* idempotent */

    UObject *proto = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
    if (proto == NULL) return URBI_ERR_OOM;

    /* Chain Lobby onto root Object so prototype-graph walk past
     * Lobby.echo finds Object.clone / setSlot / etc.  Mirrors the
     * pattern in event_native_register / tag_native_register. */
    UObject *root = urbi_object_root(vm);
    if (root == NULL) return URBI_ERR_OOM;
    urbi_object_set_protos_single(vm, proto, root);

    /* Bind to VM BEFORE installing the method so the GC walker shades
     * the partially populated proto on any allocation that triggers
     * collection during install. */
    vm->lobby_proto = proto;

    return install_native_method(vm, proto, "__builtin_lobby_send",
                                 builtin_lobby_send);
}

/* === urbi_lobby_native_register_globals =================================
 *
 * Post-registry hook: bind "Lobby" as a realm-global on `realm`.  Lands
 * at slots 15+, past the v1.0 packed-flag CONSTANT enforcement range
 * (slots 0..7).  Mirrors urbi_stdlib_register_primitives_globals. */

int
urbi_lobby_native_register_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->lobby_proto == NULL) return URBI_OK;  /* no proto -> nothing to bind */
    return urbi_realm_set_global(vm, realm, "Lobby", 5,
                                 val_obj_local(vm->lobby_proto));
}

/* === Lobby.lobbies maintenance from C side ============================== */

static UObject *
fetch_lobbies_list(UVM *vm)
{
    if (vm == NULL || vm->lobby_proto == NULL) return NULL;
    USymbol *sym = (USymbol *)ustr_intern(vm, "lobbies", 7);
    if (sym == NULL) return NULL;
    UObject *holder = NULL;
    uint32_t idx = 0U;
    int rc = urbi_object_resolve_slot(vm, vm->lobby_proto, sym,
                                      &holder, &idx);
    if (rc != 1 || holder == NULL || holder->slots == NULL) return NULL;
    UValue v = holder->slots[idx];
    if (v.kind != (uint8_t)UVAL_OBJECT || v.v.p == NULL) return NULL;
    return (UObject *)v.v.p;
}

int
urbi_lobby_register_session(UVM *vm, URealm *session_realm)
{
    if (vm == NULL || session_realm == NULL) return URBI_ERR_INVALID_ARG;
    if (session_realm->global_object == NULL) return URBI_OK;

    UObject *lobbies = fetch_lobbies_list(vm);
    /* lobbies is NULL when lobby.u hasn't yet populated Lobby.lobbies
     * (the .u overlay runs at the end of urbi_populate_realm_globals);
     * urbi_stdlib_list_append_value treats NULL as a silent no-op. */
    return urbi_stdlib_list_append_value(vm, lobbies,
                                         val_obj_local(session_realm->global_object));
}

int
urbi_lobby_unregister_session(UVM *vm, URealm *session_realm)
{
    if (vm == NULL || session_realm == NULL) return URBI_ERR_INVALID_ARG;
    if (session_realm->global_object == NULL) return URBI_OK;

    UObject *lobbies = fetch_lobbies_list(vm);
    return urbi_stdlib_list_remove_first_equal(
        vm, lobbies, val_obj_local(session_realm->global_object));
}

/* === handleDisconnect dispatch ==========================================
 *
 * Runs `Realm.handleDisconnect()` against the session's realm via
 * urbi_repl_eval.  `Realm` resolves to realm->global_object (the
 * is_self_ref slot in the realm-globals registry), which is the lobby
 * instance for this session.  Method dispatch walks Object proto chain
 * → Lobby proto → finds the default `handleDisconnect` baked from
 * lobby.u; any user override on the lobby instance wins via standard
 * slot-lookup rules.
 *
 * urbi_repl_eval is hard-gated under URBI_BYTECODE_ONLY (no compiler in
 * those builds), so this function compiles to a no-op there.  Errors
 * from the eval are intentionally dropped — session teardown shouldn't
 * abort because a user-defined cleanup hook faulted. */

int
urbi_lobby_invoke_handleDisconnect(UVM *vm, URealm *session_realm)
{
    if (vm == NULL || session_realm == NULL) return URBI_ERR_INVALID_ARG;

#if !defined(URBI_BYTECODE_ONLY)
    char throwaway[256];
    static const char SCRIPT[] = "Realm.handleDisconnect()";
    /* Ignore the return: any throw or missing-slot is fine — the spec
     * says handleDisconnect is optional + best-effort. */
    (void)urbi_repl_eval(vm, session_realm,
                         SCRIPT, sizeof(SCRIPT) - 1U,
                         throwaway, sizeof(throwaway));
#else
    (void)vm;
    (void)session_realm;
#endif
    return URBI_OK;
}
