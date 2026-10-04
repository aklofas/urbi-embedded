/* SPDX-License-Identifier: BSD-3-Clause */
/* lobby_native.c — see stdlib/lobby_native.h.
 *
 * The frame these primitives write is the one the whole corpus reads:
 *
 *   "[<ms>:tag] prefix msg\n"      tag non-empty
 *   "[<ms>] prefix msg\n"          tag empty
 *
 * where <ms> is the host clock in milliseconds, zero-padded to eight
 * digits (a minimum, not a truncation).  The .chk runner strips the
 * bracketed segment before diffing, which is what makes the timestamp
 * safe to carry. */

#include "stdlib/lobby_native.h"

#include "urbi/urbi.h"

/* Appends into buf[*off..cap) when there is room and advances *off by
 * what the full value WOULD take, the way snprintf reports length. */
static void lob_put(char *buf, size_t cap, size_t *off, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (*off < cap) buf[*off] = s[i];
        (*off)++;
    }
}

static void lob_put_char(char *buf, size_t cap, size_t *off, char c)
{ if (*off < cap) buf[*off] = c; (*off)++; }

/* Width-8 zero-padded decimal.  Eight is a MINIMUM: a value past 10^8
 * prints every digit. */
static void lob_put_ms(char *buf, size_t cap, size_t *off, uint64_t v)
{
    char tmp[20];
    size_t n = 0;
    do { tmp[n++] = (char)('0' + (unsigned)(v % 10u)); v /= 10u; } while (v > 0u);
    while (n < 8u) tmp[n++] = '0';
    while (n > 0) lob_put_char(buf, cap, off, tmp[--n]);
}

/* Builds the frame into `framed` and returns its writable length (the
 * buffer's capacity less the NUL slot when the message overflows). */
static size_t lobby_frame(UVM *vm, char *framed, size_t cap,
                          UValue msg, UValue tag, UValue prefix)
{
    uint64_t ms = vm->clock_us ? vm->clock_us(vm->clock_ud) / 1000u : 0u;
    size_t off = 0;
    lob_put_char(framed, cap, &off, '[');
    lob_put_ms(framed, cap, &off, ms);
    if (urbi_str_size(tag) > 0) {
        lob_put_char(framed, cap, &off, ':');
        lob_put(framed, cap, &off, urbi_str_cstr(tag), urbi_str_size(tag));
    }
    lob_put(framed, cap, &off, "] ", 2);
    lob_put(framed, cap, &off, urbi_str_cstr(prefix), urbi_str_size(prefix));
    lob_put_char(framed, cap, &off, ' ');
    lob_put(framed, cap, &off, urbi_str_cstr(msg), urbi_str_size(msg));
    lob_put_char(framed, cap, &off, '\n');
    return off < cap ? off : cap - 1u;
}

/* __builtin_lobby_send(msg, tag, prefix) -> nil.  Writes on the CURRENT
 * realm's writer, so a per-session writer wins over the VM-wide one. */
static int lobby_send(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)nargs;
    if (!urbi_is_str(args[0]) || !urbi_is_str(args[1]) || !urbi_is_str(args[2]))
        return urbi_raise_type(vm, "__builtin_lobby_send: msg/tag/prefix must be String", out);
    char framed[1024];
    size_t len = lobby_frame(vm, framed, sizeof framed, args[0], args[1], args[2]);
    urbi_stdlib_write(vm, "clog", 4, framed, len);
    *out = uv_nil();
    return UEXEC_OK;
}

/* __builtin_lobby_send_to(lobby, msg, tag, prefix) -> nil.  `lobby` is
 * another realm's globals object, as `Lobby.lobbies` hands them out. */
static int lobby_send_to(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)nargs;
    if (args[0].kind != UV_OBJ)
        return urbi_raise_type(vm, "__builtin_lobby_send_to: target must be a lobby", out);
    if (!urbi_is_str(args[1]) || !urbi_is_str(args[2]) || !urbi_is_str(args[3]))
        return urbi_raise_type(vm, "__builtin_lobby_send_to: msg/tag/prefix must be String", out);
    char framed[1024];
    size_t len = lobby_frame(vm, framed, sizeof framed, args[1], args[2], args[3]);
    urbi_stdlib_write_to(vm, (UObject *)args[0].v.p, "clog", 4, framed, len);
    *out = uv_nil();
    return UEXEC_OK;
}

/* echo(msg, tag = "", prefix = "***").
 *
 * A native rather than a script wrapper so the defaults do not depend on
 * default-parameter lowering, and so `echo(1)` prints `1` rather than
 * raising: a non-String argument is rendered the way the REPL renders a
 * value.  That goes through vm->render_value, the same hook
 * Object.asString uses; a build without the formatter leaves it NULL, and
 * echo of a non-String then raises a TypeError, as asString does. */
static int lobby_echo(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    UValue empty = urbi_make_str_interned(vm, "", 0);
    UValue stars = urbi_make_str_interned(vm, "***", 3);
    if (empty.kind == UV_NIL || stars.kind == UV_NIL) return urbi_raise_oom(vm, out);

    UValue a[3];
    a[0] = args[0];
    a[1] = nargs > 1 ? args[1] : empty;
    a[2] = nargs > 2 ? args[2] : stars;
    if (!urbi_is_str(a[0])) {
        if (vm->render_value == NULL)
            return urbi_raise_type(vm, "echo: this build has no value formatter for a non-String message", out);
        char rendered[512];
        size_t n = vm->render_value(vm, a[0], rendered, sizeof rendered);
        a[0] = urbi_make_str(vm, rendered, n);
        if (a[0].kind == UV_NIL) return urbi_raise_oom(vm, out);
    }
    if (!urbi_is_str(a[1]) || !urbi_is_str(a[2]))
        return urbi_raise_type(vm, "echo: tag and prefix must be String", out);
    return lobby_send(vm, self, a, 3, out);
}

const UMethodDef ustdlib_lobby_methods[USTDLIB_LOBBY_NMETHODS] = {
    { "__builtin_lobby_send",    lobby_send,    3, 3 },
    { "__builtin_lobby_send_to", lobby_send_to, 4, 4 },
    { "echo",                    lobby_echo,    1, 3 }
};

/* The session registry.  Created here, maintained in rt/urealm.c. */
int urbi_lobby_init(UVM *vm)
{
    UObject *lobby = urbi_builtin_proto(vm, UP_LOBBY);
    if (!lobby) return URBI_ERR_INVALID_STATE;
    USym *name = usym_cstr(vm, "lobbies");
    if (!name) return URBI_ERR_OOM;
    /* Two steps on purpose.  Growing the slot array allocates and may
     * collect, and a freshly built List is reachable from nothing but a
     * C local; claiming the slot FIRST means the second write only
     * overwrites an existing entry and cannot allocate at all. */
    if (urbi_object_set_local_slot(vm, lobby, name, uv_nil()) < 0) return URBI_ERR_OOM;
    UValue list = urbi_list_new(vm);
    if (list.kind == UV_NIL) return URBI_ERR_OOM;
    if (urbi_object_set_local_slot(vm, lobby, name, list) < 0) return URBI_ERR_OOM;
    return URBI_OK;
}
