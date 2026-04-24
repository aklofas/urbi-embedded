/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi — the M1 REPL binary.  Host-only. */

/* Enable POSIX interfaces: clock_gettime, struct timespec, fileno, isatty. */
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "uarena.h"
#include "uast.h"
#include "uemit.h"
#include "ulex.h"
#include "umodule.h"
#include "uparse.h"
#include "urbi.h"
#include "uvalue.h"
#include "uvm.h"

#include "linenoise.h"

/* --- helpers --- */

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
        "\n"
        "Options:\n"
        "  --version, -V        print version and exit\n"
        "  --help, -h           print this help and exit\n"
        "\n"
        "Exit status:\n"
        "  0   success\n"
        "  1   runtime or compile error\n"
        "  2   usage error (bad flag, missing file)\n",
        out);
}

static int eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* Compile src (length len) into *out_module.
   Returns true on success; caller owns the module and must call
   umodule_destroy + uarena_destroy when done.
   On failure, writes the formatted error message (no trailing newline) into
   err_buf (up to err_cap bytes, NUL-terminated), destroys internal state,
   and returns false; caller must NOT call umodule_destroy / uarena_destroy. */
static bool compile_source(const char *src, size_t len, const char *src_name,
                           UModule *out_module, UArena *arena,
                           char *err_buf, size_t err_cap) {
    ULexer lex;
    ulex_init(&lex, src, len);

    uarena_init(arena, 4096);

    *out_module = (UModule){0};
    UEmitter e;
    uemit_init(&e, out_module, arena, src_name);

    UParser p;
    uparse_init(&p, &lex, arena);

    UAstNode *node;
    bool had_error = false;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            const char *msg = node->u.err.message ? node->u.err.message : "parse error";
            snprintf(err_buf, err_cap, "%s:%d:%d: %s", src_name, node->line, node->col, msg);
            had_error = true;
            break;
        }
        (void)uemit_statement(&e, node);
        uarena_reset(arena);
    }

    if (had_error) {
        umodule_destroy(out_module);
        uarena_destroy(arena);
        return false;
    }

    if (uemit_finish(&e) != EMIT_OK) {
        snprintf(err_buf, err_cap, "%s: emit error: %s", src_name, uemit_error_name(e.error));
        umodule_destroy(out_module);
        uarena_destroy(arena);
        return false;
    }

    return true;
}

/* Compile src and print disassembly to stdout.  Does not execute.
   Returns 0 on success, 1 on compile error. */
static int run_dump(const char *src, size_t len, const char *src_name) {
    UArena arena;
    UModule module;
    char err[256] = {0};
    if (!compile_source(src, len, src_name, &module, &arena, err, sizeof err)) {
        fprintf(stderr, "urbi: %s\n", err);
        return 1;
    }
    char buf[4096];
    size_t n = uemit_disassemble(&module, buf, sizeof buf);
    fwrite(buf, 1, n, stdout);
    if (n > 0 && buf[n - 1] != '\n') fputc('\n', stdout);
    umodule_destroy(&module);
    uarena_destroy(&arena);
    return 0;
}

#define URBI_REPL_MAX_FILE (1024u * 1024u)

/* Slurp a file (or stdin, with path=="-") into a freshly-malloc'd buffer.
   Returns pointer on success, NULL on error (message printed to stderr).
   *out_len receives the byte count. */
static char *slurp(const char *path, size_t *out_len) {
    FILE *fp = NULL;
    if (strcmp(path, "-") == 0) {
        fp = stdin;
    } else {
        fp = fopen(path, "rb");
        if (!fp) {
            fprintf(stderr, "urbi: cannot open %s\n", path);
            return NULL;
        }
    }

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fprintf(stderr, "urbi: out of memory\n"); goto fail; }

    for (;;) {
        if (len + 4096 > cap) {
            if (cap >= URBI_REPL_MAX_FILE) {
                fprintf(stderr, "urbi: %s exceeds %u byte cap\n",
                        path, URBI_REPL_MAX_FILE);
                goto fail;
            }
            size_t ncap = cap * 2;
            if (ncap > URBI_REPL_MAX_FILE) ncap = URBI_REPL_MAX_FILE;
            char *n = realloc(buf, ncap);
            if (!n) { fprintf(stderr, "urbi: out of memory\n"); goto fail; }
            buf = n;
            cap = ncap;
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

/* Run a source file: compile, execute, discard result (scripts produce
   output via side effects, which do not exist at M1). */
static int run_file(UVM *vm, const char *path) {
    size_t len = 0;
    char *src = slurp(path, &len);
    if (!src) return 2;

    UArena arena;
    UModule module;
    int rc = 1;
    char err[256] = {0};
    if (compile_source(src, len, path, &module, &arena, err, sizeof err)) {
        UValue out;
        UVMError vrc = uvm_run(vm, &module, &out);
        if (vrc == UVM_OK) {
            rc = 0;
        } else {
            fprintf(stderr, "urbi: %s\n",
                    vm->last_errmsg[0] ? vm->last_errmsg : "(vm error)");
            rc = 1;
        }
        umodule_destroy(&module);
        uarena_destroy(&arena);
    } else {
        fprintf(stderr, "urbi: %s\n", err);
    }
    free(src);
    return rc;
}

/* Run one expression string, print result to stdout, return 0 on success. */
static int run_expression(UVM *vm, const char *expr) {
    /* Append " |" to terminate the statement if the expression doesn't already
       end with a pipe separator. */
    size_t len = strlen(expr);
    char *buf = malloc(len + 3);  /* expr + " |" + NUL */
    if (!buf) {
        fprintf(stderr, "urbi: out of memory\n");
        return 1;
    }
    memcpy(buf, expr, len);

    /* Trim trailing whitespace for the has-pipe check. */
    size_t t = len;
    while (t > 0 && (buf[t - 1] == ' ' || buf[t - 1] == '\t' ||
                     buf[t - 1] == '\n' || buf[t - 1] == '\r')) {
        t--;
    }

    size_t final_len;
    if (t > 0 && buf[t - 1] == '|') {
        buf[len] = '\0';
        final_len = len;
    } else {
        buf[len]     = ' ';
        buf[len + 1] = '|';
        buf[len + 2] = '\0';
        final_len = len + 2;
    }

    UArena arena;
    UModule module;
    int rc = 1;
    char err[256] = {0};
    if (compile_source(buf, final_len, "<expr>", &module, &arena, err, sizeof err)) {
        UValue out;
        UVMError vrc = uvm_run(vm, &module, &out);
        if (vrc == UVM_OK) {
            char fmt[64];
            uvalue_format(&out, fmt, sizeof fmt);
            puts(fmt);
            rc = 0;
        } else {
            fprintf(stderr, "urbi: %s\n",
                    vm->last_errmsg[0] ? vm->last_errmsg : "(vm error)");
            rc = 1;
        }
        umodule_destroy(&module);
        uarena_destroy(&arena);
    } else {
        fprintf(stderr, "urbi: %s\n", err);
    }
    free(buf);
    return rc;
}

/* --- interactive mode --- */

static struct timespec g_start_time;
static volatile sig_atomic_t g_interrupted = 0;

static uint32_t ms_since_start(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long sec  = (long)(now.tv_sec  - g_start_time.tv_sec);
    long nsec = now.tv_nsec - g_start_time.tv_nsec;
    if (nsec < 0) { sec -= 1; nsec += 1000000000L; }
    uint64_t ms = (uint64_t)sec * 1000ULL + (uint64_t)nsec / 1000000ULL;
    return (uint32_t)ms;
}

static void sigint_handler(int sig) { (void)sig; g_interrupted = 1; }

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

static int run_interactive(UVM *vm) {
    clock_gettime(CLOCK_MONOTONIC, &g_start_time);
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

        /* Append " |" if missing. */
        size_t ll = strlen(line);
        size_t t  = ll;
        while (t > 0 && (line[t - 1] == ' ' || line[t - 1] == '\t' ||
                         line[t - 1] == '\r' || line[t - 1] == '\n')) {
            t--;
        }
        size_t bufcap = ll + 3;
        char *buf = malloc(bufcap);
        if (!buf) { free(line); continue; }
        memcpy(buf, line, ll);
        size_t final_len;
        if (t > 0 && line[t - 1] == '|') {
            buf[ll] = '\0';
            final_len = ll;
        } else {
            buf[ll]     = ' ';
            buf[ll + 1] = '|';
            buf[ll + 2] = '\0';
            final_len   = ll + 2;
        }

        UArena arena;
        UModule module;
        char err[256] = {0};
        if (compile_source(buf, final_len, "<stdin>", &module, &arena, err, sizeof err)) {
            UValue out;
            UVMError vrc = uvm_run(vm, &module, &out);
            if (vrc == UVM_OK) {
                char fmt[64];
                uvalue_format(&out, fmt, sizeof fmt);
                printf("[%08u] %s\n", ms_since_start(), fmt);
            } else {
                const char *msg = vm->last_errmsg[0] ? vm->last_errmsg : "(vm error)";
                printf("[%08u] !!! %s\n", ms_since_start(), msg);
            }
            umodule_destroy(&module);
            uarena_destroy(&arena);
        } else {
            printf("[%08u] !!! %s\n", ms_since_start(), err);
        }
        fflush(stdout);

        free(buf);
        free(line);
    }

    if (histpath) linenoiseHistorySave(histpath);
    free(histpath);
    return 0;
}

/* --- main --- */

int main(int argc, char *argv[]) {
    /* Handle --version / --help short-circuits. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (eq(a, "--version") || eq(a, "-V")) {
            printf("urbi %s\n", urbi_version());
            return EXIT_SUCCESS;
        }
        if (eq(a, "--help") || eq(a, "-h")) {
            print_usage(stdout);
            return EXIT_SUCCESS;
        }
    }

    /* Scan for --dump-bytecode; incompatible with -i. */
    bool dump = false;
    bool want_interactive = false;
    for (int i = 1; i < argc; i++) {
        if (eq(argv[i], "--dump-bytecode")) dump = true;
        if (eq(argv[i], "-i")) want_interactive = true;
    }

    if (dump && want_interactive) {
        fprintf(stderr, "urbi: --dump-bytecode requires -e <expr> or a file\n");
        return 2;
    }

    /* Scan for -e. */
    const char *expr = NULL;
    for (int i = 1; i < argc; i++) {
        if (eq(argv[i], "-e")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "urbi: -e requires an argument\n");
                return 2;
            }
            expr = argv[i + 1];
            break;
        }
    }

    /* Scan for -f. */
    const char *file_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (eq(argv[i], "-f")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "urbi: -f requires a path argument\n");
                return 2;
            }
            file_arg = argv[i + 1];
            break;
        }
    }

    /* Positional file: first non-flag argument that isn't an -e/-f value. */
    if (!file_arg) {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] != '-') {
                file_arg = argv[i];
                break;
            }
        }
    }

    /* --dump-bytecode dispatch: compile and disassemble, no execution. */
    if (dump) {
        if (expr) {
            /* Append " |" to terminate the statement, matching run_expression. */
            size_t len = strlen(expr);
            char *buf = malloc(len + 3);
            if (!buf) { fprintf(stderr, "urbi: out of memory\n"); return 1; }
            memcpy(buf, expr, len);

            size_t t = len;
            while (t > 0 && (buf[t - 1] == ' ' || buf[t - 1] == '\t')) t--;

            size_t final_len;
            if (t > 0 && buf[t - 1] == '|') {
                buf[len] = '\0';
                final_len = len;
            } else {
                buf[len]     = ' ';
                buf[len + 1] = '|';
                buf[len + 2] = '\0';
                final_len = len + 2;
            }
            int rc = run_dump(buf, final_len, "<expr>");
            free(buf);
            return rc;
        }
        if (file_arg) {
            size_t flen = 0;
            char *src = slurp(file_arg, &flen);
            if (!src) return 2;
            int rc = run_dump(src, flen, file_arg);
            free(src);
            return rc;
        }
        fprintf(stderr, "urbi: --dump-bytecode requires -e <expr> or a file\n");
        return 2;
    }

    if (expr) {
        UVM vm;
        uvm_init(&vm, NULL, NULL);
        int rc = run_expression(&vm, expr);
        uvm_destroy(&vm);
        return rc;
    }

    if (file_arg) {
        UVM vm;
        uvm_init(&vm, NULL, NULL);
        int rc = run_file(&vm, file_arg);
        uvm_destroy(&vm);
        return rc;
    }

    /* No -e, no file: check stdin. */
    if (argc == 1 && !isatty(fileno(stdin))) {
        UVM vm;
        uvm_init(&vm, NULL, NULL);
        int rc = run_file(&vm, "-");
        uvm_destroy(&vm);
        return rc;
    }

    /* -i explicit or (no args, stdin is a tty). */
    if (want_interactive ||
        (argc == 1 && isatty(fileno(stdin)))) {
        UVM vm;
        uvm_init(&vm, NULL, NULL);
        int rc = run_interactive(&vm);
        uvm_destroy(&vm);
        return rc;
    }

    /* Reject unknown flags (no mode matched). */
    if (argc > 1 && argv[1][0] == '-') {
        fprintf(stderr, "urbi: unknown option: %s\n", argv[1]);
        print_usage(stderr);
        return 2;
    }

    return EXIT_SUCCESS;
}
