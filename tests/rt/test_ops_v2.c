/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/rt/test_ops_v2.c — the dispatch arms for the wire v2 opcodes and
 * the two walker paths they lean on: a finally run on the normal path
 * (SCOPE_POP with RUN_FINALLY) and a jump that unwinds (UNWIND_TO).
 *
 * The protos are built by hand rather than compiled, so each case pins
 * one arm's contract independently of any emitter. */

#include "rtest.h"
#include <stdlib.h>
#include <string.h>

#include "urbi/urbi.h"
#include "rt/uexec.h"
#include "rt/urealm.h"
#include "chunk/uchunk.h"

typedef struct { UVM *vm; URealm *realm; } Fix;

static void *rt_alloc(void *p, size_t n, void *ud) {
    (void)ud;
    if (n == 0) { free(p); return NULL; }
    return realloc(p, n);
}
static void fix_open(Fix *f) { f->vm = uvm_open(rt_alloc, NULL); f->realm = urealm_new(f->vm); }
static void fix_close(Fix *f) { uvm_close(f->vm); }

static void *chunk_alloc(void *p, size_t n, void *ud) {
    (void)ud;
    if (n == 0) { free(p); return NULL; }
    return realloc(p, n);
}

/* An unbound proto over `ins`, with integer constants `k[]` and site
 * names `sites[]`.  Owned by whichever root it is attached to. */
static UProto *proto(const uint32_t *ins, size_t n, const int64_t *k, size_t nk,
                     const char *const *sites, uint16_t nsites, uint8_t max_reg) {
    UProto *p = calloc(1, sizeof *p);
    p->alloc_fn = chunk_alloc;
    p->instructions = chunk_alloc(NULL, n * 4, NULL);
    memcpy(p->instructions, ins, n * 4);
    p->instr_count = p->instr_cap = n;
    p->line_deltas = chunk_alloc(NULL, n, NULL);
    memset(p->line_deltas, 0, n);
    p->max_reg = max_reg;
    if (nk) {
        p->constants = chunk_alloc(NULL, nk * sizeof(UValue), NULL);
        for (size_t i = 0; i < nk; i++) p->constants[i] = uv_int(k[i]);
        p->const_count = p->const_cap = nk;
    }
    if (nsites) {
        p->site_count = nsites;
        p->site_name_strs = chunk_alloc(NULL, nsites * sizeof(char *), NULL);
        for (uint16_t i = 0; i < nsites; i++) {
            size_t l = strlen(sites[i]);
            p->site_name_strs[i] = chunk_alloc(NULL, l + 1, NULL);
            memcpy(p->site_name_strs[i], sites[i], l + 1);
        }
    }
    return p;
}

/* Hands `nested[]` to `root` as its CLOSURE table P0, P1, ... */
static void adopt(UProto *root, UProto **nested, size_t n) {
    root->nested = chunk_alloc(NULL, n * sizeof(UProto *), NULL);
    for (size_t i = 0; i < n; i++) root->nested[i] = nested[i];
    root->nested_count = root->nested_cap = n;
}

/* Binds `root` to the VM and returns its chunk closure, or NULL. */
static UClosure *bind(Fix *f, UProto *root) {
    root->heap_allocated = true;
    UProtoCell *pc = uproto_bind(f->vm, root);
    if (!pc) return NULL;
    pc->cell.flags |= UCELL_F_RTPIN;
    UClosure *cl = uclosure_new(f->vm, root, 0);
    pc->cell.flags &= (uint16_t)~UCELL_F_RTPIN;
    return cl;
}

static UClosure *build(Fix *f, const uint32_t *ins, size_t n, const int64_t *k, size_t nk,
                       const char *const *sites, uint16_t nsites, uint8_t max_reg) {
    return bind(f, proto(ins, n, k, nk, sites, nsites, max_reg));
}

static int run_chunk(Fix *f, UClosure *cl, UValue *out) {
    int rc = uexec_run_chunk(f->vm, f->realm, cl, out);
    if (rc != URBI_OK) printf("    run: rc %d: %s\n", rc, f->vm->last_error);
    return rc;
}

/* The realm global `name` as an int, or -1 when absent or not an int. */
static int64_t global_int(Fix *f, const char *name) {
    UObject *g = f->realm->globals;
    int idx = uobj_find_local(g, usym_cstr(f->vm, name));
    if (idx < 0 || g->values[idx].kind != (uint8_t)UV_INT) return -1;
    return g->values[idx].v.i;
}

/* EXTARG: site 300 on the realm globals, written and read back.  Each
 * site-taking arm family (SETSLOT, GETSLOT, GETSLOT_CHANGE_EVENT) is
 * followed by a plain op on a low site: an arm that kept the high bits
 * would address site | 0x100 instead (s263 / s261), which either lands
 * the write on the wrong name or reads a name that is not there. */
static void extarg_addresses_a_site_above_255(void) {
    Fix f; fix_open(&f);
    const char *names[301]; char store[301][8];
    for (int i = 0; i < 301; i++) { snprintf(store[i], 8, "s%d", i); names[i] = store[i]; }
    int64_t k[] = { 7, 9 };
    uint32_t ins[] = {
        uinstr_enc_abc(OP_LOAD_REALM_GLOBAL, 0, 0, 0),
        uinstr_enc_abx(OP_LOADK, 1, 0),                     /* R1 = 7 */
        uinstr_enc_abx(OP_LOADK, 4, 1),                     /* R4 = 9 */
        uinstr_enc_abc(OP_SETSLOT, 4, 0, 5),                /* s5 = 9 */
        uinstr_enc_abx(OP_EXTARG, 0, 1), uinstr_enc_abc(OP_SETSLOT, 1, 0, 44),   /* s300 = 7 */
        uinstr_enc_abc(OP_SETSLOT, 1, 0, 7),                /* s7 = 7, not s263 */
        uinstr_enc_abx(OP_EXTARG, 0, 1), uinstr_enc_abc(OP_GETSLOT, 2, 0, 44),   /* R2 = s300 */
        uinstr_enc_abc(OP_GETSLOT, 3, 0, 5),                /* R3 = s5, not s261 */
        uinstr_enc_abx(OP_EXTARG, 0, 1), uinstr_enc_abc(OP_GETSLOT_CHANGE_EVENT, 5, 0, 44),
        uinstr_enc_abc(OP_GETSLOT, 6, 0, 5),                /* R6 = s5, not s261 */
        uinstr_enc_abc(OP_ADD, 2, 2, 3),
        uinstr_enc_abc(OP_ADD, 2, 2, 6),
        uinstr_enc_abc(OP_RET, 2, 0, 0),                    /* 7 + 9 + 9 */
    };
    UClosure *cl = build(&f, ins, sizeof ins / sizeof ins[0], k, 2, names, 301, 6);
    UValue out = uv_nil();
    RT_EQ(run_chunk(&f, cl, &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_INT); RT_EQ(out.v.i, 25);
    RT_EQ(global_int(&f, "s300"), 7);
    RT_EQ(global_int(&f, "s44"), -1);
    RT_EQ(global_int(&f, "s7"), 7);
    RT_EQ(global_int(&f, "s263"), -1);
    fix_close(&f);
}

/* SCOPE_POP with RUN_FINALLY runs the handler once and continues after the pop. */
static void a_run_finally_pop_runs_the_body_and_continues(void) {
    Fix f; fix_open(&f);
    /* R0 = 0; SCOPE_TRY finally -> 8; R0 += 1; SCOPE_POP try+run; R0 += 10; RET R0; 8: R0 += 100; RESUME */
    int64_t k[] = { 0, 1, 10, 100 };
    uint32_t ins[] = {
        uinstr_enc_abx(OP_LOADK, 0, 0),
        uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_FINALLY, 8),
        uinstr_enc_abx(OP_LOADK, 1, 1), uinstr_enc_abc(OP_ADD, 0, 0, 1),
        uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TRY | USCOPE_POP_RUN_FINALLY, 0, 0),
        uinstr_enc_abx(OP_LOADK, 1, 2), uinstr_enc_abc(OP_ADD, 0, 0, 1),
        uinstr_enc_abc(OP_RET, 0, 0, 0),
        uinstr_enc_abx(OP_LOADK, 1, 3), uinstr_enc_abc(OP_ADD, 0, 0, 1),
        uinstr_enc_abc(OP_RESUME, 0, 0, 0),
    };
    UClosure *cl = build(&f, ins, 11, k, 4, NULL, 0, 2);
    UValue out = uv_nil();
    RT_EQ(run_chunk(&f, cl, &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_INT); RT_EQ(out.v.i, 111);
    fix_close(&f);
}

/* UNWIND_TO across a finally scope: finally runs, then the jump lands. */
static void unwind_to_runs_the_finally_then_lands(void) {
    Fix f; fix_open(&f);
    /* R0 = 0; SCOPE_TRY finally -> 8; UNWIND_TO depth 1 -> 5; R0 += 10 (skipped); 5: R0 += 1000; RET; 8: R0 += 100; RESUME */
    int64_t k[] = { 0, 10, 1000, 100 };
    uint32_t ins[] = {
        uinstr_enc_abx(OP_LOADK, 0, 0),
        uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_FINALLY, 8),
        uinstr_enc_abx(OP_UNWIND_TO, 1, 5),
        uinstr_enc_abx(OP_LOADK, 1, 1), uinstr_enc_abc(OP_ADD, 0, 0, 1),
        uinstr_enc_abx(OP_LOADK, 1, 2), uinstr_enc_abc(OP_ADD, 0, 0, 1),
        uinstr_enc_abc(OP_RET, 0, 0, 0),
        uinstr_enc_abx(OP_LOADK, 1, 3), uinstr_enc_abc(OP_ADD, 0, 0, 1),
        uinstr_enc_abc(OP_RESUME, 0, 0, 0),
    };
    UClosure *cl = build(&f, ins, 11, k, 4, NULL, 0, 2);
    UValue out = uv_nil();
    RT_EQ(run_chunk(&f, cl, &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_INT); RT_EQ(out.v.i, 1100);
    fix_close(&f);
}

/* A throw inside a finally entered by UNWIND_TO replaces the pending jump. */
static void a_throw_inside_a_finally_replaces_a_pending_jump(void) {
    Fix f; fix_open(&f);
    /* SCOPE_TRY catch -> 9; SCOPE_TRY finally -> 6; UNWIND_TO depth 1 -> 4; RET (skipped);
     * 4: LOADK R0=1; RET; 6: LOADK R1=5; THROW R1; RESUME; 9: LOAD_CATCH_VALUE R0; RET R0 */
    int64_t k[] = { 1, 5 };
    uint32_t ins[] = {
        uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_CATCH, 9),
        uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_FINALLY, 6),
        uinstr_enc_abx(OP_UNWIND_TO, 1, 4),
        uinstr_enc_abc(OP_RET, 0, 0, 0),
        uinstr_enc_abx(OP_LOADK, 0, 0),
        uinstr_enc_abc(OP_RET, 0, 0, 0),
        uinstr_enc_abx(OP_LOADK, 1, 1),
        uinstr_enc_abc(OP_THROW, 1, 0, 0),
        uinstr_enc_abc(OP_RESUME, 0, 0, 0),
        uinstr_enc_abc(OP_LOAD_CATCH_VALUE, 0, 0, 0),   /* the walker consumed the catch entry; no pop here */
        uinstr_enc_abc(OP_RET, 0, 0, 0),
    };
    UClosure *cl = build(&f, ins, 11, k, 2, NULL, 0, 2);
    UValue out = uv_nil();
    RT_EQ(run_chunk(&f, cl, &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_INT); RT_EQ(out.v.i, 5);
    fix_close(&f);
}

/* SCOPE_TAG with the no-register sentinel opens a fresh tag; the pop
 * leaves the cleanup stack balanced, so the code after it runs. */
static void scope_tag_sentinel_opens_a_fresh_tag(void) {
    Fix f; fix_open(&f);
    int64_t k[] = { 3 };
    uint32_t ins[] = {
        uinstr_enc_abx(OP_SCOPE_TAG, USCOPE_NO_REG, 2),
        uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TAG, 0, 0),
        uinstr_enc_abx(OP_LOADK, 0, 0),
        uinstr_enc_abc(OP_RET, 0, 0, 0),
    };
    UClosure *cl = build(&f, ins, 4, k, 1, NULL, 0, 1);
    UValue out = uv_nil();
    RT_EQ(run_chunk(&f, cl, &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_INT); RT_EQ(out.v.i, 3);
    fix_close(&f);
}

/* A child that adds one to the realm global `n`. */
static UProto *bump_proto(const char *name) {
    static const int64_t one[] = { 1 };
    const char *sites[] = { name };
    uint32_t ins[] = {
        uinstr_enc_abc(OP_LOAD_REALM_GLOBAL, 0, 0, 0),
        uinstr_enc_abc(OP_GETSLOT, 1, 0, 0),
        uinstr_enc_abx(OP_LOADK, 2, 0),
        uinstr_enc_abc(OP_ADD, 1, 1, 2),
        uinstr_enc_abc(OP_SETSLOT, 1, 0, 0),
        uinstr_enc_abc(OP_RET, 1, 0, 0),
    };
    return proto(ins, 6, one, 1, sites, 1, 3);
}

/* FORK join + JOIN_WAIT over a child closure that returns; detach spawns
 * without a handle and still runs before the pump ends. */
static void fork_modes_spawn_and_join(void) {
    Fix f; fix_open(&f);
    const char *sites[] = { "n" };
    int64_t k[] = { 0 };
    /* R4 = globals; n = 0; R0 = closure P0; FORK join -> R1; JOIN_WAIT R1;
     * R2 = n (the joined child has run); FORK detach; RET R2 */
    uint32_t ins[] = {
        uinstr_enc_abc(OP_LOAD_REALM_GLOBAL, 4, 0, 0),
        uinstr_enc_abx(OP_LOADK, 5, 0),
        uinstr_enc_abc(OP_SETSLOT, 5, 4, 0),
        uinstr_enc_abx(OP_CLOSURE, 0, 0),
        uinstr_enc_abc(OP_FORK, 0, 1, UFORK_JOIN),
        uinstr_enc_abc(OP_JOIN_WAIT, 1, 0, 0),
        uinstr_enc_abc(OP_GETSLOT, 2, 4, 0),
        uinstr_enc_abc(OP_FORK, 0, 0xFF, UFORK_DETACH),
        uinstr_enc_abc(OP_RET, 2, 0, 0),
    };
    UProto *root = proto(ins, 9, k, 1, sites, 1, 6);
    UProto *child = bump_proto("n");
    adopt(root, &child, 1);
    UClosure *cl = bind(&f, root);
    UValue out = uv_nil();
    RT_EQ(run_chunk(&f, cl, &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_INT); RT_EQ(out.v.i, 1);   /* the join waited */
    RT_EQ(global_int(&f, "n"), 2);                          /* the detached child ran too */
    fix_close(&f);
}

/* The condition proto: the realm global `on`. */
static UProto *flag_proto(void) {
    const char *sites[] = { "on" };
    uint32_t ins[] = {
        uinstr_enc_abc(OP_LOAD_REALM_GLOBAL, 0, 0, 0),
        uinstr_enc_abc(OP_GETSLOT, 0, 0, 0),
        uinstr_enc_abc(OP_RET, 0, 0, 0),
    };
    return proto(ins, 3, NULL, 0, sites, 1, 1);
}

/* `at (on) hits++ [onleave alts++]` with a real closure in R[A+2] either
 * way; `flags` says whether the install may read it.  The condition goes
 * true, then false: the body runs on the rising edge, the alternate (if
 * installed) on the falling one. */
static void install_at_flag(uint8_t flags, int64_t *hits, int64_t *alts) {
    Fix f; fix_open(&f);
    const char *sites[] = { "on", "hits", "alts" };
    int64_t k[] = { 0 };
    uint32_t ins[] = {
        uinstr_enc_abc(OP_LOAD_REALM_GLOBAL, 5, 0, 0),
        uinstr_enc_abx(OP_LOADK, 6, 0),
        uinstr_enc_abc(OP_SETSLOT, 6, 5, 1),                /* hits = 0 */
        uinstr_enc_abc(OP_SETSLOT, 6, 5, 2),                /* alts = 0 */
        uinstr_enc_abc(OP_LOADBOOL, 6, 1, 0),
        uinstr_enc_abc(OP_SETSLOT, 6, 5, 0),                /* on = true */
        uinstr_enc_abx(OP_CLOSURE, 1, 0),                   /* R1 = condition */
        uinstr_enc_abx(OP_CLOSURE, 2, 1),                   /* R2 = body */
        uinstr_enc_abx(OP_CLOSURE, 3, 2),                   /* R3 = alternate */
        uinstr_enc_abc(OP_INSTALL, 1, UINSTALL_AT_COND, flags),
        uinstr_enc_abc(OP_LOADVOID, 0, 0, 0),
        uinstr_enc_abc(OP_RET, 0, 0, 0),
    };
    UProto *root = proto(ins, sizeof ins / sizeof ins[0], k, 1, sites, 3, 7);
    UProto *nested[3] = { flag_proto(), bump_proto("hits"), bump_proto("alts") };
    adopt(root, nested, 3);
    UClosure *cl = bind(&f, root);
    UValue out = uv_nil();
    RT_EQ(run_chunk(&f, cl, &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_VOID);
    for (int n = 0; n < 8 && urbi_step(f.vm, 0, NULL) == URBI_STEP_RAN; n++) { }
    RT_EQ(urbi_global_set(f.vm, f.realm, "on", uv_bool(false)), URBI_OK);
    for (int n = 0; n < 8 && urbi_step(f.vm, 0, NULL) == URBI_STEP_RAN; n++) { }
    RT_EQ(f.vm->last_error[0], '\0');
    *hits = global_int(&f, "hits");
    *alts = global_int(&f, "alts");
    fix_close(&f);
}

/* INSTALL without HAS_ALT: R[A+2] holds a closure left there (as register
 * reuse would leave one) and is not read. */
static void an_install_without_an_alternate_body_leaves_r_a_plus_2_alone(void) {
    int64_t hits = -1, alts = -1;
    install_at_flag(UINSTALL_F_HAS_BODY, &hits, &alts);
    RT_EQ(hits, 1);      /* the body ran, once */
    RT_EQ(alts, 0);      /* the stale closure in R[A+2] was never installed */
}

/* The same chunk with HAS_ALT: the alternate runs on the falling edge. */
static void an_install_with_an_alternate_body_runs_it_on_the_falling_edge(void) {
    int64_t hits = -1, alts = -1;
    install_at_flag(UINSTALL_F_HAS_BODY | UINSTALL_F_HAS_ALT, &hits, &alts);
    RT_EQ(hits, 1);
    RT_EQ(alts, 1);
}

/* UNWIND_TO depth 2 over two finally scopes: the inner body, then the
 * outer, each once, then the jump lands once. */
static void unwind_to_chains_through_two_finally_bodies(void) {
    Fix f; fix_open(&f);
    int64_t k[] = { 0, 10, 100, 1000 };
    uint32_t ins[] = {
        uinstr_enc_abx(OP_LOADK, 0, 0),
        uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_FINALLY, 11),   /* outer */
        uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_FINALLY, 8),    /* inner */
        uinstr_enc_abx(OP_UNWIND_TO, 2, 5),
        uinstr_enc_abc(OP_RET, 0, 0, 0),                          /* skipped */
        uinstr_enc_abx(OP_LOADK, 1, 3), uinstr_enc_abc(OP_ADD, 0, 0, 1),   /* 5: target */
        uinstr_enc_abc(OP_RET, 0, 0, 0),
        uinstr_enc_abx(OP_LOADK, 1, 1), uinstr_enc_abc(OP_ADD, 0, 0, 1),   /* 8: inner finally */
        uinstr_enc_abc(OP_RESUME, 0, 0, 0),
        uinstr_enc_abx(OP_LOADK, 1, 2), uinstr_enc_abc(OP_ADD, 0, 0, 1),   /* 11: outer finally */
        uinstr_enc_abc(OP_RESUME, 0, 0, 0),
    };
    UClosure *cl = build(&f, ins, sizeof ins / sizeof ins[0], k, 4, NULL, 0, 2);
    UValue out = uv_nil();
    RT_EQ(run_chunk(&f, cl, &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_INT); RT_EQ(out.v.i, 1110);
    fix_close(&f);
}

/* What the probe native saw, call by call: the calling strand's ambient
 * tag and cleanup depth. */
static struct { UTag *tag[4]; uint16_t ncleanup[4]; int n; } seen;
static int probe_native(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out) {
    (void)self; (void)args; (void)nargs;
    UStrand *s = vm->sched.current;
    if (seen.n < 4) { seen.tag[seen.n] = s->tag; seen.ncleanup[seen.n] = s->ncleanup; seen.n++; }
    *out = uv_nil();
    return UEXEC_OK;
}

/* UNWIND_TO depth 2 over [tag scope, finally]: the finally runs inside the
 * tag, then the tag scope is left on the way -- ambient tag restored,
 * leave fired -- and the jump lands with the cleanup stack empty. */
static void unwind_to_leaves_a_tag_scope_on_the_way(void) {
    Fix f; fix_open(&f);
    UTag *t = utag_new(f.vm, uv_nil());
    RT_CHECK(t != NULL);
    RT_EQ(urbi_global_set(f.vm, f.realm, "t", uv_ptr(UV_CELL, t)), URBI_OK);
    UEvent *leave = utag_leave_event(f.vm, t);
    RT_CHECK(leave != NULL);
    RT_EQ(urbi_global_set(f.vm, f.realm, "tl", uv_ptr(UV_CELL, leave)), URBI_OK);
    UClosure *probe = uclosure_native(f.vm, probe_native, 0, 0);
    RT_EQ(urbi_global_set(f.vm, f.realm, "probe", uv_ptr(UV_CELL, probe)), URBI_OK);
    seen.n = 0;

    const char *sites[] = { "t", "tl", "probe", "leaves" };
    int64_t k[] = { 0, 1000, 10 };
    uint32_t ins[] = {
        uinstr_enc_abc(OP_LOAD_REALM_GLOBAL, 0, 0, 0),
        uinstr_enc_abx(OP_LOADK, 1, 0),
        uinstr_enc_abc(OP_SETSLOT, 1, 0, 3),                /* leaves = 0 */
        uinstr_enc_abc(OP_GETSLOT, 3, 0, 1),                /* R3 = t's leave event */
        uinstr_enc_abx(OP_CLOSURE, 4, 0),                   /* R4 = leaves++ */
        uinstr_enc_abc(OP_INSTALL, 3, UINSTALL_AT_EVENT, UINSTALL_F_HAS_BODY),
        uinstr_enc_abc(OP_GETSLOT, 6, 0, 2),
        uinstr_enc_abc(OP_CALL, 6, 1, 2),                   /* probe: before the scope */
        uinstr_enc_abc(OP_GETSLOT, 2, 0, 0),                /* R2 = t */
        uinstr_enc_abx(OP_SCOPE_TAG, 2, 20),
        uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_FINALLY, 16),
        uinstr_enc_abx(OP_UNWIND_TO, 2, 13),
        uinstr_enc_abc(OP_RET, 0, 0, 0),                    /* skipped */
        uinstr_enc_abc(OP_GETSLOT, 6, 0, 2),                /* 13: target */
        uinstr_enc_abc(OP_CALL, 6, 1, 2),                   /* probe: after landing */
        uinstr_enc_abc(OP_RET, 1, 0, 0),
        uinstr_enc_abc(OP_GETSLOT, 6, 0, 2),                /* 16: finally */
        uinstr_enc_abc(OP_CALL, 6, 1, 2),                   /* probe: inside the finally */
        uinstr_enc_abc(OP_RESUME, 0, 0, 0),
        uinstr_enc_abc(OP_RET, 0, 0, 0),                    /* padding */
        uinstr_enc_abc(OP_RET, 0, 0, 0),                    /* 20: onleave target, unused */
    };
    UProto *root = proto(ins, sizeof ins / sizeof ins[0], k, 3, sites, 4, 7);
    UProto *child = bump_proto("leaves");
    adopt(root, &child, 1);
    UClosure *cl = bind(&f, root);
    UValue out = uv_nil();
    RT_EQ(run_chunk(&f, cl, &out), URBI_OK);
    for (int n = 0; n < 8 && urbi_step(f.vm, 0, NULL) == URBI_STEP_RAN; n++) { }
    RT_EQ(seen.n, 3);
    RT_CHECK(seen.tag[0] != t);                 /* before: the realm's ambient tag */
    RT_CHECK(seen.tag[1] == t);                 /* the finally ran inside the tag scope */
    RT_EQ(seen.ncleanup[1], 2);                 /* tag scope + the RUNNING marker */
    RT_CHECK(seen.tag[2] == seen.tag[0]);       /* landing restored the ambient tag */
    RT_EQ(seen.ncleanup[2], 0);                 /* both entries popped */
    RT_EQ(global_int(&f, "leaves"), 1);         /* leave fired, once */
    fix_close(&f);
}

/* UNWIND_TO depth 1 from inside a finally body counts that body's RUNNING
 * marker as its one entry: the marker is dropped with the unwind it
 * suspended, and the jump lands.  Once for a body the walker started on a
 * throw (the throw must not resume), once for one a run-finally pop
 * started (execution must not return after the pop). */
static void a_jump_out_of_a_finally_body_drops_its_marker(void) {
    {
        Fix f; fix_open(&f);
        int64_t k[] = { 0, 5, 1000, 10 };
        uint32_t ins[] = {
            uinstr_enc_abx(OP_LOADK, 0, 0),
            uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_FINALLY, 7),
            uinstr_enc_abx(OP_LOADK, 1, 1),
            uinstr_enc_abc(OP_THROW, 1, 0, 0),
            uinstr_enc_abx(OP_LOADK, 1, 2), uinstr_enc_abc(OP_ADD, 0, 0, 1),   /* 4: target */
            uinstr_enc_abc(OP_RET, 0, 0, 0),
            uinstr_enc_abx(OP_LOADK, 1, 3), uinstr_enc_abc(OP_ADD, 0, 0, 1),   /* 7: finally */
            uinstr_enc_abx(OP_UNWIND_TO, 1, 4),
            uinstr_enc_abc(OP_RESUME, 0, 0, 0),
        };
        UClosure *cl = build(&f, ins, sizeof ins / sizeof ins[0], k, 4, NULL, 0, 2);
        UValue out = uv_nil();
        RT_EQ(run_chunk(&f, cl, &out), URBI_OK);          /* the suspended throw is gone */
        RT_EQ(out.kind, (uint8_t)UV_INT); RT_EQ(out.v.i, 1010);
        fix_close(&f);
    }
    {
        Fix f; fix_open(&f);
        int64_t k[] = { 0, 1, 1000, 10 };
        uint32_t ins[] = {
            uinstr_enc_abx(OP_LOADK, 0, 0),
            uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_FINALLY, 8),
            uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TRY | USCOPE_POP_RUN_FINALLY, 0, 0),
            uinstr_enc_abx(OP_LOADK, 1, 1), uinstr_enc_abc(OP_ADD, 0, 0, 1),   /* after the pop: skipped */
            uinstr_enc_abx(OP_LOADK, 1, 2), uinstr_enc_abc(OP_ADD, 0, 0, 1),   /* 5: target */
            uinstr_enc_abc(OP_RET, 0, 0, 0),
            uinstr_enc_abx(OP_LOADK, 1, 3), uinstr_enc_abc(OP_ADD, 0, 0, 1),   /* 8: finally */
            uinstr_enc_abx(OP_UNWIND_TO, 1, 5),
            uinstr_enc_abc(OP_RESUME, 0, 0, 0),
        };
        UClosure *cl = build(&f, ins, sizeof ins / sizeof ins[0], k, 4, NULL, 0, 2);
        UValue out = uv_nil();
        RT_EQ(run_chunk(&f, cl, &out), URBI_OK);
        RT_EQ(out.kind, (uint8_t)UV_INT); RT_EQ(out.v.i, 1010);
        fix_close(&f);
    }
}

RT_SUITE(rt_ops_v2_suite) {
    rt_run("extarg_addresses_a_site_above_255", extarg_addresses_a_site_above_255);
    rt_run("a_run_finally_pop_runs_the_body_and_continues", a_run_finally_pop_runs_the_body_and_continues);
    rt_run("unwind_to_runs_the_finally_then_lands", unwind_to_runs_the_finally_then_lands);
    rt_run("a_throw_inside_a_finally_replaces_a_pending_jump", a_throw_inside_a_finally_replaces_a_pending_jump);
    rt_run("scope_tag_sentinel_opens_a_fresh_tag", scope_tag_sentinel_opens_a_fresh_tag);
    rt_run("fork_modes_spawn_and_join", fork_modes_spawn_and_join);
    rt_run("an_install_without_an_alternate_body_leaves_r_a_plus_2_alone",
           an_install_without_an_alternate_body_leaves_r_a_plus_2_alone);
    rt_run("an_install_with_an_alternate_body_runs_it_on_the_falling_edge",
           an_install_with_an_alternate_body_runs_it_on_the_falling_edge);
    rt_run("unwind_to_chains_through_two_finally_bodies", unwind_to_chains_through_two_finally_bodies);
    rt_run("unwind_to_leaves_a_tag_scope_on_the_way", unwind_to_leaves_a_tag_scope_on_the_way);
    rt_run("a_jump_out_of_a_finally_body_drops_its_marker", a_jump_out_of_a_finally_body_drops_its_marker);
}
