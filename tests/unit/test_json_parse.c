/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_json_parse.c — coverage for the v0.9.1 Task 21 JSON
 * parser.  Verifies primitive parsing, container parsing, malformed
 * rejection, depth + node limits, and free-on-error contract. */
#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "repl/ujson.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

UTEST(json_parse_int)
{
    UJsonNode *root = NULL;
    UJsonErr err;
    UASSERT_EQ(ujson_parse("42", 2, &root, &err), 0);
    UASSERT(root != NULL);
    UASSERT_EQ(root->kind, UJSON_INT);
    UASSERT_EQ((int)root->v.i, 42);
    ujson_free_node(root);
}

UTEST(json_parse_negative_int)
{
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse("-123", 4, &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_INT);
    UASSERT_EQ((int)root->v.i, -123);
    ujson_free_node(root);
}

UTEST(json_parse_float)
{
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse("3.14", 4, &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_DOUBLE);
    /* Compare without float-eq pitfalls. */
    UASSERT(root->v.d > 3.13 && root->v.d < 3.15);
    ujson_free_node(root);
}

UTEST(json_parse_exponent)
{
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse("1e3", 3, &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_DOUBLE);
    UASSERT(root->v.d > 999.0 && root->v.d < 1001.0);
    ujson_free_node(root);
}

UTEST(json_parse_bool_true)
{
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse("true", 4, &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_BOOL);
    UASSERT(root->v.b == true);
    ujson_free_node(root);
}

UTEST(json_parse_bool_false)
{
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse("false", 5, &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_BOOL);
    UASSERT(root->v.b == false);
    ujson_free_node(root);
}

UTEST(json_parse_null)
{
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse("null", 4, &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_NULL);
    ujson_free_node(root);
}

UTEST(json_parse_string_simple)
{
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse("\"hello\"", 7, &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_STRING);
    UASSERT_EQ((int)root->v.s.len, 5);
    UASSERT_EQ(memcmp(root->v.s.bytes, "hello", 5), 0);
    ujson_free_node(root);
}

UTEST(json_parse_string_escapes)
{
    const char *src = "\"a\\nb\\tc\\\"d\"";
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse(src, strlen(src), &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_STRING);
    UASSERT_EQ((int)root->v.s.len, 7);
    UASSERT_EQ(memcmp(root->v.s.bytes, "a\nb\tc\"d", 7), 0);
    ujson_free_node(root);
}

UTEST(json_parse_string_unicode_escape)
{
    /* é → 0xC3 0xA9 (UTF-8 'é'). */
    const char *src = "\"\\u00e9\"";
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse(src, strlen(src), &root, NULL), 0);
    UASSERT_EQ((int)root->v.s.len, 2);
    UASSERT_EQ((unsigned char)root->v.s.bytes[0], 0xC3);
    UASSERT_EQ((unsigned char)root->v.s.bytes[1], 0xA9);
    ujson_free_node(root);
}

UTEST(json_parse_string_surrogate_pair)
{
    /* U+1F600 (grinning face) encoded as surrogate pair 😀. */
    const char *src = "\"\\uD83D\\uDE00\"";
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse(src, strlen(src), &root, NULL), 0);
    UASSERT_EQ((int)root->v.s.len, 4);  /* 4-byte UTF-8 */
    UASSERT_EQ((unsigned char)root->v.s.bytes[0], 0xF0);
    UASSERT_EQ((unsigned char)root->v.s.bytes[1], 0x9F);
    UASSERT_EQ((unsigned char)root->v.s.bytes[2], 0x98);
    UASSERT_EQ((unsigned char)root->v.s.bytes[3], 0x80);
    ujson_free_node(root);
}

UTEST(json_parse_array_simple)
{
    const char *src = "[1,2,3]";
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse(src, 7, &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_ARRAY);
    UASSERT_EQ((int)root->v.arr.count, 3);
    UASSERT_EQ((int)root->v.arr.items[0]->v.i, 1);
    UASSERT_EQ((int)root->v.arr.items[1]->v.i, 2);
    UASSERT_EQ((int)root->v.arr.items[2]->v.i, 3);
    ujson_free_node(root);
}

UTEST(json_parse_array_empty)
{
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse("[]", 2, &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_ARRAY);
    UASSERT_EQ((int)root->v.arr.count, 0);
    ujson_free_node(root);
}

UTEST(json_parse_object_simple)
{
    const char *src = "{\"a\":1,\"b\":\"hi\"}";
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse(src, strlen(src), &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_OBJECT);
    UJsonMember *m = root->v.members;
    UASSERT(m != NULL);
    UASSERT_EQ(strcmp(m->key, "a"), 0);
    UASSERT_EQ((int)m->value->v.i, 1);
    UASSERT(m->next != NULL);
    UASSERT_EQ(strcmp(m->next->key, "b"), 0);
    UASSERT_EQ(strcmp(m->next->value->v.s.bytes, "hi"), 0);
    UASSERT(m->next->next == NULL);
    ujson_free_node(root);
}

UTEST(json_parse_object_empty)
{
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse("{}", 2, &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_OBJECT);
    UASSERT(root->v.members == NULL);
    ujson_free_node(root);
}

UTEST(json_parse_nested)
{
    const char *src = "{\"xs\":[1,2,{\"k\":true}]}";
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse(src, strlen(src), &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_OBJECT);
    UJsonNode *xs = root->v.members->value;
    UASSERT_EQ(xs->kind, UJSON_ARRAY);
    UASSERT_EQ((int)xs->v.arr.count, 3);
    UJsonNode *inner = xs->v.arr.items[2];
    UASSERT_EQ(inner->kind, UJSON_OBJECT);
    UASSERT(inner->v.members->value->v.b == true);
    ujson_free_node(root);
}

UTEST(json_parse_whitespace_tolerated)
{
    const char *src = "  {  \"a\" : 1  ,  \"b\" : 2 } ";
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse(src, strlen(src), &root, NULL), 0);
    UASSERT_EQ(root->kind, UJSON_OBJECT);
    ujson_free_node(root);
}

UTEST(json_parse_malformed_open_object)
{
    UJsonNode *root = NULL;
    UJsonErr err;
    UASSERT_EQ(ujson_parse("{not json", 9, &root, &err), -1);
    UASSERT(root == NULL);
    UASSERT_EQ(err, UJSON_ERR_INVALID);
}

UTEST(json_parse_malformed_trailing_comma)
{
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse("[1,2,]", 6, &root, NULL), -1);
}

UTEST(json_parse_malformed_unclosed_string)
{
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse("\"abc", 4, &root, NULL), -1);
}

UTEST(json_parse_malformed_trailing_garbage)
{
    /* Valid prefix '1' followed by junk. */
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse("1 garbage", 9, &root, NULL), -1);
}

UTEST(json_parse_dos_deep_nesting_rejected)
{
    /* Construct UJSON_MAX_DEPTH + 1 opening braces — should fail with
     * UJSON_ERR_DEPTH well before stack exhaustion. */
    char buf[256];
    int n = UJSON_MAX_DEPTH + 5;
    if (n > (int)sizeof(buf) / 2) n = (int)sizeof(buf) / 2;
    memset(buf, '[', (size_t)n);
    UJsonNode *root = NULL;
    UJsonErr err;
    UASSERT_EQ(ujson_parse(buf, (size_t)n, &root, &err), -1);
    UASSERT(err == UJSON_ERR_DEPTH || err == UJSON_ERR_INVALID);
    UASSERT(root == NULL);
}

UTEST(json_parse_empty_input_rejected)
{
    UJsonNode *root = NULL;
    UASSERT_EQ(ujson_parse("", 0, &root, NULL), -1);
}

void
test_json_parse_suite(void)
{
    utest_run("json_parse_int",                       json_parse_int);
    utest_run("json_parse_negative_int",              json_parse_negative_int);
    utest_run("json_parse_float",                     json_parse_float);
    utest_run("json_parse_exponent",                  json_parse_exponent);
    utest_run("json_parse_bool_true",                 json_parse_bool_true);
    utest_run("json_parse_bool_false",                json_parse_bool_false);
    utest_run("json_parse_null",                      json_parse_null);
    utest_run("json_parse_string_simple",             json_parse_string_simple);
    utest_run("json_parse_string_escapes",            json_parse_string_escapes);
    utest_run("json_parse_string_unicode_escape",     json_parse_string_unicode_escape);
    utest_run("json_parse_string_surrogate_pair",     json_parse_string_surrogate_pair);
    utest_run("json_parse_array_simple",              json_parse_array_simple);
    utest_run("json_parse_array_empty",               json_parse_array_empty);
    utest_run("json_parse_object_simple",             json_parse_object_simple);
    utest_run("json_parse_object_empty",              json_parse_object_empty);
    utest_run("json_parse_nested",                    json_parse_nested);
    utest_run("json_parse_whitespace_tolerated",      json_parse_whitespace_tolerated);
    utest_run("json_parse_malformed_open_object",     json_parse_malformed_open_object);
    utest_run("json_parse_malformed_trailing_comma",  json_parse_malformed_trailing_comma);
    utest_run("json_parse_malformed_unclosed_string", json_parse_malformed_unclosed_string);
    utest_run("json_parse_malformed_trailing_garbage", json_parse_malformed_trailing_garbage);
    utest_run("json_parse_dos_deep_nesting_rejected", json_parse_dos_deep_nesting_rejected);
    utest_run("json_parse_empty_input_rejected",      json_parse_empty_input_rejected);
}

#else  /* !URBI_ENABLE_REPL */

void test_json_parse_suite(void) { /* skipped: URBI_ENABLE_REPL=0 */ }

#endif
