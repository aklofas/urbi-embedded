/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_set_writer.c — T39: urbi_set_writer custom-writer routing.
 *
 * Tests:
 *  1. Custom writer receives correct channel + msg + non-zero ts_us when
 *     urbi_vm_write emits on "cout".
 *  2. Custom writer receives channel "cerr" when urbi_vm_write targets cerr.
 *  3. Custom writer receives a user-defined channel name ("mylog").
 *  4. NULL writer restores default behaviour (no crash; default writer
 *     invoked from urbi_vm_write).
 *  5. Custom ud pointer is threaded through unchanged.
 *
 * Note on cout << syntax: the << operator is not yet lexed in v0.7.x.
 * Tests drive the writer via the public urbi_vm_write C function, which
 * is the same routing path urbiscript's cout would use once << lands.
 */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ---- capture context --------------------------------------------------- */

#define CAP_MAX_MSG 256

typedef struct {
    char     channel[64];
    size_t   channel_len;
    char     msg[CAP_MAX_MSG];
    size_t   msg_len;
    uint64_t ts_us;
    void    *ud_received;
    int      call_count;
} WriterCapture;

static WriterCapture g_cap;

static void
capture_writer(void *ud,
               const char *channel, size_t channel_len,
               const char *msg,     size_t msg_len,
               uint64_t ts_us)
{
    g_cap.call_count++;
    g_cap.ud_received = ud;
    g_cap.ts_us       = ts_us;

    if (channel_len > (size_t)(sizeof(g_cap.channel) - 1))
        channel_len = sizeof(g_cap.channel) - 1;
    memcpy(g_cap.channel, channel, channel_len);
    g_cap.channel[channel_len] = '\0';
    g_cap.channel_len = channel_len;

    if (msg_len > (size_t)(CAP_MAX_MSG - 1))
        msg_len = (size_t)(CAP_MAX_MSG - 1);
    memcpy(g_cap.msg, msg, msg_len);
    g_cap.msg[msg_len] = '\0';
    g_cap.msg_len = msg_len;
}

static void reset_cap(void) {
    memset(&g_cap, 0, sizeof(g_cap));
}

/* ---- T39-1: cout channel routed to custom writer ----------------------- */
UTEST(writer_cout_channel_routed)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    reset_cap();
    urbi_set_writer(&vm, capture_writer, NULL);

    urbi_vm_write(&vm, "cout", 4, "hello", 5);

    UASSERT_EQ(g_cap.call_count, 1);
    UASSERT_EQ((long long)g_cap.channel_len, 4LL);
    UASSERT(strcmp(g_cap.channel, "cout") == 0);
    UASSERT_EQ((long long)g_cap.msg_len, 5LL);
    UASSERT(strcmp(g_cap.msg, "hello") == 0);
    /* ts_us must be non-zero: the default clock is installed and working */
    UASSERT(g_cap.ts_us > 0);

    urbi_vm_destroy(&vm);
}

/* ---- T39-2: cerr channel routed with correct channel name -------------- */
UTEST(writer_cerr_channel_routed)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    reset_cap();
    urbi_set_writer(&vm, capture_writer, NULL);

    urbi_vm_write(&vm, "cerr", 4, "oops", 4);

    UASSERT_EQ(g_cap.call_count, 1);
    UASSERT(strcmp(g_cap.channel, "cerr") == 0);
    UASSERT(strcmp(g_cap.msg, "oops") == 0);

    urbi_vm_destroy(&vm);
}

/* ---- T39-3: user-defined channel name passed through unchanged ---------- */
UTEST(writer_custom_channel_routed)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    reset_cap();
    urbi_set_writer(&vm, capture_writer, NULL);

    urbi_vm_write(&vm, "mylog", 5, "data", 4);

    UASSERT_EQ(g_cap.call_count, 1);
    UASSERT(strcmp(g_cap.channel, "mylog") == 0);
    UASSERT(strcmp(g_cap.msg, "data") == 0);

    urbi_vm_destroy(&vm);
}

/* ---- T39-4: NULL writer restores default (no crash, urbi_vm_write safe) */
UTEST(writer_null_restores_default)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    /* Install custom writer, then restore default with NULL. */
    urbi_set_writer(&vm, capture_writer, NULL);
    urbi_set_writer(&vm, NULL, NULL);

    /* After restore, urbi_vm_write must not crash (default writer is active).
     * We cannot observe stdout here but we can verify no crash + the custom
     * writer is no longer called. */
    reset_cap();
    urbi_vm_write(&vm, "cout", 4, "restored", 8);
    UASSERT_EQ(g_cap.call_count, 0);   /* custom writer not invoked */

    urbi_vm_destroy(&vm);
}

/* ---- T39-5: ud pointer threaded through unchanged ---------------------- */
UTEST(writer_ud_pointer_threaded)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    int sentinel = 42;
    reset_cap();
    urbi_set_writer(&vm, capture_writer, &sentinel);

    urbi_vm_write(&vm, "cout", 4, "ud-test", 7);

    UASSERT_EQ(g_cap.call_count, 1);
    UASSERT(g_cap.ud_received == (void *)&sentinel);

    urbi_vm_destroy(&vm);
}

/* ---- suite entry point ------------------------------------------------- */
void
test_set_writer_suite(void)
{
    printf("test_set_writer\n");
    utest_run("writer_cout_channel_routed",  writer_cout_channel_routed);
    utest_run("writer_cerr_channel_routed",  writer_cerr_channel_routed);
    utest_run("writer_custom_channel_routed", writer_custom_channel_routed);
    utest_run("writer_null_restores_default", writer_null_restores_default);
    utest_run("writer_ud_pointer_threaded",  writer_ud_pointer_threaded);
}
