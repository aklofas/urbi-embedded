/* SPDX-License-Identifier: BSD-3-Clause */
/* The emitter's output, pinned two ways: the bytecode shape of a
 * construct (compiled through urbi_compile, loaded back with the real
 * loader and disassembled), and what a program evaluates to (run through
 * urbi_run on a fresh VM).  The shape cases hold the register
 * discipline to its promises; the run cases hold the shapes to their
 * meaning.  The last cases drive the diagnostic buffer. */

#include "utest.h"
#include "urbi/urbi.h"
#include "chunk/uchunk.h"
#include "emit/uemit.h"
#include <string.h>
#include <stdlib.h>
#define UTEST(name) static void name(void)

static char dis[65536];
/* Returns the disassembly of `src`'s chunk, or NULL (and prints the error). */
static const char *disasm_of(UVM *vm, const char *src) {
    uint8_t *bytes = NULL; size_t n = 0; char err[256] = {0};
    if (urbi_compile(vm, src, strlen(src), NULL, &bytes, &n, err, sizeof err) != URBI_OK) { printf("    compile: %s\n", err); return NULL; }
    UProto *root = NULL;
    if (uchunk_deserialize(&root, bytes, n, NULL, NULL, err, sizeof err) != UCHUNK_LOAD_OK) { printf("    load: %s\n", err); urbi_chunk_free(vm, bytes, n); return NULL; }
    uemit_disassemble(root, dis, sizeof dis);
    uchunk_destroy(root, NULL); urbi_chunk_free(vm, bytes, n);
    return dis;
}
static int count_of(const char *hay, const char *needle) { int c = 0; for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) c++; return c; }

/* Runs `src` on the main realm and checks it evaluates to the integer `want`. */
static void run_int(UVM *vm, const char *src, int64_t want) {
    UValue out; char err[256] = {0};
    int rc = urbi_run(vm, urbi_realm_main(vm), src, strlen(src), NULL, &out, err, sizeof err);
    if (rc != URBI_OK) {
        UErrorInfo info = {0};
        (void)urbi_last_error(vm, &info);
        printf("    run: rc %d: %s %s\n", rc, err, info.message ? info.message : "");
    }
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ(out.kind, URBI_VALUE_INT);
    UASSERT_EQ(out.v.i, want);
}

UTEST(every_function_loads_the_globals_object_first) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *d = disasm_of(vm, "var f = function(a) { a }");
    UASSERT(d && strncmp(d, "0000  LOAD_REALM_GLOBAL", 22) == 0);
    UASSERT(d && strstr(d, "; proto P0") && strstr(strstr(d, "; proto P0"), "LOAD_REALM_GLOBAL"));
    urbi_close(vm);
}

UTEST(a_local_read_emits_no_move) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *d = disasm_of(vm, "var f = function() { var x = 1; x + x }");
    const char *p0 = d ? strstr(d, "; proto P0") : NULL;
    UASSERT(p0 != NULL);
    if (p0) {
        UASSERT_EQ(0, count_of(p0, "MOVE"));
        UASSERT_EQ(1, count_of(p0, "ADD"));
    }
    urbi_close(vm);
}

UTEST(statement_temporaries_are_released_between_statements) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *d = disasm_of(vm, "var f = function() { 1 + 2; 3 + 4; 5 + 6 }");
    const char *p0 = d ? strstr(d, "; proto P0") : NULL;
    UASSERT(p0 != NULL);
    /* three ADDs into the same destination register */
    const char *a1 = p0 ? strstr(p0, "ADD R") : NULL;
    const char *a2 = a1 ? strstr(a1 + 1, "ADD R") : NULL;
    const char *a3 = a2 ? strstr(a2 + 1, "ADD R") : NULL;
    UASSERT(a1 && a2 && a3 && strncmp(a1, a3, 7) == 0 && strncmp(a1, a2, 7) == 0);
    urbi_close(vm);
}

UTEST(a_method_call_uses_self_and_the_method_bit) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *d = disasm_of(vm, "var o = Object.new(); o.m(1, 2)");
    UASSERT(d && strstr(d, "SELF R") && strstr(d, "CALL [method] R"));
    UASSERT(d && strstr(d, "2 args, 1 results"));
    urbi_close(vm);
}

UTEST(a_lazy_callee_wraps_the_flagged_argument_in_a_closure) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *d = disasm_of(vm, "var f = function(lazy x, y) { x }; f(1 + 1, 2)");
    UASSERT(d != NULL);
    /* root: CLOSURE for f, then CLOSURE for the thunk, then a LOADK 2 for y, then CALL */
    if (d) UASSERT_EQ(2, count_of(d, "CLOSURE R"));   /* f itself and one thunk */
    run_int(vm, "var g = function(lazy x, y) { x * 10 + y }; g(1 + 1, 2)", 22);
    /* A lazy parameter passed on travels as its thunk, whatever the callee. */
    run_int(vm, "var inner = function(t) { t() }; var outer = function(lazy y) { inner(y) }; outer(5 + 1)", 6);
    /* A function body calling a chunk-top lazy function wraps too. */
    run_int(vm, "var lz = function(lazy x) { x }; var w = function() { lz(2 + 3) }; w()", 5);
    urbi_close(vm);
}

UTEST(the_256th_site_emits_extarg) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    char src[8192]; size_t at = 0;
    at += (size_t)snprintf(src + at, sizeof src - at, "var o = Object.new(); ");
    for (int i = 0; i < 300; i++) at += (size_t)snprintf(src + at, sizeof src - at, "o.s%d = %d; ", i, i);
    const char *d = disasm_of(vm, src);
    UASSERT(d && count_of(d, "EXTARG") >= 44);         /* sites 256..299 */
    UASSERT(d && strstr(d, "site 299"));
    /* and the high sites address the right slots */
    at += (size_t)snprintf(src + at, sizeof src - at, "o.s299 * 1000 + o.s3");
    run_int(vm, src, 299003);
    urbi_close(vm);
}

UTEST(switch_break_exits_the_switch_and_continue_reaches_the_loop) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "var i = 0; var seen = 0; while (i < 3) { i = i + 1; switch (i) { case 2: break; }; seen = seen + 1 }; seen", 3);
    run_int(vm, "var j = 0; var hits = 0; while (j < 3) { j = j + 1; switch (j) { case 2: continue; }; hits = hits + 1 }; hits", 2);
    urbi_close(vm);
}

UTEST(multiplication_binds_tighter_than_addition) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "1 + 2 * 3", 7);
    urbi_close(vm);
}

UTEST(a_closure_keeps_the_loop_variable_of_its_own_iteration) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm,
        "var o = Object.new(); var i = 0;"
        "while (i < 3) { var j = i; o.f = function() { j }; if (i == 1) { o.g = o.f }; i = i + 1 };"
        "o.g() * 10 + o.f()", 12);
    urbi_close(vm);
}

UTEST(return_leaves_nested_blocks) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "var f = function(x) { if (x > 0) { { var y = 1; return y } }; 2 }; f(1) * 10 + f(0)", 12);
    urbi_close(vm);
}

/* A block with locals of its own whose value is used: as a class
 * declaration's initializer in the middle of a function, as a function's
 * last statement, and as an if arm.  Its value has to land where the
 * enclosing code expects it, above nothing it clobbers. */
UTEST(a_block_with_locals_yields_its_value) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "var f = function(a, b) { a * 10 + b };"
                "var g = function() { var q = 7; class K { var v = q + 1 }; f(q, K.v) }; g()", 78);
    run_int(vm, "var h = function() { var k = 1; { var c = 5; c * 2 + k } }; h()", 11);
    run_int(vm, "var i = function(p) { if (p > 0) { var r = p * 3; r + 1 } else { 0 } }; i(2) * 10 + i(0)", 70);
    urbi_close(vm);
}

/* An expression-position block's value is moved down to the register the
 * block started at, so whatever the enclosing expression compiles next
 * reuses the block's locals.  `o[0] += 1` lowers to a block with two
 * hidden locals; nested a hundred deep as left operands, each level holds
 * one register with the move and three without it, which is past the
 * 255-register cap. */
UTEST(a_block_value_moves_below_its_locals) {
    static char src[8192];
    size_t at = (size_t)snprintf(src, sizeof src,
        "var o = Object.new(); var o.get = function(k) { 0 }; var o.set = function(k, v) { v };"
        "(function() { ");
    for (int d = 0; d < 100; d++) at += (size_t)snprintf(src + at, sizeof src - at, "(o[0] += 1) + (");
    at += (size_t)snprintf(src + at, sizeof src - at, "0");
    for (int d = 0; d < 100; d++) at += (size_t)snprintf(src + at, sizeof src - at, ")");
    (void)snprintf(src + at, sizeof src - at, " })()");
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, src, 100);
    urbi_close(vm);
}

/* The switch subject is pinned: a case body's own locals and
 * temporaries go above it. */
UTEST(a_switch_subject_survives_its_case_bodies) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "var f = function(x) { var acc = 0; switch (x * 1) { case 1: { var t = 10; acc = acc + t + x }; case 2: acc = 2 }; acc * 100 + x };"
                "f(1) * 1000 + f(2)", 1101202);
    urbi_close(vm);
}

UTEST(default_parameters_fill_omitted_arguments) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "var f = function(a, b = a + 1) { a * 10 + b }; f(4) * 100 + f(4, 9)", 4549);
    urbi_close(vm);
}

UTEST(logical_operators_short_circuit) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "var n = 0; var bump = function() { n = n + 1; true }; (false && bump()); (true || bump()); (true && bump()); n", 1);
    urbi_close(vm);
}

/* --- diagnostics --------------------------------------------------------- */

UTEST(an_emit_error_names_its_line_and_column) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    uint8_t *bytes = NULL; size_t n = 0; char err[256] = {0};
    const char *src = "1;\n  this";
    UASSERT_EQ(URBI_ERR_COMPILE, urbi_compile(vm, src, strlen(src), NULL, &bytes, &n, err, sizeof err));
    UASSERT_STR_EQ("<stdin>:2:3: this used outside a method or nested closure", err);
    const char *src2 = "var f = function(lazy x) { x = 1 }";
    UASSERT_EQ(URBI_ERR_COMPILE, urbi_compile(vm, src2, strlen(src2), NULL, &bytes, &n, err, sizeof err));
    UASSERT_STR_EQ("<stdin>:1:28: cannot assign to lazy parameter 'x'", err);
    urbi_close(vm);
}

/* A cap the emitter hits below a statement -- here the local-variable
 * cap, one `var` per line -- reports the position of the node being
 * compiled, not a bare file name. */
UTEST(a_too_many_locals_error_names_its_line) {
    static char src[8192];
    size_t at = (size_t)snprintf(src, sizeof src, "var f = function() {\n");
    for (int i = 0; i < 201; i++) at += (size_t)snprintf(src + at, sizeof src - at, "  var a%d = 0;\n", i);
    (void)snprintf(src + at, sizeof src - at, "}");
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    uint8_t *bytes = NULL; size_t n = 0; char err[256] = {0};
    UASSERT_EQ(URBI_ERR_COMPILE, urbi_compile(vm, src, strlen(src), NULL, &bytes, &n, err, sizeof err));
    /* Line 201 is `var a199`: the function has one hidden local already. */
    UASSERT_STR_EQ("<stdin>:201:3: too many local variables in function (max 200)", err);
    urbi_close(vm);
}

/* The break/continue patch list per loop is a fixed-size array; past its
 * cap a further break latches a diagnostic instead of overflowing it.
 * Built with a loop rather than typed out, since the count is the
 * point, not any particular break site. */
UTEST(the_break_patch_list_caps_at_sixteen_sites) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    char src[4096]; size_t at; uint8_t *bytes = NULL; size_t n = 0; char err[256] = {0};
    at = (size_t)snprintf(src, sizeof src, "while (true) { ");
    for (int k = 0; k < 16; k++) at += (size_t)snprintf(src + at, sizeof src - at, "break; ");
    (void)snprintf(src + at, sizeof src - at, "}");
    UASSERT_EQ(URBI_OK, urbi_compile(vm, src, strlen(src), NULL, &bytes, &n, err, sizeof err));
    urbi_chunk_free(vm, bytes, n);
    at = (size_t)snprintf(src, sizeof src, "while (true) { ");
    for (int k = 0; k < 17; k++) at += (size_t)snprintf(src + at, sizeof src - at, "break; ");
    (void)snprintf(src + at, sizeof src - at, "}");
    UASSERT_EQ(URBI_ERR_COMPILE, urbi_compile(vm, src, strlen(src), NULL, &bytes, &n, err, sizeof err));
    UASSERT(strstr(err, "max 16") != NULL);
    urbi_close(vm);
}

/* A line delta is one signed byte (-128 is the absolute-checkpoint
 * sentinel), so a gap past 127 lines between two instructions forces an
 * abs_lines checkpoint instead of a running delta.  Line 1 and line 200
 * sit on opposite sides of that boundary, so the second checkpoint is
 * the one `uproto_line_at` has to find for the proto's last
 * instruction -- the "line 200: " prefix on the thrown error proves the
 * lookup walked both checkpoints, not just the first. */
UTEST(a_line_table_checkpoint_survives_a_128_line_gap) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    char src[4096]; size_t at = 0;
    at += (size_t)snprintf(src + at, sizeof src - at, "var a = 1;\n");
    for (int i = 0; i < 198; i++) at += (size_t)snprintf(src + at, sizeof src - at, "\n");
    at += (size_t)snprintf(src + at, sizeof src - at, "1 + \"x\"");
    UValue out; char err[256] = {0};
    int rc = urbi_run(vm, urbi_realm_main(vm), src, strlen(src), NULL, &out, err, sizeof err);
    UASSERT_EQ(URBI_ERR_UNCAUGHT_THROW, rc);
    UErrorInfo info = {0};
    (void)urbi_last_error(vm, &info);
    UASSERT(info.message && strncmp(info.message, "line 200: ", 10) == 0);
    urbi_close(vm);
}

UTEST(the_diag_buffer_keeps_warnings_and_errors_in_order) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    UProto root;
    memset(&root, 0, sizeof root);
    UArena arena;
    uarena_init(&arena, 0);
    UEmitter *e = uemit_new(&root, &arena, vm, "probe.u");
    UASSERT(e != NULL);
    if (e) {
        UAstNode at;
        memset(&at, 0, sizeof at);
        at.line = 3; at.col = 7;
        char buf[128] = {0};
        UASSERT(!urbi_emit_diag_format_first_error(e, buf, sizeof buf));   /* a warning alone is not an error */
        urbi_emit_diag_warn(e, &at, "careful: %d", 1);
        UASSERT(!urbi_emit_diag_format_first_error(e, buf, sizeof buf));
        urbi_emit_diag_error(e, &at, "broken: %s", "x");
        urbi_emit_diag_error(e, NULL, "unpositioned");
        UASSERT_EQ(3, uemit_diag_count(e));
        const UEmitDiag *w = uemit_diag_at(e, 0), *x = uemit_diag_at(e, 1), *y = uemit_diag_at(e, 2);
        UASSERT(w && w->level == UEMIT_DIAG_WARN && w->line == 3 && w->col == 7 && strcmp(w->message, "careful: 1") == 0);
        UASSERT(x && x->level == UEMIT_DIAG_ERROR && strcmp(x->message, "broken: x") == 0);
        UASSERT(y && y->line == 0 && y->col == 0);
        UASSERT(uemit_diag_at(e, 3) == NULL);
        UASSERT(urbi_emit_diag_format_first_error(e, buf, sizeof buf));
        UASSERT_STR_EQ("probe.u:3:7: broken: x", buf);
        UASSERT_EQ(EMIT_OK, uemit_error(e));
        urbi_emit_abandon(e);
    }
    uchunk_destroy(&root, NULL);
    uarena_destroy(&arena);
    urbi_close(vm);
}

/* --- try, finally, tag scopes, and jumps across them --- */

UTEST(finally_is_emitted_once) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *d = disasm_of(vm, "var r = 0; try { r = 1 } finally { r = r + 100 }; r");
    UASSERT(d && count_of(d, "RESUME") == 1 && count_of(d, "SCOPE_POP try+finally") == 1);
    /* the constant 100 is loaded by exactly one instruction */
    UASSERT(d && 1 == count_of(d, "LOADK R1, K2") + count_of(d, "LOADK R2, K2") + count_of(d, "LOADK R3, K2"));
    urbi_close(vm);
}
UTEST(finally_runs_once_on_the_normal_path) { UVM *vm = urbi_open(utest_alloc, NULL, NULL); run_int(vm, "var r = 0; try { r = 1 } finally { r = r + 100 }; r", 101); urbi_close(vm); }
UTEST(finally_runs_once_on_a_throw_then_the_catch) { UVM *vm = urbi_open(utest_alloc, NULL, NULL); run_int(vm, "var r = 0; try { try { throw 1 } finally { r = r + 1 } } catch (var e) { r = r + 10 }; r", 11); urbi_close(vm); }
UTEST(break_across_a_finally_runs_it_and_leaves_the_loop) { UVM *vm = urbi_open(utest_alloc, NULL, NULL); run_int(vm, "var r = 0; var i = 0; while (i < 5) { i = i + 1; try { if (i == 2) break; r = r + 1 } finally { r = r + 10 } }; r + i * 100", 221); urbi_close(vm); }
UTEST(continue_across_a_tag_scope_lands_and_pops) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *src = "var t = Tag.new(); var i = 0; var n = 0; while (i < 3) { i = i + 1; t: { if (i < 10) continue; n = n + 100 } }; i * 10 + n";
    run_int(vm, src, 30);
    const char *d = disasm_of(vm, src);
    UASSERT(d && count_of(d, "UNWIND_TO") == 1 && count_of(d, "UNWIND_TO depth=1 ->") == 1);
    urbi_close(vm);
}
UTEST(return_through_a_finally_in_a_function) { UVM *vm = urbi_open(utest_alloc, NULL, NULL); run_int(vm, "var r = 0; var f = function() { try { return 5 } finally { r = 7 } }; f() + r", 12); urbi_close(vm); }
UTEST(catch_guard_rethrows_when_false) { UVM *vm = urbi_open(utest_alloc, NULL, NULL); run_int(vm, "var r = 0; try { try { throw 3 } catch (var e if e == 4) { r = 1 } } catch (var e) { r = e * 10 }; r", 30); urbi_close(vm); }
UTEST(try_else_runs_only_without_a_throw) { UVM *vm = urbi_open(utest_alloc, NULL, NULL); run_int(vm, "var r = 0; try { 1 } catch (var e) { r = 1 } else { r = 2 }; r", 2); urbi_close(vm); }
UTEST(a_tag_scope_above_register_fifteen) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "var f = function() { var a1=1; var a2=1; var a3=1; var a4=1; var a5=1; var a6=1; var a7=1; var a8=1; var a9=1; var a10=1; var a11=1; var a12=1; var a13=1; var a14=1; var a15=1; var a16=1; var a17=1; var a18=1; var t = Tag.new(); var r = 0; t: { r = 42 }; r }; f()", 42);
    const char *d = disasm_of(vm, "var f = function() { var a1=1; var a2=1; var a3=1; var a4=1; var a5=1; var a6=1; var a7=1; var a8=1; var a9=1; var a10=1; var a11=1; var a12=1; var a13=1; var a14=1; var a15=1; var a16=1; var a17=1; var a18=1; var t = Tag.new(); t: { 1 } }");
    const char *st = d ? strstr(d, "SCOPE_TAG R") : NULL;
    UASSERT(st && atoi(st + strlen("SCOPE_TAG R")) > 15);   /* the tag register is well above the old nibble cap */
    urbi_close(vm);
}
UTEST(the_try_value_is_the_body_or_the_catch) { UVM *vm = urbi_open(utest_alloc, NULL, NULL); run_int(vm, "(try { 1 } catch (var e) { 2 }) + (try { throw 0 } catch (var e) { 20 })", 21); urbi_close(vm); }

/* A `continue` lands on the back-edge CLOSE, so each iteration's captured
 * local gets a cell of its own: a jump past the CLOSE would leave the
 * three closures sharing one cell, all reading 2. */
UTEST(a_continue_closes_the_iteration_cells) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm,
        "var k0 = 0; var k1 = 0; var k2 = 0; var i = 0;"
        "while (i < 3) { var j = i; i = i + 1;"
        "  if (j == 0) { k0 = function() { j } }; if (j == 1) { k1 = function() { j } }; if (j == 2) { k2 = function() { j } };"
        "  if (i < 10) continue; i = 99 };"
        "k0() + k1() * 10 + k2() * 100", 210);
    urbi_close(vm);
}
/* The same, with the continue crossing a finally: an UNWIND_TO whose
 * target is the same back-edge CLOSE. */
UTEST(a_continue_across_a_finally_closes_the_iteration_cells) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *src =
        "var k0 = 0; var k1 = 0; var k2 = 0; var i = 0; var n = 0;"
        "while (i < 3) { var j = i; i = i + 1;"
        "  if (j == 0) { k0 = function() { j } }; if (j == 1) { k1 = function() { j } }; if (j == 2) { k2 = function() { j } };"
        "  try { if (i < 10) continue } finally { n = n + 1 }; i = 99 };"
        "k0() + k1() * 10 + k2() * 100 + n * 1000";
    run_int(vm, src, 3210);
    const char *d = disasm_of(vm, src);
    UASSERT(d && count_of(d, "UNWIND_TO depth=1 ->") == 1);
    urbi_close(vm);
}

/* A closure made in a scope the walker abandons keeps its own value: the
 * handler, the finally body and the code after a stopped tag scope reuse
 * the abandoned body's registers, so the cells must be closed first. */
UTEST(a_cell_from_an_abandoned_try_body_survives_the_catch) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "var f = function() { var g = nil; try { var x = 1; g = function() { x }; throw 7 } catch (var e) { var y = 4 }; g() }; f()", 1);
    urbi_close(vm);
}
UTEST(a_cell_from_a_body_left_by_break_survives_the_finally) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "var f = function() { var g = nil; while (true) { try { var x = 1; g = function() { x }; break } finally { var y = 4 } }; g() }; f()", 1);
    urbi_close(vm);
}
UTEST(a_cell_from_a_stopped_tag_scope_survives_it) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "var f = function() { var t = Tag.new(); var g = nil; t: { var x = 1; g = function() { x }; t.stop() }; var y = 4; g() }; f()", 1);
    urbi_close(vm);
}

/* A break counts only the scopes opened inside its loop: the catch or tag
 * scope around the loop is still in place after it. */
UTEST(a_break_leaves_the_scope_around_its_loop_in_place) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "var r = 0; try { while (true) { try { break } finally { r = r + 1 } }; throw 5 } catch (var e) { r = r + e * 10 }; r", 51);
    urbi_close(vm);
}
UTEST(a_break_leaves_the_tag_scope_around_its_loop_in_place) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    /* t.stop() after the loop resumes after t's scope, skipping r = 1. */
    run_int(vm, "var t = Tag.new(); var u = Tag.new(); var r = 0; t: { while (true) { u: { break } }; t.stop(); r = 1 }; r * 10 + 2", 2);
    urbi_close(vm);
}

UTEST(continue_across_a_tag_scope_fires_leave_each_time) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    UValue out; char err[256] = {0};
    const char *src = "var t = Tag.new(); var n = 0; at (t.leave?) n = n + 1; var i = 0; while (i < 3) { i = i + 1; t: { if (i < 10) continue; n = n + 100 } }; 0";
    UASSERT_EQ(URBI_OK, urbi_run(vm, urbi_realm_main(vm), src, strlen(src), NULL, &out, err, sizeof err));
    uint64_t wake = 0; (void)urbi_step(vm, 1000, &wake);   /* the at body runs when the scheduler steps */
    run_int(vm, "n", 3);
    urbi_close(vm);
}

UTEST(comma_forks_all_but_the_last) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *d = disasm_of(vm, "a(), b(), c()");
    UASSERT(d && count_of(d, "FORK R") == 2 && count_of(d, "detach") == 2);
    /* the last child runs inline after both forks and is the value */
    const char *f1 = d ? strstr(d, "FORK R") : NULL;
    const char *f2 = f1 ? strstr(f1 + 1, "FORK R") : NULL;
    const char *p0 = d ? strstr(d, "; proto P0") : NULL;
    const char *call = f2 ? strstr(f2, "CALL R") : NULL;
    UASSERT(call && p0 && call < p0 && !strstr(d, "LOADVOID"));
    urbi_close(vm);
    vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "1, 7", 7);
    urbi_close(vm);
}
UTEST(amp_forks_the_rhs_and_joins) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *d = disasm_of(vm, "a() & b()");
    UASSERT(d && count_of(d, "join") == 1 && count_of(d, "JOIN_WAIT") == 1);
    const char *fk = d ? strstr(d, "FORK R") : NULL, *jw = d ? strstr(d, "JOIN_WAIT R") : NULL;
    const char *arrow = fk ? strstr(fk, "-> R") : NULL;
    UASSERT(arrow && jw && atoi(arrow + 4) == atoi(jw + strlen("JOIN_WAIT R")));   /* same handle register */
    urbi_close(vm);
}
UTEST(an_install_without_an_alternate_body_leaves_r_a_plus_2_alone) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *d = disasm_of(vm, "whenever (x > 1) echo(1)");
    UASSERT(d && strstr(d, "INSTALL R") && strstr(d, "mode=3 flags=1"));
    int nclosures = d ? count_of(d, "CLOSURE R") : -1;
    UASSERT_EQ(2, nclosures);   /* cond and body only */
    const char *d2 = disasm_of(vm, "at (x > 1) echo(1) onleave echo(2)");
    UASSERT(d2 && strstr(d2, "mode=1 flags=3") && count_of(d2, "CLOSURE R") == 3);
    urbi_close(vm);
}
/* The case above pins HAS_ALT absence by disassembly text only; this one
 * pins the behaviour.  A call argument leaves a closure sitting in R3
 * (the watcher's base+2, its else-arm slot) right before a `whenever`
 * with no else installs at the same base register -- the VM must never
 * read that slot without HAS_ALT, so the leftover closure (which would
 * bump a counter if called) must never run, neither at install time nor
 * across the watcher's own rising and falling edges.  (Confirmed
 * sensitive: unconditionally reading R[base+2] regardless of HAS_ALT
 * makes this fail with bumped == 1 after the falling edge.) */
UTEST(an_install_without_an_alternate_never_calls_the_leftover_in_r_a_plus_2) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *src =
        "Realm.bumped = 0; Realm.c = 0; var ignore2 = function(a, b) { 0 };"
        "ignore2(0, function() { Realm.bumped = Realm.bumped + 1 });"
        "whenever (Realm.c == 1) { echo(1); Realm.c = 0 }";
    const char *d = disasm_of(vm, src);
    UASSERT(d && strstr(d, "INSTALL R1 mode=3 flags=1"));
    UASSERT(d && strstr(d, "CLOSURE R3, P1"));   /* the leftover lands at base+2 = R3 */

    UValue out; char err[256] = {0};
    UASSERT_EQ(URBI_OK, urbi_run(vm, urbi_realm_main(vm), src, strlen(src), NULL, &out, err, sizeof err));
    uint64_t wake = 0;
    for (int i = 0; i < 5; i++) (void)urbi_step(vm, 1000, &wake);
    run_int(vm, "Realm.bumped", 0);   /* install alone must not call it */
    UASSERT_EQ(URBI_OK, urbi_run(vm, urbi_realm_main(vm), "Realm.c = 1", 11, NULL, &out, err, sizeof err));
    for (int i = 0; i < 5; i++) (void)urbi_step(vm, 1000, &wake);
    run_int(vm, "Realm.bumped", 0);   /* nor the rising edge (body) and its own falling edge */
    urbi_close(vm);
}
UTEST(event_bodies_take_the_payload_parameter) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    UValue out; char err[256] = {0};
    const char *src = "var e = Event.new(); var got = 0; at (e?(var v)) got = v; e!(7); got";
    /* at fires after the emitting statement yields; drive a step */
    int rc = urbi_run(vm, urbi_realm_main(vm), src, strlen(src), NULL, &out, err, sizeof err);
    UASSERT_EQ(URBI_OK, rc);
    uint64_t wake = 0; (void)urbi_step(vm, 1000, &wake);
    rc = urbi_run(vm, urbi_realm_main(vm), "got", 3, NULL, &out, err, sizeof err);
    UASSERT_EQ(URBI_OK, rc); UASSERT_EQ(7, (int)out.v.i);
    urbi_close(vm);
}
UTEST(slot_change_source_uses_getslot_change_event) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *d = disasm_of(vm, "var o = Object.new(); var o.x = 1; at (o.x.changed?) echo(1)");
    UASSERT(d && strstr(d, "GETSLOT_CHANGE_EVENT") && strstr(d, "mode=4"));
    urbi_close(vm);
}
UTEST(waituntil_installs_with_the_waituntil_mode) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *d = disasm_of(vm, "waituntil (x > 1)");
    UASSERT(d && strstr(d, "mode=7 flags=0"));
    urbi_close(vm);
}

UTEST(a_local_operand_is_copied_before_a_call_in_the_right_operand) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    run_int(vm, "var g = function() { var x = 1; var bump = function() { x = 2; 10 }; x + bump() }; g()", 11);
    run_int(vm, "var h = function() { var x = 1; var bump = function() { x = 5; 3 }; if (x < bump()) { 1 } else { 0 } }; h()", 1);
    run_int(vm, "var k = function() { var o = Object.new(); o.v = 0; var o1 = o; var swap = function() { o = Object.new(); 9 }; o.v = swap(); o1.v }; k()", 9);
    urbi_close(vm);
}
UTEST(a_local_operand_is_not_copied_before_a_pure_right_operand) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *d = disasm_of(vm, "var f = function() { var x = 1; var y = 2; x + 1; x + y; x < y }");
    const char *p0 = d ? strstr(d, "; proto P0") : NULL;
    UASSERT(p0 != NULL);
    if (p0) UASSERT_EQ(0, count_of(p0, "MOVE"));
    /* A global callee loads straight into a fresh temporary, so the one
     * MOVE is the snapshot of x, emitted before the CALL. */
    d = disasm_of(vm, "var g = function() { var x = 1; x + h() }");
    p0 = d ? strstr(d, "; proto P0") : NULL;
    UASSERT(p0 != NULL);
    if (p0) {
        UASSERT_EQ(1, count_of(p0, "MOVE"));
        UASSERT(strstr(p0, "MOVE") < strstr(p0, "CALL"));
    }
    urbi_close(vm);
}

UTEST(an_upvalue_read_is_a_pure_right_operand) {
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    /* The inner function reads y through GETUPVAL, which runs no code, so
     * the register of x is used in place with no snapshot. */
    const char *d = disasm_of(vm, "var f = function(y) { function(x) { x + y } }");
    const char *up = d ? strstr(d, "GETUPVAL") : NULL;
    UASSERT(up != NULL);
    if (up) {
        const char *start = up;
        while (start > d && strncmp(start, "; proto", 7) != 0) start--;
        const char *end = strstr(up, "; proto");
        size_t len = end ? (size_t)(end - start) : strlen(start);
        char inner[2048] = {0};
        if (len >= sizeof inner) len = sizeof inner - 1U;
        memcpy(inner, start, len);
        UASSERT(strstr(inner, "ADD") != NULL);
        UASSERT_EQ(0, count_of(inner, "MOVE"));
    }
    urbi_close(vm);
}

void test_emit_bytecode_suite(void) {
    utest_run("every_function_loads_the_globals_object_first", every_function_loads_the_globals_object_first);
    utest_run("a_local_read_emits_no_move", a_local_read_emits_no_move);
    utest_run("statement_temporaries_are_released_between_statements", statement_temporaries_are_released_between_statements);
    utest_run("a_method_call_uses_self_and_the_method_bit", a_method_call_uses_self_and_the_method_bit);
    utest_run("a_lazy_callee_wraps_the_flagged_argument_in_a_closure", a_lazy_callee_wraps_the_flagged_argument_in_a_closure);
    utest_run("the_256th_site_emits_extarg", the_256th_site_emits_extarg);
    utest_run("switch_break_exits_the_switch_and_continue_reaches_the_loop", switch_break_exits_the_switch_and_continue_reaches_the_loop);
    utest_run("multiplication_binds_tighter_than_addition", multiplication_binds_tighter_than_addition);
    utest_run("a_closure_keeps_the_loop_variable_of_its_own_iteration", a_closure_keeps_the_loop_variable_of_its_own_iteration);
    utest_run("return_leaves_nested_blocks", return_leaves_nested_blocks);
    utest_run("a_block_with_locals_yields_its_value", a_block_with_locals_yields_its_value);
    utest_run("a_block_value_moves_below_its_locals", a_block_value_moves_below_its_locals);
    utest_run("a_switch_subject_survives_its_case_bodies", a_switch_subject_survives_its_case_bodies);
    utest_run("default_parameters_fill_omitted_arguments", default_parameters_fill_omitted_arguments);
    utest_run("logical_operators_short_circuit", logical_operators_short_circuit);
    utest_run("an_emit_error_names_its_line_and_column", an_emit_error_names_its_line_and_column);
    utest_run("a_too_many_locals_error_names_its_line", a_too_many_locals_error_names_its_line);
    utest_run("the_break_patch_list_caps_at_sixteen_sites", the_break_patch_list_caps_at_sixteen_sites);
    utest_run("a_line_table_checkpoint_survives_a_128_line_gap", a_line_table_checkpoint_survives_a_128_line_gap);
    utest_run("the_diag_buffer_keeps_warnings_and_errors_in_order", the_diag_buffer_keeps_warnings_and_errors_in_order);
    utest_run("finally_is_emitted_once", finally_is_emitted_once);
    utest_run("finally_runs_once_on_the_normal_path", finally_runs_once_on_the_normal_path);
    utest_run("finally_runs_once_on_a_throw_then_the_catch", finally_runs_once_on_a_throw_then_the_catch);
    utest_run("break_across_a_finally_runs_it_and_leaves_the_loop", break_across_a_finally_runs_it_and_leaves_the_loop);
    utest_run("continue_across_a_tag_scope_lands_and_pops", continue_across_a_tag_scope_lands_and_pops);
    utest_run("return_through_a_finally_in_a_function", return_through_a_finally_in_a_function);
    utest_run("catch_guard_rethrows_when_false", catch_guard_rethrows_when_false);
    utest_run("try_else_runs_only_without_a_throw", try_else_runs_only_without_a_throw);
    utest_run("a_tag_scope_above_register_fifteen", a_tag_scope_above_register_fifteen);
    utest_run("the_try_value_is_the_body_or_the_catch", the_try_value_is_the_body_or_the_catch);
    utest_run("a_continue_closes_the_iteration_cells", a_continue_closes_the_iteration_cells);
    utest_run("a_continue_across_a_finally_closes_the_iteration_cells", a_continue_across_a_finally_closes_the_iteration_cells);
    utest_run("a_cell_from_an_abandoned_try_body_survives_the_catch", a_cell_from_an_abandoned_try_body_survives_the_catch);
    utest_run("a_cell_from_a_body_left_by_break_survives_the_finally", a_cell_from_a_body_left_by_break_survives_the_finally);
    utest_run("a_cell_from_a_stopped_tag_scope_survives_it", a_cell_from_a_stopped_tag_scope_survives_it);
    utest_run("a_break_leaves_the_scope_around_its_loop_in_place", a_break_leaves_the_scope_around_its_loop_in_place);
    utest_run("a_break_leaves_the_tag_scope_around_its_loop_in_place", a_break_leaves_the_tag_scope_around_its_loop_in_place);
    utest_run("continue_across_a_tag_scope_fires_leave_each_time", continue_across_a_tag_scope_fires_leave_each_time);
    utest_run("comma_forks_all_but_the_last", comma_forks_all_but_the_last);
    utest_run("amp_forks_the_rhs_and_joins", amp_forks_the_rhs_and_joins);
    utest_run("an_install_without_an_alternate_body_leaves_r_a_plus_2_alone", an_install_without_an_alternate_body_leaves_r_a_plus_2_alone);
    utest_run("an_install_without_an_alternate_never_calls_the_leftover_in_r_a_plus_2", an_install_without_an_alternate_never_calls_the_leftover_in_r_a_plus_2);
    utest_run("event_bodies_take_the_payload_parameter", event_bodies_take_the_payload_parameter);
    utest_run("slot_change_source_uses_getslot_change_event", slot_change_source_uses_getslot_change_event);
    utest_run("waituntil_installs_with_the_waituntil_mode", waituntil_installs_with_the_waituntil_mode);
    utest_run("a_local_operand_is_copied_before_a_call_in_the_right_operand", a_local_operand_is_copied_before_a_call_in_the_right_operand);
    utest_run("a_local_operand_is_not_copied_before_a_pure_right_operand", a_local_operand_is_not_copied_before_a_pure_right_operand);
    utest_run("an_upvalue_read_is_a_pure_right_operand", an_upvalue_read_is_a_pure_right_operand);
}
