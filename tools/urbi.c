/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi — the M1 REPL binary.  Host-only. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Format a parser error (AST_ERROR node) to stderr.
   Prefix: "urbi: <src>:<line>:<col>: <message>\n". */
static void print_parse_error(const UAstNode *node, const char *src_name) {
    const char *msg = node->u.err.message ? node->u.err.message : "parse error";
    fprintf(stderr, "urbi: %s:%d:%d: %s\n", src_name, node->line, node->col, msg);
}

static void print_emit_error(const UEmitter *e, const char *src_name) {
    const char *name = uemit_error_name(e->error);
    fprintf(stderr, "urbi: %s: emit error: %s\n", src_name, name);
}

static void print_vm_error(const UVM *vm) {
    const char *msg = vm->last_errmsg[0] ? vm->last_errmsg : "(vm error)";
    fprintf(stderr, "urbi: %s\n", msg);
}

/* Compile src (length len) into *out_module.
   Returns true on success; caller owns the module and must call
   umodule_destroy + uarena_destroy when done.
   On error, prints to stderr, destroys internal state, and returns false;
   caller must NOT call umodule_destroy / uarena_destroy (already done). */
static bool compile_source(const char *src, size_t len, const char *src_name,
                           UModule *out_module, UArena *arena) {
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
            print_parse_error(node, src_name);
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
        print_emit_error(&e, src_name);
        umodule_destroy(out_module);
        uarena_destroy(arena);
        return false;
    }

    return true;
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
    if (compile_source(src, len, path, &module, &arena)) {
        UValue out;
        UVMError vrc = uvm_run(vm, &module, &out);
        if (vrc == UVM_OK) {
            rc = 0;
        } else {
            print_vm_error(vm);
            rc = 1;
        }
        umodule_destroy(&module);
        uarena_destroy(&arena);
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
    if (compile_source(buf, final_len, "<expr>", &module, &arena)) {
        UValue out;
        UVMError vrc = uvm_run(vm, &module, &out);
        if (vrc == UVM_OK) {
            char fmt[64];
            uvalue_format(&out, fmt, sizeof fmt);
            puts(fmt);
            rc = 0;
        } else {
            print_vm_error(vm);
            rc = 1;
        }
        umodule_destroy(&module);
        uarena_destroy(&arena);
    }
    free(buf);
    return rc;
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

    if (expr) {
        UVM vm;
        uvm_init(&vm, NULL, NULL);
        int rc = run_expression(&vm, expr);
        uvm_destroy(&vm);
        return rc;
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

    /* Reject unknown flags (no mode matched). */
    if (argc > 1 && argv[1][0] == '-') {
        fprintf(stderr, "urbi: unknown option: %s\n", argv[1]);
        print_usage(stderr);
        return 2;
    }

    /* No other modes implemented yet — further modes land in later tasks. */
    return EXIT_SUCCESS;
}
