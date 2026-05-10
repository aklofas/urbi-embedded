/* SPDX-License-Identifier: BSD-3-Clause */
/* test_lex_unicode.c — Phase 1 of v0.6.1: \uXXXX / \u{HHHHHH} Unicode
 * escape support at lex.
 *
 * Covers the encode_utf8 helper that emits 1-4 UTF-8 bytes for a code
 * point, and the lex-time + parse-time wiring of the two escape forms
 * (the latter via end-to-end string-literal fixtures under
 * tests/chk/lex/strings/).  This file pins the encoder's bit-level
 * behaviour against four-byte boundary inputs that the chk fixtures
 * cannot easily express. */

#include "utest.h"
#include "lex/ulex_internal.h"
#include <string.h>

static void encode_utf8_ascii(void) {
    /* U+0041 'A' → 1 byte 0x41. */
    unsigned char buf[4] = {0};
    int n = urbi_encode_utf8(0x41, buf);
    UASSERT_EQ(n, 1);
    UASSERT_EQ(buf[0], 0x41);
}

static void encode_utf8_two_byte(void) {
    /* U+00E9 'é' (Latin small letter e with acute) → 2 bytes 0xC3 0xA9. */
    unsigned char buf[4] = {0};
    int n = urbi_encode_utf8(0xE9, buf);
    UASSERT_EQ(n, 2);
    UASSERT_EQ(buf[0], 0xC3);
    UASSERT_EQ(buf[1], 0xA9);
}

static void encode_utf8_three_byte(void) {
    /* U+20AC '€' (Euro sign) → 3 bytes 0xE2 0x82 0xAC. */
    unsigned char buf[4] = {0};
    int n = urbi_encode_utf8(0x20AC, buf);
    UASSERT_EQ(n, 3);
    UASSERT_EQ(buf[0], 0xE2);
    UASSERT_EQ(buf[1], 0x82);
    UASSERT_EQ(buf[2], 0xAC);
}

static void encode_utf8_four_byte(void) {
    /* U+1F600 (grinning face emoji) → 4 bytes 0xF0 0x9F 0x98 0x80. */
    unsigned char buf[4] = {0};
    int n = urbi_encode_utf8(0x1F600, buf);
    UASSERT_EQ(n, 4);
    UASSERT_EQ(buf[0], 0xF0);
    UASSERT_EQ(buf[1], 0x9F);
    UASSERT_EQ(buf[2], 0x98);
    UASSERT_EQ(buf[3], 0x80);
}

static void encode_utf8_max_codepoint(void) {
    /* U+10FFFF (Unicode max) → 4 bytes 0xF4 0x8F 0xBF 0xBF. */
    unsigned char buf[4] = {0};
    int n = urbi_encode_utf8(0x10FFFF, buf);
    UASSERT_EQ(n, 4);
    UASSERT_EQ(buf[0], 0xF4);
    UASSERT_EQ(buf[1], 0x8F);
    UASSERT_EQ(buf[2], 0xBF);
    UASSERT_EQ(buf[3], 0xBF);
}

void test_lex_unicode_suite(void) {
    utest_run("encode_utf8_ascii", encode_utf8_ascii);
    utest_run("encode_utf8_two_byte", encode_utf8_two_byte);
    utest_run("encode_utf8_three_byte", encode_utf8_three_byte);
    utest_run("encode_utf8_four_byte", encode_utf8_four_byte);
    utest_run("encode_utf8_max_codepoint", encode_utf8_max_codepoint);
}
