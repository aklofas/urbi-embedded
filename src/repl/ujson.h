/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/ujson.h — tiny JSON parser for the v0.9.1 introspect surface.
 *
 * Scope: validate that bytes parse as JSON, surface the top-level kind,
 * and provide a sandboxed walker for key lookup.  Returns a tree of
 * UJsonNode allocated from a single arena (the caller frees the whole
 * tree with ujson_free_node(root)).
 *
 * NOT a general-purpose JSON parser:
 *   - depth bounded to UJSON_MAX_DEPTH (32);
 *   - total node count bounded to UJSON_MAX_NODES (10000);
 *   - input bounded to UJSON_MAX_LEN (1 MiB);
 *   - rejects deeply-nested DoS attempts and integer / depth overflows.
 *
 * Why this lives next to the REPL: it is only used by the Debug urbiscript
 * namespace to validate introspect-JSON round-trips, and is intended for the
 * NDJSON request-parser (not yet wired to it).  Putting it under src/repl/
 * keeps the URBI_ENABLE_REPL=1 build gating natural — non-REPL builds
 * never link this TU. */
#ifndef UJSON_H
#define UJSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UJSON_MAX_DEPTH  32
#define UJSON_MAX_NODES  10000
#define UJSON_MAX_LEN    (1u << 20)

typedef enum {
    UJSON_NULL    = 0,
    UJSON_BOOL    = 1,
    UJSON_INT     = 2,
    UJSON_DOUBLE  = 3,
    UJSON_STRING  = 4,
    UJSON_ARRAY   = 5,
    UJSON_OBJECT  = 6
} UJsonKind;

typedef struct UJsonNode UJsonNode;

/* Members of an object: linked list of key→value pairs. */
typedef struct UJsonMember {
    char               *key;       /* heap-owned NUL-terminated */
    size_t              key_len;
    UJsonNode          *value;
    struct UJsonMember *next;
} UJsonMember;

struct UJsonNode {
    UJsonKind kind;
    union {
        bool    b;
        int64_t i;
        double  d;
        struct { char *bytes; size_t len; } s;     /* STRING */
        struct { UJsonNode **items; size_t count; } arr; /* ARRAY */
        UJsonMember *members;                       /* OBJECT */
    } v;
};

/* Parse src[0..len) into a fresh tree.
 *
 * Returns 0 on success and sets *out_root.  Returns -1 on any failure
 * (malformed input, depth/node/length overflow, OOM).  The error code is
 * surfaced via *out_err — see UJSON_ERR_* below.  On error *out_root is
 * NULL and no caller cleanup is needed.
 *
 * The caller frees the result via ujson_free_node(*out_root). */
typedef enum {
    UJSON_OK             =  0,
    UJSON_ERR_INVALID    = -1,  /* malformed bytes */
    UJSON_ERR_DEPTH      = -2,  /* nesting exceeds UJSON_MAX_DEPTH */
    UJSON_ERR_NODES      = -3,  /* total nodes exceeds UJSON_MAX_NODES */
    UJSON_ERR_LEN        = -4,  /* input length exceeds UJSON_MAX_LEN */
    UJSON_ERR_OOM        = -5
} UJsonErr;

int ujson_parse(const char *src, size_t len,
                UJsonNode **out_root, UJsonErr *out_err);

/* Free a node tree (recursive).  NULL-safe. */
void ujson_free_node(UJsonNode *node);

#ifdef __cplusplus
}
#endif

#endif /* UJSON_H */
