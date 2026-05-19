/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_chk_corpus.c — REPL NDJSON .chk corpus runner.
 *
 * v0.9.1 Phase 8 Task 32.
 *
 * Drives the in-process REPL dispatcher against tests/chk/repl/*.chk
 * fixtures.  Each .chk file mixes:
 *
 *   # comment           — ignored
 *                       — (blank line) ignored
 *   > {ndjson request}  — request fed via urepl_dispatch_job
 *   < substr1 [substr2] — assertion that the next response line contains
 *                         every listed substring
 *   @pragma             — pragma (e.g. @no-auto-session)
 *
 * Each `<` line consumes one response line from the session's output
 * ringbuf.  Wildcards (timestamps, lobby ids, ts:..., coro addresses)
 * are handled by omitting those fields from the `<` line and relying on
 * substring matching of the stable parts.
 *
 * Why this shape:
 *  - The existing tests/integration/run_chk.sh drives urbiscript via the
 *    `urbi` REPL binary.  This is a different protocol (NDJSON, not echo
 *    text), so a separate runner makes sense.
 *  - The runner stays in-process (buffer transport) — no socket /
 *    pthread races.  Fixtures stay deterministic across hosts.
 *  - One C TEST per .chk file via utest harness.
 *
 * Discovery:  tests/chk/repl/ is read at runtime via opendir.  cwd at
 * runtime is the urbi-embedded/ root (the Makefile runs $(RUNNER) from
 * that dir).  Override with URBI_REPL_CHK_DIR for out-of-tree runs.
 */
#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "repl/urepl.h"
#include "repl/urepl_dispatch.h"
#include "repl/urepl_ndjson.h"
#include "repl/urepl_queue.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define UTEST(name) static void name(void)

/* Default chk dir relative to the cwd from which the runner is invoked
 * (the Makefile sets cwd = urbi-embedded/). */
static const char *
chk_dir(void)
{
    const char *e = getenv("URBI_REPL_CHK_DIR");
    return (e != NULL && *e != '\0') ? e : "tests/chk/repl";
}

/* ---- Fixture state --------------------------------------------------- */

typedef struct {
    char        *path;
    char        *base;   /* basename minus .chk */
    /* Parsed body. */
    char       **req_lines;     /* '>' lines, NDJSON */
    size_t       req_count;
    char      ***resp_lines;    /* resp_lines[i] is a NULL-terminated array
                                 * of substrings expected in the i'th
                                 * response line. */
    size_t      *resp_substr_count;
    size_t       resp_count;
    bool         no_auto_session;
    /* Optional budget overrides (0 = use REPL default).  Set via
     * @budget-depth=N / @budget-nodes=N / @budget-source=N pragmas. */
    uint32_t     budget_depth;
    uint32_t     budget_nodes;
    uint32_t     budget_source;
    int          line_no;
} Fixture;

static void
fixture_free(Fixture *fx)
{
    if (fx == NULL) return;
    free(fx->path);
    free(fx->base);
    for (size_t i = 0; i < fx->req_count; ++i) free(fx->req_lines[i]);
    free(fx->req_lines);
    for (size_t i = 0; i < fx->resp_count; ++i) {
        if (fx->resp_lines[i] != NULL) {
            for (size_t j = 0; fx->resp_lines[i][j] != NULL; ++j) {
                free(fx->resp_lines[i][j]);
            }
            free(fx->resp_lines[i]);
        }
    }
    free(fx->resp_lines);
    free(fx->resp_substr_count);
}

/* Trim leading/trailing whitespace IN PLACE.  Returns pointer to first
 * non-ws byte (may be == s + strlen(s) for an empty line). */
static char *
trim(char *s)
{
    while (*s == ' ' || *s == '\t') ++s;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t'
                  || s[n-1] == '\r' || s[n-1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

/* Append a substring (heap-dup) to fx->resp_lines[idx].  Grows the
 * inner array. */
static int
append_substr(Fixture *fx, size_t idx, const char *substr)
{
    size_t old = fx->resp_substr_count[idx];
    char **new_arr = (char **)realloc(fx->resp_lines[idx],
                                      (old + 2) * sizeof(char *));
    if (new_arr == NULL) return -1;
    new_arr[old] = strdup(substr);
    new_arr[old + 1] = NULL;
    if (new_arr[old] == NULL) {
        fx->resp_lines[idx] = new_arr;
        return -1;
    }
    fx->resp_lines[idx] = new_arr;
    fx->resp_substr_count[idx] = old + 1;
    return 0;
}

/* Parse the substrings from a '<' line.  Format: each substring is
 * either bare text (matches up to next whitespace) or a quoted-with-pipe
 * literal `"foo ... bar"` (the surrounding quotes are stripped; the
 * literal allows internal whitespace).  Simpler: split on pipe '|'.
 *
 * To keep the format friendly we split on the literal sequence ` | ` —
 * space-pipe-space — so JSON like `"foo":"bar"` doesn't get torn apart.
 * If a `<` line contains no ` | ` separator, the whole line is one
 * substring.  Leading `< ` is stripped by the caller. */
static int
parse_substrs(Fixture *fx, size_t idx, const char *raw)
{
    const char *p = raw;
    while (*p != '\0') {
        const char *sep = strstr(p, " | ");
        size_t span = (sep != NULL) ? (size_t)(sep - p) : strlen(p);
        char *tok = (char *)malloc(span + 1);
        if (tok == NULL) return -1;
        memcpy(tok, p, span);
        tok[span] = '\0';
        /* Trim trailing whitespace on this token. */
        size_t tl = span;
        while (tl > 0 && (tok[tl-1] == ' ' || tok[tl-1] == '\t')) tok[--tl] = '\0';
        if (tl > 0) {
            if (append_substr(fx, idx, tok) != 0) { free(tok); return -1; }
        }
        free(tok);
        if (sep == NULL) break;
        p = sep + 3;
    }
    return 0;
}

static int
parse_fixture(const char *path, Fixture *fx)
{
    memset(fx, 0, sizeof(*fx));
    fx->path = strdup(path);
    /* Derive base name (last '/' to last '.'). */
    const char *slash = strrchr(path, '/');
    const char *base = (slash != NULL) ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t bn = (dot != NULL) ? (size_t)(dot - base) : strlen(base);
    fx->base = (char *)malloc(bn + 1);
    if (fx->base == NULL) return -1;
    memcpy(fx->base, base, bn);
    fx->base[bn] = '\0';

    FILE *fp = fopen(path, "r");
    if (fp == NULL) return -1;
    char line[4096];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        ++line_no;
        char *t = trim(line);
        if (*t == '\0' || *t == '#') continue;
        if (*t == '@') {
            if (strncmp(t, "@no-auto-session", 16) == 0) {
                fx->no_auto_session = true;
            } else if (strncmp(t, "@budget-depth=", 14) == 0) {
                fx->budget_depth = (uint32_t)strtoul(t + 14, NULL, 10);
            } else if (strncmp(t, "@budget-nodes=", 14) == 0) {
                fx->budget_nodes = (uint32_t)strtoul(t + 14, NULL, 10);
            } else if (strncmp(t, "@budget-source=", 15) == 0) {
                fx->budget_source = (uint32_t)strtoul(t + 15, NULL, 10);
            }
            /* Future pragmas land here. */
            continue;
        }
        if (*t == '>') {
            /* Request line. */
            char *rest = t + 1;
            while (*rest == ' ' || *rest == '\t') ++rest;
            char **new_reqs = (char **)realloc(fx->req_lines,
                                       (fx->req_count + 1) * sizeof(char *));
            if (new_reqs == NULL) { fclose(fp); return -1; }
            new_reqs[fx->req_count] = strdup(rest);
            if (new_reqs[fx->req_count] == NULL) {
                fx->req_lines = new_reqs;
                fclose(fp);
                return -1;
            }
            fx->req_lines = new_reqs;
            fx->req_count += 1;
            continue;
        }
        if (*t == '<') {
            char *rest = t + 1;
            while (*rest == ' ' || *rest == '\t') ++rest;
            size_t idx = fx->resp_count;
            char ***new_resp = (char ***)realloc(fx->resp_lines,
                                       (idx + 1) * sizeof(char **));
            if (new_resp == NULL) { fclose(fp); return -1; }
            new_resp[idx] = NULL;
            fx->resp_lines = new_resp;
            size_t *new_cnt = (size_t *)realloc(fx->resp_substr_count,
                                       (idx + 1) * sizeof(size_t));
            if (new_cnt == NULL) { fclose(fp); return -1; }
            new_cnt[idx] = 0;
            fx->resp_substr_count = new_cnt;
            fx->resp_count += 1;
            if (parse_substrs(fx, idx, rest) != 0) {
                fclose(fp);
                return -1;
            }
            continue;
        }
        /* Anything else is a fixture-author error. */
        fprintf(stderr, "  fixture %s:%d: unrecognized line: %s\n",
                path, line_no, t);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

/* ---- Runner --------------------------------------------------------- */

/* mk_server / free_server mirror the dispatcher test helper, sized for
 * an inline VM (calloc to keep the bigger struct off the stack).  When
 * `fx` carries @budget-* overrides, the server's default_budget is
 * populated so urepl_session_create applies the tightened budget. */
static UReplServer *
mk_server(UVM **out_vm, const Fixture *fx)
{
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    if (vm == NULL) return NULL;
    if (urbi_vm_init(vm, NULL, NULL) != URBI_OK) {
        free(vm);
        return NULL;
    }
    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port = -1;
    if (fx != NULL && (fx->budget_depth != 0U
                       || fx->budget_nodes != 0U
                       || fx->budget_source != 0U)) {
        /* Default unset fields to the REPL default so the override
         * tightens ONE axis without inadvertently zeroing the others
         * (zero means "no limit" in the budget machinery, which would
         * disable the very check the fixture is exercising). */
        cfg.default_budget.max_parser_depth = (fx->budget_depth != 0U)
            ? fx->budget_depth
            : URBI_DEFAULT_REPL_BUDGET.max_parser_depth;
        cfg.default_budget.max_ast_nodes = (fx->budget_nodes != 0U)
            ? fx->budget_nodes
            : URBI_DEFAULT_REPL_BUDGET.max_ast_nodes;
        cfg.default_budget.max_source_bytes = (fx->budget_source != 0U)
            ? fx->budget_source
            : URBI_DEFAULT_REPL_BUDGET.max_source_bytes;
    }
    int err = 0;
    UReplServer *server = urbi_repl_serve(vm, &cfg, &err);
    if (server == NULL) {
        urbi_vm_destroy(vm);
        free(vm);
        return NULL;
    }
    *out_vm = vm;
    return server;
}

static void
free_server(UReplServer *server, UVM *vm)
{
    urbi_repl_stop(server);
    urbi_vm_destroy(vm);
    free(vm);
}

/* Build a UReplJob for a session by parsing the request line.  Returns
 * NULL on parse failure (the runner reports it). */
static UReplJob *
make_job_from_line(UReplSession *s, const char *line)
{
    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    if (job == NULL) return NULL;
    job->session_id = s->session_id;
    if (urepl_ndjson_parse(line, strlen(line), &job->req) != 0) {
        free(job);
        return NULL;
    }
    return job;
}

/* Per-fixture accumulator for the line reader.  Reset between fixtures
 * by read_next_line_reset().  Sized at 32 KiB to absorb the largest
 * introspect envelope (~10 KiB result + JSON-string wrapping) plus
 * pipelined output envelopes that may arrive in the same drain. */
static char g_acc[32 * 1024];
static size_t g_acc_n = 0;
static const size_t g_acc_cap = sizeof(g_acc);

/* Try to read one NDJSON line from the session's output ringbuf.  Drives
 * a few urbi_steps in case streaming output is in flight.  Returns the
 * (heap-allocated, NUL-terminated) line on success, or NULL on
 * timeout/EOF.  The trailing newline is stripped. */
static char *
read_next_line(UVM *vm, UReplSession *s, int max_steps)
{
    for (int attempt = 0; attempt < max_steps; ++attempt) {
        /* Slurp whatever is currently buffered into the global acc. */
        size_t got = urepl_ringbuf_read(&s->output, g_acc + g_acc_n,
                                        g_acc_cap - 1 - g_acc_n);
        g_acc_n += got;
        g_acc[g_acc_n] = '\0';
        char *nl = (char *)memchr(g_acc, '\n', g_acc_n);
        if (nl != NULL) {
            size_t llen = (size_t)(nl - g_acc);
            char *line = (char *)malloc(llen + 1);
            if (line == NULL) return NULL;
            memcpy(line, g_acc, llen);
            line[llen] = '\0';
            size_t rest = g_acc_n - (llen + 1);
            memmove(g_acc, nl + 1, rest);
            g_acc_n = rest;
            g_acc[g_acc_n] = '\0';
            return line;
        }
        /* No newline yet — drive vm + sleep briefly so async output has
         * a chance to settle. */
        urbi_step(vm, 256, NULL);
        struct timespec ts = { 0, 2 * 1000 * 1000 };  /* 2 ms */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void
read_next_line_reset(void)
{
    /* Drop any stragglers left over from the previous fixture's
     * session.  Without this, a partial line buffered between fixture
     * runs would corrupt the first match of the next fixture. */
    g_acc_n = 0;
    g_acc[0] = '\0';
}

/* Confirm every substr appears somewhere in `line`. */
static int
all_substrs_in(const char *line, char *const *substrs)
{
    for (size_t i = 0; substrs[i] != NULL; ++i) {
        if (strstr(line, substrs[i]) == NULL) return 0;
    }
    return 1;
}

/* Format the substr list as `subA | subB` for diagnostics. */
static void
fmt_substrs(char *const *substrs, char *out, size_t cap)
{
    size_t off = 0;
    for (size_t i = 0; substrs[i] != NULL; ++i) {
        const char *sep = (i == 0) ? "" : " | ";
        int n = snprintf(out + off, cap - off, "%s%s", sep, substrs[i]);
        if (n < 0 || (size_t)n >= cap - off) break;
        off += (size_t)n;
    }
}

/* Run one fixture.  Returns 0 on PASS, -1 on FAIL.  Logs diagnostics
 * via printf so the utest_run wrapper picks up the FAIL line. */
static int
run_fixture(const Fixture *fx)
{
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm, fx);
    if (server == NULL) {
        printf("  fx %s: server create failed\n", fx->base);
        return -1;
    }
    UReplSession *s = NULL;
    if (!fx->no_auto_session) {
        s = urepl_session_create(server);
        if (s == NULL) {
            printf("  fx %s: session create failed\n", fx->base);
            free_server(server, vm);
            return -1;
        }
        s->authed = true;
        /* Drain the hello envelope emitted on session create iff the
         * listener path normally pushes one.  For sessions created
         * outside the listener (this path) the hello is NOT pushed
         * (see urepl_listener.c spawn_reader).  Nothing to drain. */
    }

    read_next_line_reset();
    int rc = 0;
    size_t resp_idx = 0;
    for (size_t r = 0; r < fx->req_count; ++r) {
        UReplJob *job = make_job_from_line(s, fx->req_lines[r]);
        if (job == NULL) {
            printf("  fx %s: failed to parse '>' #%zu: %s\n",
                   fx->base, r, fx->req_lines[r]);
            rc = -1;
            break;
        }
        urepl_dispatch_job(server, job);

        /* Read response lines until we've consumed enough '<' lines for
         * the typical 2-3-line envelope shape (result + done, or auth_ok,
         * or error + done).  We allow up to MAX_RESP_PER_REQ lines per
         * request slot — fixtures explicitly list the lines they care
         * about. */
        const int MAX_RESP_PER_REQ = 16;
        for (int rl = 0; rl < MAX_RESP_PER_REQ && resp_idx < fx->resp_count; ++rl) {
            char *line = read_next_line(vm, s, 50);
            if (line == NULL) {
                printf("  fx %s: timed out waiting for response #%zu "
                       "(after req #%zu)\n",
                       fx->base, resp_idx, r);
                rc = -1;
                break;
            }
            /* Match against expected substrs. */
            if (!all_substrs_in(line, fx->resp_lines[resp_idx])) {
                char fmtd[512];
                fmt_substrs(fx->resp_lines[resp_idx], fmtd, sizeof(fmtd));
                printf("  fx %s: response #%zu mismatch\n"
                       "    got:      %s\n"
                       "    expected: %s\n",
                       fx->base, resp_idx, line, fmtd);
                free(line);
                rc = -1;
                break;
            }
            free(line);
            resp_idx += 1;
            /* If the line we matched contained "kind":"done" we stop
             * draining responses for this request to keep alignment. */
            const char *done = strstr(fx->resp_lines[resp_idx - 1][0],
                                      "kind\":\"done\"");
            if (done == NULL) {
                /* Check across all substrs of the last matched line. */
                for (size_t k = 0; fx->resp_lines[resp_idx - 1][k] != NULL; ++k) {
                    if (strstr(fx->resp_lines[resp_idx - 1][k],
                               "kind\":\"done\"") != NULL) {
                        done = fx->resp_lines[resp_idx - 1][k];
                        break;
                    }
                }
            }
            if (done != NULL) break;
        }
        if (rc != 0) break;
    }
    if (rc == 0 && resp_idx < fx->resp_count) {
        printf("  fx %s: only %zu/%zu expected response lines consumed\n",
               fx->base, resp_idx, fx->resp_count);
        rc = -1;
    }

    if (s != NULL) urepl_session_destroy(server, s);
    free_server(server, vm);
    return rc;
}

/* ---- Driver: iterate the chk/repl/ dir and run each .chk ------------ */

static int
ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    return (ls >= lf) && (strcmp(s + ls - lf, suffix) == 0);
}

static int g_chk_files_seen = 0;
static int g_chk_files_pass = 0;

static void
run_all_chk_fixtures(void)
{
    const char *dir = chk_dir();
    DIR *d = opendir(dir);
    if (d == NULL) {
        printf("  WARN: cannot open %s: %s — skipping chk corpus\n",
               dir, strerror(errno));
        return;
    }
    /* Collect entries first so we can run them in sorted order — the
     * Linux readdir order is filesystem-dependent. */
    char *names[64];
    size_t n_names = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n_names < 64) {
        if (e->d_name[0] == '.') continue;
        if (!ends_with(e->d_name, ".chk")) continue;
        names[n_names] = strdup(e->d_name);
        if (names[n_names] != NULL) n_names += 1;
    }
    closedir(d);
    /* Sort lexicographically. */
    for (size_t i = 0; i + 1 < n_names; ++i) {
        for (size_t j = i + 1; j < n_names; ++j) {
            if (strcmp(names[i], names[j]) > 0) {
                char *tmp = names[i]; names[i] = names[j]; names[j] = tmp;
            }
        }
    }

    for (size_t i = 0; i < n_names; ++i) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        Fixture fx;
        if (parse_fixture(path, &fx) != 0) {
            printf("  fx %s: parse failed\n", path);
            utest_checks++;
            utest_failures++;
            fixture_free(&fx);
            free(names[i]);
            continue;
        }
        g_chk_files_seen += 1;
        utest_checks++;
        int rc = run_fixture(&fx);
        if (rc == 0) {
            g_chk_files_pass += 1;
            printf("    chk PASS %s\n", fx.base);
        } else {
            utest_failures++;
        }
        fixture_free(&fx);
        free(names[i]);
    }
}

/* ---- utest suite entry ---------------------------------------------- */

UTEST(chk_corpus_runs_all_fixtures)
{
    g_chk_files_seen = 0;
    g_chk_files_pass = 0;
    run_all_chk_fixtures();
    /* If we found NO fixtures, that's a sanity failure (means the
     * corpus directory is missing or the discovery path is wrong). */
    UASSERT(g_chk_files_seen > 0);
    UASSERT_EQ(g_chk_files_pass, g_chk_files_seen);
}

void
test_repl_chk_corpus_suite(void)
{
    printf("test_repl_chk_corpus\n");
    utest_run("chk_corpus_runs_all_fixtures",
              chk_corpus_runs_all_fixtures);
}

#else  /* !URBI_ENABLE_REPL */

void test_repl_chk_corpus_suite(void) { /* skipped: URBI_ENABLE_REPL=0 */ }

#endif
