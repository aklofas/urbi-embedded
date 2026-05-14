/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_event_payload_layout.c — Gap C: urbi_event_payload_t layout pin
 *
 * Runtime mirror of the compile-time _Static_asserts in include/urbi/types.h.
 * Covers:
 *   1. sizeof(urbi_event_payload_t) == URBI_EVENT_PAYLOAD_MAX (16)
 *   2. _Alignof(urbi_event_payload_t) == URBI_EVENT_PAYLOAD_ALIGN (8)
 *   3. u32 round-trip readable via bytes[] (LE hosts only)
 *   4. f64 round-trip via bytes[] using memcpy */

#include "utest.h"
#include "urbi/types.h"
#include <string.h>

static void payload_size_is_16(void)
{
    UASSERT_EQ((int)sizeof(urbi_event_payload_t), (int)URBI_EVENT_PAYLOAD_MAX);
    UASSERT_EQ((int)sizeof(urbi_event_payload_t), 16);
}

static void payload_align_is_8(void)
{
    /* Use __alignof__ (GCC/Clang extension) instead of C11 _Alignof to keep
     * -Wpedantic clean under -std=c99 (project convention, see types.h). */
    UASSERT_EQ((int)__alignof__(urbi_event_payload_t), (int)URBI_EVENT_PAYLOAD_ALIGN);
    UASSERT_EQ((int)__alignof__(urbi_event_payload_t), 8);
}

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
static void payload_u32_roundtrip_le(void)
{
    urbi_event_payload_t p;
    memset(&p, 0, sizeof(p));
    p.u32[0] = 0xDEADBEEFU;
    p.u32[1] = 0xCAFEF00DU;

    /* Little-endian byte layout for 0xDEADBEEF: EF BE AD DE */
    UASSERT_EQ((unsigned)p.bytes[0], 0xEFU);
    UASSERT_EQ((unsigned)p.bytes[1], 0xBEU);
    UASSERT_EQ((unsigned)p.bytes[2], 0xADU);
    UASSERT_EQ((unsigned)p.bytes[3], 0xDEU);
    /* Little-endian byte layout for 0xCAFEF00D: 0D F0 FE CA */
    UASSERT_EQ((unsigned)p.bytes[4], 0x0DU);
    UASSERT_EQ((unsigned)p.bytes[5], 0xF0U);
    UASSERT_EQ((unsigned)p.bytes[6], 0xFEU);
    UASSERT_EQ((unsigned)p.bytes[7], 0xCAU);
}
#endif /* LE only */

static void payload_f64_roundtrip(void)
{
    /* Use memcpy for type-punning to avoid UB. */
    urbi_event_payload_t p;
    memset(&p, 0, sizeof(p));

    double val = 3.14159;
    memcpy(&p.bytes[0], &val, sizeof(double));

    double result;
    memcpy(&result, &p.bytes[0], sizeof(double));

    UASSERT(result == val);
}

void
test_event_payload_layout_suite(void)
{
    printf("test_event_payload_layout\n");
    utest_run("payload_size_is_16",   payload_size_is_16);
    utest_run("payload_align_is_8",   payload_align_is_8);
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    utest_run("payload_u32_roundtrip_le", payload_u32_roundtrip_le);
#endif
    utest_run("payload_f64_roundtrip", payload_f64_roundtrip);
}
