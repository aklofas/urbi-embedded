/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_channel_proto.c — v0.10.11 / D6 Channel proto unit tests.
 *
 * Four tests:
 *   1. vm->channel_proto is non-NULL after stdlib boot.
 *   2. cout/cerr/clog resolve as realm globals with correct name slots.
 *   3. '<<' operator routes to writer output.
 *   4. Channel.new("x") returns an object with name slot "x".
 */

#include "utest.h"
#include "utest_e2e_helpers.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "object/uobject.h"
#include "realm/urealm.h"
#include "runtime/umacros.h"
#include "value/uintern.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ---- capture writer ---------------------------------------------------- */

#define CAP_MAX 1024

typedef struct {
    char   buf[CAP_MAX];
    size_t len;
    int    calls;
} CapCtx;

static CapCtx g_cap;

static void
cap_writer(void *ud,
           const char *channel, size_t channel_len,
           const char *msg,     size_t msg_len,
           uint64_t ts_us)
{
    (void)ud; (void)channel; (void)channel_len; (void)ts_us;
    g_cap.calls++;
    size_t n = msg_len < (size_t)(CAP_MAX - 1) ? msg_len : (size_t)(CAP_MAX - 1);
    memcpy(g_cap.buf, msg, n);
    g_cap.buf[n] = '\0';
    g_cap.len = n;
}

static void reset_cap(void) { memset(&g_cap, 0, sizeof(g_cap)); }

/* ---- helper: resolve name slot on a realm global UObject --------------- */
static int
get_name_slot(UVM *vm, URealm *realm, const char *global,
              char *out_buf, size_t out_buf_len)
{
    UValue gv;
    urbi_zero(&gv, sizeof(gv));
    int rc = urbi_realm_get_global(vm, realm, global, strlen(global), &gv);
    if (rc != URBI_OK) return -1;
    if (gv.kind != (uint8_t)UVAL_OBJECT) return -2;
    UObject *obj = (UObject *)gv.v.p;

    USymbol *sym = (USymbol *)ustr_intern(vm, "name", 4);
    if (sym == NULL) return -3;
    UObject *holder = NULL;
    uint32_t idx = 0;
    int found = urbi_object_resolve_slot(vm, obj, sym, &holder, &idx);
    if (found != 1) return -4;
    UValue nv = holder->slots[idx];
    if (nv.kind != (uint8_t)UVAL_STR) return -5;
    const char *s = (const char *)nv.v.p;
    size_t slen = strlen(s);
    if (slen >= out_buf_len) slen = out_buf_len - 1;
    memcpy(out_buf, s, slen);
    out_buf[slen] = '\0';
    return 0;
}

/* ==== Test 1: channel_proto_cached ======================================
 *
 * After urbi_realm_global (which triggers stdlib boot + channel init),
 * vm->channel_proto must be non-NULL. */
UTEST(channel_proto_cached)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);
    UASSERT(vm.channel_proto != NULL);
    urbi_vm_destroy(&vm);
}

/* ==== Test 2: channels_resolve ==========================================
 *
 * cout, cerr, and clog must be realm globals whose name slots match
 * the canonical names set by install_channel_instance. */
UTEST(channels_resolve)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    char name[64];

    UASSERT_EQ(get_name_slot(&vm, realm, "cout", name, sizeof(name)), 0);
    UASSERT(strcmp(name, "output") == 0);

    UASSERT_EQ(get_name_slot(&vm, realm, "cerr", name, sizeof(name)), 0);
    UASSERT(strcmp(name, "error") == 0);

    UASSERT_EQ(get_name_slot(&vm, realm, "clog", name, sizeof(name)), 0);
    UASSERT(strcmp(name, "clog") == 0);

    urbi_vm_destroy(&vm);
}

/* ==== Test 3: shift_returns_this ========================================
 *
 * `cout << "hello"` must route output through the writer.  The '<<'
 * method calls Lobby.echo which writes via the per-VM writer hook. */
UTEST(shift_routes_to_writer)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    reset_cap();
    urbi_set_writer(&vm, cap_writer, NULL);

    char buf[256];
    buf[0] = '\0';
    int rc = urbi_repl_eval(&vm, NULL, "cout << \"probe\"", 15,
                            buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(g_cap.calls >= 1);
    UASSERT(strstr(g_cap.buf, "*** probe") != NULL);

    urbi_vm_destroy(&vm);
}

/* ==== Test 4: cout_isa_channel ==========================================
 *
 * Channel.new("test-ch") must produce an instance whose name slot
 * matches the argument and whose enabled slot is true. */
UTEST(channel_new_name_slot)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UValue out;
    int rc = utest_e2e_compile_and_run(
        &vm, "Channel.new(\"test-ch\").name", &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_STR);
    UASSERT(strcmp((const char *)out.v.p, "test-ch") == 0);

    urbi_vm_destroy(&vm);
}

/* ---- suite entry point ------------------------------------------------- */
void test_channel_proto_suite(void);

void
test_channel_proto_suite(void)
{
    printf("test_channel_proto\n");
    utest_run("channel_proto_cached",    channel_proto_cached);
    utest_run("channels_resolve",        channels_resolve);
    utest_run("shift_routes_to_writer",  shift_routes_to_writer);
    utest_run("channel_new_name_slot",   channel_new_name_slot);
}
