/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_ndjson.h — the request parser and the response emitter.
 *
 * One JSON document per line.  This is a SCHEMA scanner, not a JSON
 * library: it recognises the handful of keys the two live ops use,
 * skips any other key, and rejects an op it does not know.  A general
 * reader is not needed, because nothing on this side of the wire reads
 * arbitrary JSON.
 *
 * Allocation: a parsed request owns its string fields, so a successful
 * parse must be matched by urepl_ndjson_free_req. */

#ifndef UREPL_NDJSON_H
#define UREPL_NDJSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The live op set.  Two, because two is what the service does: run a
 * line, and answer a question about the VM.  The old server's auth,
 * cancel, lobby_new and lobby_close are parked with it — auth guards a
 * network there is none of, cancel needs the job queue, and the two
 * lobby ops named a multi-lobby-per-connection model that never shipped.
 * An unrecognised op is a parse failure, not a silent no-op. */
typedef enum {
    UREPL_OP_NONE = 0,
    UREPL_OP_EVAL,
    UREPL_OP_INTROSPECT
} UReplOp;

/* Field caps.  A string over its cap fails the parse rather than
 * truncating, so a client never gets a half-request run. */
#define UREPL_MAX_LINE   (1u << 20)   /* framing cap for one request line */
#define UREPL_MAX_CODE   (1u << 20)
#define UREPL_MAX_WHAT   32u

typedef struct {
    uint64_t id;
    UReplOp  op;
    char    *code;      /* NUL-terminated AND length-prefixed */
    size_t   code_len;
    char    *what;
} UReplNdjsonReq;

/* 0 on success with *out populated (free it with urepl_ndjson_free_req),
 * negative on failure with *out zeroed and nothing to free. */
int  urepl_ndjson_parse(const char *line, size_t len, UReplNdjsonReq *out);
void urepl_ndjson_free_req(UReplNdjsonReq *req);

/* ---- Emitter ----------------------------------------------------------
 *
 * Each writes ONE line, trailing newline included, into buf[0..cap).
 * Returns 0 with *out_len set to the byte count (a NUL is always planted
 * past it), or -1 when the line does not fit, having written nothing. */

int urepl_ndjson_emit_result(char *buf, size_t cap, uint64_t id,
                             const char *value_json, size_t *out_len);
int urepl_ndjson_emit_output(char *buf, size_t cap, uint64_t id_or_zero,
                             const char *channel, const char *msg, size_t msg_len,
                             size_t *out_len);
int urepl_ndjson_emit_done  (char *buf, size_t cap, uint64_t id, size_t *out_len);
int urepl_ndjson_emit_error (char *buf, size_t cap, uint64_t id_or_zero,
                             const char *code, const char *msg, size_t *out_len);

/* JSON-escapes src[0..src_len) into dst[0..dst_cap), NOT NUL-terminated.
 * Returns the byte count, or -1 on overflow.  Exposed because the
 * session writer escapes a message before framing it. */
int urepl_json_escape(const char *src, size_t src_len, char *dst, size_t dst_cap);

#endif /* UREPL_NDJSON_H */
