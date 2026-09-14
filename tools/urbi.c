/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi — the host REPL binary.  Host-only.
 *
 * Everything here goes through <urbi/urbi.h>; the one exception is
 * --dump-bytecode, which reaches the kept disassembler through
 * src/emit/ufront.h because disassembly is a build-time tool, not part
 * of the embedding surface. */

/* POSIX interfaces: clock_gettime, struct timespec, fileno, isatty. */
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "urbi/urbi.h"
#include "emit/ufront.h"
#include "chunk/uchunk.h"

#include "linenoise.h"

#define URBI_CLI_MAX_FILE (16u * 1024u * 1024u)

static void print_usage(FILE *out) {
    fputs(
        "Usage: urbi [options] [file]\n"
        "\n"
        "Modes:\n"
        "  (no args)            interactive REPL if stdin is a terminal,\n"
        "                       otherwise read stdin as a source file\n"
        "  -i                   force interactive mode\n"
        "  -e <expr>            evaluate <expr> and print the result\n"
        "  -f <file>            run <file> as a source script\n"
        "  <file>               run <file> as a source script (positional)\n"
        "  --dump-bytecode      print disassembly instead of running\n"
        "                       (combine with -e <expr> or a file)\n"
        "  --dump-wire-format   print the on-disk serialized bytes (raw binary)\n"
        "                       to stdout instead of running\n"
        "\n"
        "Options:\n"
        "  --version, -V        print version and exit\n"
        "  --help, -h           print this help and exit\n",
        out);
}

static bool eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* --- allocator ---------------------------------------------------------- */

static void *cli_alloc(void *p, size_t n, void *ud) {
    (void)ud;
    if (n == 0) { free(p); return NULL; }
    return realloc(p, n);
}

/* --- source loading ----------------------------------------------------- */

static char *slurp(const char *path, size_t *out_len) {
    FILE *fp = eq(path, "-") ? stdin : fopen(path, "rb");
    if (!fp) { fprintf(stderr, "urbi: cannot open %s\n", path); return NULL; }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fprintf(stderr, "urbi: out of memory\n"); goto fail; }
    for (;;) {
        if (len + 4096 > cap) {
            if (cap >= URBI_CLI_MAX_FILE) {
                fprintf(stderr, "urbi: %s exceeds %u byte cap\n", path, URBI_CLI_MAX_FILE);
                goto fail;
            }
            size_t ncap = cap * 2;
            if (ncap > URBI_CLI_MAX_FILE) ncap = URBI_CLI_MAX_FILE;
            char *n = realloc(buf, ncap);
            if (!n) { fprintf(stderr, "urbi: out of memory\n"); goto fail; }
            buf = n; cap = ncap;
        }
        size_t r = fread(buf + len, 1, cap - len, fp);
        len += r;
        if (r == 0) break;
    }
    if (fp != stdin) fclose(fp);
    *out_len = len;
    return buf;
fail:
    free(buf);
    if (fp && fp != stdin) fclose(fp);
    return NULL;
}

/* A bare expression is not a statement until it is terminated.  Append
 * " |" unless the text already ends in a separator, matching what the
 * REPL does for a typed line. */
static char *terminate(const char *src, size_t len, size_t *out_len) {
    char *buf = malloc(len + 3);
    if (!buf) { fprintf(stderr, "urbi: out of memory\n"); return NULL; }
    memcpy(buf, src, len);
    size_t t = len;
    while (t > 0 && (buf[t - 1] == ' ' || buf[t - 1] == '\t' ||
                     buf[t - 1] == '\n' || buf[t - 1] == '\r')) t--;
    if (t > 0 && buf[t - 1] == '|') { buf[len] = '\0'; *out_len = len; }
    else { buf[len] = ' '; buf[len + 1] = '|'; buf[len + 2] = '\0'; *out_len = len + 2; }
    return buf;
}

/* --- batch modes --------------------------------------------------------- */

/* Runs one whole source text and prints the result, the way -e and a
 * file argument both behave.  Returns the process exit status. */
static int run_once(UVM *vm, const char *src, size_t len, const char *name, bool print_result) {
    char err[512] = {0};
    UValue out;
    int rc = urbi_run(vm, urbi_realm_main(vm), src, len, name, &out, err, sizeof err);
    if (rc == URBI_OK) {
        if (print_result && !urbi_value_is_void(out)) {
            char fmt[512];
            urbi_value_to_string(vm, out, fmt, sizeof fmt);
            puts(fmt);
        }
        return 0;
    }
    if (rc == URBI_ERR_UNCAUGHT_THROW) {
        UErrorInfo info;
        urbi_last_error(vm, &info);
        fprintf(stderr, "urbi: %s\n", info.message && info.message[0] ? info.message : "uncaught throw");
    } else {
        fprintf(stderr, "urbi: %s\n", err[0] ? err : "run failed");
    }
    return 1;
}

static int run_dump(UVM *vm, const char *src, size_t len, const char *name, bool wire) {
    char err[512] = {0};
    UProto *root = NULL;
    if (wire) {
        uint8_t *bytes = NULL; size_t n = 0;
        int rc = urbi_compile(vm, src, len, name, &bytes, &n, err, sizeof err);
        if (rc != URBI_OK) { fprintf(stderr, "urbi: %s\n", err); return 1; }
        fwrite(bytes, 1, n, stdout);
        urbi_chunk_free(vm, bytes, n);
        return 0;
    }
    if (ufront_compile(vm, src, len, name, &root, err, sizeof err) != URBI_OK) {
        fprintf(stderr, "urbi: %s\n", err);
        return 1;
    }
    ufront_disassemble(root, NULL);
    uchunk_destroy(root, NULL);
    return 0;
}

/* --- interactive / line mode ---------------------------------------------- */

static struct timespec g_start_time;
static volatile sig_atomic_t g_interrupted = 0;

static uint32_t ms_since_start(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long sec  = (long)(now.tv_sec - g_start_time.tv_sec);
    long nsec = now.tv_nsec - g_start_time.tv_nsec;
    if (nsec < 0) { sec -= 1; nsec += 1000000000L; }
    uint64_t ms = (uint64_t)sec * 1000ULL + (uint64_t)nsec / 1000000ULL;
    return (uint32_t)ms;
}

static void sigint_handler(int sig) { (void)sig; g_interrupted = 1; }

/* Script output.  Everything a program echoes arrives here already
 * framed by the Lobby primitive, so the CLI writes it through
 * unchanged and interleaved with the REPL's own result lines. */
static void cli_writer(void *ud, const char *chan, size_t cl, const char *msg, size_t ml)
{
    (void)ud; (void)chan; (void)cl;
    fwrite(msg, 1, ml, stdout);
    fflush(stdout);
}

/* Runtime diagnostics, framed like a result line.
 *
 * This is the ONLY way a strand that nobody is waiting on can report a
 * failure: a detached arm's uncaught throw has no caller to return a code
 * to, so without a diag hook `{ throw 1 } , { ok() }` succeeds silently.
 * Same "!!!" spelling as the REPL's own error frame, because it means the
 * same thing. */
static void cli_diag(UVM *vm, void *ud, int level, const char *msg, size_t len)
{
    (void)vm; (void)ud; (void)level;
    printf("[%08u] !!! %.*s\n", ms_since_start(), (int)len, msg);
    fflush(stdout);
}

/* Monotonic microseconds since process start, so the timestamp the
 * Lobby frame carries and the one the REPL prints share an origin. */
static uint64_t cli_clock(void *ud)
{
    (void)ud;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long sec = (long)(now.tv_sec - g_start_time.tv_sec);
    long nsec = now.tv_nsec - g_start_time.tv_nsec;
    if (nsec < 0) { sec -= 1; nsec += 1000000000L; }
    return (uint64_t)sec * 1000000ULL + (uint64_t)nsec / 1000ULL;
}

static char *history_path(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return NULL;
    size_t hlen = strlen(home);
    const char *suffix = "/.urbi_history";
    size_t slen = strlen(suffix);
    char *path = malloc(hlen + slen + 1);
    if (!path) return NULL;
    memcpy(path, home, hlen);
    memcpy(path + hlen, suffix, slen);
    path[hlen + slen] = '\0';
    return path;
}

/* Track bracket depth across lines, ignoring brackets inside string
 * literals and comments, so a statement may span several input lines. */
typedef struct {
    int  depth;          /* net open ( [ { */
    bool in_str;         /* inside "..." */
    bool in_block;       /* inside a slash-star comment */
} UContState;

static void cont_scan(UContState *st, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (st->in_block) {
            if (c == '*' && i + 1 < n && s[i + 1] == '/') { st->in_block = false; i++; }
            continue;
        }
        if (st->in_str) {
            if (c == '\\') { i++; continue; }
            if (c == '"') st->in_str = false;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') return;          /* line comment */
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { st->in_block = true; i++; continue; }
        if (c == '"') { st->in_str = true; continue; }
        if (c == '(' || c == '[' || c == '{') st->depth++;
        if (c == ')' || c == ']' || c == '}') st->depth--;
    }
}

static char      *pending;
static size_t     pending_len;
static size_t     pending_cap;
static UContState cont;

static bool cont_append(const char *line, size_t ll) {
    if (pending_len + ll + 4 > pending_cap) {
        size_t nc = (pending_cap ? pending_cap * 2 : 256);
        while (nc < pending_len + ll + 4) nc *= 2;
        char *np = realloc(pending, nc);
        if (!np) return false;
        pending = np; pending_cap = nc;
    }
    memcpy(pending + pending_len, line, ll);
    pending_len += ll;
    pending[pending_len++] = '\n';
    cont_scan(&cont, line, ll);
    return cont.depth <= 0 && !cont.in_str && !cont.in_block;   /* true = ready to eval */
}

/* Evaluate the accumulated statement and print the framed result line.
 * cont_append always appends a '\n' after the last physical line, which
 * is bookkeeping rather than statement text — dropping it here keeps the
 * line/column of a single-line diagnostic correct. */
static void cont_flush(UVM *vm) {
    size_t ll = pending_len - 1;
    size_t final_len = 0;
    char *buf = terminate(pending, ll, &final_len);
    if (buf) {
        char err[512] = {0};
        UValue out;
        int rc = urbi_run(vm, urbi_realm_main(vm), buf, final_len, NULL, &out, err, sizeof err);
        if (rc == URBI_OK) {
            char fmt[512];
            urbi_value_to_string(vm, out, fmt, sizeof fmt);
            printf("[%08u] %s\n", ms_since_start(), fmt);
        } else {
            const char *msg = err[0] ? err : NULL;
            if (!msg) {
                UErrorInfo info;
                urbi_last_error(vm, &info);
                msg = (info.message && info.message[0]) ? info.message : "(vm error)";
            }
            printf("[%08u] !!! %s\n", ms_since_start(), msg);
        }
        fflush(stdout);
        free(buf);
    }
    pending_len = 0;
    cont = (UContState){0};
}

static int run_interactive(UVM *vm) {
    signal(SIGINT, sigint_handler);

    char *histpath = history_path();
    linenoiseHistorySetMaxLen(1000);
    if (histpath) linenoiseHistoryLoad(histpath);

    for (;;) {
        if (g_interrupted) { g_interrupted = 0; continue; }
        char *line = linenoise("");
        if (line == NULL) break;   /* Ctrl-D or error */
        if (line[0] == '\0') { free(line); continue; }
        linenoiseHistoryAdd(line);
        if (histpath) linenoiseHistorySave(histpath);
        bool ready = cont_append(line, strlen(line));
        free(line);
        if (ready) cont_flush(vm);
    }

    /* An unbalanced tail at EOF is still evaluated, so a genuine parse
     * error (unclosed bracket) is reported rather than swallowed. */
    if (pending_len > 0) cont_flush(vm);

    if (histpath) linenoiseHistorySave(histpath);
    free(histpath);
    free(pending);
    pending = NULL;
    pending_cap = 0;
    return 0;
}

/* --- main ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (eq(argv[i], "--version") || eq(argv[i], "-V")) { printf("urbi %s\n", urbi_version()); return EXIT_SUCCESS; }
        if (eq(argv[i], "--help") || eq(argv[i], "-h")) { print_usage(stdout); return EXIT_SUCCESS; }
    }

    bool dump = false, dump_wire = false, want_interactive = false;
    for (int i = 1; i < argc; i++) {
        if (eq(argv[i], "--dump-bytecode")) dump = true;
        if (eq(argv[i], "--dump-wire-format")) dump_wire = true;
        if (eq(argv[i], "-i")) want_interactive = true;
    }
    if ((dump || dump_wire) && want_interactive) {
        fprintf(stderr, "urbi: --dump-bytecode/--dump-wire-format requires -e <expr> or a file\n");
        return 2;
    }
    if (dump && dump_wire) {
        fprintf(stderr, "urbi: --dump-bytecode and --dump-wire-format are mutually exclusive\n");
        return 2;
    }

    const char *expr = NULL, *file_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (eq(argv[i], "-e")) {
            if (i + 1 >= argc) { fprintf(stderr, "urbi: -e requires an argument\n"); return 2; }
            expr = argv[i + 1];
            break;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (eq(argv[i], "-f")) {
            if (i + 1 >= argc) { fprintf(stderr, "urbi: -f requires a path argument\n"); return 2; }
            file_arg = argv[i + 1];
            break;
        }
    }
    if (!file_arg) {
        for (int i = 1; i < argc; i++) {
            if (eq(argv[i], "-e") || eq(argv[i], "-f")) { i++; continue; }
            if (argv[i][0] != '-') { file_arg = argv[i]; break; }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &g_start_time);
    UVM *vm = urbi_open(cli_alloc, NULL, NULL);
    if (!vm) { fprintf(stderr, "urbi: out of memory\n"); return 1; }
    urbi_set_writer(vm, cli_writer, NULL);
    urbi_set_diag(vm, cli_diag, NULL);
    urbi_set_clock(vm, cli_clock, NULL);
    int rc = EXIT_SUCCESS;

    if (dump || dump_wire) {
        if (expr) {
            size_t final_len = 0;
            char *buf = terminate(expr, strlen(expr), &final_len);
            rc = buf ? run_dump(vm, buf, final_len, "<expr>", dump_wire) : 1;
            free(buf);
        } else if (file_arg) {
            size_t flen = 0;
            char *src = slurp(file_arg, &flen);
            rc = src ? run_dump(vm, src, flen, file_arg, dump_wire) : 2;
            free(src);
        } else {
            fprintf(stderr, "urbi: --dump-bytecode requires -e <expr> or a file\n");
            rc = 2;
        }
    } else if (expr) {
        size_t final_len = 0;
        char *buf = terminate(expr, strlen(expr), &final_len);
        rc = buf ? run_once(vm, buf, final_len, "<expr>", true) : 1;
        free(buf);
    } else if (file_arg) {
        size_t flen = 0;
        char *src = slurp(file_arg, &flen);
        rc = src ? run_once(vm, src, flen, file_arg, false) : 2;
        free(src);
    } else if (want_interactive || isatty(fileno(stdin))) {
        /* -i, or a bare invocation on a terminal.  linenoise falls back
         * to plain line reading when stdin is not a tty, which is how
         * the .chk runner drives one statement per line. */
        rc = run_interactive(vm);
    } else if (argc == 1) {
        size_t flen = 0;
        char *src = slurp("-", &flen);
        rc = src ? run_once(vm, src, flen, "<stdin>", false) : 2;
        free(src);
    } else {
        fprintf(stderr, "urbi: unknown option: %s\n", argv[1]);
        print_usage(stderr);
        rc = 2;
    }

    urbi_close(vm);
    return rc;
}
