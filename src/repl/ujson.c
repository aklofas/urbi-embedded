/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/ujson.c — tiny recursive-descent JSON parser (v0.9.1 Task 21).
 *
 * Strictly RFC 8259 with the following limits:
 *   - max nesting depth UJSON_MAX_DEPTH (32) — guards against DoS
 *   - max total nodes UJSON_MAX_NODES (10000)
 *   - max input length UJSON_MAX_LEN (1 MiB)
 *
 * No comments accepted; no trailing commas; UTF-16 surrogate pairs in
 * \uXXXX escapes are decoded to UTF-8.
 *
 * Each parser entry takes a Parser state and a depth counter; deep recursion
 * is gated by the depth counter at object/array entry.  Total node count
 * is also tracked to bound array-of-arrays style attacks.
 *
 * Allocator: malloc/free (this TU is hosted-only; REPL itself depends on
 * pthread/socket APIs that already pull libc in). */

#include "repl/ujson.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *end;
    size_t      depth;
    size_t      nodes;
    UJsonErr    err;
} Parser;

/* --- forward decls --- */
static UJsonNode *parse_value(Parser *st);

/* --- helpers --- */

static void
skip_ws(Parser *st)
{
    while (st->p < st->end) {
        char c = *st->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') st->p++;
        else break;
    }
}

static UJsonNode *
new_node(Parser *st, UJsonKind k)
{
    if (st->nodes >= UJSON_MAX_NODES) {
        st->err = UJSON_ERR_NODES;
        return NULL;
    }
    UJsonNode *n = (UJsonNode *)calloc(1, sizeof(*n));
    if (n == NULL) {
        st->err = UJSON_ERR_OOM;
        return NULL;
    }
    n->kind = k;
    st->nodes++;
    return n;
}

/* Append a UTF-8-encoded codepoint to dst.  Returns the byte count written
 * (1-4) or 0 if cp is invalid. */
static size_t
utf8_emit(uint32_t cp, char *dst)
{
    if (cp <= 0x7Fu) {
        dst[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FFu) {
        dst[0] = (char)(0xC0u | (cp >> 6));
        dst[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    } else if (cp <= 0xFFFFu) {
        dst[0] = (char)(0xE0u |  (cp >> 12));
        dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    } else if (cp <= 0x10FFFFu) {
        dst[0] = (char)(0xF0u |  (cp >> 18));
        dst[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        dst[2] = (char)(0x80u | ((cp >> 6)  & 0x3Fu));
        dst[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

static int
hex_digit(char c, uint32_t *out)
{
    if (c >= '0' && c <= '9') { *out = (uint32_t)(c - '0');       return 0; }
    if (c >= 'a' && c <= 'f') { *out = (uint32_t)(c - 'a') + 10u; return 0; }
    if (c >= 'A' && c <= 'F') { *out = (uint32_t)(c - 'A') + 10u; return 0; }
    return -1;
}

/* Read four hex digits at st->p..st->p+4 into *out.  Does not advance the
 * parser position. */
static int
read_4hex(Parser *st, uint32_t *out)
{
    if (st->end - st->p < 4) return -1;
    uint32_t v = 0, d;
    for (int i = 0; i < 4; i++) {
        if (hex_digit(st->p[i], &d) != 0) return -1;
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

/* Parse a JSON string.  On entry, *st->p == '"'.  On success advances past
 * the closing quote; returns malloc'd buffer + length.  Caller frees. */
static char *
parse_string(Parser *st, size_t *out_len)
{
    if (st->p >= st->end || *st->p != '"') {
        st->err = UJSON_ERR_INVALID;
        return NULL;
    }
    st->p++;
    /* Worst case: every byte is a one-byte char OR \uXXXX which decodes
     * to up to 3 bytes UTF-8 (4 if surrogate pair, which consumes 12 src
     * bytes → 4 dst bytes — still smaller).  Allocating end - p is a
     * conservative upper bound. */
    size_t cap = (size_t)(st->end - st->p) + 1;
    char *buf = (char *)malloc(cap);
    if (buf == NULL) { st->err = UJSON_ERR_OOM; return NULL; }
    size_t n = 0;
    while (st->p < st->end) {
        char c = *st->p;
        if (c == '"') {
            st->p++;
            buf[n] = '\0';
            *out_len = n;
            return buf;
        }
        if (c == '\\') {
            if (st->p + 1 >= st->end) goto bad;
            char esc = st->p[1];
            st->p += 2;
            switch (esc) {
            case '"': buf[n++] = '"'; break;
            case '\\': buf[n++] = '\\'; break;
            case '/':  buf[n++] = '/';  break;
            case 'b':  buf[n++] = '\b'; break;
            case 'f':  buf[n++] = '\f'; break;
            case 'n':  buf[n++] = '\n'; break;
            case 'r':  buf[n++] = '\r'; break;
            case 't':  buf[n++] = '\t'; break;
            case 'u': {
                uint32_t cp;
                if (read_4hex(st, &cp) != 0) goto bad;
                st->p += 4;
                /* Surrogate pair handling: 0xD800..0xDBFF (high) + low. */
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    if (st->end - st->p < 6) goto bad;
                    if (st->p[0] != '\\' || st->p[1] != 'u') goto bad;
                    st->p += 2;
                    uint32_t lo;
                    if (read_4hex(st, &lo) != 0) goto bad;
                    st->p += 4;
                    if (lo < 0xDC00u || lo > 0xDFFFu) goto bad;
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                    goto bad;  /* unpaired low surrogate */
                }
                size_t w = utf8_emit(cp, buf + n);
                if (w == 0) goto bad;
                n += w;
                break;
            }
            default: goto bad;
            }
            if (n >= cap - 4) {
                /* Should not happen given cap calc above, but defensive. */
                size_t new_cap = cap * 2;
                char *nb = (char *)realloc(buf, new_cap);
                if (nb == NULL) { st->err = UJSON_ERR_OOM; free(buf); return NULL; }
                buf = nb;
                cap = new_cap;
            }
        } else if ((unsigned char)c < 0x20) {
            goto bad;  /* unescaped control char */
        } else {
            buf[n++] = c;
            st->p++;
        }
    }
bad:
    free(buf);
    st->err = UJSON_ERR_INVALID;
    return NULL;
}

/* Parse a JSON number into either INT (no '.', 'e', or 'E') or DOUBLE.
 * On entry st->p points at the leading digit or '-'. */
static UJsonNode *
parse_number(Parser *st)
{
    const char *start = st->p;
    if (*st->p == '-') st->p++;
    if (st->p >= st->end || !isdigit((unsigned char)*st->p)) {
        st->err = UJSON_ERR_INVALID;
        return NULL;
    }
    /* Leading zero must be followed by '.' / 'e' / end. */
    if (*st->p == '0') {
        st->p++;
    } else {
        while (st->p < st->end && isdigit((unsigned char)*st->p)) st->p++;
    }
    bool is_float = false;
    if (st->p < st->end && *st->p == '.') {
        is_float = true;
        st->p++;
        if (st->p >= st->end || !isdigit((unsigned char)*st->p)) {
            st->err = UJSON_ERR_INVALID;
            return NULL;
        }
        while (st->p < st->end && isdigit((unsigned char)*st->p)) st->p++;
    }
    if (st->p < st->end && (*st->p == 'e' || *st->p == 'E')) {
        is_float = true;
        st->p++;
        if (st->p < st->end && (*st->p == '+' || *st->p == '-')) st->p++;
        if (st->p >= st->end || !isdigit((unsigned char)*st->p)) {
            st->err = UJSON_ERR_INVALID;
            return NULL;
        }
        while (st->p < st->end && isdigit((unsigned char)*st->p)) st->p++;
    }
    size_t len = (size_t)(st->p - start);
    /* Copy to a small NUL-terminated buffer for strtoll / strtod. */
    char tmp[64];
    if (len >= sizeof(tmp)) {
        /* Number too long; surface as DOUBLE via strtod or invalid. */
        st->err = UJSON_ERR_INVALID;
        return NULL;
    }
    memcpy(tmp, start, len);
    tmp[len] = '\0';
    UJsonNode *n;
    if (is_float) {
        n = new_node(st, UJSON_DOUBLE);
        if (n == NULL) return NULL;
        n->v.d = strtod(tmp, NULL);
    } else {
        n = new_node(st, UJSON_INT);
        if (n == NULL) return NULL;
        n->v.i = (int64_t)strtoll(tmp, NULL, 10);
    }
    return n;
}

static int
match_literal(Parser *st, const char *lit, size_t lit_len)
{
    if ((size_t)(st->end - st->p) < lit_len) return -1;
    if (memcmp(st->p, lit, lit_len) != 0) return -1;
    st->p += lit_len;
    return 0;
}

static UJsonNode *
parse_array(Parser *st)
{
    if (st->depth >= UJSON_MAX_DEPTH) { st->err = UJSON_ERR_DEPTH; return NULL; }
    st->depth++;
    st->p++;  /* consume '[' */

    UJsonNode *node = new_node(st, UJSON_ARRAY);
    if (node == NULL) { st->depth--; return NULL; }
    /* Grow items[] geometrically. */
    size_t cap = 0;
    UJsonNode **items = NULL;
    size_t count = 0;

    skip_ws(st);
    if (st->p < st->end && *st->p == ']') {
        st->p++;
        st->depth--;
        node->v.arr.items = items;
        node->v.arr.count = count;
        return node;
    }
    for (;;) {
        skip_ws(st);
        UJsonNode *item = parse_value(st);
        if (item == NULL) goto bad;
        if (count == cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            UJsonNode **ni = (UJsonNode **)realloc(items, new_cap * sizeof(*ni));
            if (ni == NULL) { st->err = UJSON_ERR_OOM; ujson_free_node(item); goto bad; }
            items = ni;
            cap = new_cap;
        }
        items[count++] = item;
        skip_ws(st);
        if (st->p >= st->end) goto bad;
        if (*st->p == ',') { st->p++; continue; }
        if (*st->p == ']') { st->p++; break; }
        goto bad;
    }
    st->depth--;
    node->v.arr.items = items;
    node->v.arr.count = count;
    return node;
bad:
    {
        /* Free anything we've built so far. */
        for (size_t i = 0; i < count; i++) ujson_free_node(items[i]);
        free(items);
        node->v.arr.items = NULL;
        node->v.arr.count = 0;
        ujson_free_node(node);
        if (st->err == UJSON_OK) st->err = UJSON_ERR_INVALID;
        st->depth--;
        return NULL;
    }
}

static UJsonNode *
parse_object(Parser *st)
{
    if (st->depth >= UJSON_MAX_DEPTH) { st->err = UJSON_ERR_DEPTH; return NULL; }
    st->depth++;
    st->p++;  /* consume '{' */

    UJsonNode *node = new_node(st, UJSON_OBJECT);
    if (node == NULL) { st->depth--; return NULL; }
    UJsonMember *head = NULL;
    UJsonMember *tail = NULL;

    skip_ws(st);
    if (st->p < st->end && *st->p == '}') {
        st->p++;
        st->depth--;
        node->v.members = NULL;
        return node;
    }
    for (;;) {
        skip_ws(st);
        if (st->p >= st->end || *st->p != '"') goto bad;
        size_t key_len = 0;
        char *key = parse_string(st, &key_len);
        if (key == NULL) goto bad;
        skip_ws(st);
        if (st->p >= st->end || *st->p != ':') { free(key); goto bad; }
        st->p++;
        skip_ws(st);
        UJsonNode *val = parse_value(st);
        if (val == NULL) { free(key); goto bad; }
        UJsonMember *m = (UJsonMember *)calloc(1, sizeof(*m));
        if (m == NULL) { st->err = UJSON_ERR_OOM; free(key); ujson_free_node(val); goto bad; }
        m->key = key;
        m->key_len = key_len;
        m->value = val;
        if (head == NULL) head = m; else tail->next = m;
        tail = m;
        skip_ws(st);
        if (st->p >= st->end) goto bad;
        if (*st->p == ',') { st->p++; continue; }
        if (*st->p == '}') { st->p++; break; }
        goto bad;
    }
    st->depth--;
    node->v.members = head;
    return node;
bad:
    {
        while (head != NULL) {
            UJsonMember *next = head->next;
            free(head->key);
            ujson_free_node(head->value);
            free(head);
            head = next;
        }
        node->v.members = NULL;
        ujson_free_node(node);
        if (st->err == UJSON_OK) st->err = UJSON_ERR_INVALID;
        st->depth--;
        return NULL;
    }
}

static UJsonNode *
parse_value(Parser *st)
{
    skip_ws(st);
    if (st->p >= st->end) {
        st->err = UJSON_ERR_INVALID;
        return NULL;
    }
    char c = *st->p;
    switch (c) {
    case '{': return parse_object(st);
    case '[': return parse_array(st);
    case '"': {
        UJsonNode *n = new_node(st, UJSON_STRING);
        if (n == NULL) return NULL;
        n->v.s.bytes = parse_string(st, &n->v.s.len);
        if (n->v.s.bytes == NULL) { ujson_free_node(n); return NULL; }
        return n;
    }
    case 't': {
        if (match_literal(st, "true", 4) != 0) { st->err = UJSON_ERR_INVALID; return NULL; }
        UJsonNode *n = new_node(st, UJSON_BOOL);
        if (n == NULL) return NULL;
        n->v.b = true;
        return n;
    }
    case 'f': {
        if (match_literal(st, "false", 5) != 0) { st->err = UJSON_ERR_INVALID; return NULL; }
        UJsonNode *n = new_node(st, UJSON_BOOL);
        if (n == NULL) return NULL;
        n->v.b = false;
        return n;
    }
    case 'n': {
        if (match_literal(st, "null", 4) != 0) { st->err = UJSON_ERR_INVALID; return NULL; }
        UJsonNode *n = new_node(st, UJSON_NULL);
        return n;
    }
    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number(st);
        }
        st->err = UJSON_ERR_INVALID;
        return NULL;
    }
}

int
ujson_parse(const char *src, size_t len,
            UJsonNode **out_root, UJsonErr *out_err)
{
    UJsonErr local_err = UJSON_OK;
    if (out_err == NULL) out_err = &local_err;
    *out_err = UJSON_OK;
    if (out_root != NULL) *out_root = NULL;
    if (src == NULL || out_root == NULL) {
        *out_err = UJSON_ERR_INVALID;
        return -1;
    }
    if (len > UJSON_MAX_LEN) {
        *out_err = UJSON_ERR_LEN;
        return -1;
    }
    Parser st;
    st.p = src;
    st.end = src + len;
    st.depth = 0;
    st.nodes = 0;
    st.err = UJSON_OK;

    UJsonNode *root = parse_value(&st);
    if (root == NULL) {
        *out_err = (st.err != UJSON_OK) ? st.err : UJSON_ERR_INVALID;
        return -1;
    }
    skip_ws(&st);
    if (st.p != st.end) {
        ujson_free_node(root);
        *out_err = UJSON_ERR_INVALID;
        return -1;
    }
    *out_root = root;
    return 0;
}

void
ujson_free_node(UJsonNode *node)
{
    if (node == NULL) return;
    switch (node->kind) {
    case UJSON_STRING:
        free(node->v.s.bytes);
        break;
    case UJSON_ARRAY: {
        for (size_t i = 0; i < node->v.arr.count; i++) {
            ujson_free_node(node->v.arr.items[i]);
        }
        free(node->v.arr.items);
        break;
    }
    case UJSON_OBJECT: {
        UJsonMember *m = node->v.members;
        while (m != NULL) {
            UJsonMember *next = m->next;
            free(m->key);
            ujson_free_node(m->value);
            free(m);
            m = next;
        }
        break;
    }
    default: break;
    }
    free(node);
}
