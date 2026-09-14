/* SPDX-License-Identifier: BSD-3-Clause */
/* repl-chk-driver — runs the NDJSON fixtures in tests/chk/repl/.
 *
 * Those fixtures are not urbiscript, so `urbi -i` cannot run them and the
 * diff-based path in run_chk.sh cannot check them: an expectation line
 * lists SUBSTRINGS that must appear in the next response, not the
 * response itself, because ids and addresses are not reproducible.  So
 * the matching happens here and the verdict is the exit status.
 *
 * Invocation: repl-chk-driver <chk-fixture>
 *   0  every expectation matched
 *   1  an expectation failed (the mismatch is printed)
 *   2  the fixture or the VM could not be set up
 *
 * Fixture grammar:
 *   # ...                      comment
 *   ## mode: repl              the runner's routing directive, ignored here
 *   @sessions=<n>              open n sessions instead of one
 *   @budget-depth=<n>          compile limits for every session's realm
 *   @budget-nodes=<n>
 *   @budget-source=<n>
 *   > {"id":1,"op":"eval",...} one request on session 1; sent, then swept
 *   >2 {...}                   the same, on session 2
 *   < tok | tok | tok          session 1's next response must contain
 *                              every token (order within the line is free)
 *   <2 tok | tok               the same, on session 2
 *
 * Two sessions are what makes the claims that MATTER checkable: that one
 * session's output does not reach another, that `Global` does, and that
 * `wall` reaches every lobby except the sender's.  One session can only
 * show that the plumbing exists.
 *
 * At end of fixture every client declares itself finished and one more
 * sweep runs, taking each session through the disconnect path — so every
 * fixture also exercises `handleDisconnect` and realm teardown, and a
 * leak or a use-after-free there shows up under the sanitizers on the
 * whole corpus at once. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "urbi/repl.h"
#include "urbi/urbi.h"

#include "repl/urepl_buffer_transport.h"

#define LINE_CAP     8192
#define RESP_CAP     (256u * 1024u)
#define MAX_SESSIONS 4

static void *drv_alloc(void *ptr, size_t n, void *ud)
{
    (void)ud;
    if (n == 0) { free(ptr); return NULL; }
    return realloc(ptr, n);
}

/* Fixed, because nothing in this corpus asserts on a timestamp and a
 * moving one only adds noise to the framed output. */
static uint64_t drv_clock(void *ud) { (void)ud; return 0; }

/* One client end: the transport, plus the response lines it has been
 * handed and no expectation has claimed yet. */
typedef struct {
    UBufferTransport *bt;
    char   resp[RESP_CAP];
    size_t fill;
    size_t off;
} Client;

static Client g_client[MAX_SESSIONS];
static int    g_nsessions = 1;

/* Moves everything the service has written into the client's buffer. */
static void collect(Client *c)
{
    for (;;) {
        if (c->fill >= RESP_CAP) return;
        size_t got = urepl_buffer_client_read(c->bt, c->resp + c->fill, RESP_CAP - c->fill);
        if (got == 0) return;
        c->fill += got;
    }
}

/* The next unclaimed response line, without its newline, or NULL when
 * none has arrived.  Terminated in place. */
static char *next_response(Client *c)
{
    if (c->off >= c->fill) return NULL;
    char *start = c->resp + c->off;
    char *nl = (char *)memchr(start, '\n', c->fill - c->off);
    if (nl == NULL) return NULL;
    *nl = '\0';
    c->off = (size_t)(nl - c->resp) + 1u;
    return start;
}

/* Trims leading and trailing blanks in place and returns the start. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'
                     || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
    return s;
}

/* Every '|'-separated token of `spec` must appear in `line`. */
static bool line_matches(const char *line, char *spec, const char **missing)
{
    char *save = spec;
    for (;;) {
        char *bar = strchr(save, '|');
        if (bar != NULL) *bar = '\0';
        char *tok = trim(save);
        if (tok[0] != '\0' && strstr(line, tok) == NULL) { *missing = tok; return false; }
        if (bar == NULL) return true;
        save = bar + 1;
    }
}

/* Reads the optional session number a `>` or `<` may carry and advances
 * `p` past it.  Returns a 0-based index, or -1 when it is out of range. */
static int session_prefix(char **p)
{
    char *s = *p;
    if (*s < '1' || *s > '9') return 0;
    int n = *s - '1';
    *p = s + 1;
    return n < g_nsessions ? n : -1;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: repl-chk-driver <chk-fixture>\n"); return 2; }
    FILE *fp = fopen(argv[1], "rb");
    if (fp == NULL) { fprintf(stderr, "repl-chk-driver: cannot open %s\n", argv[1]); return 2; }

    /* Pass one: the pragmas, which have to be known before any session's
     * realm is built. */
    UReplConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    char line[LINE_CAP];
    while (fgets(line, sizeof line, fp) != NULL) {
        unsigned long v = 0;
        if      (sscanf(line, "@budget-depth=%lu",  &v) == 1) cfg.default_budget.max_parser_depth = (uint32_t)v;
        else if (sscanf(line, "@budget-nodes=%lu",  &v) == 1) cfg.default_budget.max_ast_nodes    = (uint32_t)v;
        else if (sscanf(line, "@budget-source=%lu", &v) == 1) cfg.default_budget.max_source_bytes = (uint32_t)v;
        else if (sscanf(line, "@sessions=%lu",      &v) == 1) g_nsessions = (int)v;
    }
    rewind(fp);
    if (g_nsessions < 1 || g_nsessions > MAX_SESSIONS) {
        fclose(fp);
        fprintf(stderr, "repl-chk-driver: @sessions must be 1..%d\n", MAX_SESSIONS);
        return 2;
    }

    UVM *vm = urbi_open(drv_alloc, NULL, NULL);
    if (vm == NULL) { fclose(fp); fprintf(stderr, "repl-chk-driver: out of memory\n"); return 2; }
    urbi_set_clock(vm, drv_clock, NULL);

    UReplServer *server = NULL;
    if (urbi_repl_serve_init(vm, &cfg, &server) != URBI_OK) {
        urbi_close(vm); fclose(fp);
        fprintf(stderr, "repl-chk-driver: serve_init failed\n");
        return 2;
    }

    for (int i = 0; i < g_nsessions; i++) {
        g_client[i].bt = urepl_buffer_transport_create();
        UTransport vtable;
        if (g_client[i].bt == NULL) {
            fprintf(stderr, "repl-chk-driver: out of memory\n");
            urbi_repl_serve_shutdown(server); urbi_close(vm); fclose(fp);
            return 2;
        }
        urepl_buffer_transport_vtable(g_client[i].bt, &vtable);
        if (urbi_repl_register_transport(server, &vtable) != URBI_OK) {
            fprintf(stderr, "repl-chk-driver: register_transport failed\n");
            urbi_repl_serve_shutdown(server); urbi_close(vm); fclose(fp);
            return 2;
        }
    }

    int rc = 0;
    int lineno = 0;
    int checks = 0;
    while (fgets(line, sizeof line, fp) != NULL) {
        lineno++;
        char *s = trim(line);
        if (s[0] == '\0' || s[0] == '#' || s[0] == '@') continue;

        if (s[0] == '>' || s[0] == '<') {
            char kind = s[0];
            char *p = s + 1;
            int idx = session_prefix(&p);
            if (idx < 0) {
                fprintf(stderr, "%s:%d: session out of range (only %d open)\n",
                        argv[1], lineno, g_nsessions);
                rc = 2;
                break;
            }
            char *rest = trim(p);

            if (kind == '>') {
                size_t n = strlen(rest);
                (void)urepl_buffer_client_write(g_client[idx].bt, rest, n);
                (void)urepl_buffer_client_write(g_client[idx].bt, "\n", 1);
                (void)urbi_repl_serve_step(server, 0);
                /* One VM step after the request, so anything the eval
                 * armed that is due immediately has run before the next
                 * request — the courtesy a host's event loop gives. */
                (void)urbi_step(vm, 0, NULL);
                (void)urbi_repl_serve_step(server, 0);
                /* Every client, not just the sender: a broadcast lands on
                 * someone else's stream, which is the point of it. */
                for (int i = 0; i < g_nsessions; i++) collect(&g_client[i]);
                continue;
            }

            char *resp = next_response(&g_client[idx]);
            checks++;
            if (resp == NULL) {
                fprintf(stderr, "%s:%d: session %d: expected a response, none arrived\n  want: %s\n",
                        argv[1], lineno, idx + 1, rest);
                rc = 1;
                break;
            }
            const char *missing = NULL;
            if (!line_matches(resp, rest, &missing)) {
                fprintf(stderr, "%s:%d: session %d: response does not contain \"%s\"\n  got: %s\n",
                        argv[1], lineno, idx + 1, missing, resp);
                rc = 1;
                break;
            }
            continue;
        }

        fprintf(stderr, "%s:%d: unrecognised fixture line: %s\n", argv[1], lineno, s);
        rc = 2;
        break;
    }
    fclose(fp);

    /* A response nobody claimed is as much a failure as one that never
     * came: an extra envelope means the service said something the
     * fixture did not expect. */
    if (rc == 0) {
        for (int i = 0; i < g_nsessions; i++) {
            char *extra = next_response(&g_client[i]);
            if (extra != NULL) {
                fprintf(stderr, "%s: session %d: unclaimed response: %s\n",
                        argv[1], i + 1, extra);
                rc = 1;
                break;
            }
        }
    }

    if (rc == 0 && checks == 0) {
        /* A fixture that asserts nothing passes for the wrong reason.  The
         * corpus runner has the same rule for the script path (VACUOUS). */
        fprintf(stderr, "%s: no expectation lines; the fixture asserts nothing\n", argv[1]);
        rc = 1;
    }

    /* End of stream: the disconnect path, then teardown. */
    for (int i = 0; i < g_nsessions; i++) urepl_buffer_client_finish(g_client[i].bt);
    (void)urbi_repl_serve_step(server, 0);
    urbi_repl_serve_shutdown(server);
    for (int i = 0; i < g_nsessions; i++) urepl_buffer_transport_destroy(g_client[i].bt);
    urbi_close(vm);
    return rc;
}
