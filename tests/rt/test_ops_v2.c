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

/* EXTARG: site 300 on the realm globals: write, then read it back. */
static void extarg_addresses_a_site_above_255(void) {
    Fix f; fix_open(&f);
    const char *names[301]; char store[301][8];
    for (int i = 0; i < 301; i++) { snprintf(store[i], 8, "s%d", i); names[i] = store[i]; }
    /* R0 = globals; R1 = 7; EXTARG; SETSLOT R1 -> R0.s300; EXTARG; GETSLOT R2 = R0.s300; RET R2 */
    int64_t k[] = { 7 };
    uint32_t ins[] = {
        uinstr_enc_abc(OP_LOAD_REALM_GLOBAL, 0, 0, 0),
        uinstr_enc_abx(OP_LOADK, 1, 0),
        uinstr_enc_abx(OP_EXTARG, 0, 1), uinstr_enc_abc(OP_SETSLOT, 1, 0, 44),
        uinstr_enc_abx(OP_EXTARG, 0, 1), uinstr_enc_abc(OP_GETSLOT, 2, 0, 44),
        uinstr_enc_abc(OP_RET, 2, 0, 0),
    };
    UClosure *cl = build(&f, ins, 7, k, 1, names, 301, 3);
    UValue out = uv_nil();
    RT_EQ(run_chunk(&f, cl, &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_INT); RT_EQ(out.v.i, 7);
    /* The write landed on site 300's name, not on site 44's. */
    RT_EQ(global_int(&f, "s300"), 7);
    RT_EQ(global_int(&f, "s44"), -1);
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

/* INSTALL without an alternate body: R[A+2] holds an int and is not read. */
static void an_install_without_an_alternate_body_leaves_r_a_plus_2_alone(void) {
    Fix f; fix_open(&f);
    const char *sites[] = { "hits" };
    int64_t k[] = { 0, 42 };
    /* R5 = globals; hits = 0; R1 = cond P0; R2 = body P1; R3 = 42;
     * INSTALL base=1 AT_COND HAS_BODY; R0 = void; RET R0 */
    uint32_t ins[] = {
        uinstr_enc_abc(OP_LOAD_REALM_GLOBAL, 5, 0, 0),
        uinstr_enc_abx(OP_LOADK, 6, 0),
        uinstr_enc_abc(OP_SETSLOT, 6, 5, 0),
        uinstr_enc_abx(OP_CLOSURE, 1, 0),
        uinstr_enc_abx(OP_CLOSURE, 2, 1),
        uinstr_enc_abx(OP_LOADK, 3, 1),
        uinstr_enc_abc(OP_INSTALL, 1, UINSTALL_AT_COND, UINSTALL_F_HAS_BODY),
        uinstr_enc_abc(OP_LOADVOID, 0, 0, 0),
        uinstr_enc_abc(OP_RET, 0, 0, 0),
    };
    uint32_t cond_ins[] = {
        uinstr_enc_abc(OP_LOADBOOL, 0, 1, 0),
        uinstr_enc_abc(OP_RET, 0, 0, 0),
    };
    UProto *root = proto(ins, 9, k, 2, sites, 1, 7);
    UProto *nested[2] = { proto(cond_ins, 2, NULL, 0, NULL, 0, 1), bump_proto("hits") };
    adopt(root, nested, 2);
    UClosure *cl = bind(&f, root);
    UValue out = uv_nil();
    RT_EQ(run_chunk(&f, cl, &out), URBI_OK);
    RT_EQ(out.kind, (uint8_t)UV_VOID);
    for (int n = 0; n < 8 && urbi_step(f.vm, 0, NULL) == URBI_STEP_RAN; n++) { }
    RT_EQ(f.vm->last_error[0], '\0');      /* no TypeError from R[A+2] */
    RT_EQ(global_int(&f, "hits"), 1);      /* the body ran, once */
    fix_close(&f);
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
}
