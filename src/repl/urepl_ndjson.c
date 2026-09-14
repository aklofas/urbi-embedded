/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_ndjson.c — see repl/urepl_ndjson.h.
 *
 * A schema scanner for the request side and a hand-rolled emitter for the
 * response side.  Neither is a JSON library: the request grammar is four
 * keys wide, and every response this service produces is built here, so
 * there is nothing to generalise over. */
#include "repl/urepl_ndjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Scanner ---------------------------------------------------------- */

typedef struct {
    const char *p;
    const char *end;
} Scan;

static void
skip_ws(Scan *s)
{
    while (s->p < s->end) {
        char c = *s->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s->p++;
        } else {
            break;
        }
    }
}

static int
expect(Scan *s, char c)
{
    skip_ws(s);
    if (s->p >= s->end || *s->p != c) {
        return -1;
    }
    s->p++;
    return 0;
}

/* Parse a JSON string into a newly malloc'd, NUL-terminated buffer.
 * On success, *out_buf is heap-owned, *out_len is byte count excluding
 * the NUL.  Returns 0 on success, negative on parse failure.  Returns
 * -2 specifically when the decoded length would exceed max_len. */
static int
parse_string(Scan *s, char **out_buf, size_t *out_len, size_t max_len)
{
    *out_buf = NULL;
    *out_len = 0;
    skip_ws(s);
    if (s->p >= s->end || *s->p != '"') {
        return -1;
    }
    s->p++;
    /* First pass: locate end + decoded length. */
    const char *start = s->p;
    const char *scan = s->p;
    size_t decoded_len = 0;
    while (scan < s->end) {
        unsigned char c = (unsigned char)*scan;
        if (c == '"') {
            break;
        }
        if (c == '\\') {
            scan++;
            if (scan >= s->end) {
                return -1;
            }
            char e = *scan;
            if (e == 'u') {
                /* \uXXXX: 4 hex digits required.  We will encode the
                 * codepoint as UTF-8 (1-3 bytes for BMP; surrogate pair
                 * not supported in v0.9.1).  Estimate 3 bytes max. */
                if (scan + 4 >= s->end) {
                    return -1;
                }
                for (int i = 1; i <= 4; i++) {
                    char h = scan[i];
                    bool ok = (h >= '0' && h <= '9')
                              || (h >= 'a' && h <= 'f')
                              || (h >= 'A' && h <= 'F');
                    if (!ok) {
                        return -1;
                    }
                }
                decoded_len += 3;
                scan += 5;  /* 'u' + 4 hex */
                continue;
            }
            if (e != '"' && e != '\\' && e != '/' && e != 'b' && e != 'f'
                && e != 'n' && e != 'r' && e != 't') {
                return -1;
            }
            decoded_len++;
            scan++;
            continue;
        }
        if (c < 0x20) {
            return -1;  /* control chars must be escaped */
        }
        decoded_len++;
        scan++;
    }
    if (scan >= s->end) {
        return -1;  /* unterminated */
    }
    if (decoded_len > max_len) {
        return -2;
    }
    /* Allocate + second pass decode. */
    char *out = (char *)malloc(decoded_len + 1);
    if (out == NULL) {
        return -1;
    }
    const char *q = start;
    size_t j = 0;
    while (q < scan) {
        unsigned char c = (unsigned char)*q;
        if (c == '\\') {
            q++;
            char e = *q;
            switch (e) {
            case '"':  out[j++] = '"';  q++; break;
            case '\\': out[j++] = '\\'; q++; break;
            case '/':  out[j++] = '/';  q++; break;
            case 'b':  out[j++] = '\b'; q++; break;
            case 'f':  out[j++] = '\f'; q++; break;
            case 'n':  out[j++] = '\n'; q++; break;
            case 'r':  out[j++] = '\r'; q++; break;
            case 't':  out[j++] = '\t'; q++; break;
            case 'u': {
                unsigned int cp = 0;
                for (int i = 1; i <= 4; i++) {
                    char h = q[i];
                    unsigned int v = 0;
                    if (h >= '0' && h <= '9') {
                        v = (unsigned int)(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        v = (unsigned int)(h - 'a' + 10);
                    } else {
                        v = (unsigned int)(h - 'A' + 10);
                    }
                    cp = (cp << 4) | v;
                }
                q += 5;
                if (cp < 0x80U) {
                    out[j++] = (char)cp;
                } else if (cp < 0x800U) {
                    out[j++] = (char)(0xC0U | (cp >> 6));
                    out[j++] = (char)(0x80U | (cp & 0x3FU));
                } else {
                    /* Surrogate pairs (D800-DFFF) are not validated/
                     * combined in v0.9.1 — emit replacement char. */
                    if (cp >= 0xD800U && cp <= 0xDFFFU) {
                        out[j++] = (char)0xEF;
                        out[j++] = (char)0xBF;
                        out[j++] = (char)0xBD;
                    } else {
                        out[j++] = (char)(0xE0U | (cp >> 12));
                        out[j++] = (char)(0x80U | ((cp >> 6) & 0x3FU));
                        out[j++] = (char)(0x80U | (cp & 0x3FU));
                    }
                }
                break;
            }
            default:
                free(out);
                return -1;
            }
        } else {
            out[j++] = (char)c;
            q++;
        }
    }
    out[j] = '\0';
    *out_buf = out;
    *out_len = j;
    s->p = scan + 1;  /* consume closing quote */
    return 0;
}

static int
parse_uint64(Scan *s, uint64_t *out)
{
    skip_ws(s);
    const char *q = s->p;
    if (q >= s->end) {
        return -1;
    }
    /* Optional leading '+' is not valid in JSON; '-' is for negatives
     * which we reject for id / line / coro_id. */
    if (*q < '0' || *q > '9') {
        return -1;
    }
    uint64_t v = 0;
    while (q < s->end && *q >= '0' && *q <= '9') {
        uint64_t digit = (uint64_t)(*q - '0');
        if (v > (UINT64_MAX - digit) / 10U) {
            return -1;  /* overflow */
        }
        v = v * 10U + digit;
        q++;
    }
    s->p = q;
    *out = v;
    return 0;
}

/* Skip a single JSON value at s->p.  Used for unknown keys. */
static int
skip_value(Scan *s)
{
    skip_ws(s);
    if (s->p >= s->end) {
        return -1;
    }
    char c = *s->p;
    if (c == '"') {
        s->p++;
        while (s->p < s->end && *s->p != '"') {
            if (*s->p == '\\') {
                s->p++;
                if (s->p >= s->end) {
                    return -1;
                }
            }
            s->p++;
        }
        if (s->p >= s->end) {
            return -1;
        }
        s->p++;
        return 0;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        if (c == '-') {
            s->p++;
        }
        while (s->p < s->end) {
            char d = *s->p;
            if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || d == 'E'
                || d == '+' || d == '-') {
                s->p++;
            } else {
                break;
            }
        }
        return 0;
    }
    if (c == 't' || c == 'f' || c == 'n') {
        /* true / false / null */
        const char *exp = (c == 't') ? "true" : (c == 'f') ? "false" : "null";
        size_t n = strlen(exp);
        if ((size_t)(s->end - s->p) < n || memcmp(s->p, exp, n) != 0) {
            return -1;
        }
        s->p += n;
        return 0;
    }
    if (c == '[' || c == '{') {
        char open = c;
        char close = (c == '[') ? ']' : '}';
        int depth = 1;
        s->p++;
        while (s->p < s->end && depth > 0) {
            char d = *s->p;
            if (d == '"') {
                /* Skip string contents */
                s->p++;
                while (s->p < s->end && *s->p != '"') {
                    if (*s->p == '\\') {
                        s->p++;
                        if (s->p >= s->end) {
                            return -1;
                        }
                    }
                    s->p++;
                }
                if (s->p >= s->end) {
                    return -1;
                }
                s->p++;
            } else if (d == open) {
                depth++;
                s->p++;
            } else if (d == close) {
                depth--;
                s->p++;
            } else {
                s->p++;
            }
        }
        return depth == 0 ? 0 : -1;
    }
    return -1;
}
/* ---- Op-name dispatch ------------------------------------------------- */

static UReplOp
op_from_name(const char *s, size_t n)
{
    if (n == 4  && memcmp(s, "eval", 4) == 0)        return UREPL_OP_EVAL;
    if (n == 10 && memcmp(s, "introspect", 10) == 0) return UREPL_OP_INTROSPECT;
    return UREPL_OP_NONE;
}

void
urepl_ndjson_free_req(UReplNdjsonReq *req)
{
    if (req == NULL) return;
    free(req->code);
    free(req->what);
    memset(req, 0, sizeof(*req));
}

int
urepl_ndjson_parse(const char *line, size_t len, UReplNdjsonReq *out)
{
    if (out == NULL || line == NULL) return -1;
    memset(out, 0, sizeof(*out));
    if (len > UREPL_MAX_LINE) return -1;

    Scan s = { line, line + len };
    if (expect(&s, '{') != 0) return -1;
    skip_ws(&s);
    if (s.p < s.end && *s.p == '}') return -1;   /* {} is not a request */

    bool first = true;
    while (s.p < s.end) {
        skip_ws(&s);
        if (s.p >= s.end) { urepl_ndjson_free_req(out); return -1; }
        if (*s.p == '}') {
            s.p++;
            /* An object that named no op parses but means nothing. */
            if (out->op == UREPL_OP_NONE) { urepl_ndjson_free_req(out); return -1; }
            return 0;
        }
        if (!first) {
            if (expect(&s, ',') != 0) { urepl_ndjson_free_req(out); return -1; }
            skip_ws(&s);
        }
        first = false;

        char *key = NULL;
        size_t key_len = 0;
        if (parse_string(&s, &key, &key_len, 64) != 0 || key == NULL) {
            urepl_ndjson_free_req(out);
            return -1;
        }
        if (expect(&s, ':') != 0) { free(key); urepl_ndjson_free_req(out); return -1; }
        skip_ws(&s);

        if (strcmp(key, "id") == 0) {
            free(key);
            if (parse_uint64(&s, &out->id) != 0) { urepl_ndjson_free_req(out); return -1; }
        } else if (strcmp(key, "op") == 0) {
            free(key);
            char *opname = NULL;
            size_t opn = 0;
            if (parse_string(&s, &opname, &opn, 32) != 0) { urepl_ndjson_free_req(out); return -1; }
            UReplOp op = op_from_name(opname, opn);
            free(opname);
            if (op == UREPL_OP_NONE) { urepl_ndjson_free_req(out); return -1; }
            out->op = op;
        } else if (strcmp(key, "code") == 0) {
            free(key);
            if (out->code != NULL) { urepl_ndjson_free_req(out); return -1; }
            if (parse_string(&s, &out->code, &out->code_len, UREPL_MAX_CODE) != 0) {
                urepl_ndjson_free_req(out);
                return -1;
            }
        } else if (strcmp(key, "what") == 0) {
            free(key);
            if (out->what != NULL) { urepl_ndjson_free_req(out); return -1; }
            size_t n = 0;
            if (parse_string(&s, &out->what, &n, UREPL_MAX_WHAT) != 0) {
                urepl_ndjson_free_req(out);
                return -1;
            }
        } else {
            /* An unknown key is skipped rather than refused, so a newer
             * client talking to an older service still gets its request
             * run instead of a parse error. */
            free(key);
            if (skip_value(&s) != 0) { urepl_ndjson_free_req(out); return -1; }
        }
    }

    urepl_ndjson_free_req(out);   /* ran out before the closing brace */
    return -1;
}

/* ---- Emitter ---------------------------------------------------------- */

int
urepl_json_escape(const char *src, size_t src_len, char *dst, size_t dst_cap)
{
    size_t j = 0;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 > dst_cap) { return -1; }
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c == '\n') {
            if (j + 2 > dst_cap) { return -1; }
            dst[j++] = '\\'; dst[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 > dst_cap) { return -1; }
            dst[j++] = '\\'; dst[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 > dst_cap) { return -1; }
            dst[j++] = '\\'; dst[j++] = 't';
        } else if (c == '\b') {
            if (j + 2 > dst_cap) { return -1; }
            dst[j++] = '\\'; dst[j++] = 'b';
        } else if (c == '\f') {
            if (j + 2 > dst_cap) { return -1; }
            dst[j++] = '\\'; dst[j++] = 'f';
        } else if (c < 0x20) {
            if (j + 6 > dst_cap) { return -1; }
            int w = snprintf(dst + j, dst_cap - j, "\\u%04x", c);
            if (w < 0 || (size_t)w >= dst_cap - j) { return -1; }
            j += (size_t)w;
        } else {
            if (j + 1 > dst_cap) { return -1; }
            dst[j++] = (char)c;
        }
    }
    return (int)j;
}

/* Helper: append literal text to buf at *off; bounds-check. */
static int
append_lit(char *buf, size_t cap, size_t *off, const char *s)
{
    size_t n = strlen(s);
    if (*off + n >= cap) { return -1; }
    /* Appends raw bytes — caller manages NUL-termination at end of envelope.
     * NOLINTNEXTLINE(bugprone-not-null-terminated-result) */
    memcpy(buf + *off, s, n);
    *off += n;
    return 0;
}

static int
append_escaped(char *buf, size_t cap, size_t *off,
               const char *s, size_t s_len)
{
    if (*off >= cap) { return -1; }
    int w = urepl_json_escape(s, s_len, buf + *off, cap - *off);
    if (w < 0) { return -1; }
    *off += (size_t)w;
    return 0;
}

static int
append_quoted(char *buf, size_t cap, size_t *off, const char *s)
{
    if (append_lit(buf, cap, off, "\"") != 0) return -1;
    if (append_escaped(buf, cap, off, s, strlen(s)) != 0) return -1;
    if (append_lit(buf, cap, off, "\"") != 0) return -1;
    return 0;
}

static int
append_u64(char *buf, size_t cap, size_t *off, uint64_t v)
{
    if (*off >= cap) { return -1; }
    int w = snprintf(buf + *off, cap - *off, "%llu", (unsigned long long)v);
    if (w < 0 || (size_t)w >= cap - *off) { return -1; }
    *off += (size_t)w;
    return 0;
}

static int
finish(char *buf, size_t cap, size_t off, size_t *out_len)
{
    if (off + 1 >= cap) {
        if (out_len) { *out_len = off + 2; }
        return -1;
    }
    buf[off++] = '\n';
    buf[off] = '\0';
    if (out_len) { *out_len = off; }
    return 0;
}

int
urepl_ndjson_emit_result(char *buf, size_t cap, uint64_t id,
                         const char *value_json, size_t *out_len)
{
    size_t off = 0;
    if (append_lit(buf, cap, &off, "{\"id\":") != 0) return -1;
    if (append_u64(buf, cap, &off, id) != 0) return -1;
    if (append_lit(buf, cap, &off, ",\"kind\":\"result\",\"value\":") != 0) return -1;
    /* value_json is a JSON fragment the caller built: an eval hands over a
     * quoted string, an introspect hands over an object. */
    if (value_json == NULL || value_json[0] == '\0') {
        if (append_lit(buf, cap, &off, "null") != 0) return -1;
    } else if (append_lit(buf, cap, &off, value_json) != 0) {
        return -1;
    }
    if (append_lit(buf, cap, &off, "}") != 0) return -1;
    return finish(buf, cap, off, out_len);
}

int
urepl_ndjson_emit_output(char *buf, size_t cap, uint64_t id_or_zero,
                         const char *channel, const char *msg, size_t msg_len,
                         size_t *out_len)
{
    size_t off = 0;
    if (append_lit(buf, cap, &off, "{") != 0) return -1;
    /* No id means the write did not come from inside an eval frame — a
     * watcher or a timer the client armed earlier. */
    if (id_or_zero != 0) {
        if (append_lit(buf, cap, &off, "\"id\":") != 0) return -1;
        if (append_u64(buf, cap, &off, id_or_zero) != 0) return -1;
        if (append_lit(buf, cap, &off, ",") != 0) return -1;
    }
    if (append_lit(buf, cap, &off, "\"kind\":\"output\",\"channel\":") != 0) return -1;
    if (append_quoted(buf, cap, &off, channel != NULL ? channel : "") != 0) return -1;
    if (append_lit(buf, cap, &off, ",\"msg\":\"") != 0) return -1;
    if (append_escaped(buf, cap, &off, msg, msg_len) != 0) return -1;
    if (append_lit(buf, cap, &off, "\"}") != 0) return -1;
    return finish(buf, cap, off, out_len);
}

int
urepl_ndjson_emit_done(char *buf, size_t cap, uint64_t id, size_t *out_len)
{
    size_t off = 0;
    if (append_lit(buf, cap, &off, "{\"id\":") != 0) return -1;
    if (append_u64(buf, cap, &off, id) != 0) return -1;
    if (append_lit(buf, cap, &off, ",\"kind\":\"done\"}") != 0) return -1;
    return finish(buf, cap, off, out_len);
}

int
urepl_ndjson_emit_error(char *buf, size_t cap, uint64_t id_or_zero,
                        const char *code, const char *msg, size_t *out_len)
{
    size_t off = 0;
    if (append_lit(buf, cap, &off, "{") != 0) return -1;
    if (id_or_zero != 0) {
        if (append_lit(buf, cap, &off, "\"id\":") != 0) return -1;
        if (append_u64(buf, cap, &off, id_or_zero) != 0) return -1;
        if (append_lit(buf, cap, &off, ",") != 0) return -1;
    }
    if (append_lit(buf, cap, &off, "\"kind\":\"error\",\"code\":") != 0) return -1;
    if (append_quoted(buf, cap, &off, code != NULL ? code : "error") != 0) return -1;
    if (msg != NULL && msg[0] != '\0') {
        if (append_lit(buf, cap, &off, ",\"message\":") != 0) return -1;
        if (append_quoted(buf, cap, &off, msg) != 0) return -1;
    }
    if (append_lit(buf, cap, &off, "}") != 0) return -1;
    return finish(buf, cap, off, out_len);
}
