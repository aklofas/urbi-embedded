/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include "value/uvarint.h"

#include <stdint.h>
#include <stddef.h>

#define UTEST(name) static void name(void)

/* --- Decode (moved from test_module.c) --- */

UTEST(decode_u_single_byte) {
    const uint8_t buf[] = {0x00};
    uint64_t v = 0;
    size_t consumed = 0;
    UASSERT_EQ(UVARINT_OK, uvarint_decode_u(buf, sizeof buf, &v, &consumed));
    UASSERT_EQ((uint64_t)0, v);
    UASSERT_EQ((size_t)1, consumed);

    const uint8_t buf2[] = {0x7F};          /* 127 — max single-byte */
    UASSERT_EQ(UVARINT_OK, uvarint_decode_u(buf2, sizeof buf2, &v, &consumed));
    UASSERT_EQ((uint64_t)127, v);
    UASSERT_EQ((size_t)1, consumed);
}

UTEST(decode_u_multi_byte) {
    /* 128 = 0x80 0x01  — low 7 bits (0) with continuation, then high bits (1) */
    const uint8_t buf[] = {0x80, 0x01};
    uint64_t v = 0;
    size_t consumed = 0;
    UASSERT_EQ(UVARINT_OK, uvarint_decode_u(buf, sizeof buf, &v, &consumed));
    UASSERT_EQ((uint64_t)128, v);
    UASSERT_EQ((size_t)2, consumed);

    /* 300 = 0xAC 0x02 */
    const uint8_t buf2[] = {0xAC, 0x02};
    UASSERT_EQ(UVARINT_OK, uvarint_decode_u(buf2, sizeof buf2, &v, &consumed));
    UASSERT_EQ((uint64_t)300, v);
    UASSERT_EQ((size_t)2, consumed);
}

UTEST(decode_u_truncation) {
    const uint8_t buf[] = {0x80};           /* continuation set, no next byte */
    uint64_t v = 0;
    size_t consumed = 0;
    UASSERT_EQ(UVARINT_TRUNCATED, uvarint_decode_u(buf, sizeof buf, &v, &consumed));
}

UTEST(decode_u_oversize) {
    /* 11 continuation bytes — exceeds 10-byte max for uint64 */
    const uint8_t buf[] = {0x80, 0x80, 0x80, 0x80, 0x80,
                           0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
    uint64_t v = 0;
    size_t consumed = 0;
    UASSERT_EQ(UVARINT_OVERSIZE, uvarint_decode_u(buf, sizeof buf, &v, &consumed));
}

UTEST(decode_u_oversize_terminal_byte_overflow) {
    /* 10 bytes total — within the byte budget — but the terminal byte
       has a payload value > 0x01 at shift==63. Only bit 0 fits there;
       0x02..0x7F would overflow uint64_t silently without the explicit
       guard. Must be rejected as oversize. */
    const uint8_t buf[] = {0x80, 0x80, 0x80, 0x80, 0x80,
                           0x80, 0x80, 0x80, 0x80, 0x02};
    uint64_t v = 0;
    size_t consumed = 0;
    UASSERT_EQ(UVARINT_OVERSIZE, uvarint_decode_u(buf, sizeof buf, &v, &consumed));
}

UTEST(decode_u_max_uint64) {
    /* Boundary case that must SUCCEED: the canonical 10-byte encoding
       of UINT64_MAX (bit 63 set at shift==63). Pairs with the
       oversize-terminal-byte test above. */
    const uint8_t buf[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                           0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    uint64_t v = 0;
    size_t consumed = 0;
    UASSERT_EQ(UVARINT_OK, uvarint_decode_u(buf, sizeof buf, &v, &consumed));
    UASSERT_EQ((uint64_t)UINT64_MAX, v);
    UASSERT_EQ((size_t)10, consumed);
}

UTEST(decode_zz_round_trip) {
    /* zigzag: 0->0, -1->1, 1->2, -2->3, 2->4, ... */
    struct { int64_t decoded; uint8_t enc[10]; size_t enc_len; } cases[] = {
        { 0,   {0x00},       1 },
        { -1,  {0x01},       1 },
        { 1,   {0x02},       1 },
        { -2,  {0x03},       1 },
        { 2,   {0x04},       1 },
        { 63,  {0x7E},       1 },   /* 63*2 = 126 */
        { -64, {0x7F},       1 },   /* zigzag(-64) = 127 */
        { 64,  {0x80, 0x01}, 2 },   /* 64*2 = 128 */
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int64_t v = 0;
        size_t consumed = 0;
        UVarintError rc = uvarint_decode_zz(cases[i].enc, cases[i].enc_len, &v, &consumed);
        UASSERT_EQ(UVARINT_OK, rc);
        UASSERT_EQ(cases[i].decoded, v);
        UASSERT_EQ(cases[i].enc_len, consumed);
    }
}

/* --- Encode (new direct coverage) --- */

UTEST(size_u_boundaries) {
    UASSERT_EQ((size_t)1,  uvarint_size_u(0));
    UASSERT_EQ((size_t)1,  uvarint_size_u(0x7F));        /* 127 */
    UASSERT_EQ((size_t)2,  uvarint_size_u(0x80));        /* 128 */
    UASSERT_EQ((size_t)2,  uvarint_size_u(0x3FFF));      /* 16383 */
    UASSERT_EQ((size_t)3,  uvarint_size_u(0x4000));      /* 16384 */
    UASSERT_EQ((size_t)10, uvarint_size_u(UINT64_MAX));
}

UTEST(size_zz_boundaries) {
    /* zigzag(0) = 0 → 1 byte; zigzag(-1) = 1 → 1 byte;
       zigzag(INT64_MIN) = UINT64_MAX → 10 bytes;
       zigzag(INT64_MAX) = UINT64_MAX - 1 → 10 bytes. */
    UASSERT_EQ((size_t)1,  uvarint_size_zz(0));
    UASSERT_EQ((size_t)1,  uvarint_size_zz(-1));
    UASSERT_EQ((size_t)10, uvarint_size_zz(INT64_MIN));
    UASSERT_EQ((size_t)10, uvarint_size_zz(INT64_MAX));
}

UTEST(write_u_round_trip) {
    const uint64_t values[] = { 0, 1, 127, 128, 16383, 16384, 0xDEADBEEFULL, UINT64_MAX };
    size_t i;
    for (i = 0; i < sizeof values / sizeof values[0]; i++) {
        uint8_t buf[10] = {0};
        const size_t expected_len = uvarint_size_u(values[i]);
        const size_t end = uvarint_write_u(buf, 0, values[i]);
        UASSERT_EQ(expected_len, end);

        uint64_t got = 0;
        size_t consumed = 0;
        UASSERT_EQ(UVARINT_OK, uvarint_decode_u(buf, end, &got, &consumed));
        UASSERT_EQ(values[i], got);
        UASSERT_EQ(end, consumed);
    }
}

UTEST(write_zz_round_trip) {
    const int64_t values[] = { 0, 1, -1, 63, -64, 64, -65, 8191, -8192, 0x12345678 };
    size_t i;
    for (i = 0; i < sizeof values / sizeof values[0]; i++) {
        uint8_t buf[10] = {0};
        const size_t expected_len = uvarint_size_zz(values[i]);
        const size_t end = uvarint_write_zz(buf, 0, values[i]);
        UASSERT_EQ(expected_len, end);

        int64_t got = 0;
        size_t consumed = 0;
        UASSERT_EQ(UVARINT_OK, uvarint_decode_zz(buf, end, &got, &consumed));
        UASSERT_EQ(values[i], got);
        UASSERT_EQ(end, consumed);
    }
}

UTEST(write_zz_extremes) {
    /* INT64_MIN and INT64_MAX round-trip through 10-byte encodings. */
    const int64_t values[] = { INT64_MIN, INT64_MAX };
    size_t i;
    for (i = 0; i < sizeof values / sizeof values[0]; i++) {
        uint8_t buf[10] = {0};
        const size_t end = uvarint_write_zz(buf, 0, values[i]);
        UASSERT_EQ((size_t)10, end);

        int64_t got = 0;
        size_t consumed = 0;
        UASSERT_EQ(UVARINT_OK, uvarint_decode_zz(buf, end, &got, &consumed));
        UASSERT_EQ(values[i], got);
        UASSERT_EQ((size_t)10, consumed);
    }
}

UTEST(write_u_honors_offset) {
    /* write_u should append at off, not overwrite earlier bytes. */
    uint8_t buf[6] = { 0xAA, 0xBB, 0xCC, 0, 0, 0 };
    const size_t end = uvarint_write_u(buf, 3U, 300U);   /* 300 = 0xAC 0x02 */
    UASSERT_EQ((size_t)5, end);
    UASSERT_EQ((uint8_t)0xAA, buf[0]);
    UASSERT_EQ((uint8_t)0xBB, buf[1]);
    UASSERT_EQ((uint8_t)0xCC, buf[2]);
    UASSERT_EQ((uint8_t)0xAC, buf[3]);
    UASSERT_EQ((uint8_t)0x02, buf[4]);
}

void test_varint_suite(void);

void test_varint_suite(void) {
    utest_run("varint decode u single byte",     decode_u_single_byte);
    utest_run("varint decode u multi byte",      decode_u_multi_byte);
    utest_run("varint decode u truncation",      decode_u_truncation);
    utest_run("varint decode u oversize",        decode_u_oversize);
    utest_run("varint decode u oversize terminal-byte overflow",
                                                 decode_u_oversize_terminal_byte_overflow);
    utest_run("varint decode u max uint64",      decode_u_max_uint64);
    utest_run("varint decode zz round trip",     decode_zz_round_trip);
    utest_run("varint size u boundaries",        size_u_boundaries);
    utest_run("varint size zz boundaries",       size_zz_boundaries);
    utest_run("varint write u round trip",       write_u_round_trip);
    utest_run("varint write zz round trip",      write_zz_round_trip);
    utest_run("varint write zz extremes",        write_zz_extremes);
    utest_run("varint write u honors offset",    write_u_honors_offset);
}
