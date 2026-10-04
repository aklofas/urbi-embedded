/* SPDX-License-Identifier: BSD-3-Clause */
/* The loader's frame-layout rule: parameters and the hidden argument-count
 * register must fit the register window; upvalues live outside it.  And the
 * verifier's terminal-return rule: every proto ends in an executable RET.
 * Each hand-built case is a complete wire-v2 chunk, byte for byte. */
#include "utest.h"
#include "urbi/urbi.h"
#include "chunk/uchunk.h"
#include <string.h>
#define UTEST(name) static void name(void)

static UChunkLoadError load_bytes(const uint8_t *b, size_t n, char *err, size_t cap) {
    UProto *root = NULL;
    UChunkLoadError rc = uchunk_deserialize(&root, b, n, NULL, NULL, err, cap);
    if (root) uchunk_destroy(root, NULL);
    return rc;
}

/* Header flag bit 0 set (arity prologue).  Root: max_reg 8, one nested
 * proto with max_reg 0, nupvals 0, nparams 1 -- the parameter fits but
 * the hidden count register R[1] does not. */
static const uint8_t hidden_nargs_overflow[] = {
    0x55,0x52,0x42,0x49,0x20,0x01,0x19,0x93,0x0d,0x0a,0x1a,0x0a,0x08,0x08,0x04,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x06,0x3c,0x65,0x78,0x70,0x72,0x3e,0x08,
    0x00,0x00,0x00,0x03,0x0d,0x08,0x00,0x00,0x0f,0x08,0x01,0x01,0x07,0x08,0x00,0x00,
    0x03,0x00,0x00,0x00,0x00,0x00,0x01,0x06,0x00,0x07,0x00,0x01,0x07,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x00
};

UTEST(a_callee_whose_hidden_count_register_falls_outside_its_window_is_rejected) {
    char err[160] = {0};
    UASSERT_EQ((int)UCHUNK_LOAD_CORRUPT, (int)load_bytes(hidden_nargs_overflow, sizeof hidden_nargs_overflow, err, sizeof err));
    UASSERT(strstr(err, "hidden") != NULL || strstr(err, "nparams") != NULL);
}

UTEST(upvalues_are_not_counted_against_the_register_window) {
    /* A closure with four upvalues and two registers compiles from source;
     * the loader must accept what the emitter produced. */
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    const char *src = "var make = function(a,b,c,d){ function(){ a; b; c; d } }; make(1,2,3,4)()";
    uint8_t *bytes = NULL; size_t n = 0; char err[256] = {0};
    UASSERT_EQ(URBI_OK, urbi_compile(vm, src, strlen(src), NULL, &bytes, &n, err, sizeof err));
    UValue out = urbi_make_nil();
    UASSERT_EQ(URBI_OK, urbi_load(vm, NULL, bytes, n, &out));
    urbi_chunk_free(vm, bytes, n);
    urbi_close(vm);
}

/* A correctly framed chunk whose root has no instructions at all. */
static const uint8_t empty_root[] = {
    0x55,0x52,0x42,0x49,0x20,0x01,0x19,0x93,0x0d,0x0a,0x1a,0x0a,0x08,0x08,0x04,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x06,0x3c,0x65,0x78,0x70,0x72,0x3e,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
/* Root: CLOSURE R0 <- P0 whose one-entry upvalue prelude word has OP_RET in
 * its low byte and is the last word of the stream. */
static const uint8_t prelude_word_as_last[] = {
    0x55,0x52,0x42,0x49,0x20,0x01,0x19,0x93,0x0d,0x0a,0x1a,0x0a,0x08,0x08,0x04,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x06,0x3c,0x65,0x78,0x70,0x72,0x3e,0x00,
    0x00,0x00,0x00,0x02,0x0d,0x00,0x00,0x00,0x07,0x00,0x01,0x00,0x02,0x00,0x00,0x00,
    0x00,0x01,0x00,0x01,0x00,0x00,0x01,0x00,0x07,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x00
};

UTEST(a_proto_with_no_instructions_is_rejected) {
    char err[160] = {0};
    UASSERT_EQ((int)UCHUNK_LOAD_CORRUPT, (int)load_bytes(empty_root, sizeof empty_root, err, sizeof err));
}
UTEST(an_upvalue_prelude_word_is_not_a_terminal_return) {
    char err[160] = {0};
    UASSERT_EQ((int)UCHUNK_LOAD_CORRUPT, (int)load_bytes(prelude_word_as_last, sizeof prelude_word_as_last, err, sizeof err));
}

void test_loader_frame_layout_suite(void) {
    utest_run("a_callee_whose_hidden_count_register_falls_outside_its_window_is_rejected",
              a_callee_whose_hidden_count_register_falls_outside_its_window_is_rejected);
    utest_run("upvalues_are_not_counted_against_the_register_window",
              upvalues_are_not_counted_against_the_register_window);
    utest_run("a_proto_with_no_instructions_is_rejected", a_proto_with_no_instructions_is_rejected);
    utest_run("an_upvalue_prelude_word_is_not_a_terminal_return", an_upvalue_prelude_word_is_not_a_terminal_return);
}
