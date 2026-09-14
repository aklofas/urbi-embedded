#include <string.h>
#include "rtest.h"
#include "fakevm.h"
#include "rt/ustrand.h"

/* Zero-initialised UProto: ustrand.c only reads max_reg, nparams, and
 * instructions, so every other field (all pointers/counts from the kept
 * frontend) can stay at its zero value for these tests. */
static UProto make_proto(uint8_t max_reg) {
    UProto p; memset(&p, 0, sizeof p);
    p.max_reg = max_reg;
    return p;
}

static void push_frame_grows_stack_for_max_reg(void) {
    UCell *roots[2] = { NULL, NULL };
    struct UVM vm; fakevm_init(&vm, roots, 2);
    UStrand *s = ustrand_new(&vm, NULL); roots[0] = &s->cell;
    UProto proto = make_proto(40);
    UClosure *cl = uclosure_new(&vm, &proto, 0); roots[1] = &cl->cell;
    RT_CHECK(ustrand_push_frame(s, cl, uv_nil(), 0, 0) == 0);
    RT_CHECK(s->stack_cap >= 41);
    RT_EQ(s->nframes, 1);
    fakevm_destroy(&vm);
}

static void push_frame_native_closure_uses_single_register(void) {
    UCell *roots[2] = { NULL, NULL };
    struct UVM vm; fakevm_init(&vm, roots, 2);
    UStrand *s = ustrand_new(&vm, NULL); roots[0] = &s->cell;
    UClosure *cl = uclosure_new(&vm, NULL, 0); roots[1] = &cl->cell;   /* proto == NULL: native */
    RT_CHECK(ustrand_push_frame(s, cl, uv_nil(), 3, 0) == 0);
    RT_CHECK(s->stack_cap >= 4);
    RT_CHECK(s->frames[0].pc == NULL);
    fakevm_destroy(&vm);
}

static void nested_frames_grow_stack_and_preserve_upvals(void) {
    /* 10 frames with base += 40 and max_reg 40 forces the 32-slot initial
     * stack through several doublings; an upvalue opened on register 5
     * before any of that growth must still point at the same live value
     * afterward -- ustrand_ensure_stack is responsible for rewriting
     * every open upvalue's ptr whenever the backing array moves. */
    UCell *roots[2] = { NULL, NULL };
    struct UVM vm; fakevm_init(&vm, roots, 2);
    UStrand *s = ustrand_new(&vm, NULL); roots[0] = &s->cell;
    UProto proto = make_proto(40);
    UClosure *cl = uclosure_new(&vm, &proto, 0); roots[1] = &cl->cell;
    RT_CHECK(ustrand_push_frame(s, cl, uv_nil(), 0, 0) == 0);
    s->stack[5] = uv_int(777);
    UUpval *up = ustrand_find_or_open_upval(s, 5);
    RT_CHECK(up != NULL && up->ptr == &s->stack[5] && up->ptr->v.i == 777);
    uint32_t cap_after_first_push = s->stack_cap;
    uint32_t base = 40;
    for (int i = 1; i < 10; i++) {
        RT_CHECK(ustrand_push_frame(s, cl, uv_nil(), base, 0) == 0);
        base += 40;
    }
    RT_EQ(s->nframes, 10);
    RT_CHECK(s->stack_cap >= cap_after_first_push * 4);   /* doubled at least twice more */
    RT_CHECK(up->ptr == &s->stack[5]);
    RT_CHECK(up->ptr->v.i == 777);
    fakevm_destroy(&vm);
}

static void close_upvals_partitions_by_index(void) {
    UCell *roots[2] = { NULL, NULL };
    struct UVM vm; fakevm_init(&vm, roots, 2);
    UStrand *s = ustrand_new(&vm, NULL); roots[0] = &s->cell;
    UProto proto = make_proto(10);
    UClosure *cl = uclosure_new(&vm, &proto, 0); roots[1] = &cl->cell;
    RT_CHECK(ustrand_push_frame(s, cl, uv_nil(), 0, 0) == 0);
    s->stack[2] = uv_int(2);
    s->stack[3] = uv_int(3);
    s->stack[7] = uv_int(7);
    UUpval *u2 = ustrand_find_or_open_upval(s, 2);
    UUpval *u3 = ustrand_find_or_open_upval(s, 3);
    UUpval *u7 = ustrand_find_or_open_upval(s, 7);
    ustrand_close_upvals(s, 3);
    RT_CHECK(u3->ptr == &u3->closed && u3->closed.v.i == 3);
    RT_CHECK(u7->ptr == &u7->closed && u7->closed.v.i == 7);
    RT_CHECK(u2->ptr == &s->stack[2] && u2->ptr->v.i == 2);   /* index < 3: stays open */
    RT_CHECK(s->open_upvals == u2 && u2->next_open == NULL);  /* the sorted list stopped early at u2 */
    fakevm_destroy(&vm);
}

static void pop_frame_closes_its_upvals(void) {
    UCell *roots[2] = { NULL, NULL };
    struct UVM vm; fakevm_init(&vm, roots, 2);
    UStrand *s = ustrand_new(&vm, NULL); roots[0] = &s->cell;
    UProto proto = make_proto(5);
    UClosure *cl = uclosure_new(&vm, &proto, 0); roots[1] = &cl->cell;
    RT_CHECK(ustrand_push_frame(s, cl, uv_nil(), 0, 0) == 0);   /* frame 0: base 0 */
    RT_CHECK(ustrand_push_frame(s, cl, uv_nil(), 6, 0) == 0);   /* frame 1: base 6 */
    s->stack[8] = uv_int(42);
    UUpval *u = ustrand_find_or_open_upval(s, 8);
    ustrand_pop_frame(s);
    RT_EQ(s->nframes, 1);
    RT_CHECK(u->ptr == &u->closed && u->closed.v.i == 42);
    fakevm_destroy(&vm);
}

static void cleanup_stack_grows_past_initial_cap(void) {
    UCell *roots[1] = { NULL };
    struct UVM vm; fakevm_init(&vm, roots, 1);
    UStrand *s = ustrand_new(&vm, NULL); roots[0] = &s->cell;
    for (int i = 0; i < 10; i++) {
        UCleanup c; memset(&c, 0, sizeof c);
        c.kind = UCLEAN_TRY; c.frame = (uint16_t)i;
        RT_CHECK(ustrand_push_cleanup(s, c) == 0);
    }
    RT_EQ(s->ncleanup, 10);
    RT_CHECK(s->cleanup_cap >= 10);
    for (int i = 0; i < 10; i++) RT_EQ(s->cleanup[i].frame, (uint16_t)i);
    fakevm_destroy(&vm);
}

static void strand_struct_is_small(void) {
    /* Idle-strand footprint target (spec S7): the fixed struct itself,
     * not counting the lazily-allocated stack/frames/cleanup arrays.
     * Assumes a 64-bit host, the only target `make test-rt` builds for. */
    RT_CHECK(sizeof(UStrand) <= 200);
}

static void gc_traces_live_registers_only(void) {
    UCell *roots[2] = { NULL, NULL };
    struct UVM vm; fakevm_init(&vm, roots, 2);
    UStrand *s = ustrand_new(&vm, NULL); roots[0] = &s->cell;
    UProto proto = make_proto(5);
    UClosure *cl = uclosure_new(&vm, &proto, 0); roots[1] = &cl->cell;
    RT_CHECK(ustrand_push_frame(s, cl, uv_nil(), 0, 0) == 0);
    /* top = base(0) + max_reg(5) + 1 = 6. Register 2 is inside that
     * live window; register 50 (beyond top, though within stack_cap
     * after the explicit grow below) is a "dead" register no frame
     * claims -- ustrand_trace must not mark it. */
    UStr *live = ustr_new(&vm, "keep", 4);
    s->stack[2] = uv_str(live);
    RT_CHECK(ustrand_ensure_stack(s, 60) == 0);
    UStr *dead = ustr_new(&vm, "gone", 4);
    s->stack[50] = uv_str(dead);
    ugc_collect(&vm);
    uint32_t len; const char *bytes = uv_str_bytes(s->stack[2], &len);
    RT_CHECK(len == 4 && memcmp(bytes, "keep", 4) == 0);
    RT_EQ(vm.gc.cells_live, 3u);   /* s, cl, and the live string only */
    fakevm_destroy(&vm);
}

static void c_root_macro_keeps_value_alive(void) {
    UCell *roots[1] = { NULL };
    struct UVM vm; fakevm_init(&vm, roots, 1);
    UStrand *s = ustrand_new(&vm, NULL); roots[0] = &s->cell;
    UValue held = uv_str(ustr_new(&vm, "rooted", 6));
    USTRAND_ROOT(s, held);
    ugc_alloc(&vm, UCELL_STR, 16);   /* garbage: swept by the collect below */
    ugc_collect(&vm);
    uint32_t len; const char *bytes = uv_str_bytes(held, &len);
    RT_CHECK(len == 6 && memcmp(bytes, "rooted", 6) == 0);
    RT_EQ(vm.gc.cells_live, 2u);   /* s + the rooted string */
    USTRAND_UNROOT(s, held);
    fakevm_destroy(&vm);
}

RT_SUITE(rt_strand_suite) {
    rt_run("push_frame_grows_stack_for_max_reg", push_frame_grows_stack_for_max_reg);
    rt_run("push_frame_native_closure_uses_single_register", push_frame_native_closure_uses_single_register);
    rt_run("nested_frames_grow_stack_and_preserve_upvals", nested_frames_grow_stack_and_preserve_upvals);
    rt_run("close_upvals_partitions_by_index", close_upvals_partitions_by_index);
    rt_run("pop_frame_closes_its_upvals", pop_frame_closes_its_upvals);
    rt_run("cleanup_stack_grows_past_initial_cap", cleanup_stack_grows_past_initial_cap);
    rt_run("strand_struct_is_small", strand_struct_is_small);
    rt_run("gc_traces_live_registers_only", gc_traces_live_registers_only);
    rt_run("c_root_macro_keeps_value_alive", c_root_macro_keeps_value_alive);
}
