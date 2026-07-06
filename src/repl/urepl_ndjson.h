/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_ndjson.h - NDJSON request parser + response emitter (v0.9.1)
 *
 * Wire format per spec §6.  One JSON document per line.  This codec is
 * scoped to the REPL request schema — it is NOT a general-purpose JSON
 * parser (see src/json.c, Phase 4 Task 21, for that).
 *
 * Allocation: parsed string fields are strdup-style owned by the
 * request struct; caller MUST call urepl_ndjson_free_req on success. */
#ifndef UREPL_NDJSON_H
#define UREPL_NDJSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Op tag.  UREPL_OP_NONE is a sentinel for "parse failed before
 * recognizing op". */
typedef enum {
    UREPL_OP_NONE = 0,
    UREPL_OP_AUTH,
    UREPL_OP_EVAL,
    UREPL_OP_CANCEL,
    UREPL_OP_INTROSPECT,
    UREPL_OP_LOBBY_NEW,
    UREPL_OP_LOBBY_CLOSE
} UReplOp;

/* Field size caps.  Strings exceeding these caps cause parse failure. */
#define UREPL_MAX_LINE   (1u << 20)   /* 1 MiB framing cap, spec §6.4 */
#define UREPL_MAX_CODE   (1u << 20)
#define UREPL_MAX_TOKEN  256u
#define UREPL_MAX_LOBBY  64u
#define UREPL_MAX_WHAT   32u
#define UREPL_MAX_FILE   256u
#define UREPL_MAX_TAG    128u
#define UREPL_MAX_OBJ    256u

/* Parsed request.  All char* fields are NUL-terminated and heap-owned.
 * code is NUL-terminated AND length-prefixed (code_len is byte count
 * excluding the trailing NUL). */
typedef struct {
    uint64_t id;
    UReplOp  op;
    char    *lobby;
    char    *token;
    char    *code;
    size_t   code_len;
    char    *what;
    char    *tag;
    char    *file;
    uint32_t line;
    uint32_t coro_id;
    char    *obj;
} UReplNdjsonReq;

/* Parse a single NDJSON line.  Returns 0 on success, negative on error.
 * On success, *out is populated; caller must free via urepl_ndjson_free_req.
 * On error, *out is fully zeroed (no leaks; nothing to free). */
int  urepl_ndjson_parse(const char *line, size_t len, UReplNdjsonReq *out);

/* Free heap-owned fields and zero the struct.  Safe on a zeroed req. */
void urepl_ndjson_free_req(UReplNdjsonReq *req);

/* ---- Emitter ---------------------------------------------------------- */

/* All emit functions write a single NDJSON line (including trailing '\n')
 * into buf[0..cap).  On success: returns 0, *out_len = bytes written
 * excluding the NUL terminator that emit always plants for safety.
 * On overflow: returns -1, *out_len = required cap (estimate). */

int urepl_ndjson_emit_hello   (char *buf, size_t cap, const char *lobby,
                               bool synclines, bool auth_required,
                               size_t *out_len);
int urepl_ndjson_emit_auth_ok (char *buf, size_t cap, uint64_t id,
                               size_t *out_len);
int urepl_ndjson_emit_result  (char *buf, size_t cap, uint64_t id,
                               const char *value_json, uint64_t ts_us,
                               size_t *out_len);
int urepl_ndjson_emit_output  (char *buf, size_t cap,
                               uint64_t id_or_zero,
                               const char *lobby_or_null,
                               const char *channel,
                               const char *msg, size_t msg_len,
                               uint64_t ts_us,
                               size_t *out_len);
int urepl_ndjson_emit_done    (char *buf, size_t cap, uint64_t id,
                               size_t *out_len);
int urepl_ndjson_emit_error   (char *buf, size_t cap, uint64_t id_or_zero,
                               const char *code, const char *msg,
                               size_t *out_len);
int urepl_ndjson_emit_event   (char *buf, size_t cap,
                               const char *lobby, const char *name,
                               const char *payload_json, uint64_t ts_us,
                               size_t *out_len);
int urepl_ndjson_emit_goodbye (char *buf, size_t cap, const char *reason,
                               size_t *out_len);

/* JSON-escape src[0..src_len) into dst[0..dst_cap) (NOT NUL-terminated).
 * Returns bytes written or -1 on overflow.  Visible for tests + the
 * dispatcher's session_writer. */
int urepl_json_escape(const char *src, size_t src_len,
                      char *dst, size_t dst_cap);

#endif /* UREPL_NDJSON_H */
