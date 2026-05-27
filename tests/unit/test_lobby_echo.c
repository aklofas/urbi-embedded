/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_lobby_echo.c — lobby_echo: W4 fix verification.
 *
 * Verifies that Lobby.echo("msg", "", "***") routes through
 * __builtin_lobby_send to the realm writer.  Pre-W4 this failed with
 * "slot '__builtin_lobby_send' not found" (closure-body bare-name gap).
 *
 * Uses urbi_set_writer + capture_writer to intercept the output without
 * requiring a TCP/pty transport, mirroring the pattern in test_set_writer.c.
 */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ---- capture context --------------------------------------------------- */

#define CAP_MAX 512

typedef struct {
    char     msg[CAP_MAX];
    size_t   msg_len;
    int      call_count;
} LobbyCapture;

static LobbyCapture g_lcap;

static void
lobby_capture_writer(void *ud,
                     const char *channel, size_t channel_len,
                     const char *msg,     size_t msg_len,
                     uint64_t ts_us)
{
    (void)ud;
    (void)channel;
    (void)channel_len;
    (void)ts_us;

    g_lcap.call_count++;
    if (msg_len > (size_t)(CAP_MAX - 1))
        msg_len = (size_t)(CAP_MAX - 1);
    memcpy(g_lcap.msg, msg, msg_len);
    g_lcap.msg[msg_len] = '\0';
    g_lcap.msg_len = msg_len;
}

static void reset_lcap(void) {
    memset(&g_lcap, 0, sizeof(g_lcap));
}

/* ---- T1: Lobby.echo routes to per-VM writer ---------------------------- */
UTEST(lobby_echo_routes_to_writer)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    reset_lcap();
    urbi_set_writer(&vm, lobby_capture_writer, NULL);

    /* urbi_repl_eval drives a full parse+emit+execute cycle, so the
     * baked lobby.u closure body runs end-to-end including the
     * this.__builtin_lobby_send dispatch fixed in W4. */
    char buf[256];
    buf[0] = '\0';
    rc = urbi_repl_eval(&vm, NULL,
                        "Lobby.echo(\"hello\", \"\", \"***\")",
                        30,
                        buf, sizeof(buf));

    UASSERT_EQ(rc, URBI_OK);
    UASSERT(g_lcap.call_count >= 1);
    /* The framed output is "[ts] *** hello\n"; check for the body text. */
    UASSERT(strstr(g_lcap.msg, "*** hello") != NULL);

    urbi_vm_destroy(&vm);
}

/* ---- T2: Lobby.echo with empty prefix ---------------------------------- */
UTEST(lobby_echo_empty_prefix)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    reset_lcap();
    urbi_set_writer(&vm, lobby_capture_writer, NULL);

    char buf[256];
    buf[0] = '\0';
    rc = urbi_repl_eval(&vm, NULL,
                        "Lobby.echo(\"world\", \"\", \"\")",
                        27,
                        buf, sizeof(buf));

    UASSERT_EQ(rc, URBI_OK);
    UASSERT(g_lcap.call_count >= 1);
    UASSERT(strstr(g_lcap.msg, "world") != NULL);

    urbi_vm_destroy(&vm);
}

/* ---- suite entry point ------------------------------------------------- */
void
test_lobby_echo_suite(void)
{
    printf("test_lobby_echo\n");
    utest_run("lobby_echo_routes_to_writer", lobby_echo_routes_to_writer);
    utest_run("lobby_echo_empty_prefix",     lobby_echo_empty_prefix);
}
