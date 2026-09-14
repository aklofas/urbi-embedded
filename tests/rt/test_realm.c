/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/rt/test_realm.c — realms, the boot table, and legacy top-level
 * `var` scoping.
 *
 * Unlike the other rt suites these go through the PUBLIC API (urbi_open /
 * urbi_run / urbi_global_get), because what is being pinned is the shape
 * an embedder sees: one VM root object holding every built-in, one
 * globals object per realm inheriting from it, and a chunk-top `var`
 * landing on that globals object rather than in a register. */

#include <stdlib.h>
#include <string.h>

#include "rtest.h"
#include "rt/uboot.h"
#include "urbi/urbi.h"

/* --- a counting allocator, for the boot-heap and realm-cost gates ----- */

typedef struct { size_t live, peak; unsigned long allocs; } CountAlloc;

/* realloc-shaped, with an 16-byte header carrying the block size so the
 * accounting survives free and resize. */
static void *counting_alloc(void *ptr, size_t n, void *ud)
{
    CountAlloc *ca = (CountAlloc *)ud;
    size_t *hdr = ptr ? ((size_t *)ptr) - 2 : NULL;
    size_t old = hdr ? hdr[0] : 0;
    if (n == 0) {
        if (hdr) { ca->live -= old; free(hdr); }
        return NULL;
    }
    size_t *nh = (size_t *)realloc(hdr, n + 2 * sizeof(size_t));
    if (!nh) return NULL;
    nh[0] = n;
    ca->live += n - old;
    ca->allocs++;
    if (ca->live > ca->peak) ca->peak = ca->live;
    return (void *)(nh + 2);
}

static UVM *open_counted(CountAlloc *ca)
{
    memset(ca, 0, sizeof *ca);
    return urbi_open(counting_alloc, ca, NULL);
}

/* --- helpers ---------------------------------------------------------- */

static UValue run(UVM *vm, URealm *r, const char *src)
{
    UValue out = urbi_make_nil();
    char err[256] = { 0 };
    int rc = urbi_run(vm, r, src, strlen(src), "<test>", &out, err, sizeof err);
    if (rc != URBI_OK) {
        UErrorInfo info;
        urbi_last_error(vm, &info);
        printf("    run(%s) failed rc=%d err=%s last=%s\n", src, rc, err,
               info.message ? info.message : "");
    }
    RT_EQ(rc, URBI_OK);
    return out;
}

static bool str_is(UValue v, const char *want)
{
    if (v.kind != UV_STR && v.kind != UV_SYM) return false;
    uint32_t n;
    const char *b = uv_str_bytes(v, &n);
    return n == strlen(want) && memcmp(b, want, n) == 0;
}

/* --- realms ------------------------------------------------------------ */

static void t_realm_shape(void)
{
    CountAlloc ca;
    UVM *vm = open_counted(&ca);
    RT_CHECK(vm != NULL);
    if (!vm) return;

    URealm *m = urbi_realm_main(vm);
    RT_CHECK(m != NULL);
    /* Every built-in lives on the single VM root object, and the realm's
     * globals inherits from it rather than owning a copy. */
    RT_CHECK(vm->root_globals != NULL);
    RT_CHECK(m->globals != NULL);
    RT_EQ(m->globals->proto0, vm->root_globals);
    RT_EQ(m->vm, vm);
    /* The scheduler task creates the root tag; until then it is documented
     * as absent, not forgotten. */
    RT_CHECK(m->root_tag == NULL);

    urbi_close(vm);
}

static void t_realm_is_cheap(void)
{
    CountAlloc ca;
    UVM *vm = open_counted(&ca);
    if (!vm) { RT_CHECK(0); return; }
    (void)urbi_realm_main(vm);

    unsigned long before = ca.allocs;
    URealm *r2 = urbi_realm_new(vm);
    unsigned long cost = ca.allocs - before;
    RT_CHECK(r2 != NULL);
    printf("    realm creation: %lu allocations\n", cost);
    /* The realm cell, its globals object, and the one slot block the
     * `Realm` self-reference forces into existence.  Everything else a
     * realm can see is shared through root_globals. */
    RT_CHECK(cost < 5);

    urbi_close(vm);
}

static void t_realms_are_isolated(void)
{
    CountAlloc ca;
    UVM *vm = open_counted(&ca);
    if (!vm) { RT_CHECK(0); return; }
    URealm *a = urbi_realm_main(vm);
    URealm *b = urbi_realm_new(vm);
    RT_CHECK(b != NULL);
    if (!b) { urbi_close(vm); return; }

    (void)run(vm, a, "var x = 5");
    UValue v = urbi_make_nil();
    RT_EQ(urbi_global_get(vm, a, "x", &v), URBI_OK);
    RT_EQ(v.kind, UV_INT);
    RT_EQ(v.v.i, 5);
    /* A second realm does not see the first realm's top-level var... */
    RT_CHECK(urbi_global_get(vm, b, "x", &v) != URBI_OK);
    /* ...but does see the shared built-ins. */
    RT_EQ(urbi_global_get(vm, b, "Object", &v), URBI_OK);
    RT_EQ(v.kind, UV_OBJ);

    urbi_close(vm);
}

static void t_realm_self_reference(void)
{
    CountAlloc ca;
    UVM *vm = open_counted(&ca);
    if (!vm) { RT_CHECK(0); return; }
    URealm *a = urbi_realm_main(vm);
    URealm *b = urbi_realm_new(vm);
    if (!b) { RT_CHECK(0); urbi_close(vm); return; }

    /* `Realm` must be per-realm: each one names its OWN globals, which is
     * why the slot cannot live on the shared root object. */
    UValue ra = urbi_make_nil(), rb = urbi_make_nil();
    RT_EQ(urbi_global_get(vm, a, "Realm", &ra), URBI_OK);
    RT_EQ(urbi_global_get(vm, b, "Realm", &rb), URBI_OK);
    RT_EQ(ra.kind, UV_OBJ);
    RT_EQ((UObject *)ra.v.p, a->globals);
    RT_EQ((UObject *)rb.v.p, b->globals);
    RT_CHECK(ra.v.p != rb.v.p);

    UValue v = run(vm, a, "var q = 7; Realm.q");
    RT_EQ(v.kind, UV_INT);
    RT_EQ(v.v.i, 7);

    urbi_close(vm);
}

/* --- legacy top-level var --------------------------------------------- */

static void t_toplevel_var_is_a_global_slot(void)
{
    CountAlloc ca;
    UVM *vm = open_counted(&ca);
    if (!vm) { RT_CHECK(0); return; }
    URealm *r = urbi_realm_main(vm);

    UValue v = run(vm, r, "var x = 5; x + 1");
    RT_EQ(v.kind, UV_INT);
    RT_EQ(v.v.i, 6);

    /* The slot outlives the chunk that declared it. */
    UValue g = urbi_make_nil();
    RT_EQ(urbi_global_get(vm, r, "x", &g), URBI_OK);
    RT_EQ(g.kind, UV_INT);
    RT_EQ(g.v.i, 5);

    /* A later, separate chunk reads and rebinds the same slot. */
    v = run(vm, r, "x");
    RT_EQ(v.v.i, 5);
    (void)run(vm, r, "var x = 11");
    RT_EQ(urbi_global_get(vm, r, "x", &g), URBI_OK);
    RT_EQ(g.v.i, 11);

    /* Bare `x` and `Realm.x` are the same slot, in both directions. */
    v = run(vm, r, "Realm.x");
    RT_EQ(v.v.i, 11);
    (void)run(vm, r, "Realm.x = 12");
    v = run(vm, r, "x");
    RT_EQ(v.v.i, 12);

    urbi_close(vm);
}

static void t_toplevel_var_from_a_closure(void)
{
    CountAlloc ca;
    UVM *vm = open_counted(&ca);
    if (!vm) { RT_CHECK(0); return; }
    URealm *r = urbi_realm_main(vm);

    /* A closure made at chunk top reads the global slot... */
    UValue v = run(vm, r, "var n = 3; var read = function () { n }; read()");
    RT_EQ(v.kind, UV_INT);
    RT_EQ(v.v.i, 3);

    /* ...and writes it. */
    v = run(vm, r, "var n = 3; var bump = function () { n = n + 1 }; bump(); bump(); n");
    RT_EQ(v.kind, UV_INT);
    RT_EQ(v.v.i, 5);

    /* A closure compiled in a LATER chunk still reaches the slot the
     * earlier chunk declared — the REPL types one chunk per line. */
    (void)run(vm, r, "var k = 40");
    v = run(vm, r, "var peek = function () { k + 2 }; peek()");
    RT_EQ(v.v.i, 42);

    /* And writes it, from a chunk that never saw the declaration.  This
     * is the case the REPL hits constantly: `var x = 5` on one line,
     * `x = 9` on the next. */
    (void)run(vm, r, "k = 41");
    v = run(vm, r, "k");
    RT_EQ(v.v.i, 41);
    (void)run(vm, r, "var poke = function () { k = 7 }; poke()");
    v = run(vm, r, "k");
    RT_EQ(v.v.i, 7);

    urbi_close(vm);
}

/* --- the boot table ---------------------------------------------------- */

static void t_boot_installs_the_protos(void)
{
    CountAlloc ca;
    UVM *vm = open_counted(&ca);
    if (!vm) { RT_CHECK(0); return; }
    URealm *r = urbi_realm_main(vm);

    RT_CHECK(vm->stdlib_booted != 0);
    /* Every row of the table produced a prototype.  A UP_* slot with no
     * row (Debug, which needs the REPL) is legitimately absent. */
    for (uint16_t i = 0; i < uboot_table_len; i++) {
        int idx = uboot_table[i].proto_index;
        if (vm->protos[idx] == NULL) printf("    proto for %s missing\n", uboot_table[i].global);
        RT_CHECK(vm->protos[idx] != NULL);
    }
    if (vm->protos[UP_EXCEPTION] == NULL) { urbi_close(vm); return; }
    /* The exception family is linked under Exception, and the atoms under
     * Object, by the table's parent_index column. */
    RT_CHECK(uobj_is_a(vm, vm->protos[UP_TYPEERROR], vm->protos[UP_EXCEPTION]));
    RT_CHECK(uobj_is_a(vm, vm->protos[UP_DIVBYZERO], vm->protos[UP_EXCEPTION]));
    RT_CHECK(uobj_is_a(vm, vm->protos[UP_INTEGER], vm->protos[UP_OBJECT]));
    RT_CHECK(uobj_is_a(vm, vm->protos[UP_STRING], vm->protos[UP_OBJECT]));

    UValue v = urbi_make_nil();
    RT_EQ(urbi_global_get(vm, r, "Object", &v), URBI_OK);
    RT_EQ((UObject *)v.v.p, vm->protos[UP_OBJECT]);
    RT_EQ(urbi_global_get(vm, r, "Lobby", &v), URBI_OK);
    RT_EQ((UObject *)v.v.p, vm->protos[UP_LOBBY]);

    urbi_close(vm);
}

static void t_boot_heap_is_small(void)
{
    CountAlloc ca;
    UVM *vm = open_counted(&ca);
    if (!vm) { RT_CHECK(0); return; }
    (void)urbi_realm_main(vm);
    size_t heap = ca.live;
    printf("    boot heap: %lu bytes live, %lu peak, %lu allocations\n",
           (unsigned long)ca.live, (unsigned long)ca.peak, ca.allocs);
    RT_CHECK(heap < 64u * 1024u);
    urbi_close(vm);
    /* Nothing leaks: every byte the VM took is handed back at close. */
    RT_EQ(ca.live, 0u);
}

static void t_atoms_dispatch_on_their_proto(void)
{
    CountAlloc ca;
    UVM *vm = open_counted(&ca);
    if (!vm) { RT_CHECK(0); return; }
    URealm *r = urbi_realm_main(vm);

    UValue v = run(vm, r, "var o = Object.clone(); o.setSlot(\"x\", 42); o.getSlot(\"x\")");
    RT_EQ(v.kind, UV_INT);
    RT_EQ(v.v.i, 42);

    v = run(vm, r, "var o = Object.clone(); o.isA(Object)");
    RT_EQ(v.kind, UV_BOOL);
    RT_CHECK(v.v.i != 0);

    /* Atom receivers resolve through the dispatch proto with no boxing. */
    v = run(vm, r, "1.clone()");
    RT_EQ(v.kind, UV_INT);
    RT_EQ(v.v.i, 1);

    v = run(vm, r, "\"ab\" + \"cd\"");
    RT_CHECK(str_is(v, "abcd"));

    v = run(vm, r, "\"abc\".size()");
    RT_EQ(v.kind, UV_INT);
    RT_EQ(v.v.i, 3);

    v = run(vm, r, "[1,2,3].size()");
    RT_EQ(v.kind, UV_INT);
    RT_EQ(v.v.i, 3);

    urbi_close(vm);
}

static void t_slot_write_on_an_atom_throws(void)
{
    CountAlloc ca;
    UVM *vm = open_counted(&ca);
    if (!vm) { RT_CHECK(0); return; }
    URealm *r = urbi_realm_main(vm);

    UValue out = urbi_make_nil();
    char err[256] = { 0 };
    const char *src = "var n = 1; n.zz = 2";
    int rc = urbi_run(vm, r, src, strlen(src), "<test>", &out, err, sizeof err);
    RT_EQ(rc, URBI_ERR_UNCAUGHT_THROW);
    UErrorInfo info;
    urbi_last_error(vm, &info);
    RT_CHECK(info.message != NULL && strstr(info.message, "not an Object") != NULL);

    urbi_close(vm);
}

/* --- the writer -------------------------------------------------------- */

static char g_echo[256];
static size_t g_echo_len;
static void capture_writer(void *ud, const char *chan, size_t cl, const char *msg, size_t ml)
{
    (void)ud; (void)chan; (void)cl;
    if (g_echo_len + ml < sizeof g_echo) {
        memcpy(g_echo + g_echo_len, msg, ml);
        g_echo_len += ml;
        g_echo[g_echo_len] = '\0';
    }
}

static void t_lobby_echo_reaches_the_writer(void)
{
    CountAlloc ca;
    UVM *vm = open_counted(&ca);
    if (!vm) { RT_CHECK(0); return; }
    URealm *r = urbi_realm_main(vm);
    g_echo[0] = '\0'; g_echo_len = 0;
    urbi_set_writer(vm, capture_writer, NULL);

    /* The Lobby primitive frames its output "[<ms>] *** <msg>\n"; what
     * matters here is that the message reached the writer at all. */
    (void)run(vm, r, "Lobby.echo(\"hello\")");
    RT_CHECK(strstr(g_echo, "*** hello") != NULL);

    /* Every realm has the Lobby in its chain, so `echo` works unqualified
     * and from a second realm too. */
    g_echo[0] = '\0'; g_echo_len = 0;
    URealm *b = urbi_realm_new(vm);
    if (b) {
        (void)run(vm, b, "echo(\"two\")");
        RT_CHECK(strstr(g_echo, "*** two") != NULL);
    }

    urbi_close(vm);
}

void rt_realm_suite(void)
{
    t_realm_shape();
    t_realm_is_cheap();
    t_realms_are_isolated();
    t_realm_self_reference();
    t_toplevel_var_is_a_global_slot();
    t_toplevel_var_from_a_closure();
    t_boot_installs_the_protos();
    t_boot_heap_is_small();
    t_atoms_dispatch_on_their_proto();
    t_slot_write_on_an_atom_throws();
    t_lobby_echo_reaches_the_writer();
}
