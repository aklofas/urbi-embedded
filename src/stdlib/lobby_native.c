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
#include "stdlib/containers.h"        /* urbi_stdlib_list_append_value / urbi_stdlib_list_new_empty */
#include "stdlib/object_root.h"       /* urbi_native_closure_create + raise helpers */

#include "event/uevent.h"             /* urbi_event_create */
#include "event/uevent_native.h"      /* uvalue_from_event */
#include "chunk/uchunk.h"           /* UValue / UVAL_* */
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
#include <string.h>                   /* strlen */

/* Append helpers for the lobby_send framer.  Each writes into buf[*off..cap)
 * if room remains and advances *off by the number of bytes the full value
 * would occupy (mirrors snprintf's "would-be length" return semantics).
 * Hand-rolled so the freestanding liburbi.a doesn't link against stdio.
 *
 * cap is the buffer capacity including the slot reserved for the NUL
 * terminator; callers terminate at min(*off, cap - 1) after the format. */

static void
append_cstr(char *buf, size_t cap, size_t *off, const char *s)
{
    while (*s != '\0') {
        if (*off < cap) buf[*off] = *s;
        (*off)++;
        s++;
    }
}

static void
append_char(char *buf, size_t cap, size_t *off, char c)
{
    if (*off < cap) buf[*off] = c;
    (*off)++;
}

/* Width-8 zero-padded decimal for uint64.  Matches "%08llu":  the 8 is a
 * MINIMUM width, not a truncation cap — values >= 10^8 emit all digits. */
static void
append_u64_pad8(char *buf, size_t cap, size_t *off, uint64_t v)
{
    char tmp[20];                            /* max uint64 = 20 digits */
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (unsigned)(v % 10U));
        v /= 10U;
    } while (v > 0U);
    while (n < 8U) tmp[n++] = '0';           /* zero-pad to min width 8 */
    for (size_t i = n; i > 0; i--) {
        append_char(buf, cap, off, tmp[i - 1]);
    }
}

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
 * NUL-terminated C strings via USymbol).  Non-string args raise TypeError
 * at this boundary; the lobby.u overlay forwards `msg` unchanged (String
 * has no `.asString` method at v0.10.11 — see workspace design-risks
 * v0.10.11-A for the v1.x stdlib gap).
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
        ms = vm->host_time_us(vm->host_time_ud) / 1000ULL;
    }

    /* Format the framed line into a stack-bounded buffer.  v0.9.1 wire-
     * line cap is 1 MiB per spec §6, but typical script-side echo writes
     * are O(100 B).  Truncate any oversize message — the consumer ringbuf
     * line discipline splits on bytes, not on \n. */
    char framed[1024];
    size_t tag_len = strlen(tag);
    size_t off = 0;
    append_char(framed, sizeof framed, &off, '[');
    append_u64_pad8(framed, sizeof framed, &off, ms);
    if (tag_len > 0U) {
        append_char(framed, sizeof framed, &off, ':');
        append_cstr(framed, sizeof framed, &off, tag);
    }
    append_cstr(framed, sizeof framed, &off, "] ");
    append_cstr(framed, sizeof framed, &off, prefix);
    append_char(framed, sizeof framed, &off, ' ');
    append_cstr(framed, sizeof framed, &off, msg);
    append_char(framed, sizeof framed, &off, '\n');

    /* Clamp the writable length to the buffer (minus the NUL slot).
     * `off` is the would-be length, mirroring snprintf semantics. */
    size_t out_len = off;
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

/* === urbi_lobby_native_register =========================================
 *
 * Allocates vm->lobby_proto + installs three slots that are intentionally
 * VM-SINGLETONS (one each per VM, shared across every realm):
 *
 *   __builtin_lobby_send  — native primitive that all Lobby.echo /
 *                           Lobby.wall calls funnel through (per-realm
 *                           writer routing happens inside).
 *   lobbies               — empty List, mutated by urbi_lobby_register_-
 *                           session / unregister_session as REPL sessions
 *                           come and go.
 *   onDisconnect          — Event subscribed to by user code; the default
 *                           handleDisconnect (defined in lobby.u)
 *                           emits this with the session lobby as payload.
 *
 * The script-side methods (echo / wall / handleDisconnect) live on a
 * LobbyMethods proto installed by lobby.u via Lobby.addProto.  That
 * design accepts that lobby.u runs per-realm (so each realm prepends a
 * fresh LobbyMethods to Lobby.protos) — the methods themselves are
 * functionally identical across realms, so most-recent-wins via DFS is
 * a non-issue.  Critically, lobbies + onDisconnect live on Lobby itself
 * (not on LobbyMethods), so they are TRULY shared regardless of how
 * many LobbyMethods get prepended.
 *
 * Pre-mark-readonly: this function runs inside urbi_stdlib_boot before
 * urbi_atom_protos_mark_readonly, so urbi_object_set_local_slot succeeds
 * on the freshly-allocated proto.  After mark_readonly the proto is
 * readonly to urbiscript; C-side mutators (urbi_lobby_register_session)
 * still work because they reach into the List's UList backing directly,
 * not via OP_SETSLOT.  Idempotent: re-entry returns URBI_OK without
 * re-running.  Returns URBI_OK / URBI_ERR_INVALID_ARG / URBI_ERR_OOM. */

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

    /* Bind to VM BEFORE installing slots so the GC walker shades the
     * partially populated proto on any allocation that triggers
     * collection during install. */
    vm->lobby_proto = proto;

    int rc = install_native_method(vm, proto, "__builtin_lobby_send",
                                   builtin_lobby_send);
    if (rc != URBI_OK) return rc;

    /* Install `lobbies` (empty List) — the VM-singleton collection of
     * active sessions.  Containers boot phase already ran (urbi_stdlib_-
     * register_containers fires before lobby_native in stdlib_boot), so
     * the List atom proto + its native methods are available. */
    UObject *lobbies = urbi_stdlib_list_new_empty(vm);
    if (lobbies == NULL) return URBI_ERR_OOM;
    {
        USymbol *sym = (USymbol *)ustr_intern(vm, "lobbies", 7);
        if (sym == NULL) return URBI_ERR_OOM;
        UValue v = urbi_make_nil();
        v.kind = (uint8_t)UVAL_OBJECT;
        v.v.p  = lobbies;
        if (urbi_object_set_local_slot(vm, proto, sym, v) != 0)
            return URBI_ERR_OOM;
    }

    /* Install `onDisconnect` (fresh Event) — shared across every session
     * lobby instance so a subscriber from any realm sees every
     * disconnect.  Event_native already registered at this point
     * (event_native_register fires at urbi_vm_init / urbi_native_-
     * protos_init in urealm_globals.c before urbi_stdlib_boot). */
    {
        struct UEvent *e = urbi_event_create(vm);
        if (e == NULL) return URBI_ERR_OOM;
        USymbol *sym = (USymbol *)ustr_intern(vm, "onDisconnect", 12);
        if (sym == NULL) return URBI_ERR_OOM;
        UValue v = uvalue_from_event(e);
        if (urbi_object_set_local_slot(vm, proto, sym, v) != 0)
            return URBI_ERR_OOM;
    }

    return URBI_OK;
}

/* === urbi_lobby_native_register_globals =================================
 *
 * Post-registry hook: bind "Lobby" as a realm-global on `realm`.  Lands
 * at slots 15+, past the v1.0 packed-flag CONSTANT enforcement range
 * (slots 0..7).  Mirrors urbi_stdlib_register_primitives_globals.
 *
 * v0.10.10 / D7-E: also installs the `connectionTag` slot on
 * vm->lobby_proto pointing at realm->tag — the realm-root UTag — so that
 * the script expression `Lobby.connectionTag` resolves to a UVAL_TAG.
 * The slot is set via urbi_object_set_local_slot (C-side, bypasses the
 * OP_SETSLOT URBI_OBJ_FLAG_READONLY check that fires only on script-
 * side writes).  Idempotent — repeated registration overwrites with the
 * same value.
 *
 * Multi-realm note: vm->lobby_proto is a VM-singleton, so the
 * connectionTag slot value reflects the last realm registered.  Per-
 * session correctness is delivered by urbi_lobby_register_session below,
 * which sets connectionTag on the session's per-instance Lobby
 * (session_realm->global_object) — `this.connectionTag` inside a session
 * always sees the session's own realm->tag.  Single-realm hosts (the
 * embedded default) see the correct value via Lobby.connectionTag. */

int
urbi_lobby_native_register_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->lobby_proto == NULL) return URBI_OK;  /* no proto -> nothing to bind */

    int rc = urbi_realm_set_global(vm, realm, "Lobby", 5,
                                   val_obj_local(vm->lobby_proto));
    if (rc != URBI_OK) return rc;

    /* v0.10.10 / D7-E: install connectionTag slot on vm->lobby_proto
     * pointing at realm->tag.  Honors REVIVAL §14.9 S11 commitment —
     * the auto-cancel-on-disconnect behavior shipped at v0.9.1; the
     * script-visible slot lands here. */
    if (realm->tag != NULL) {
        USymbol *sym = (USymbol *)ustr_intern(vm, "connectionTag", 13);
        if (sym == NULL) return URBI_ERR_OOM;
        UValue tv = urbi_make_nil();
        tv.kind = (uint8_t)UVAL_TAG;
        tv.v.p  = (void *)realm->tag;
        if (urbi_object_set_local_slot(vm, vm->lobby_proto, sym, tv) != 0) {
            return URBI_ERR_OOM;
        }
    }
    return URBI_OK;
}

/* === Lobby.lobbies maintenance from C side ============================== */

static UObject *
fetch_lobbies_list(UVM *vm)
{
    if (vm == NULL || vm->lobby_proto == NULL) return NULL;
    const USymbol *sym = (const USymbol *)ustr_intern(vm, "lobbies", 7);
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

    /* v0.10.10 / D7-E: set the per-session Lobby instance's connectionTag
     * slot pointing at the session's realm-root tag (session_realm->tag).
     * On session disconnect the realm-teardown path at urealm.c stops
     * realm->tag which cascades to every strand tagged under it — the
     * connection-tag is the per-session boundary.  `this.connectionTag`
     * inside the session resolves here (instance wins over Lobby proto). */
    if (session_realm->tag != NULL) {
        USymbol *sym = (USymbol *)ustr_intern(vm, "connectionTag", 13);
        if (sym == NULL) return URBI_ERR_OOM;
        UValue tv = urbi_make_nil();
        tv.kind = (uint8_t)UVAL_TAG;
        tv.v.p  = (void *)session_realm->tag;
        if (urbi_object_set_local_slot(vm, session_realm->global_object,
                                       sym, tv) != 0) {
            return URBI_ERR_OOM;
        }
    }

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
